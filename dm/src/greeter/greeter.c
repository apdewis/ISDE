#define _POSIX_C_SOURCE 200809L
/*
 * greeter.c — isde-greeter UI construction and event handling
 */
#include "greeter.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <xcb/randr.h>

/* ---------- Geometry ---------- */

#define LOGIN_FORM_W     isde_scale(320)
#define INPUT_W          isde_scale(200)
#define LABEL_W          isde_scale(80)
#define BUTTON_W         isde_scale(80)
#define BUTTON_PAD       isde_scale(8)
#define ROW_GAP          isde_scale(8)
#define SECTION_GAP      isde_scale(16)
#define BOTTOM_MARGIN    isde_scale(32)

/* ---------- Callbacks ---------- */

static void login_cb(Widget w, XtPointer cd, XtPointer call)
{
    (void)w; (void)call;
    Greeter *g = (Greeter *)cd;

    /* Get username and password from text widgets */
    Arg args[20];
    String username = NULL;
    String password = NULL;

    XtSetArg(args[0], XtNstring, &username);
    XtGetValues(g->user_text, args, 1);

    XtSetArg(args[0], XtNstring, &password);
    XtGetValues(g->pass_text, args, 1);

    if (!username || !*username) {
        greeter_set_error(g, "Username is required");
        return;
    }
    if (!password) {
        password = "";
    }

    greeter_clear_error(g);

    /* Send session selection first if we have one */
    if (g->active_session >= 0 && g->active_session < g->nsessions) {
        char buf[512];
        snprintf(buf, sizeof(buf), "SESSION %s",
                 g->sessions[g->active_session].desktop_file);
        greeter_ipc_send(g, buf);
    }

    /* Send auth request */
    char buf[2048];
    snprintf(buf, sizeof(buf), "AUTH %s %s", username, password);
    greeter_ipc_send(g, buf);
}

static void shutdown_cb(Widget w, XtPointer cd, XtPointer call)
{
    (void)w; (void)call;
    Greeter *g = (Greeter *)cd;
    greeter_ipc_send(g, "SHUTDOWN");
}

static void reboot_cb(Widget w, XtPointer cd, XtPointer call)
{
    (void)w; (void)call;
    Greeter *g = (Greeter *)cd;
    greeter_ipc_send(g, "REBOOT");
}

static void suspend_cb(Widget w, XtPointer cd, XtPointer call)
{
    (void)w; (void)call;
    Greeter *g = (Greeter *)cd;
    greeter_ipc_send(g, "SUSPEND");
}

static void session_dropdown_cb(Widget w, XtPointer cd, XtPointer call)
{
    (void)w;
    Greeter *g = (Greeter *)cd;
    IswListReturnStruct *ret = (IswListReturnStruct *)call;
    if (ret->list_index >= 0 && ret->list_index < g->nsessions) {
        g->active_session = ret->list_index;
    }
}

/* Handle Enter key in password field → trigger login */
static void pass_action(Widget w, xcb_generic_event_t *ev, String *params,
                        Cardinal *nparams)
{
    (void)ev; (void)params; (void)nparams;

    /* Walk up to find the greeter — get the shell's parent data.
     * Simpler: use a static pointer since there's only one greeter. */
    Widget shell = w;
    while (shell && !XtIsShell(shell)) {
        shell = XtParent(shell);
    }
    if (!shell) {
        return;
    }

    /* Trigger login callback via the login button */
    XtCallCallbacks(XtNameToWidget(shell, "*loginBtn"), XtNcallback, NULL);
}

static XtActionsRec greeter_actions[] = {
    { "greeter-login", pass_action },
};

static char greeter_translations[] =
    "<Key>Return: greeter-login()\n";

/* ---------- IPC response handler ---------- */

static void handle_ipc_line(Greeter *g, const char *line);

static void ipc_input_cb(XtPointer client_data, int *fd, XtInputId *id)
{
    (void)fd; (void)id;
    Greeter *g = (Greeter *)client_data;

    int space = (int)sizeof(g->ipc_buf) - g->ipc_buf_len - 1;
    if (space <= 0) {
        g->ipc_buf_len = 0;
        return;
    }

    ssize_t n = read(g->ipc_fd, g->ipc_buf + g->ipc_buf_len, space);
    if (n <= 0) {
        fprintf(stderr, "isde-greeter: daemon disconnected\n");
        g->running = 0;
        return;
    }

    g->ipc_buf_len += n;
    g->ipc_buf[g->ipc_buf_len] = '\0';

    /* Process complete lines */
    char *start = g->ipc_buf;
    char *nl;
    while ((nl = strchr(start, '\n')) != NULL) {
        *nl = '\0';
        if (nl > start) {
            handle_ipc_line(g, start);
        }
        start = nl + 1;
    }

    int remaining = g->ipc_buf_len - (start - g->ipc_buf);
    if (remaining > 0 && start != g->ipc_buf) {
        memmove(g->ipc_buf, start, remaining);
    }
    g->ipc_buf_len = remaining;
}

static void handle_ipc_line(Greeter *g, const char *line)
{
    if (strcmp(line, "AUTH_OK") == 0) {
        fprintf(stderr, "isde-greeter: auth success\n");
        if (g->mode_lock) {
            /* Daemon handles unlock; we just exit */
        }
        g->running = 0;
    } else if (strncmp(line, "AUTH_FAIL ", 10) == 0) {
        greeter_set_error(g, line + 10);
        /* Clear password field */
        Arg args[20];
        XtSetArg(args[0], XtNstring, "");
        XtSetValues(g->pass_text, args, 1);
    } else if (strncmp(line, "MODE_LOCK ", 10) == 0) {
        greeter_enter_lock_mode(g, line + 10);
    } else if (strcmp(line, "MODE_LOGIN") == 0) {
        /* Switch back to login mode (not typical, but handle it) */
        greeter_enter_login_mode(g);
    } else {
        fprintf(stderr, "isde-greeter: unknown IPC: %s\n", line);
    }
}

/* ---------- Config ---------- */

static void load_config(Greeter *g)
{
    g->clock_time_fmt = strdup("%H:%M");
    g->clock_date_fmt = strdup("%Y-%m-%d");
    g->allow_shutdown = 1;
    g->allow_reboot = 1;
    g->allow_suspend = 1;

    /* Read from system-wide DM config, path set by CMake */
    char errbuf[256];
    IsdeConfig *cfg = isde_config_load(DM_CONFIG_PATH,
                                       errbuf, sizeof(errbuf));
    if (!cfg) {
        return;
    }

    IsdeConfigTable *root = isde_config_root(cfg);
    const char *cs = isde_config_string(root, "color_scheme", NULL);
    if (cs) {
        g->color_scheme = strdup(cs);
    }
    g->scale = isde_config_double(root, "scale", 0.0);
    g->allow_shutdown = isde_config_bool(root, "allow_shutdown", 1);
    g->allow_reboot   = isde_config_bool(root, "allow_reboot", 1);
    g->allow_suspend  = isde_config_bool(root, "allow_suspend", 1);

    IsdeConfigTable *clock = isde_config_table(root, "clock");
    if (clock) {
        const char *tf = isde_config_string(clock, "time_format", NULL);
        if (tf) { free(g->clock_time_fmt); g->clock_time_fmt = strdup(tf); }
        const char *df = isde_config_string(clock, "date_format", NULL);
        if (df) { free(g->clock_date_fmt); g->clock_date_fmt = strdup(df); }
    }

    isde_config_free(cfg);
}

/* ---------- UI construction ---------- */

static void build_ui(Greeter *g)
{
    Arg args[20];
    Cardinal n;

    /* Fullscreen OverrideShell */
    n = 0;
    XtSetArg(args[n], XtNx, 0);                          n++;
    XtSetArg(args[n], XtNy, 0);                          n++;
    XtSetArg(args[n], XtNwidth, g->screen_w);             n++;
    XtSetArg(args[n], XtNheight, g->screen_h);            n++;
    XtSetArg(args[n], XtNoverrideRedirect, True);         n++;
    XtSetArg(args[n], XtNborderWidth, 0);                 n++;
    g->shell = XtCreatePopupShell("greeter", overrideShellWidgetClass,
                                  g->toplevel, args, n);

    /* Main form */
    n = 0;
    XtSetArg(args[n], XtNdefaultDistance, 0); n++;
    XtSetArg(args[n], XtNborderWidth, 0);     n++;
    g->form = XtCreateManagedWidget("greeterForm", formWidgetClass,
                                    g->shell, args, n);

    /* --- Clock area (centered at top) --- */
    greeter_clock_init(g);

    /* --- Login form (centered) --- */
    int form_x = (g->screen_w - LOGIN_FORM_W) / 2;
    int form_y = g->screen_h / 3;  /* roughly 1/3 down */

    /* Username label */
    n = 0;
    XtSetArg(args[n], XtNlabel, "Username");              n++;
    XtSetArg(args[n], XtNwidth, LABEL_W);                  n++;
    XtSetArg(args[n], XtNjustify, XtJustifyRight);         n++;
    XtSetArg(args[n], XtNborderWidth, 0);                  n++;
    XtSetArg(args[n], XtNhorizDistance, form_x);           n++;
    XtSetArg(args[n], XtNvertDistance, form_y);            n++;
    XtSetArg(args[n], XtNtop, XtChainTop);                 n++;
    XtSetArg(args[n], XtNbottom, XtChainTop);              n++;
    XtSetArg(args[n], XtNleft, XtChainLeft);               n++;
    XtSetArg(args[n], XtNright, XtChainLeft);              n++;
    g->user_label = XtCreateManagedWidget("userLabel", labelWidgetClass,
                                          g->form, args, n);

    /* Username text input */
    n = 0;
    XtSetArg(args[n], XtNwidth, INPUT_W);                  n++;
    XtSetArg(args[n], XtNeditType, IswtextEdit);           n++;
    XtSetArg(args[n], XtNborderWidth, 1);                  n++;
    XtSetArg(args[n], XtNfromHoriz, g->user_label);        n++;
    XtSetArg(args[n], XtNvertDistance, form_y);            n++;
    XtSetArg(args[n], XtNhorizDistance, ROW_GAP);          n++;
    XtSetArg(args[n], XtNtop, XtChainTop);                 n++;
    XtSetArg(args[n], XtNbottom, XtChainTop);              n++;
    XtSetArg(args[n], XtNleft, XtChainLeft);               n++;
    XtSetArg(args[n], XtNright, XtChainLeft);              n++;
    g->user_text = XtCreateManagedWidget("userText", asciiTextWidgetClass,
                                         g->form, args, n);

    /* Password label */
    n = 0;
    XtSetArg(args[n], XtNlabel, "Password");              n++;
    XtSetArg(args[n], XtNwidth, LABEL_W);                  n++;
    XtSetArg(args[n], XtNjustify, XtJustifyRight);         n++;
    XtSetArg(args[n], XtNborderWidth, 0);                  n++;
    XtSetArg(args[n], XtNhorizDistance, form_x);           n++;
    XtSetArg(args[n], XtNfromVert, g->user_label);         n++;
    XtSetArg(args[n], XtNvertDistance, ROW_GAP);           n++;
    XtSetArg(args[n], XtNtop, XtChainTop);                 n++;
    XtSetArg(args[n], XtNbottom, XtChainTop);              n++;
    XtSetArg(args[n], XtNleft, XtChainLeft);               n++;
    XtSetArg(args[n], XtNright, XtChainLeft);              n++;
    g->pass_label = XtCreateManagedWidget("passLabel", labelWidgetClass,
                                          g->form, args, n);

    /* Password text input (echo off) */
    n = 0;
    XtSetArg(args[n], XtNwidth, INPUT_W);                  n++;
    XtSetArg(args[n], XtNeditType, IswtextEdit);           n++;
    XtSetArg(args[n], XtNecho, False);                     n++;
    XtSetArg(args[n], XtNborderWidth, 1);                  n++;
    XtSetArg(args[n], XtNfromHoriz, g->pass_label);        n++;
    XtSetArg(args[n], XtNfromVert, g->user_text);          n++;
    XtSetArg(args[n], XtNhorizDistance, ROW_GAP);          n++;
    XtSetArg(args[n], XtNvertDistance, ROW_GAP);           n++;
    XtSetArg(args[n], XtNtop, XtChainTop);                 n++;
    XtSetArg(args[n], XtNbottom, XtChainTop);              n++;
    XtSetArg(args[n], XtNleft, XtChainLeft);               n++;
    XtSetArg(args[n], XtNright, XtChainLeft);              n++;
    g->pass_text = XtCreateManagedWidget("passText", asciiTextWidgetClass,
                                         g->form, args, n);

    /* Install Enter-to-login translation on password field */
    XtOverrideTranslations(g->pass_text,
                           XtParseTranslationTable(greeter_translations));

    /* Session label */
    n = 0;
    XtSetArg(args[n], XtNlabel, "Session");                n++;
    XtSetArg(args[n], XtNwidth, LABEL_W);                  n++;
    XtSetArg(args[n], XtNjustify, XtJustifyRight);         n++;
    XtSetArg(args[n], XtNborderWidth, 0);                  n++;
    XtSetArg(args[n], XtNhorizDistance, form_x);           n++;
    XtSetArg(args[n], XtNfromVert, g->pass_label);         n++;
    XtSetArg(args[n], XtNvertDistance, ROW_GAP);           n++;
    XtSetArg(args[n], XtNtop, XtChainTop);                 n++;
    XtSetArg(args[n], XtNbottom, XtChainTop);              n++;
    XtSetArg(args[n], XtNleft, XtChainLeft);               n++;
    XtSetArg(args[n], XtNright, XtChainLeft);              n++;
    g->session_label = XtCreateManagedWidget("sessionLabel", labelWidgetClass,
                                             g->form, args, n);

    /* Session dropdown */
    g->session_names = malloc((g->nsessions + 1) * sizeof(String));
    for (int i = 0; i < g->nsessions; i++) {
        g->session_names[i] = g->sessions[i].name;
    }
    g->session_names[g->nsessions] = NULL;

    n = 0;
    XtSetArg(args[n], XtNlist, g->session_names);          n++;
    XtSetArg(args[n], XtNnumberStrings, g->nsessions);    n++;
    XtSetArg(args[n], XtNdropdownMode, True);              n++;
    XtSetArg(args[n], XtNdefaultColumns, 1);               n++;
    XtSetArg(args[n], XtNforceColumns, True);              n++;
    XtSetArg(args[n], XtNwidth, INPUT_W);                  n++;
    XtSetArg(args[n], XtNborderWidth, 1);                  n++;
    XtSetArg(args[n], XtNfromHoriz, g->session_label);     n++;
    XtSetArg(args[n], XtNfromVert, g->pass_text);          n++;
    XtSetArg(args[n], XtNhorizDistance, ROW_GAP);          n++;
    XtSetArg(args[n], XtNvertDistance, ROW_GAP);           n++;
    XtSetArg(args[n], XtNtop, XtChainTop);                 n++;
    XtSetArg(args[n], XtNbottom, XtChainTop);              n++;
    XtSetArg(args[n], XtNleft, XtChainLeft);               n++;
    XtSetArg(args[n], XtNright, XtChainLeft);              n++;
    g->session_btn = XtCreateManagedWidget("sessionBtn", listWidgetClass,
                                           g->form, args, n);
    XtAddCallback(g->session_btn, XtNcallback, session_dropdown_cb, g);
    if (g->nsessions > 0) {
        IswListHighlight(g->session_btn, g->active_session);
    }

    /* Error label */
    n = 0;
    XtSetArg(args[n], XtNlabel, " ");                     n++;
    XtSetArg(args[n], XtNwidth, LOGIN_FORM_W);            n++;
    XtSetArg(args[n], XtNborderWidth, 0);                  n++;
    XtSetArg(args[n], XtNhorizDistance, form_x);           n++;
    XtSetArg(args[n], XtNfromVert, g->session_btn);        n++;
    XtSetArg(args[n], XtNvertDistance, SECTION_GAP);       n++;
    XtSetArg(args[n], XtNtop, XtChainTop);                 n++;
    XtSetArg(args[n], XtNbottom, XtChainTop);              n++;
    XtSetArg(args[n], XtNleft, XtChainLeft);               n++;
    XtSetArg(args[n], XtNright, XtChainLeft);              n++;
    g->error_label = XtCreateManagedWidget("errorLabel", labelWidgetClass,
                                           g->form, args, n);

    /* Login button */
    n = 0;
    XtSetArg(args[n], XtNlabel, "Log In");                 n++;
    XtSetArg(args[n], XtNwidth, BUTTON_W);                 n++;
    XtSetArg(args[n], XtNinternalWidth, BUTTON_PAD);       n++;
    XtSetArg(args[n], XtNinternalHeight, BUTTON_PAD);      n++;
    int login_x = form_x + LABEL_W + ROW_GAP;
    XtSetArg(args[n], XtNhorizDistance, login_x);          n++;
    XtSetArg(args[n], XtNfromVert, g->error_label);        n++;
    XtSetArg(args[n], XtNvertDistance, ROW_GAP);           n++;
    XtSetArg(args[n], XtNtop, XtChainTop);                 n++;
    XtSetArg(args[n], XtNbottom, XtChainTop);              n++;
    XtSetArg(args[n], XtNleft, XtChainLeft);               n++;
    XtSetArg(args[n], XtNright, XtChainLeft);              n++;
    g->login_btn = XtCreateManagedWidget("loginBtn", commandWidgetClass,
                                         g->form, args, n);
    XtAddCallback(g->login_btn, XtNcallback, login_cb, g);

    /* --- Power buttons (bottom-left) --- */
    int btn_y = g->screen_h - isde_scale(40) - BOTTOM_MARGIN;
    int btn_x = BUTTON_PAD;

    if (g->allow_shutdown) {
        n = 0;
        XtSetArg(args[n], XtNlabel, "Shut Down");         n++;
        XtSetArg(args[n], XtNwidth, BUTTON_W);             n++;
        XtSetArg(args[n], XtNinternalWidth, BUTTON_PAD);   n++;
        XtSetArg(args[n], XtNinternalHeight, BUTTON_PAD);  n++;
        XtSetArg(args[n], XtNhorizDistance, btn_x);        n++;
        XtSetArg(args[n], XtNvertDistance, btn_y);         n++;
        XtSetArg(args[n], XtNtop, XtChainTop);             n++;
        XtSetArg(args[n], XtNbottom, XtChainTop);          n++;
        XtSetArg(args[n], XtNleft, XtChainLeft);           n++;
        XtSetArg(args[n], XtNright, XtChainLeft);          n++;
        g->shutdown_btn = XtCreateManagedWidget("shutdownBtn",
                                                commandWidgetClass,
                                                g->form, args, n);
        XtAddCallback(g->shutdown_btn, XtNcallback, shutdown_cb, g);
        btn_x += BUTTON_W + BUTTON_PAD;
    }

    if (g->allow_reboot) {
        n = 0;
        XtSetArg(args[n], XtNlabel, "Reboot");            n++;
        XtSetArg(args[n], XtNwidth, BUTTON_W);             n++;
        XtSetArg(args[n], XtNinternalWidth, BUTTON_PAD);   n++;
        XtSetArg(args[n], XtNinternalHeight, BUTTON_PAD);  n++;
        XtSetArg(args[n], XtNhorizDistance, btn_x);        n++;
        XtSetArg(args[n], XtNvertDistance, btn_y);         n++;
        XtSetArg(args[n], XtNtop, XtChainTop);             n++;
        XtSetArg(args[n], XtNbottom, XtChainTop);          n++;
        XtSetArg(args[n], XtNleft, XtChainLeft);           n++;
        XtSetArg(args[n], XtNright, XtChainLeft);          n++;
        g->reboot_btn = XtCreateManagedWidget("rebootBtn",
                                              commandWidgetClass,
                                              g->form, args, n);
        XtAddCallback(g->reboot_btn, XtNcallback, reboot_cb, g);
        btn_x += BUTTON_W + BUTTON_PAD;
    }

    if (g->allow_suspend) {
        n = 0;
        XtSetArg(args[n], XtNlabel, "Suspend");            n++;
        XtSetArg(args[n], XtNwidth, BUTTON_W);             n++;
        XtSetArg(args[n], XtNinternalWidth, BUTTON_PAD);   n++;
        XtSetArg(args[n], XtNinternalHeight, BUTTON_PAD);  n++;
        XtSetArg(args[n], XtNhorizDistance, btn_x);        n++;
        XtSetArg(args[n], XtNvertDistance, btn_y);         n++;
        XtSetArg(args[n], XtNtop, XtChainTop);             n++;
        XtSetArg(args[n], XtNbottom, XtChainTop);          n++;
        XtSetArg(args[n], XtNleft, XtChainLeft);           n++;
        XtSetArg(args[n], XtNright, XtChainLeft);          n++;
        g->suspend_btn = XtCreateManagedWidget("suspendBtn",
                                               commandWidgetClass,
                                               g->form, args, n);
        XtAddCallback(g->suspend_btn, XtNcallback, suspend_cb, g);
    }
}

/* ---------- HiDPI detection ---------- */

/*
 * Query the primary (or first connected) output via xcb-randr and compute
 * a scale factor from its physical size and pixel resolution.
 * Sets ISW_SCALE_FACTOR so that isde_scale() picks it up.
 * If config_scale > 0, that value is used as an explicit override.
 */
static void detect_hidpi(xcb_connection_t *conn, xcb_screen_t *screen,
                         double config_scale)
{
    /* Explicit env var already set — honour it */
    if (getenv("ISW_SCALE_FACTOR")) {
        return;
    }

    /* Config override from isde-dm.toml */
    if (config_scale > 0) {
        char buf[32];
        snprintf(buf, sizeof(buf), "%.2f", config_scale);
        setenv("ISW_SCALE_FACTOR", buf, 1);
        return;
    }

    /* Auto-detect from randr output physical size */
    xcb_randr_get_screen_resources_current_reply_t *res =
        xcb_randr_get_screen_resources_current_reply(conn,
            xcb_randr_get_screen_resources_current(conn, screen->root), NULL);
    if (!res) {
        return;
    }

    /* Find the primary output, or fall back to the first connected output */
    xcb_randr_get_output_primary_reply_t *pri =
        xcb_randr_get_output_primary_reply(conn,
            xcb_randr_get_output_primary(conn, screen->root), NULL);
    xcb_randr_output_t primary_id = pri ? pri->output : XCB_NONE;
    free(pri);

    xcb_randr_output_t *outs =
        xcb_randr_get_screen_resources_current_outputs(res);
    int nouts =
        xcb_randr_get_screen_resources_current_outputs_length(res);

    unsigned int best_px_w = 0;
    unsigned int best_mm_w = 0;
    int found = 0;

    for (int i = 0; i < nouts; i++) {
        xcb_randr_get_output_info_reply_t *oinfo =
            xcb_randr_get_output_info_reply(conn,
                xcb_randr_get_output_info(conn, outs[i],
                                          XCB_CURRENT_TIME), NULL);
        if (!oinfo) { continue; }

        if (oinfo->connection != XCB_RANDR_CONNECTION_CONNECTED ||
            oinfo->crtc == XCB_NONE) {
            free(oinfo);
            continue;
        }

        xcb_randr_get_crtc_info_reply_t *cinfo =
            xcb_randr_get_crtc_info_reply(conn,
                xcb_randr_get_crtc_info(conn, oinfo->crtc,
                                        XCB_CURRENT_TIME), NULL);
        if (!cinfo) {
            free(oinfo);
            continue;
        }

        /* Prefer the primary output; otherwise take the first connected */
        if (outs[i] == primary_id || !found) {
            best_px_w = cinfo->width;
            best_mm_w = oinfo->mm_width;
            found = 1;
        }

        free(cinfo);
        free(oinfo);

        /* If we just matched the primary, stop looking */
        if (outs[i] == primary_id) {
            break;
        }
    }

    free(res);

    if (!found || best_mm_w == 0) {
        return;  /* no usable physical size — stay at 1x */
    }

    double dpi = (double)best_px_w / ((double)best_mm_w / 25.4);

    /* Map DPI to a discrete scale factor:
     *   < 144  → 1x
     *   < 192  → 1.5x
     *   < 240  → 2x
     *   >= 240 → 2.5x  */
    double scale;
    if (dpi < 144) {
        scale = 1.0;
    } else if (dpi < 192) {
        scale = 1.5;
    } else if (dpi < 240) {
        scale = 2.0;
    } else {
        scale = 2.5;
    }

    if (scale != 1.0) {
        char buf[32];
        snprintf(buf, sizeof(buf), "%.2f", scale);
        setenv("ISW_SCALE_FACTOR", buf, 1);
    }
}

/* ---------- Public API ---------- */

int greeter_init(Greeter *g, int *argc, char **argv)
{
    memset(g, 0, sizeof(*g));
    g->ipc_fd = -1;
    g->active_session = 0;

    load_config(g);

    /* Load available sessions */
    greeter_sessions_load(g);

    /* Apply colour scheme from DM config if set */
    if (g->color_scheme) {
        IsdeColorScheme *scheme = isde_scheme_load(g->color_scheme);
        if (scheme) {
            isde_theme_set_scheme(scheme);
        }
    }

    /* Initialize Xt with theme resources */
    char **fallbacks = isde_theme_build_resources();
    g->toplevel = XtAppInitialize(&g->app, "ISDE-Greeter",
                                  NULL, 0, argc, argv,
                                  fallbacks, NULL, 0);

    /* Register custom actions */
    XtAppAddActions(g->app, greeter_actions,
                    sizeof(greeter_actions) / sizeof(greeter_actions[0]));

    /* Get screen size */
    xcb_connection_t *conn = XtDisplay(g->toplevel);
    xcb_screen_t *screen = XtScreen(g->toplevel);
    g->screen_w = screen->width_in_pixels;
    g->screen_h = screen->height_in_pixels;

    /* Detect HiDPI and set ISW_SCALE_FACTOR before layout */
    detect_hidpi(conn, screen, g->scale);

    /* Build UI */
    build_ui(g);

    /* Realize and show */
    XtRealizeWidget(g->shell);
    XtPopup(g->shell, XtGrabNone);

    /* Connect to daemon IPC */
    if (greeter_ipc_init(g) != 0) {
        fprintf(stderr, "isde-greeter: cannot connect to daemon\n");
        return -1;
    }

    /* Register IPC fd with Xt event loop */
    g->ipc_input = XtAppAddInput(g->app, g->ipc_fd,
                                 (XtPointer)XtInputReadMask,
                                 ipc_input_cb, g);

    g->running = 1;
    return 0;
}

void greeter_run(Greeter *g)
{
    while (g->running) {
        XtAppProcessEvent(g->app, XtIMAll);

        if (XtAppGetExitFlag(g->app)) {
            break;
        }
    }
}

void greeter_cleanup(Greeter *g)
{
    greeter_clock_cleanup(g);

    if (g->ipc_input) {
        XtRemoveInput(g->ipc_input);
    }
    greeter_ipc_cleanup(g);
    greeter_sessions_cleanup(g);

    free(g->color_scheme);
    free(g->clock_time_fmt);
    free(g->clock_date_fmt);
    free(g->lock_user);

    if (g->toplevel) {
        XtDestroyWidget(g->toplevel);
    }
}

void greeter_set_error(Greeter *g, const char *msg)
{
    Arg args[20];
    XtSetArg(args[0], XtNlabel, msg);
    XtSetValues(g->error_label, args, 1);
}

void greeter_clear_error(Greeter *g)
{
    Arg args[20];
    XtSetArg(args[0], XtNlabel, " ");
    XtSetValues(g->error_label, args, 1);
}

/* ---------- Lock mode ---------- */

void greeter_enter_lock_mode(Greeter *g, const char *username)
{
    fprintf(stderr, "isde-greeter: entering lock mode for '%s'\n", username);
    g->mode_lock = 1;
    free(g->lock_user);
    g->lock_user = strdup(username);

    Arg args[20];

    /* Pre-fill username and make it read-only */
    XtSetArg(args[0], XtNstring, username);
    XtSetValues(g->user_text, args, 1);
    XtSetSensitive(g->user_text, False);

    /* Clear password and focus it */
    XtSetArg(args[0], XtNstring, "");
    XtSetValues(g->pass_text, args, 1);
    XtSetKeyboardFocus(g->form, g->pass_text);

    /* Hide session selector in lock mode */
    XtUnmanageChild(g->session_label);
    XtUnmanageChild(g->session_btn);

    /* Hide power buttons in lock mode */
    if (g->shutdown_btn) { XtUnmanageChild(g->shutdown_btn); }
    if (g->reboot_btn)   { XtUnmanageChild(g->reboot_btn); }
    if (g->suspend_btn)  { XtUnmanageChild(g->suspend_btn); }

    greeter_clear_error(g);

    /* Grab keyboard and pointer for lock screen security.
     * Grab keyboard on the password field so key events are delivered
     * directly to it — grabbing on the shell would intercept events
     * before Xt can dispatch them to the text widget. */
    xcb_connection_t *conn = XtDisplay(g->toplevel);
    xcb_window_t win = XtWindow(g->shell);
    xcb_window_t pass_win = XtWindow(g->pass_text);

    xcb_grab_keyboard(conn, 1, pass_win, XCB_CURRENT_TIME,
                      XCB_GRAB_MODE_ASYNC, XCB_GRAB_MODE_ASYNC);
    xcb_grab_pointer(conn, 1, win,
                     XCB_EVENT_MASK_BUTTON_PRESS |
                     XCB_EVENT_MASK_BUTTON_RELEASE |
                     XCB_EVENT_MASK_POINTER_MOTION,
                     XCB_GRAB_MODE_ASYNC, XCB_GRAB_MODE_ASYNC,
                     win, XCB_NONE, XCB_CURRENT_TIME);
    xcb_flush(conn);
}

void greeter_enter_login_mode(Greeter *g)
{
    g->mode_lock = 0;
    free(g->lock_user);
    g->lock_user = NULL;

    Arg args[20];

    /* Clear and re-enable username field */
    XtSetArg(args[0], XtNstring, "");
    XtSetValues(g->user_text, args, 1);
    XtSetSensitive(g->user_text, True);

    /* Clear password */
    XtSetArg(args[0], XtNstring, "");
    XtSetValues(g->pass_text, args, 1);

    /* Show session selector */
    XtManageChild(g->session_label);
    XtManageChild(g->session_btn);

    /* Show power buttons */
    if (g->shutdown_btn) { XtManageChild(g->shutdown_btn); }
    if (g->reboot_btn)   { XtManageChild(g->reboot_btn); }
    if (g->suspend_btn)  { XtManageChild(g->suspend_btn); }

    greeter_clear_error(g);

    /* Release grabs */
    xcb_connection_t *conn = XtDisplay(g->toplevel);
    xcb_ungrab_keyboard(conn, XCB_CURRENT_TIME);
    xcb_ungrab_pointer(conn, XCB_CURRENT_TIME);
    xcb_flush(conn);
}
