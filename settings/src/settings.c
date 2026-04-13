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

#define PANEL_LIST_WIDTH 140

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

static void common_save_cb(Widget w, IswPointer cd, IswPointer call)
{
    (void)w; (void)call;
    Settings *s = (Settings *)cd;
    if (s->active_panel >= 0 && s->panels[s->active_panel]->apply) {
        s->panels[s->active_panel]->apply();
    }
}

static void common_revert_cb(Widget w, IswPointer cd, IswPointer call)
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
        IswUnmanageChild(s->panel_widgets[s->active_panel]);
    }

    s->active_panel = index;

    /* Create panel widget if needed */
    if (!s->panel_widgets[index]) {
        /* Query the viewport clip width so panels fit without overflow.
         * The Viewport expands its child to its own width, but the
         * vertical scrollbar eats into the visible area. */
        Dimension vpw, vph;
        Arg qa[20];
        IswSetArg(qa[0], IswNwidth, &vpw);
        IswSetArg(qa[1], IswNheight, &vph);
        IswGetValues(s->content_vp, qa, 2);

        /* Scrollbar width — use 14 as the standard ISW scrollbar size */
        Dimension usable_w = (vpw > 20) ? vpw - 20 : vpw;

        /* Set content_area to usable width so panels query it correctly */
        Arg ca[20];
        Cardinal cn = 0;
        if (usable_w > 0) { IswSetArg(ca[cn], IswNwidth, usable_w); cn++; }
        if (cn > 0) {
            IswSetValues(s->content_area, ca, cn);
        }

        s->panel_widgets[index] =
            s->panels[index]->create(s->content_area, s->app);

        /* Size the panel to fill the usable area */
        Arg sa[20];
        Cardinal sn = 0;
        if (usable_w > 0) { IswSetArg(sa[sn], IswNwidth, usable_w); sn++; }
        if (vph > 0) { IswSetArg(sa[sn], IswNheight, vph); sn++; }
        if (sn > 0) {
            IswSetValues(s->panel_widgets[index], sa, sn);
        }
    }

    IswManageChild(s->panel_widgets[index]);

    /* Debug: dump widget dimensions */
    {
        Dimension tw, th, cfw, cfh, vpw, vph, caw, cah, pw, pph;
        Arg da[20];
        IswSetArg(da[0], IswNwidth, &tw); IswSetArg(da[1], IswNheight, &th);
        IswGetValues(s->toplevel, da, 2);
        IswSetArg(da[0], IswNwidth, &cfw); IswSetArg(da[1], IswNheight, &cfh);
        IswGetValues(s->content_form, da, 2);
        IswSetArg(da[0], IswNwidth, &vpw); IswSetArg(da[1], IswNheight, &vph);
        IswGetValues(s->content_vp, da, 2);
        IswSetArg(da[0], IswNwidth, &caw); IswSetArg(da[1], IswNheight, &cah);
        IswGetValues(s->content_area, da, 2);
        IswSetArg(da[0], IswNwidth, &pw); IswSetArg(da[1], IswNheight, &pph);
        IswGetValues(s->panel_widgets[index], da, 2);
        fprintf(stderr, "DEBUG: toplevel=%dx%d content_form=%dx%d viewport=%dx%d content_area=%dx%d panel=%dx%d\n",
                tw, th, cfw, cfh, vpw, vph, caw, cah, pw, pph);
    }
}

/* ---------- list selection callback ---------- */

static void panel_list_cb(Widget w, IswPointer cd, IswPointer call)
{
    (void)w;
    Settings *s = (Settings *)cd;
    IswListReturnStruct *ret = (IswListReturnStruct *)call;
    if (ret->list_index >= 0 && ret->list_index < s->npanels) {
        settings_switch_panel(s, ret->list_index);
    }
}

/* ---------- D-Bus ---------- */

static void settings_dbus_input_cb(IswPointer client_data, int *fd,
                                    IswInputId *id)
{
    (void)fd; (void)id;
    isde_dbus_dispatch((IsdeDBus *)client_data);
}

/* ---------- close handling ---------- */

static void settings_destroy_cb(Widget w, IswPointer cd, IswPointer call)
{
    (void)w; (void)call;
    Settings *s = (Settings *)cd;
    s->running = 0;
    IswAppSetExitFlag(s->app);
}

/* ---------- init ---------- */

int settings_init(Settings *s, int *argc, char **argv)
{
    memset(s, 0, sizeof(*s));
    s->active_panel = -1;

    char **fallbacks = isde_theme_build_resources();
    s->toplevel = IswAppInitialize(&s->app, "ISDE-Settings",
                                  NULL, 0, argc, argv,
                                  fallbacks, NULL, 0);

    int init_w = 600;
    int init_h = 450;
    isde_clamp_to_workarea(IswDisplay(s->toplevel), 0, &init_w, &init_h);

    Arg args[20];
    Cardinal n = 0;
    IswSetArg(args[n], IswNwidth, init_w);              n++;
    IswSetArg(args[n], IswNheight, init_h);             n++;
    IswSetArg(args[n], IswNminWidth, 400);  n++;
    IswSetArg(args[n], IswNminHeight, 300); n++;
    IswSetValues(s->toplevel, args, n);

    IswAddCallback(s->toplevel, IswNdestroyCallback,
                  settings_destroy_cb, s);

    /* Main layout form — direct child of toplevel */
    n = 0;
    IswSetArg(args[n], IswNdefaultDistance, 0); n++;
    IswSetArg(args[n], IswNborderWidth, 0);    n++;
    Widget form = IswCreateManagedWidget("layout", formWidgetClass,
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
    IswSetArg(args[n], IswNlist, panel_names);        n++;
    IswSetArg(args[n], IswNnumberStrings, s->npanels); n++;
    IswSetArg(args[n], IswNdefaultColumns, 1);         n++;
    IswSetArg(args[n], IswNforceColumns, True);        n++;
    IswSetArg(args[n], IswNverticalList, True);        n++;
    IswSetArg(args[n], IswNwidth, PANEL_LIST_WIDTH);   n++;
    IswSetArg(args[n], IswNheight, init_h - 10);       n++;
    IswSetArg(args[n], IswNborderWidth, 0);            n++;
    IswSetArg(args[n], IswNresizable, True);           n++;
    IswSetArg(args[n], IswNtop, IswChainTop);           n++;
    IswSetArg(args[n], IswNbottom, IswChainBottom);     n++;
    IswSetArg(args[n], IswNleft, IswChainLeft);         n++;
    IswSetArg(args[n], IswNright, IswChainLeft);        n++;
    s->panel_bar = IswCreateManagedWidget("panelList", listWidgetClass,
                                         form, args, n);
    IswAddCallback(s->panel_bar, IswNcallback, panel_list_cb, s);

    /* Right pane: form with scrollable content + fixed buttons at bottom */
    int right_w = init_w - PANEL_LIST_WIDTH - 4;
    int right_h = init_h - 10;
    int btn_h = 32;
    int btn_pad = 8;
    int sb_w = 14;  /* scrollbar thickness */

    n = 0;
    IswSetArg(args[n], IswNfromHoriz, s->panel_bar);  n++;
    IswSetArg(args[n], IswNborderWidth, 0);            n++;
    IswSetArg(args[n], IswNdefaultDistance, 0);        n++;
    IswSetArg(args[n], IswNwidth, right_w);            n++;
    IswSetArg(args[n], IswNheight, right_h);           n++;
    IswSetArg(args[n], IswNresizable, True);           n++;
    IswSetArg(args[n], IswNtop, IswChainTop);           n++;
    IswSetArg(args[n], IswNbottom, IswChainBottom);     n++;
    IswSetArg(args[n], IswNleft, IswChainLeft);         n++;
    IswSetArg(args[n], IswNright, IswChainRight);       n++;
    s->content_form = IswCreateManagedWidget("contentForm", formWidgetClass,
                                            form, args, n);

    /* Scrollable viewport for panel content */
    n = 0;
    IswSetArg(args[n], IswNallowVert, True);          n++;
    IswSetArg(args[n], IswNuseRight, True);            n++;
    IswSetArg(args[n], IswNborderWidth, 0);            n++;
    IswSetArg(args[n], IswNwidth, right_w);            n++;
    IswSetArg(args[n], IswNheight, right_h - btn_h - btn_pad); n++;
    IswSetArg(args[n], IswNresizable, True);           n++;
    IswSetArg(args[n], IswNtop, IswChainTop);           n++;
    IswSetArg(args[n], IswNbottom, IswChainBottom);     n++;
    IswSetArg(args[n], IswNleft, IswChainLeft);         n++;
    IswSetArg(args[n], IswNright, IswChainRight);       n++;
    s->content_vp = IswCreateManagedWidget("contentScroll", viewportWidgetClass,
                                          s->content_form, args, n);

    /* Panel content container inside viewport */
    n = 0;
    IswSetArg(args[n], IswNdefaultDistance, 0); n++;
    IswSetArg(args[n], IswNborderWidth, 0);            n++;
    s->content_area = IswCreateManagedWidget("content", formWidgetClass,
                                            s->content_vp, args, n);

    /* Save / Revert buttons — fixed at bottom right. */
    int btn_w = 80;

    n = 0;
    IswSetArg(args[n], IswNfromVert, s->content_vp);          n++;
    IswSetArg(args[n], IswNlabel, "Save");                     n++;
    IswSetArg(args[n], IswNborderWidth, 0);                    n++;
    IswSetArg(args[n], IswNwidth, btn_w);                      n++;
    IswSetArg(args[n], IswNheight, btn_h - btn_pad);           n++;
    IswSetArg(args[n], IswNinternalWidth, btn_pad);            n++;
    IswSetArg(args[n], IswNinternalHeight, btn_pad);           n++;
    IswSetArg(args[n], IswNvertDistance, btn_pad);             n++;
    IswSetArg(args[n], IswNhorizDistance, right_w - btn_w * 2 - btn_pad * 2 - sb_w); n++;
    IswSetArg(args[n], IswNresizable, True);                   n++;
    IswSetArg(args[n], IswNright, IswChainRight);               n++;
    IswSetArg(args[n], IswNleft, IswChainRight);                n++;
    IswSetArg(args[n], IswNbottom, IswChainBottom);             n++;
    IswSetArg(args[n], IswNtop, IswChainBottom);                n++;
    s->save_btn = IswCreateManagedWidget("saveBtn", commandWidgetClass,
                                        s->content_form, args, n);
    IswAddCallback(s->save_btn, IswNcallback, common_save_cb, s);

    n = 0;
    IswSetArg(args[n], IswNfromVert, s->content_vp);          n++;
    IswSetArg(args[n], IswNfromHoriz, s->save_btn);           n++;
    IswSetArg(args[n], IswNhorizDistance, btn_pad);            n++;
    IswSetArg(args[n], IswNvertDistance, btn_pad);             n++;
    IswSetArg(args[n], IswNlabel, "Revert");                   n++;
    IswSetArg(args[n], IswNborderWidth, 0);                    n++;
    IswSetArg(args[n], IswNwidth, btn_w);                      n++;
    IswSetArg(args[n], IswNheight, btn_h - btn_pad);           n++;
    IswSetArg(args[n], IswNinternalWidth, btn_pad);            n++;
    IswSetArg(args[n], IswNinternalHeight, btn_pad);           n++;
    IswSetArg(args[n], IswNresizable, True);                   n++;
    IswSetArg(args[n], IswNright, IswChainRight);               n++;
    IswSetArg(args[n], IswNleft, IswChainRight);                n++;
    IswSetArg(args[n], IswNbottom, IswChainBottom);             n++;
    IswSetArg(args[n], IswNtop, IswChainBottom);                n++;
    s->revert_btn = IswCreateManagedWidget("revertBtn", commandWidgetClass,
                                          s->content_form, args, n);
    IswAddCallback(s->revert_btn, IswNcallback, common_revert_cb, s);

    /* D-Bus */
    s->dbus = isde_dbus_init();
    if (s->dbus) {
        int fd = isde_dbus_get_fd(s->dbus);
        if (fd >= 0) {
            IswAppAddInput(s->app, fd, (IswPointer)IswInputReadMask,
                          settings_dbus_input_cb, s->dbus);
        }
        panel_appearance_set_dbus(s->dbus);
        panel_fonts_set_dbus(s->dbus);
        panel_display_set_dbus(s->dbus);
        panel_desktops_set_dbus(s->dbus);
    }

    IswRealizeWidget(s->toplevel);

    /* Show first panel by default */
    if (s->npanels > 0) {
        settings_switch_panel(s, 0);
    }

    /* Debug: post-realize dimensions */
    {
        Dimension tw, th, plw, plh, cfw, cfh, vpw, vph, caw, cah;
        Arg da[20];
        IswSetArg(da[0], IswNwidth, &tw); IswSetArg(da[1], IswNheight, &th);
        IswGetValues(s->toplevel, da, 2);
        IswSetArg(da[0], IswNwidth, &plw); IswSetArg(da[1], IswNheight, &plh);
        IswGetValues(s->panel_bar, da, 2);
        IswSetArg(da[0], IswNwidth, &cfw); IswSetArg(da[1], IswNheight, &cfh);
        IswGetValues(s->content_form, da, 2);
        IswSetArg(da[0], IswNwidth, &vpw); IswSetArg(da[1], IswNheight, &vph);
        IswGetValues(s->content_vp, da, 2);
        IswSetArg(da[0], IswNwidth, &caw); IswSetArg(da[1], IswNheight, &cah);
        IswGetValues(s->content_area, da, 2);
        fprintf(stderr, "POST-REALIZE: toplevel=%dx%d panel_bar=%dx%d content_form=%dx%d viewport=%dx%d content_area=%dx%d\n",
                tw, th, plw, plh, cfw, cfh, vpw, vph, caw, cah);
        if (s->panel_widgets[0]) {
            Dimension pw, pph;
            IswSetArg(da[0], IswNwidth, &pw); IswSetArg(da[1], IswNheight, &pph);
            IswGetValues(s->panel_widgets[0], da, 2);
            fprintf(stderr, "POST-REALIZE: panel[0]=%dx%d\n", pw, pph);
        }
    }

    s->running = 1;
    return 0;
}

void settings_run(Settings *s)
{
    while (s->running && !IswAppGetExitFlag(s->app)) {
        IswAppProcessEvent(s->app, IswIMAll);
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
    IswDestroyApplicationContext(s->app);
}
