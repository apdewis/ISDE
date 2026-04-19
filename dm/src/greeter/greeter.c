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
#include <ISW/IswArgMacros.h>

/* ---------- Geometry ---------- */

#define LABEL_W          100
#define INPUT_W          300
#define LOGIN_FORM_W     (LABEL_W + ROW_GAP + INPUT_W)
#define BUTTON_W         80
#define BUTTON_PAD       8
#define ROW_GAP          8
#define SECTION_GAP      16
#define BOTTOM_MARGIN    32

/* ---------- Callbacks ---------- */

static void login_cb(Widget w, IswPointer cd, IswPointer call)
{
    (void)w; (void)call;
    Greeter *g = (Greeter *)cd;

    /* Get username and password from text widgets */
    Arg args[20];
    String username = NULL;
    String password = NULL;

    IswSetArg(args[0], IswNstring, &username);
    IswGetValues(g->user_text, args, 1);

    IswSetArg(args[0], IswNstring, &password);
    IswGetValues(g->pass_text, args, 1);

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

static void shutdown_cb(Widget w, IswPointer cd, IswPointer call)
{
    (void)w; (void)call;
    Greeter *g = (Greeter *)cd;
    greeter_ipc_send(g, "SHUTDOWN");
}

static void reboot_cb(Widget w, IswPointer cd, IswPointer call)
{
    (void)w; (void)call;
    Greeter *g = (Greeter *)cd;
    greeter_ipc_send(g, "REBOOT");
}

static void suspend_cb(Widget w, IswPointer cd, IswPointer call)
{
    (void)w; (void)call;
    Greeter *g = (Greeter *)cd;
    greeter_ipc_send(g, "SUSPEND");
}

static void session_dropdown_cb(Widget w, IswPointer cd, IswPointer call)
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
    while (shell && !IswIsShell(shell)) {
        shell = IswParent(shell);
    }
    if (!shell) {
        return;
    }

    /* Trigger login callback via the login button */
    IswCallCallbacks(IswNameToWidget(shell, "*loginBtn"), IswNcallback, NULL);
}

static IswActionsRec greeter_actions[] = {
    { "greeter-login", pass_action },
};

static char greeter_translations[] =
    "<Key>Return: greeter-login()\n";

/* ---------- IPC response handler ---------- */

static void handle_ipc_line(Greeter *g, const char *line);

static void ipc_input_cb(IswPointer client_data, int *fd, IswInputId *id)
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
        IswSetArg(args[0], IswNstring, "");
        IswSetValues(g->pass_text, args, 1);
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

    IsdeConfigTable *fonts = isde_config_table(root, "fonts");
    if (fonts) {
        const char *fam = isde_config_string(fonts, "family", NULL);
        if (fam) { g->font_family = strdup(fam); }
        g->font_size = (int)isde_config_int(fonts, "size", 10);
    }

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
    IswArgBuilder ab = IswArgBuilderInit();

    /* Fullscreen OverrideShell */
    IswArgX(&ab, 0);
    IswArgY(&ab, 0);
    IswArgWidth(&ab, g->logical_w);
    IswArgHeight(&ab, g->logical_h);
    IswArgOverrideRedirect(&ab, True);
    IswArgBorderWidth(&ab, 0);
    g->shell = IswCreatePopupShell("greeter", overrideShellWidgetClass,
                                  g->toplevel, ab.args, ab.count);

    /* Main form — uses logical pixels; ISW scales internally */
    IswArgBuilderReset(&ab);
    IswArgDefaultDistance(&ab, 0);
    IswArgBorderWidth(&ab, 0);
    IswArgWidth(&ab, g->logical_w);
    IswArgHeight(&ab, g->logical_h);
    g->form = IswCreateManagedWidget("greeterForm", formWidgetClass,
                                    g->shell, ab.args, ab.count);

    /* --- Clock area (centered at top) --- */
    greeter_clock_init(g);

    /* Measure font-derived row height and label width: create a temporary
     * label with the widest label text, read its natural size, destroy it. */
    IswArgBuilderReset(&ab);
    IswArgLabel(&ab, "Username");
    IswArgBorderWidth(&ab, 0);
    Widget probe = IswCreateWidget("probe", labelWidgetClass,
                                  g->form, ab.args, ab.count);
    Dimension natural_h = 0, natural_lw = 0;
    IswVaGetValues(probe, IswNheight, &natural_h,
                         IswNwidth, &natural_lw, NULL);
    IswDestroyWidget(probe);

    int input_h = natural_h;
    int label_w = natural_lw;
    int input_x = (g->logical_w - INPUT_W) / 2;
    int label_x = input_x - ROW_GAP - label_w;

    int form_total_h = 4 * input_h + 3 * ROW_GAP + SECTION_GAP;
    int form_y = g->logical_h * 5 / 8 - form_total_h / 2;
    int row1_y = form_y;
    int row2_y = row1_y + input_h + ROW_GAP;
    int row3_y = row2_y + input_h + ROW_GAP;
    int row4_y = row3_y + input_h + SECTION_GAP;

    /* Username label */
    IswArgBuilderReset(&ab);
    IswArgLabel(&ab, "Username");
    IswArgWidth(&ab, label_w);
    IswArgHeight(&ab, input_h);
    IswArgJustify(&ab, IswJustifyRight);
    IswArgBorderWidth(&ab, 0);
    IswArgHorizDistance(&ab, label_x);
    IswArgVertDistance(&ab, row1_y);
    IswArgTop(&ab, IswChainTop);
    IswArgBottom(&ab, IswChainTop);
    IswArgLeft(&ab, IswChainLeft);
    IswArgRight(&ab, IswChainLeft);
    g->user_label = IswCreateManagedWidget("userLabel", labelWidgetClass,
                                          g->form, ab.args, ab.count);

    /* Username text input */
    IswArgBuilderReset(&ab);
    IswArgWidth(&ab, INPUT_W);
    IswArgHeight(&ab, input_h);
    IswArgEditType(&ab, IswtextEdit);
    IswArgBorderWidth(&ab, 1);
    IswArgHorizDistance(&ab, input_x);
    IswArgVertDistance(&ab, row1_y);
    IswArgTop(&ab, IswChainTop);
    IswArgBottom(&ab, IswChainTop);
    IswArgLeft(&ab, IswChainLeft);
    IswArgRight(&ab, IswChainLeft);
    g->user_text = IswCreateManagedWidget("userText", asciiTextWidgetClass,
                                         g->form, ab.args, ab.count);

    /* Password label */
    IswArgBuilderReset(&ab);
    IswArgLabel(&ab, "Password");
    IswArgWidth(&ab, label_w);
    IswArgHeight(&ab, input_h);
    IswArgJustify(&ab, IswJustifyRight);
    IswArgBorderWidth(&ab, 0);
    IswArgHorizDistance(&ab, label_x);
    IswArgVertDistance(&ab, row2_y);
    IswArgTop(&ab, IswChainTop);
    IswArgBottom(&ab, IswChainTop);
    IswArgLeft(&ab, IswChainLeft);
    IswArgRight(&ab, IswChainLeft);
    g->pass_label = IswCreateManagedWidget("passLabel", labelWidgetClass,
                                          g->form, ab.args, ab.count);

    /* Password text input (echo off) */
    IswArgBuilderReset(&ab);
    IswArgWidth(&ab, INPUT_W);
    IswArgHeight(&ab, input_h);
    IswArgEditType(&ab, IswtextEdit);
    IswArgEcho(&ab, False);
    IswArgBorderWidth(&ab, 1);
    IswArgHorizDistance(&ab, input_x);
    IswArgVertDistance(&ab, row2_y);
    IswArgTop(&ab, IswChainTop);
    IswArgBottom(&ab, IswChainTop);
    IswArgLeft(&ab, IswChainLeft);
    IswArgRight(&ab, IswChainLeft);
    g->pass_text = IswCreateManagedWidget("passText", asciiTextWidgetClass,
                                         g->form, ab.args, ab.count);

    /* Install Enter-to-login translation on password field */
    IswOverrideTranslations(g->pass_text,
                           IswParseTranslationTable(greeter_translations));

    /* Session label */
    IswArgBuilderReset(&ab);
    IswArgLabel(&ab, "Session");
    IswArgWidth(&ab, label_w);
    IswArgHeight(&ab, input_h);
    IswArgJustify(&ab, IswJustifyRight);
    IswArgBorderWidth(&ab, 0);
    IswArgHorizDistance(&ab, label_x);
    IswArgVertDistance(&ab, row3_y);
    IswArgTop(&ab, IswChainTop);
    IswArgBottom(&ab, IswChainTop);
    IswArgLeft(&ab, IswChainLeft);
    IswArgRight(&ab, IswChainLeft);
    g->session_label = IswCreateManagedWidget("sessionLabel", labelWidgetClass,
                                             g->form, ab.args, ab.count);

    /* Session dropdown */
    g->session_names = malloc((g->nsessions + 1) * sizeof(String));
    for (int i = 0; i < g->nsessions; i++) {
        g->session_names[i] = g->sessions[i].name;
    }
    g->session_names[g->nsessions] = NULL;

    IswArgBuilderReset(&ab);
    IswArgList(&ab, g->session_names);
    IswArgNumberStrings(&ab, g->nsessions);
    IswArgDropdownMode(&ab, True);
    IswArgDefaultColumns(&ab, 1);
    IswArgForceColumns(&ab, True);
    IswArgWidth(&ab, INPUT_W);
    IswArgHeight(&ab, input_h);
    IswArgBorderWidth(&ab, 1);
    IswArgHorizDistance(&ab, input_x);
    IswArgVertDistance(&ab, row3_y);
    IswArgTop(&ab, IswChainTop);
    IswArgBottom(&ab, IswChainTop);
    IswArgLeft(&ab, IswChainLeft);
    IswArgRight(&ab, IswChainLeft);
    g->session_btn = IswCreateManagedWidget("sessionBtn", listWidgetClass,
                                           g->form, ab.args, ab.count);
    IswAddCallback(g->session_btn, IswNcallback, session_dropdown_cb, g);
    if (g->nsessions > 0) {
        IswListHighlight(g->session_btn, g->active_session);
    }

    /* Error label */
    IswArgBuilderReset(&ab);
    IswArgLabel(&ab, " ");
    IswArgWidth(&ab, label_w + ROW_GAP + INPUT_W);
    IswArgHeight(&ab, input_h);
    IswArgBorderWidth(&ab, 0);
    IswArgHorizDistance(&ab, label_x);
    IswArgVertDistance(&ab, row4_y);
    IswArgTop(&ab, IswChainTop);
    IswArgBottom(&ab, IswChainTop);
    IswArgLeft(&ab, IswChainLeft);
    IswArgRight(&ab, IswChainLeft);
    g->error_label = IswCreateManagedWidget("errorLabel", labelWidgetClass,
                                           g->form, ab.args, ab.count);

    /* Login button (inline right of password field) */
    char *login_icon = isde_icon_find("actions", "system-log-in");
    IswArgBuilderReset(&ab);
    IswArgLabel(&ab, "");
    if (login_icon) {
        IswArgImage(&ab, login_icon);
    }
    IswArgHeight(&ab, input_h);
    IswArgInternalWidth(&ab, BUTTON_PAD);
    IswArgInternalHeight(&ab, 0);
    IswArgHorizDistance(&ab, input_x + INPUT_W + ROW_GAP);
    IswArgVertDistance(&ab, row2_y);
    IswArgTop(&ab, IswChainTop);
    IswArgBottom(&ab, IswChainTop);
    IswArgLeft(&ab, IswChainLeft);
    IswArgRight(&ab, IswChainLeft);
    g->login_btn = IswCreateManagedWidget("loginBtn", commandWidgetClass,
                                         g->form, ab.args, ab.count);
    IswAddCallback(g->login_btn, IswNcallback, login_cb, g);

    /* --- Power buttons (horizontally centered at bottom) --- */
    int btn_h = 40;
    int btn_y = g->logical_h * 7 / 8 - btn_h / 2;
    int btn_count = g->allow_shutdown + g->allow_reboot + g->allow_suspend;
    int btn_total_w = btn_count * BUTTON_W + (btn_count - 1) * BUTTON_PAD;
    int btn_x = (g->logical_w - btn_total_w) / 2;

    char *shutdown_icon = isde_icon_find("actions", "system-shutdown");
    char *reboot_icon   = isde_icon_find("actions", "system-reboot");
    char *suspend_icon  = isde_icon_find("actions", "system-suspend");

    if (g->allow_shutdown) {
        IswArgBuilderReset(&ab);
        IswArgLabel(&ab, "");
        if (shutdown_icon) {
            IswArgImage(&ab, shutdown_icon);
        }
        IswArgWidth(&ab, BUTTON_W);
        IswArgInternalWidth(&ab, BUTTON_PAD);
        IswArgInternalHeight(&ab, BUTTON_PAD);
        IswArgHorizDistance(&ab, btn_x);
        IswArgVertDistance(&ab, btn_y);
        IswArgTop(&ab, IswChainTop);
        IswArgBottom(&ab, IswChainTop);
        IswArgLeft(&ab, IswChainLeft);
        IswArgRight(&ab, IswChainLeft);
        g->shutdown_btn = IswCreateManagedWidget("shutdownBtn",
                                                commandWidgetClass,
                                                g->form, ab.args, ab.count);
        IswAddCallback(g->shutdown_btn, IswNcallback, shutdown_cb, g);
        btn_x += BUTTON_W + BUTTON_PAD;
    }

    if (g->allow_reboot) {
        IswArgBuilderReset(&ab);
        IswArgLabel(&ab, "");
        if (reboot_icon) {
            IswArgImage(&ab, reboot_icon);
        }
        IswArgWidth(&ab, BUTTON_W);
        IswArgInternalWidth(&ab, BUTTON_PAD);
        IswArgInternalHeight(&ab, BUTTON_PAD);
        IswArgHorizDistance(&ab, btn_x);
        IswArgVertDistance(&ab, btn_y);
        IswArgTop(&ab, IswChainTop);
        IswArgBottom(&ab, IswChainTop);
        IswArgLeft(&ab, IswChainLeft);
        IswArgRight(&ab, IswChainLeft);
        g->reboot_btn = IswCreateManagedWidget("rebootBtn",
                                              commandWidgetClass,
                                              g->form, ab.args, ab.count);
        IswAddCallback(g->reboot_btn, IswNcallback, reboot_cb, g);
        btn_x += BUTTON_W + BUTTON_PAD;
    }

    if (g->allow_suspend) {
        IswArgBuilderReset(&ab);
        IswArgLabel(&ab, "");
        if (suspend_icon) {
            IswArgImage(&ab, suspend_icon);
        }
        IswArgWidth(&ab, BUTTON_W);
        IswArgInternalWidth(&ab, BUTTON_PAD);
        IswArgInternalHeight(&ab, BUTTON_PAD);
        IswArgHorizDistance(&ab, btn_x);
        IswArgVertDistance(&ab, btn_y);
        IswArgTop(&ab, IswChainTop);
        IswArgBottom(&ab, IswChainTop);
        IswArgLeft(&ab, IswChainLeft);
        IswArgRight(&ab, IswChainLeft);
        g->suspend_btn = IswCreateManagedWidget("suspendBtn",
                                               commandWidgetClass,
                                               g->form, ab.args, ab.count);
        IswAddCallback(g->suspend_btn, IswNcallback, suspend_cb, g);
    }

    free(shutdown_icon);
    free(reboot_icon);
    free(suspend_icon);
}

/* ---------- HiDPI detection ---------- */

/*
 * Query the primary (or first connected) output via xcb-randr and compute
 * a scale factor from its physical size and pixel resolution.
 * Sets ISW_SCALE_FACTOR so that ISW picks it up.
 * If config_scale > 0, that value is used as an explicit override.
 */
/*
 * Detect HiDPI and set ISW_SCALE_FACTOR *before* IswAppInitialize so the
 * toolkit picks up the correct scale factor during display initialisation.
 * Opens its own temporary xcb connection for the randr query.
 */
static void detect_hidpi(double config_scale)
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

    /* Open a temporary connection for the randr query */
    int screen_num;
    xcb_connection_t *conn = xcb_connect(NULL, &screen_num);
    if (!conn || xcb_connection_has_error(conn)) {
        if (conn) {
            xcb_disconnect(conn);
        }
        return;
    }

    /* Find the screen */
    const xcb_setup_t *setup = xcb_get_setup(conn);
    xcb_screen_iterator_t iter = xcb_setup_roots_iterator(setup);
    for (int i = 0; i < screen_num; i++) {
        xcb_screen_next(&iter);
    }
    xcb_screen_t *screen = iter.data;
    if (!screen) {
        xcb_disconnect(conn);
        return;
    }

    /* Auto-detect from randr output physical size */
    xcb_randr_get_screen_resources_current_reply_t *res =
        xcb_randr_get_screen_resources_current_reply(conn,
            xcb_randr_get_screen_resources_current(conn, screen->root), NULL);
    if (!res) {
        xcb_disconnect(conn);
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
    xcb_disconnect(conn);

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

    /* Apply colour scheme — default to light if not configured */
    {
        const char *cs = g->color_scheme ? g->color_scheme : "default-light";
        IsdeColorScheme *scheme = isde_scheme_load(cs);
        if (scheme) {
            isde_theme_set_scheme(scheme);
        }
    }

    /* Detect HiDPI and set ISW_SCALE_FACTOR before Xt init so the
       toolkit picks up the correct scale factor during display setup. */
    detect_hidpi(g->scale);

    /* Initialize Xt with theme resources */
    char **fallbacks = isde_theme_build_resources();

    /* Append font resources from DM config — the greeter runs in a
       minimal environment where isde.toml (user config) is unavailable,
       so isde_theme_build_resources() won't produce font entries. */
    if (g->font_family) {
        int count = 0;
        if (fallbacks) {
            while (fallbacks[count]) count++;
        }
        /* 5 entries + NULL */
        fallbacks = realloc(fallbacks, (count + 6) * sizeof(char *));
        char buf[128];
        snprintf(buf, sizeof(buf), "*font: %s-%d",
                 g->font_family, g->font_size);
        fallbacks[count++] = strdup(buf);
        snprintf(buf, sizeof(buf), "*Text.font: %s-%d",
                 g->font_family, g->font_size);
        fallbacks[count++] = strdup(buf);
        snprintf(buf, sizeof(buf), "*AsciiSink.font: %s-%d",
                 g->font_family, g->font_size);
        fallbacks[count++] = strdup(buf);
        snprintf(buf, sizeof(buf), "*Text*textSink.font: %s-%d",
                 g->font_family, g->font_size);
        fallbacks[count++] = strdup(buf);
        snprintf(buf, sizeof(buf), "*textSink.font: %s-%d",
                 g->font_family, g->font_size);
        fallbacks[count++] = strdup(buf);
        fallbacks[count] = NULL;
    }

    g->toplevel = IswAppInitialize(&g->app, "ISDE-Greeter",
                                  NULL, 0, argc, argv,
                                  fallbacks, NULL, 0);

    /* Register custom actions */
    IswAppAddActions(g->app, greeter_actions,
                    sizeof(greeter_actions) / sizeof(greeter_actions[0]));

    /* Get screen size */
    xcb_connection_t *conn = IswDisplay(g->toplevel);
    xcb_screen_t *screen = IswScreen(g->toplevel);
    g->screen_w = screen->width_in_pixels;
    g->screen_h = screen->height_in_pixels;

    /* Compute logical screen dimensions — ISW scales widget dimensions
       and Form constraints internally, so all layout math must use logical
       pixels.  The shell gets physical pixels (Shells are not scaled). */
    double sf = ISWScaleFactor(g->toplevel);
    if (sf < 1.0) {
        sf = 1.0;
    }
    g->logical_w = (int)(g->screen_w / sf + 0.5);
    g->logical_h = (int)(g->screen_h / sf + 0.5);

    /* Build UI */
    build_ui(g);

    /* Default focus to username field so the cursor is visible */
    IswSetKeyboardFocus(g->form, g->user_text);

    /* Realize and show */
    IswRealizeWidget(g->shell);
    IswPopup(g->shell, IswGrabNone);

    /* Override-redirect windows don't receive X focus automatically.
       Focus the text widget's window directly so key events reach it. */
    xcb_connection_t *xc = IswDisplay(g->shell);
    xcb_set_input_focus(xc, XCB_INPUT_FOCUS_PARENT,
                        IswWindow(g->user_text), XCB_CURRENT_TIME);
    xcb_flush(xc);

    /* Connect to daemon IPC */
    if (greeter_ipc_init(g) != 0) {
        fprintf(stderr, "isde-greeter: cannot connect to daemon\n");
        return -1;
    }

    /* Register IPC fd with Xt event loop */
    g->ipc_input = IswAppAddInput(g->app, g->ipc_fd,
                                 (IswPointer)IswInputReadMask,
                                 ipc_input_cb, g);

    g->running = 1;
    return 0;
}

void greeter_run(Greeter *g)
{
    while (g->running) {
        IswAppProcessEvent(g->app, IswIMAll);

        if (IswAppGetExitFlag(g->app)) {
            break;
        }
    }
}

void greeter_cleanup(Greeter *g)
{
    greeter_clock_cleanup(g);

    if (g->ipc_input) {
        IswRemoveInput(g->ipc_input);
    }
    greeter_ipc_cleanup(g);
    greeter_sessions_cleanup(g);

    free(g->color_scheme);
    free(g->font_family);
    free(g->clock_time_fmt);
    free(g->clock_date_fmt);
    free(g->lock_user);

    if (g->toplevel) {
        IswDestroyWidget(g->toplevel);
    }
}

void greeter_set_error(Greeter *g, const char *msg)
{
    Arg args[20];
    IswSetArg(args[0], IswNlabel, msg);
    IswSetValues(g->error_label, args, 1);
}

void greeter_clear_error(Greeter *g)
{
    Arg args[20];
    IswSetArg(args[0], IswNlabel, " ");
    IswSetValues(g->error_label, args, 1);
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
    IswSetArg(args[0], IswNstring, username);
    IswSetValues(g->user_text, args, 1);
    IswSetSensitive(g->user_text, False);

    /* Clear password and focus it */
    IswSetArg(args[0], IswNstring, "");
    IswSetValues(g->pass_text, args, 1);
    IswSetKeyboardFocus(g->form, g->pass_text);

    /* Hide session selector in lock mode */
    IswUnmanageChild(g->session_label);
    IswUnmanageChild(g->session_btn);

    /* Hide power buttons in lock mode */
    if (g->shutdown_btn) { IswUnmanageChild(g->shutdown_btn); }
    if (g->reboot_btn)   { IswUnmanageChild(g->reboot_btn); }
    if (g->suspend_btn)  { IswUnmanageChild(g->suspend_btn); }

    greeter_clear_error(g);

    /* Grab keyboard and pointer for lock screen security.
     * Grab keyboard on the password field so key events are delivered
     * directly to it — grabbing on the shell would intercept events
     * before Xt can dispatch them to the text widget. */
    xcb_connection_t *conn = IswDisplay(g->toplevel);
    xcb_window_t win = IswWindow(g->shell);
    xcb_window_t pass_win = IswWindow(g->pass_text);

    xcb_set_input_focus(conn, XCB_INPUT_FOCUS_PARENT,
                        pass_win, XCB_CURRENT_TIME);
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
    IswSetArg(args[0], IswNstring, "");
    IswSetValues(g->user_text, args, 1);
    IswSetSensitive(g->user_text, True);

    /* Clear password */
    IswSetArg(args[0], IswNstring, "");
    IswSetValues(g->pass_text, args, 1);

    /* Show session selector */
    IswManageChild(g->session_label);
    IswManageChild(g->session_btn);

    /* Show power buttons */
    if (g->shutdown_btn) { IswManageChild(g->shutdown_btn); }
    if (g->reboot_btn)   { IswManageChild(g->reboot_btn); }
    if (g->suspend_btn)  { IswManageChild(g->suspend_btn); }

    greeter_clear_error(g);

    /* Release grabs */
    xcb_connection_t *conn = IswDisplay(g->toplevel);
    xcb_ungrab_keyboard(conn, XCB_CURRENT_TIME);
    xcb_ungrab_pointer(conn, XCB_CURRENT_TIME);
    xcb_flush(conn);
}
