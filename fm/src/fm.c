#define _POSIX_C_SOURCE 200809L
/*
 * fm.c — app init, window management, config reload
 */
#include "fm.h"

#include <stdio.h>
#include "isde/isde-ewmh.h"
#include "isde/isde-dialog.h"
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <ISW/IswArgMacros.h>
#include <ISW/ISWRender.h>

/* App-wide shared state (will move to separate allocation in phase 2) */
static FmApp g_app;

/* Context key for storing Fm* on shell windows */
XContext fm_window_context = 0;

/* ---------- forward declarations ---------- */

static void app_remove_window(FmApp *app, Fm *fm);

/* ---------- D-Bus settings reload ---------- */

static void fm_reload_config(Fm *fm)
{
    char errbuf[256];
    IsdeConfig *cfg = isde_config_load_xdg("fm.toml", errbuf, sizeof(errbuf));
    if (cfg) {
        IsdeConfigTable *root = isde_config_root(cfg);
        IsdeConfigTable *gen = isde_config_table(root, "general");
        if (gen) {
            const char *click = isde_config_string(gen, "click_to_open", "double");
            fm->double_click = (strcmp(click, "single") != 0);
            fm->show_hidden = isde_config_bool(gen, "show_hidden", 0);
        }
        isde_config_free(cfg);
    }
    isde_config_invalidate_cache();
    fm_refresh(fm);
}

static void on_settings_changed(const char *section, const char *key,
                                void *user_data)
{
    (void)key;
    Fm *fm = (Fm *)user_data;
    FmApp *app = fm->app_state;
    if (strcmp(section, "general") == 0 ||
        strcmp(section, "input") == 0 ||
        strcmp(section, "appearance") == 0 ||
        strcmp(section, "*") == 0) {
        if (strcmp(section, "appearance") == 0 || strcmp(section, "*") == 0) {
            isde_theme_reload();
            icons_init(app);
        }
        fm_reload_config(fm);
    }
}

static void dbus_input_cb(IswPointer client_data, int *fd, IswInputId *id)
{
    (void)fd;
    (void)id;
    IsdeDBus *bus = (IsdeDBus *)client_data;
    isde_dbus_dispatch(bus);
}

/* ---------- close handling ---------- */

static void fm_destroy_cb(Widget w, IswPointer cd, IswPointer call)
{
    (void)w; (void)call;
    Fm *fm = (Fm *)cd;
    FmApp *app = fm->app_state;
    app_remove_window(app, fm);
    thumbs_cancel(fm);
    cwd_watch_stop(fm);
    fm_dismiss_context(fm);
    ctx_free_dynamic(fm);
    dnd_cleanup(fm);
    clipboard_cleanup(fm);
    browser_free_entries(fm);
    fileview_cleanup(fm);
    places_cleanup(fm);
    free(fm->cwd);
    for (int i = 0; i < fm->hist_count; i++) {
        free(fm->history[i]);
    }
    free(fm);
}

/* ---------- window tracking ---------- */

static void app_add_window(FmApp *app, Fm *fm)
{
    app->windows = realloc(app->windows,
                           (app->nwindows + 1) * sizeof(Fm *));
    app->windows[app->nwindows++] = fm;
}

static void app_remove_window(FmApp *app, Fm *fm)
{
    for (int i = 0; i < app->nwindows; i++) {
        if (app->windows[i] == fm) {
            app->windows[i] = app->windows[--app->nwindows];
            break;
        }
    }
    if (app->nwindows == 0) {
        app->running = 0;
        IswAppSetExitFlag(app->app);
    }
}

/* ---------- per-window config ---------- */

static void load_window_config(Fm *fm)
{
    fm->double_click = 1;
    char errbuf[256];
    IsdeConfig *cfg = isde_config_load_xdg("fm.toml", errbuf, sizeof(errbuf));
    if (cfg) {
        IsdeConfigTable *root = isde_config_root(cfg);
        IsdeConfigTable *gen = isde_config_table(root, "general");
        if (gen) {
            const char *click = isde_config_string(gen, "click_to_open", "double");
            fm->double_click = (strcmp(click, "single") != 0);
            fm->show_hidden = isde_config_bool(gen, "show_hidden", 0);
        }
        isde_config_free(cfg);
    }
}

/* ---------- app init / run / cleanup ---------- */

int fm_app_init(FmApp *app, int *argc, char **argv)
{
    memset(app, 0, sizeof(*app));
    app->mount_inotify_fd = -1;

    /* Initialize context key for fm_from_widget lookups */
    fm_window_context = IswUniqueContext();

    app->first_toplevel = IswAppInitialize(&app->app, "ISDE-FM",
                                          NULL, 0, argc, argv,
                                          NULL, NULL, 0);
    isde_theme_merge_xrm(app->first_toplevel);

    /* Register actions globally (once) */
    fm_register_actions(app->app);

    /* Determine path to open */
    {
        const char *path = NULL;
        for (int i = 1; i < *argc; i++) {
            if (argv[i] && argv[i][0] != '-') {
                path = argv[i];
                break;
            }
        }
        if (!path) {
            const char *home = getenv("HOME");
            path = home ? home : "/";
        }
        if (strncmp(path, "file://", 7) == 0) {
            path += 7;
            if (path[0] == '\0')
                path = "/";
        }
        char *resolved = realpath(path, NULL);
        app->initial_path = resolved ? resolved : strdup(path);
    }

    /* Background file operations */
    jobqueue_init(app);

    /* Shared caches */
    icons_init(app);
    {
        static const char *app_dirs[] = {
            "/usr/share/applications",
            "/usr/local/share/applications",
            NULL
        };
        const char *home_env = getenv("HOME");
        char local_apps[512] = "";
        if (home_env) {
            snprintf(local_apps, sizeof(local_apps),
                     "%s/.local/share/applications", home_env);
        }

        int cap = 0;
        for (int d = -1; app_dirs[d + 1] || d < 0; d++) {
            const char *dir = (d < 0) ? local_apps : app_dirs[d];
            if (!dir[0]) {
                continue;
            }
            int count = 0;
            IsdeDesktopEntry **batch = isde_desktop_scan_dir(dir, &count);
            if (!batch) {
                continue;
            }
            if (app->ndesktop + count > cap) {
                cap = app->ndesktop + count + 64;
                app->desktop_entries = realloc(app->desktop_entries,
                                               cap * sizeof(IsdeDesktopEntry *));
            }
            for (int i = 0; i < count; i++) {
                app->desktop_entries[app->ndesktop++] = batch[i];
            }
            free(batch);
        }
    }

    /* D-Bus settings notifications (shared) */
    app->dbus = isde_dbus_init();
    if (app->dbus) {
        int dbus_fd = isde_dbus_get_fd(app->dbus);
        if (dbus_fd >= 0) {
            IswAppAddInput(app->app, dbus_fd,
                          (IswPointer)IswInputReadMask,
                          dbus_input_cb, app->dbus);
        }
    }

    /* Thumbnail cache */
    thumbs_init(app);

    /* Custom script actions */
    actions_scan(app);

    /* Connect to mountd before creating windows so the sidebar
     * can show all devices (mounted and unmounted). */
    fm_mountd_init(app);

    /* Open initial window */
    Fm *first = fm_window_new(app, app->initial_path);
    if (!first) {
        return -1;
    }

    /* Single-instance check — now that the first window is realized,
     * use its toplevel for the selection ownership. */
    int rc = instance_try_primary(app, app->initial_path);
    if (rc == 0) {
        /* Another instance will handle the path — tear down and exit */
        fm_window_destroy(first);
        free(app->initial_path);
        app->initial_path = NULL;
        IswDestroyApplicationContext(app->app);
        return 1;
    }

    /* Subscribe D-Bus settings for first window (TODO: broadcast to all) */
    if (app->dbus) {
        isde_dbus_settings_subscribe(app->dbus, on_settings_changed, first);
    }

#ifdef __linux__
    /* Only use inotify mount monitor when mountd is not available —
     * mountd signals handle device changes when it's running. */
    if (!app->has_mountd)
        mount_monitor_init(app);
#endif

    app->running = 1;
    return 0;
}

void fm_update_title(Fm *fm)
{
    const char *name = strrchr(fm->cwd, '/');
    if (!name || name[1] == '\0')
        name = fm->cwd;
    else
        name++;

    IswArgBuilder ab = IswArgBuilderInit();
    IswArgTitle(&ab, (String)name);
    IswSetValues(fm->toplevel, ab.args, ab.count);
}

Fm *fm_window_new(FmApp *app, const char *path)
{
    Fm *fm = calloc(1, sizeof(Fm));
    if (!fm) {
        return NULL;
    }
    fm->app_state = app;
    fm->rename_index = -1;
    fm->last_click_index = -1;
    fm->cwd_inotify_fd = -1;
    fm->cwd_wd = -1;
    fm->ctx_target_index = -1;

    load_window_config(fm);

    /* Create toplevel shell — first window reuses IswAppInitialize's shell,
     * subsequent windows create new application shells. */
    int fm_w = 700;
    int fm_h = 500;
    isde_clamp_to_workarea(IswDisplay(app->first_toplevel), 0, &fm_w, &fm_h);

    if (app->nwindows == 0) {
        fm->toplevel = app->first_toplevel;
    } else {
        IswArgBuilder ab = IswArgBuilderInit();
        IswArgWidth(&ab, fm_w);
        IswArgHeight(&ab, fm_h);
        IswArgMinWidth(&ab, 400);
        IswArgMinHeight(&ab, 300);
        fm->toplevel = IswAppCreateShell("isde-fm", "ISDE-FM",
                                        applicationShellWidgetClass,
                                        IswDisplay(app->first_toplevel),
                                        ab.args, ab.count);
    }

    if (app->nwindows == 0) {
        IswArgBuilder ab = IswArgBuilderInit();
        IswArgWidth(&ab, fm_w);
        IswArgHeight(&ab, fm_h);
        IswArgMinWidth(&ab, 400);
        IswArgMinHeight(&ab, 300);
        IswSetValues(fm->toplevel, ab.args, ab.count);
    }

    IswAddCallback(fm->toplevel, IswNdestroyCallback, fm_destroy_cb, fm);

    /* Override Shell's default WM_DELETE_WINDOW handler (which calls
     * IswAppSetExitFlag, killing all windows) with one that only
     * closes this window. */
    IswOverrideTranslations(fm->toplevel, IswParseTranslationTable(
        "<Message>WM_PROTOCOLS: fm-close-window()\n"));

    /* MainWindow */
    fm->main_window = IswCreateManagedWidget("mainWin", mainWindowWidgetClass,
                                            fm->toplevel, NULL, 0);
    IswUnmanageChild(IswMainWindowGetMenuBar(fm->main_window));

    /* Status bar (MainWindow auto-claims StatusBar children) */
    IswCreateManagedWidget("statusBar", statusBarWidgetClass,
                           fm->main_window, NULL, 0);

    /* Outer FlexBox: vertical */
    IswArgBuilder ab = IswArgBuilderInit();
    IswArgOrientation(&ab, IswOrientVertical);
    IswArgBorderWidth(&ab, 0);
    fm->vbox = IswCreateManagedWidget("vbox", flexBoxWidgetClass,
                                      fm->main_window, ab.args, ab.count);

    navbar_init(fm);

    /* Content area: horizontal FlexBox */
    IswArgBuilderReset(&ab);
    IswArgOrientation(&ab, IswOrientHorizontal);
    IswArgBorderWidth(&ab, 0);
    IswArgFlexGrow(&ab, 1);
    fm->hbox = IswCreateManagedWidget("hbox", flexBoxWidgetClass,
                                      fm->vbox, ab.args, ab.count);

    places_init(fm);
    fileview_init(fm);
    clipboard_init(fm);

    /* Navigate to initial path — fall back to home or / if path is invalid */
    const char *start = path;
    if (browser_read_dir(fm, start) != 0) {
        const char *home = getenv("HOME");
        start = (home && home[0]) ? home : "/";
        if (browser_read_dir(fm, start) != 0)
            start = "/";
        browser_read_dir(fm, start);
    }
    fm->cwd = strdup(start);
    fm->history[0] = strdup(start);
    fm->hist_pos = 0;
    fm->hist_count = 1;

    cwd_watch_start(fm, fm->cwd);
    fm_update_title(fm);

    IswRealizeWidget(fm->toplevel);

    /* Store Fm* for fm_from_widget lookups */
    fm_set_context(fm->toplevel, fm);

    /* XDND init must be after realize — re-apply keyboard shortcuts
     * afterward since DnD translation overrides can clobber them. */
    dnd_init(fm);
    places_register_drop_targets(fm);
    fm_install_shortcuts(fm->iconview);
    fm_install_shortcuts(fm->listview);

    fileview_populate(fm);
    navbar_update(fm);

    app_add_window(app, fm);
    return fm;
}

void fm_window_destroy(Fm *fm)
{
    /* All actual cleanup happens in fm_destroy_cb (the IswNdestroyCallback).
     * This just triggers the widget destruction. */
    IswDestroyWidget(fm->toplevel);
}

void fm_app_run(FmApp *app)
{
    while (app->running && !IswAppGetExitFlag(app->app)) {
        IswAppProcessEvent(app->app, IswIMAll);
    }
}

void fm_app_cleanup(FmApp *app)
{
    /* Destroy any remaining windows */
    while (app->nwindows > 0) {
        Fm *fm = app->windows[0];
        IswDestroyWidget(fm->toplevel);
    }
    free(app->windows);

    fm_mountd_cleanup(app);
#ifdef __linux__
    mount_monitor_cleanup(app);
#endif
    jobqueue_shutdown(app);
    isde_dbus_free(app->dbus);
    icons_cleanup(app);
    thumbs_cleanup(app);
    actions_cleanup(app);
    free(app->initial_path);
    for (int i = 0; i < app->ndesktop; i++) {
        isde_desktop_free(app->desktop_entries[i]);
    }
    free(app->desktop_entries);
    IswDestroyApplicationContext(app->app);
}
