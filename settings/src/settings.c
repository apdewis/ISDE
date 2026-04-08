#define _POSIX_C_SOURCE 200809L
/*
 * settings.c — isde-settings UI shell, plugin loading, panel switching
 *
 * Layout: left pane = scrollable List of panel names
 *         right pane = active panel content
 */
#include "settings.h"

#include <stdio.h>
#include "isde/isde-ewmh.h"
#include <stdlib.h>
#include <string.h>
#include <dlfcn.h>
#include <dirent.h>

#include <ISW/List.h>
#include <ISW/Viewport.h>
#include <ISW/Dialog.h>

#define PANEL_LIST_WIDTH isde_scale(140)

/* ---------- panel management ---------- */

static void register_panel(Settings *s, const IsdeSettingsPanel *panel,
                           void *dl_handle)
{
    if (s->npanels >= MAX_PANELS) {
        return;
    }
    int idx = s->npanels;
    s->panels[idx] = panel;
    s->plugin_handles[idx] = dl_handle;
    s->panel_widgets[idx] = NULL;
    s->npanels++;
}

static void load_plugins_from_dir(Settings *s, const char *dir)
{
    DIR *d = opendir(dir);
    if (!d) {
        return;
    }

    struct dirent *de;
    while ((de = readdir(d))) {
        size_t nlen = strlen(de->d_name);
        if (nlen < 4 || strcmp(de->d_name + nlen - 3, ".so") != 0) {
            continue;
        }

        char path[512];
        snprintf(path, sizeof(path), "%s/%s", dir, de->d_name);

        void *handle = dlopen(path, RTLD_LAZY);
        if (!handle) {
            continue;
        }

        IsdeSettingsPanelFunc fn = (IsdeSettingsPanelFunc)
            dlsym(handle, ISDE_SETTINGS_PANEL_SYMBOL);
        if (!fn) { dlclose(handle); continue; }

        const IsdeSettingsPanel *panel = fn();
        if (panel) {
            register_panel(s, panel, handle);
        } else {
            dlclose(handle);
        }
    }
    closedir(d);
}

static void load_plugins(Settings *s)
{
    char path[512];
    snprintf(path, sizeof(path), "%s/isde/settings-plugins",
             isde_xdg_data_home());
    load_plugins_from_dir(s, path);

    const char *dirs = isde_xdg_data_dirs();
    const char *p = dirs;
    while (p && *p) {
        const char *colon = strchr(p, ':');
        size_t dlen = colon ? (size_t)(colon - p) : strlen(p);
        if (dlen > 0) {
            snprintf(path, sizeof(path), "%.*s/isde/settings-plugins",
                     (int)dlen, p);
            load_plugins_from_dir(s, path);
        }
        p = colon ? colon + 1 : NULL;
    }
}

/* ---------- unsaved changes warning ---------- */

static int check_unsaved(Settings *s)
{
    int idx = s->active_panel;
    if (idx < 0 || idx >= s->npanels) {
        return 0;
    }
    if (!s->panels[idx]->has_changes) {
        return 0;
    }
    return s->panels[idx]->has_changes();
}

/* ---------- common save/revert callbacks ---------- */

static void common_save_cb(Widget w, XtPointer cd, XtPointer call)
{
    (void)w; (void)call;
    Settings *s = (Settings *)cd;
    if (s->active_panel >= 0 && s->panels[s->active_panel]->apply) {
        s->panels[s->active_panel]->apply();
    }
}

static void common_revert_cb(Widget w, XtPointer cd, XtPointer call)
{
    (void)w; (void)call;
    Settings *s = (Settings *)cd;
    if (s->active_panel >= 0 && s->panels[s->active_panel]->revert) {
        s->panels[s->active_panel]->revert();
    }
}

/* ---------- panel switching ---------- */

void settings_switch_panel(Settings *s, int index)
{
    if (index < 0 || index >= s->npanels) {
        return;
    }
    if (index == s->active_panel && s->panel_widgets[index]) {
        return;
    }

    /* Check for unsaved changes in current panel */
    if (check_unsaved(s)) {
        fprintf(stderr, "isde-settings: warning: unsaved changes discarded\n");
    }

    /* Unmanage current panel */
    if (s->active_panel >= 0 && s->panel_widgets[s->active_panel]) {
        XtUnmanageChild(s->panel_widgets[s->active_panel]);
    }

    s->active_panel = index;

    /* Create panel widget if needed */
    if (!s->panel_widgets[index]) {
        s->panel_widgets[index] =
            s->panels[index]->create(s->content_area, s->app);

        /* Size the panel to fill the content area */
        Dimension cw, ch;
        Arg qa[20];
        XtSetArg(qa[0], XtNwidth, &cw);
        XtSetArg(qa[1], XtNheight, &ch);
        XtGetValues(s->content_area, qa, 2);
        Arg sa[20];
        Cardinal sn = 0;
        if (cw > 0) { XtSetArg(sa[sn], XtNwidth, cw);  sn++; }
        if (ch > 0) { XtSetArg(sa[sn], XtNheight, ch); sn++; }
        if (sn > 0) {
            XtSetValues(s->panel_widgets[index], sa, sn);
        }
    }

    XtManageChild(s->panel_widgets[index]);
}

/* ---------- list selection callback ---------- */

static void panel_list_cb(Widget w, XtPointer cd, XtPointer call)
{
    (void)w;
    Settings *s = (Settings *)cd;
    IswListReturnStruct *ret = (IswListReturnStruct *)call;
    if (ret->list_index >= 0 && ret->list_index < s->npanels) {
        settings_switch_panel(s, ret->list_index);
    }
}

/* ---------- D-Bus ---------- */

static void settings_dbus_input_cb(XtPointer client_data, int *fd,
                                    XtInputId *id)
{
    (void)fd; (void)id;
    isde_dbus_dispatch((IsdeDBus *)client_data);
}

/* ---------- close handling ---------- */

static void settings_destroy_cb(Widget w, XtPointer cd, XtPointer call)
{
    (void)w; (void)call;
    Settings *s = (Settings *)cd;
    s->running = 0;
    XtAppSetExitFlag(s->app);
}

/* ---------- init ---------- */

int settings_init(Settings *s, int *argc, char **argv)
{
    memset(s, 0, sizeof(*s));
    s->active_panel = -1;

    char **fallbacks = isde_theme_build_resources();
    s->toplevel = XtAppInitialize(&s->app, "ISDE-Settings",
                                  NULL, 0, argc, argv,
                                  fallbacks, NULL, 0);

    int init_w = isde_scale(600);
    int init_h = isde_scale(450);
    isde_clamp_to_workarea(XtDisplay(s->toplevel), 0, &init_w, &init_h);

    Arg args[20];
    Cardinal n = 0;
    XtSetArg(args[n], XtNwidth, init_w);              n++;
    XtSetArg(args[n], XtNheight, init_h);             n++;
    XtSetArg(args[n], XtNminWidth, isde_scale(400));  n++;
    XtSetArg(args[n], XtNminHeight, isde_scale(300)); n++;
    XtSetValues(s->toplevel, args, n);

    XtAddCallback(s->toplevel, XtNdestroyCallback,
                  settings_destroy_cb, s);

    /* Main layout form — direct child of toplevel */
    n = 0;
    XtSetArg(args[n], XtNdefaultDistance, 0); n++;
    XtSetArg(args[n], XtNborderWidth, 0);    n++;
    Widget form = XtCreateManagedWidget("layout", formWidgetClass,
                                        s->toplevel, args, n);

    /* Register core panels first (so names are available for the list) */
    register_panel(s, &panel_input, NULL);
    register_panel(s, &panel_keyboard, NULL);
    register_panel(s, &panel_appearance, NULL);
    register_panel(s, &panel_fonts, NULL);
    register_panel(s, &panel_display, NULL);
    register_panel(s, &panel_desktops, NULL);
    register_panel(s, &panel_dm, NULL);
    load_plugins(s);

    /* Left pane: panel name list */
    static String *panel_names = NULL;
    free(panel_names);
    panel_names = malloc((s->npanels + 1) * sizeof(String));
    for (int i = 0; i < s->npanels; i++) {
        panel_names[i] = (String)s->panels[i]->name;
    }
    panel_names[s->npanels] = NULL;

    n = 0;
    XtSetArg(args[n], XtNlist, panel_names);        n++;
    XtSetArg(args[n], XtNnumberStrings, s->npanels); n++;
    XtSetArg(args[n], XtNdefaultColumns, 1);         n++;
    XtSetArg(args[n], XtNforceColumns, True);        n++;
    XtSetArg(args[n], XtNverticalList, True);        n++;
    XtSetArg(args[n], XtNwidth, PANEL_LIST_WIDTH);   n++;
    XtSetArg(args[n], XtNheight, isde_scale(440));    n++;
    XtSetArg(args[n], XtNborderWidth, 0);            n++;
    XtSetArg(args[n], XtNtop, XtChainTop);           n++;
    XtSetArg(args[n], XtNbottom, XtChainBottom);     n++;
    XtSetArg(args[n], XtNleft, XtChainLeft);         n++;
    XtSetArg(args[n], XtNright, XtChainLeft);        n++;
    s->panel_bar = XtCreateManagedWidget("panelList", listWidgetClass,
                                         form, args, n);
    XtAddCallback(s->panel_bar, XtNcallback, panel_list_cb, s);

    /* Right pane: form with scrollable content + fixed buttons at bottom */
    int right_w = isde_scale(600) - PANEL_LIST_WIDTH - isde_scale(4);
    int right_h = isde_scale(440);
    int btn_h = isde_scale(32);
    int btn_pad = isde_scale(8);
    int sb_w = isde_scale(14);  /* scrollbar thickness */

    n = 0;
    XtSetArg(args[n], XtNfromHoriz, s->panel_bar);  n++;
    XtSetArg(args[n], XtNborderWidth, 0);            n++;
    XtSetArg(args[n], XtNdefaultDistance, 0);        n++;
    XtSetArg(args[n], XtNwidth, right_w);            n++;
    XtSetArg(args[n], XtNheight, right_h);           n++;
    XtSetArg(args[n], XtNtop, XtChainTop);           n++;
    XtSetArg(args[n], XtNbottom, XtChainBottom);     n++;
    XtSetArg(args[n], XtNleft, XtChainLeft);         n++;
    XtSetArg(args[n], XtNright, XtChainRight);       n++;
    s->content_form = XtCreateManagedWidget("contentForm", formWidgetClass,
                                            form, args, n);

    /* Scrollable viewport for panel content */
    n = 0;
    XtSetArg(args[n], XtNallowVert, True);          n++;
    XtSetArg(args[n], XtNuseRight, True);            n++;
    XtSetArg(args[n], XtNborderWidth, 0);            n++;
    XtSetArg(args[n], XtNwidth, right_w);            n++;
    XtSetArg(args[n], XtNheight, right_h - btn_h - btn_pad); n++;
    XtSetArg(args[n], XtNtop, XtChainTop);           n++;
    XtSetArg(args[n], XtNbottom, XtChainBottom);     n++;
    XtSetArg(args[n], XtNleft, XtChainLeft);         n++;
    XtSetArg(args[n], XtNright, XtChainRight);       n++;
    s->content_vp = XtCreateManagedWidget("contentScroll", viewportWidgetClass,
                                          s->content_form, args, n);

    /* Panel content container inside viewport */
    n = 0;
    XtSetArg(args[n], XtNdefaultDistance, isde_scale(8)); n++;
    XtSetArg(args[n], XtNborderWidth, 0);            n++;
    XtSetArg(args[n], XtNwidth, right_w - isde_scale(20)); n++;
    XtSetArg(args[n], XtNheight, right_h - btn_h);       n++;
    s->content_area = XtCreateManagedWidget("content", formWidgetClass,
                                            s->content_vp, args, n);

    /* Save / Revert buttons — fixed at bottom right. */
    int btn_w = isde_scale(80);

    n = 0;
    XtSetArg(args[n], XtNfromVert, s->content_vp);          n++;
    XtSetArg(args[n], XtNlabel, "Save");                     n++;
    XtSetArg(args[n], XtNborderWidth, 0);                    n++;
    XtSetArg(args[n], XtNwidth, btn_w);                      n++;
    XtSetArg(args[n], XtNheight, btn_h - btn_pad);           n++;
    XtSetArg(args[n], XtNinternalWidth, btn_pad);            n++;
    XtSetArg(args[n], XtNinternalHeight, btn_pad);           n++;
    XtSetArg(args[n], XtNvertDistance, btn_pad);             n++;
    XtSetArg(args[n], XtNhorizDistance, right_w - btn_w * 2 - btn_pad * 2 - sb_w); n++;
    XtSetArg(args[n], XtNright, XtChainRight);               n++;
    XtSetArg(args[n], XtNleft, XtChainRight);                n++;
    XtSetArg(args[n], XtNbottom, XtChainBottom);             n++;
    XtSetArg(args[n], XtNtop, XtChainBottom);                n++;
    s->save_btn = XtCreateManagedWidget("saveBtn", commandWidgetClass,
                                        s->content_form, args, n);
    XtAddCallback(s->save_btn, XtNcallback, common_save_cb, s);

    n = 0;
    XtSetArg(args[n], XtNfromVert, s->content_vp);          n++;
    XtSetArg(args[n], XtNfromHoriz, s->save_btn);           n++;
    XtSetArg(args[n], XtNhorizDistance, btn_pad);            n++;
    XtSetArg(args[n], XtNvertDistance, btn_pad);             n++;
    XtSetArg(args[n], XtNlabel, "Revert");                   n++;
    XtSetArg(args[n], XtNborderWidth, 0);                    n++;
    XtSetArg(args[n], XtNwidth, btn_w);                      n++;
    XtSetArg(args[n], XtNheight, btn_h - btn_pad);           n++;
    XtSetArg(args[n], XtNinternalWidth, btn_pad);            n++;
    XtSetArg(args[n], XtNinternalHeight, btn_pad);           n++;
    XtSetArg(args[n], XtNright, XtChainRight);               n++;
    XtSetArg(args[n], XtNleft, XtChainRight);                n++;
    XtSetArg(args[n], XtNbottom, XtChainBottom);             n++;
    XtSetArg(args[n], XtNtop, XtChainBottom);                n++;
    s->revert_btn = XtCreateManagedWidget("revertBtn", commandWidgetClass,
                                          s->content_form, args, n);
    XtAddCallback(s->revert_btn, XtNcallback, common_revert_cb, s);

    /* D-Bus */
    s->dbus = isde_dbus_init();
    if (s->dbus) {
        int fd = isde_dbus_get_fd(s->dbus);
        if (fd >= 0) {
            XtAppAddInput(s->app, fd, (XtPointer)XtInputReadMask,
                          settings_dbus_input_cb, s->dbus);
        }
        panel_appearance_set_dbus(s->dbus);
        panel_fonts_set_dbus(s->dbus);
        panel_display_set_dbus(s->dbus);
        panel_desktops_set_dbus(s->dbus);
    }

    XtRealizeWidget(s->toplevel);

    /* Show first panel by default */
    if (s->npanels > 0) {
        settings_switch_panel(s, 0);
    }

    s->running = 1;
    return 0;
}

void settings_run(Settings *s)
{
    while (s->running && !XtAppGetExitFlag(s->app)) {
        XtAppProcessEvent(s->app, XtIMAll);
    }
}

void settings_cleanup(Settings *s)
{
    for (int i = 0; i < s->npanels; i++) {
        if (s->panels[i]->destroy) {
            s->panels[i]->destroy();
        }
        if (s->plugin_handles[i]) {
            dlclose(s->plugin_handles[i]);
        }
    }
    isde_dbus_free(s->dbus);
    XtDestroyApplicationContext(s->app);
}
