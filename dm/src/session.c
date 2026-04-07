#define _POSIX_C_SOURCE 200809L
/*
 * session.c — X server and user session lifecycle
 */
#include "dm.h"
#include "isde/isde-desktop.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <pwd.h>
#include <grp.h>
#include <sys/stat.h>
#include <time.h>

/* ---------- Xauthority cookie ---------- */

static int generate_xauth(Dm *dm)
{
    /* Generate random cookie */
    int fd = open("/dev/urandom", O_RDONLY);
    if (fd < 0) {
        return -1;
    }
    ssize_t n = read(fd, dm->xauth_cookie, sizeof(dm->xauth_cookie));
    close(fd);
    if (n != (ssize_t)sizeof(dm->xauth_cookie)) {
        return -1;
    }

    /* Write Xauthority file */
    snprintf(dm->xauth_path, sizeof(dm->xauth_path),
             "%s/Xauthority", dm->plat->rundir);

    fd = open(dm->xauth_path, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (fd < 0) {
        return -1;
    }

    /*
     * Xauthority file format (binary):
     *   u16 family (0x0100 = FamilyLocal, but we use FamilyWild = 0xFFFF)
     *   u16 address_len + address
     *   u16 display_len + display number string
     *   u16 name_len + "MIT-MAGIC-COOKIE-1"
     *   u16 data_len + cookie bytes
     *
     * Use FamilyWild (65535) so any host matches.
     */
    char display_str[8];
    int dlen = snprintf(display_str, sizeof(display_str), "%d",
                       dm->display_num);

    const char *proto = "MIT-MAGIC-COOKIE-1";
    int proto_len = strlen(proto);

    unsigned char buf[512];
    int off = 0;

    /* Family: FamilyWild */
    buf[off++] = 0xFF;
    buf[off++] = 0xFF;
    /* Address: empty for wild */
    buf[off++] = 0;
    buf[off++] = 0;
    /* Display number */
    buf[off++] = (dlen >> 8) & 0xFF;
    buf[off++] = dlen & 0xFF;
    memcpy(buf + off, display_str, dlen);
    off += dlen;
    /* Protocol name */
    buf[off++] = (proto_len >> 8) & 0xFF;
    buf[off++] = proto_len & 0xFF;
    memcpy(buf + off, proto, proto_len);
    off += proto_len;
    /* Cookie data */
    buf[off++] = (sizeof(dm->xauth_cookie) >> 8) & 0xFF;
    buf[off++] = sizeof(dm->xauth_cookie) & 0xFF;
    memcpy(buf + off, dm->xauth_cookie, sizeof(dm->xauth_cookie));
    off += sizeof(dm->xauth_cookie);

    write(fd, buf, off);
    close(fd);

    return 0;
}

/* ---------- X server ---------- */

static int find_free_display(void)
{
    for (int i = 1; i < 64; i++) {
        char lockfile[64];
        snprintf(lockfile, sizeof(lockfile), "/tmp/.X%d-lock", i);
        if (access(lockfile, F_OK) != 0) {
            return i;
        }
    }
    return -1;
}

int dm_xserver_start(Dm *dm, int vt)
{
    /* In dev mode, find an unused display since we have no seat manager */
    if (dm->dev_mode) {
        int free_display = find_free_display();
        if (free_display < 0) {
            fprintf(stderr, "isde-dm: no free display number found\n");
            return -1;
        }
        dm->display_num = free_display;
    }

    snprintf(dm->display, sizeof(dm->display), ":%d", dm->display_num);

    if (generate_xauth(dm) != 0) {
        fprintf(stderr, "isde-dm: cannot generate Xauthority\n");
        return -1;
    }

    pid_t pid = fork();
    if (pid < 0) {
        return -1;
    }

    if (pid == 0) {
        /* Child: exec X server */
        if (dm->dev_mode) {
            /* Dev mode: launch Xephyr (nested X server for testing).
             * Xephyr does not take VT or -auth args. */
            execlp(dm->xserver_cmd, dm->xserver_cmd,
                   dm->display,
                   "-screen", "1024x768",
                   "-noreset",
                   (char *)NULL);
        } else {
            /* Production: launch Xorg with VT and auth */
            char vt_arg[16];
            snprintf(vt_arg, sizeof(vt_arg), "vt%d", vt);

            execlp(dm->xserver_cmd, dm->xserver_cmd,
                   dm->display, vt_arg,
                   "-auth", dm->xauth_path,
                   "-nolisten", "tcp",
                   "-noreset",
                   (char *)NULL);
        }
        fprintf(stderr, "isde-dm: exec '%s' failed: %s\n",
                dm->xserver_cmd, strerror(errno));
        _exit(1);
    }

    dm->xserver_pid = pid;
    fprintf(stderr, "isde-dm: X server started (pid %d) on %s %s\n",
            pid, dm->display, dm->xauth_path);
    return 0;
}

void dm_xserver_stop(Dm *dm)
{
    if (dm->xserver_pid > 0) {
        kill(dm->xserver_pid, SIGTERM);
        /* Give it a moment, then force */
        struct timespec ts = { .tv_sec = 0, .tv_nsec = 500000000 };
        nanosleep(&ts, NULL);
        if (dm->xserver_pid > 0) {
            kill(dm->xserver_pid, SIGKILL);
        }
    }
}

int dm_xserver_ready(Dm *dm)
{
    /* Poll for the X server display socket to appear */
    char lockfile[64];
    snprintf(lockfile, sizeof(lockfile), "/tmp/.X%d-lock", dm->display_num);

    for (int i = 0; i < 50; i++) {  /* up to 5 seconds */
        if (access(lockfile, F_OK) == 0) {
            /* Also try connecting to verify it's ready */
            xcb_connection_t *conn = xcb_connect(dm->display, NULL);
            if (conn && !xcb_connection_has_error(conn)) {
                xcb_disconnect(conn);
                fprintf(stderr, "isde-dm: X server ready on %s\n",
                        dm->display);
                return 0;
            }
            if (conn) {
                xcb_disconnect(conn);
            }
        }

        struct timespec ts = { .tv_sec = 0, .tv_nsec = 100000000 };
        nanosleep(&ts, NULL);

        /* Check if X server died */
        if (dm->xserver_pid == 0) {
            return -1;
        }
    }

    fprintf(stderr, "isde-dm: X server did not start in time\n");
    return -1;
}

/* ---------- User session ---------- */

static char *find_session_exec(const char *desktop_name)
{
    /* Look up the .desktop file in /usr/share/xsessions/ */
    char path[512];
    snprintf(path, sizeof(path), "/usr/share/xsessions/%s", desktop_name);

    IsdeDesktopEntry *entry = isde_desktop_load(path);
    if (!entry) {
        return NULL;
    }

    const char *exec = isde_desktop_exec(entry);
    char *result = exec ? strdup(exec) : NULL;
    isde_desktop_free(entry);
    return result;
}

int dm_session_start(Dm *dm, const char *username,
                     const char *desktop_file)
{
    /* Look up user */
    struct passwd *pw = getpwnam(username);
    if (!pw) {
        fprintf(stderr, "isde-dm: unknown user '%s'\n", username);
        return -1;
    }

    /* Find session command */
    char *exec_cmd = find_session_exec(desktop_file);
    if (!exec_cmd) {
        fprintf(stderr, "isde-dm: cannot find session '%s'\n", desktop_file);
        return -1;
    }

    /* Start PAM session */
    void *pam_handle = dm_auth_start_session(username);

    pid_t pid = fork();
    if (pid < 0) {
        free(exec_cmd);
        if (pam_handle) {
            dm_auth_end_session(pam_handle);
        }
        return -1;
    }

    if (pid == 0) {
        /* Child: become the user and exec session */

        /* Set up environment */
        clearenv();
        setenv("DISPLAY", dm->display, 1);
        if (!dm->dev_mode) {
            setenv("XAUTHORITY", dm->xauth_path, 1);
        }
        setenv("HOME", pw->pw_dir, 1);
        setenv("USER", pw->pw_name, 1);
        setenv("LOGNAME", pw->pw_name, 1);
        setenv("SHELL", pw->pw_shell ? pw->pw_shell : "/bin/sh", 1);
        setenv("PATH", "/usr/local/bin:/usr/bin:/bin", 1);
        setenv("XDG_SESSION_TYPE", "x11", 1);
        setenv("XDG_SEAT", "seat0", 1);

        char vtnr[8];
        snprintf(vtnr, sizeof(vtnr), "%d", dm->greeter_vt);
        setenv("XDG_VTNR", vtnr, 1);

        /* Set XDG directories */
        char xdg_cfg[512], xdg_data[512], xdg_cache[512], xdg_rt[512];
        snprintf(xdg_cfg, sizeof(xdg_cfg), "%s/.config", pw->pw_dir);
        snprintf(xdg_data, sizeof(xdg_data), "%s/.local/share", pw->pw_dir);
        snprintf(xdg_cache, sizeof(xdg_cache), "%s/.cache", pw->pw_dir);
        snprintf(xdg_rt, sizeof(xdg_rt), "/run/user/%d", pw->pw_uid);
        setenv("XDG_CONFIG_HOME", xdg_cfg, 1);
        setenv("XDG_DATA_HOME", xdg_data, 1);
        setenv("XDG_CACHE_HOME", xdg_cache, 1);
        setenv("XDG_RUNTIME_DIR", xdg_rt, 1);

        /* Ensure XDG_RUNTIME_DIR exists */
        mkdir(xdg_rt, 0700);
        chown(xdg_rt, pw->pw_uid, pw->pw_gid);

        /* Drop privileges */
        if (initgroups(pw->pw_name, pw->pw_gid) != 0 ||
            setgid(pw->pw_gid) != 0 ||
            setuid(pw->pw_uid) != 0) {
            fprintf(stderr, "isde-dm: failed to drop privileges: %s\n",
                    strerror(errno));
            _exit(1);
        }

        /* Change to home directory */
        if (chdir(pw->pw_dir) != 0) {
            chdir("/");
        }

        /* Exec the session */
        execl("/bin/sh", "sh", "-c", exec_cmd, (char *)NULL);
        fprintf(stderr, "isde-dm: exec session failed: %s\n",
                strerror(errno));
        _exit(1);
    }

    /* Parent — note: desktop_file may alias dm->session_desktop,
     * so we must strdup before freeing. */
    dm->session_pid = pid;
    free(dm->session_user);
    dm->session_user = strdup(username);

    char *saved_desktop = desktop_file ? strdup(desktop_file) : NULL;
    free(dm->session_desktop);
    dm->session_desktop = saved_desktop;

    free(exec_cmd);

    dm->session_active_since = time(NULL);

    const char *session_name = saved_desktop ? saved_desktop : "";
    fprintf(stderr, "isde-dm: session started for '%s' (pid %d)\n",
            username, pid);

    dm_dbus_emit_session_started(dm, username, session_name);
    return 0;
}

void dm_session_cleanup(Dm *dm)
{
    const char *user = dm->session_user ? dm->session_user : "";
    dm_dbus_emit_session_ended(dm, user);

    dm->session_pid = 0;
    dm->locked = 0;
    free(dm->session_user);
    dm->session_user = NULL;

    fprintf(stderr, "isde-dm: session ended, restarting greeter\n");

    /* Restart greeter in login mode */
    dm_greeter_start(dm);
}
