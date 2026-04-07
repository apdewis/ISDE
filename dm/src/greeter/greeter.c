#define _POSIX_C_SOURCE 200809L
/*
 * greeter.c — isde-greeter UI construction and event handling
 */
#include "greeter.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

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

static void session_select_cb(Widget w, XtPointer cd, XtPointer call)
{
    (void)w; (void)call;
    Greeter *g = (Greeter *)cd;

    /* Find which SmeBSB was selected */
    for (int i = 0; i < g->nsessions; i++) {
        /* The client_data carries the index encoded as a pointer */
        /* We use the widget name to identify */
    }
    /* This is handled per-item in the menu creation */
}

static void session_item_cb(Widget w, XtPointer cd, XtPointer call)
{
    (void)w; (void)call;
    Greeter *g = (Greeter *)((void **)cd)[0];
    int idx = (int)(long)((void **)cd)[1];

    if (idx >= 0 && idx < g->nsessions) {
        g->active_session = idx;
        Arg args[20];
        XtSetArg(args[0], XtNlabel, g->sessions[idx].name);
        XtSetValues(g->session_btn, args, 1);
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
        g->running = 0;  /* Daemon will kill us and start the session */
    } else if (strncmp(line, "AUTH_FAIL ", 10) == 0) {
        greeter_set_error(g, line + 10);
        /* Clear password field */
        Arg args[20];
        XtSetArg(args[0], XtNstring, "");
        XtSetValues(g->pass_text, args, 1);
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

static void build_session_menu(Greeter *g)
{
    /* Create SimpleMenu popup */
    g->session_menu = XtCreatePopupShell("sessionMenu",
                                         simpleMenuWidgetClass,
                                         g->session_btn, NULL, 0);

    /* Static storage for callback data — we need it to persist */
    static void *cb_data[64][2];

    for (int i = 0; i < g->nsessions && i < 64; i++) {
        Arg args[20];
        Cardinal n = 0;
        XtSetArg(args[n], XtNlabel, g->sessions[i].name); n++;
        char name[32];
        snprintf(name, sizeof(name), "sess%d", i);

        Widget item = XtCreateManagedWidget(name, smeBSBObjectClass,
                                            g->session_menu, args, n);

        cb_data[i][0] = g;
        cb_data[i][1] = (void *)(long)i;
        XtAddCallback(item, XtNcallback, session_item_cb, cb_data[i]);
    }
}

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

    /* Session MenuButton */
    const char *sess_label = g->nsessions > 0
        ? g->sessions[g->active_session].name : "(none)";
    n = 0;
    XtSetArg(args[n], XtNlabel, sess_label);               n++;
    XtSetArg(args[n], XtNwidth, INPUT_W);                  n++;
    XtSetArg(args[n], XtNborderWidth, 1);                  n++;
    XtSetArg(args[n], XtNfromHoriz, g->session_label);     n++;
    XtSetArg(args[n], XtNfromVert, g->pass_text);          n++;
    XtSetArg(args[n], XtNhorizDistance, ROW_GAP);          n++;
    XtSetArg(args[n], XtNvertDistance, ROW_GAP);           n++;
    XtSetArg(args[n], XtNmenuName, "sessionMenu");         n++;
    XtSetArg(args[n], XtNtop, XtChainTop);                 n++;
    XtSetArg(args[n], XtNbottom, XtChainTop);              n++;
    XtSetArg(args[n], XtNleft, XtChainLeft);               n++;
    XtSetArg(args[n], XtNright, XtChainLeft);              n++;
    g->session_btn = XtCreateManagedWidget("sessionBtn", menuButtonWidgetClass,
                                           g->form, args, n);

    if (g->nsessions > 0) {
        build_session_menu(g);
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

/* ---------- Public API ---------- */

int greeter_init(Greeter *g, int *argc, char **argv)
{
    memset(g, 0, sizeof(*g));
    g->ipc_fd = -1;
    g->active_session = 0;

    load_config(g);

    /* Load available sessions */
    greeter_sessions_load(g);

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
    (void)conn;
    g->screen_w = screen->width_in_pixels;
    g->screen_h = screen->height_in_pixels;

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

    free(g->clock_time_fmt);
    free(g->clock_date_fmt);

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
