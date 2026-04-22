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
#include <ISW/IswArgMacros.h>

#define PANEL_LIST_WIDTH 180

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
        /* Query viewport size for initial panel dimensions. */
        Dimension vpw, vph;
        IswArgBuilder qb = IswArgBuilderInit();
        IswArgWidth(&qb, &vpw);
        IswArgHeight(&qb, &vph);
        IswGetValues(s->content_vp, qb.args, qb.count);

        /* Set content_area so panels can query parent dimensions */
        if (vpw > 0 || vph > 0) {
            IswArgBuilder ab = IswArgBuilderInit();
            if (vpw > 0) { IswArgWidth(&ab, vpw); }
            if (vph > 0) { IswArgHeight(&ab, vph); }
            IswSetValues(s->content_area, ab.args, ab.count);
        }

        s->panel_widgets[index] =
            s->panels[index]->create(s->content_area, s->app);

        /* Anchor panel top-left so rubber constraints don't scale it
         * when the viewport resizes content_area.  Size it to the
         * current viewport so it realises with nonzero geometry. */
        {
            IswArgBuilder ab = IswArgBuilderInit();
            if (vpw > 0) { IswArgWidth(&ab, vpw); }
            if (vph > 0) { IswArgHeight(&ab, vph); }
            IswArgTop(&ab, IswChainTop);
            IswArgBottom(&ab, IswChainTop);
            IswArgLeft(&ab, IswChainLeft);
            IswArgRight(&ab, IswChainLeft);
            IswSetValues(s->panel_widgets[index], ab.args, ab.count);
        }
    }

    IswManageChild(s->panel_widgets[index]);

    /* Debug: dump widget dimensions */
    {
        Dimension tw, th, cfw, cfh, vpw, vph, caw, cah, pw, pph;
        IswArgBuilder db = IswArgBuilderInit();

        IswArgWidth(&db, &tw); IswArgHeight(&db, &th);
        IswGetValues(s->toplevel, db.args, db.count);

        IswArgBuilderReset(&db);
        IswArgWidth(&db, &cfw); IswArgHeight(&db, &cfh);
        IswGetValues(s->content_form, db.args, db.count);

        IswArgBuilderReset(&db);
        IswArgWidth(&db, &vpw); IswArgHeight(&db, &vph);
        IswGetValues(s->content_vp, db.args, db.count);

        IswArgBuilderReset(&db);
        IswArgWidth(&db, &caw); IswArgHeight(&db, &cah);
        IswGetValues(s->content_area, db.args, db.count);

        IswArgBuilderReset(&db);
        IswArgWidth(&db, &pw); IswArgHeight(&db, &pph);
        IswGetValues(s->panel_widgets[index], db.args, db.count);
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

    int init_w = 960;
    int init_h = 450;
    isde_clamp_to_workarea(IswDisplay(s->toplevel), 0, &init_w, &init_h);

    IswArgBuilder ab = IswArgBuilderInit();
    IswArgWidth(&ab, init_w);
    IswArgHeight(&ab, init_h);
    IswArgMinWidth(&ab, 400);
    IswArgMinHeight(&ab, 300);
    IswSetValues(s->toplevel, ab.args, ab.count);

    IswAddCallback(s->toplevel, IswNdestroyCallback,
                  settings_destroy_cb, s);

    /* Main layout form — direct child of toplevel */
    IswArgBuilderReset(&ab);
    IswArgDefaultDistance(&ab, 0);
    IswArgBorderWidth(&ab, 0);
    Widget form = IswCreateManagedWidget("layout", formWidgetClass,
                                        s->toplevel, ab.args, ab.count);

    /* Register core panels first (so names are available for the list) */
    register_panel(s, &panel_input, NULL);
    register_panel(s, &panel_keyboard, NULL);
    register_panel(s, &panel_appearance, NULL);
    register_panel(s, &panel_fonts, NULL);
    register_panel(s, &panel_terminal, NULL);
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

    IswArgBuilderReset(&ab);
    IswArgList(&ab, panel_names);
    IswArgNumberStrings(&ab, s->npanels);
    IswArgDefaultColumns(&ab, 1);
    IswArgForceColumns(&ab, True);
    IswArgVerticalList(&ab, True);
    IswArgWidth(&ab, PANEL_LIST_WIDTH);
    IswArgHeight(&ab, init_h - 10);
    IswArgBorderWidth(&ab, 0);
    IswArgResizable(&ab, True);
    IswArgTop(&ab, IswChainTop);
    IswArgBottom(&ab, IswChainBottom);
    IswArgLeft(&ab, IswChainLeft);
    IswArgRight(&ab, IswChainLeft);
    s->panel_bar = IswCreateManagedWidget("panelList", listWidgetClass,
                                         form, ab.args, ab.count);
    IswAddCallback(s->panel_bar, IswNcallback, panel_list_cb, s);

    /* Right pane: form with scrollable content + fixed buttons at bottom */
    int right_w = init_w - PANEL_LIST_WIDTH - 4;
    int right_h = init_h - 10;
    int btn_h = 32;
    int btn_pad = 8;
    int sb_w = 14;  /* scrollbar thickness */

    IswArgBuilderReset(&ab);
    IswArgFromHoriz(&ab, s->panel_bar);
    IswArgBorderWidth(&ab, 0);
    IswArgDefaultDistance(&ab, 0);
    IswArgWidth(&ab, right_w);
    IswArgHeight(&ab, right_h);
    IswArgResizable(&ab, False);
    IswArgTop(&ab, IswChainTop);
    IswArgBottom(&ab, IswChainBottom);
    IswArgLeft(&ab, IswChainLeft);
    IswArgRight(&ab, IswChainRight);
    s->content_form = IswCreateManagedWidget("contentForm", formWidgetClass,
                                            form, ab.args, ab.count);

    /* Scrollable viewport for panel content */
    IswArgBuilderReset(&ab);
    IswArgAllowVert(&ab, True);
    IswArgAllowHoriz(&ab, True);
    IswArgUseBottom(&ab, True);
    IswArgUseRight(&ab, True);
    IswArgBorderWidth(&ab, 0);
    IswArgWidth(&ab, right_w);
    IswArgHeight(&ab, right_h - btn_h - btn_pad);
    IswArgTop(&ab, IswChainTop);
    IswArgBottom(&ab, IswChainBottom);
    IswArgLeft(&ab, IswChainLeft);
    IswArgRight(&ab, IswChainRight);
    s->content_vp = IswCreateManagedWidget("contentScroll", viewportWidgetClass,
                                          s->content_form, ab.args, ab.count);

    /* Panel content container inside viewport */
    IswArgBuilderReset(&ab);
    IswArgDefaultDistance(&ab, 0);
    IswArgBorderWidth(&ab, 0);
    IswArgWidth(&ab, right_w);
    IswArgHeight(&ab, right_h - btn_h - btn_pad);
    s->content_area = IswCreateManagedWidget("content", formWidgetClass,
                                            s->content_vp, ab.args, ab.count);

    /* Save / Revert buttons — fixed at bottom right. */
    int btn_w = 80;

    IswArgBuilderReset(&ab);
    IswArgFromVert(&ab, s->content_vp);
    IswArgLabel(&ab, "Save");
    IswArgBorderWidth(&ab, 0);
    IswArgWidth(&ab, btn_w);
    IswArgHeight(&ab, btn_h - btn_pad);
    IswArgInternalWidth(&ab, btn_pad);
    IswArgInternalHeight(&ab, btn_pad);
    IswArgVertDistance(&ab, btn_pad);
    IswArgHorizDistance(&ab, right_w - btn_w * 2 - btn_pad * 2 - sb_w);
    IswArgResizable(&ab, True);
    IswArgRight(&ab, IswChainRight);
    IswArgLeft(&ab, IswChainRight);
    IswArgBottom(&ab, IswChainBottom);
    IswArgTop(&ab, IswChainBottom);
    s->save_btn = IswCreateManagedWidget("saveBtn", commandWidgetClass,
                                        s->content_form, ab.args, ab.count);
    IswAddCallback(s->save_btn, IswNcallback, common_save_cb, s);

    IswArgBuilderReset(&ab);
    IswArgFromVert(&ab, s->content_vp);
    IswArgFromHoriz(&ab, s->save_btn);
    IswArgHorizDistance(&ab, btn_pad);
    IswArgVertDistance(&ab, btn_pad);
    IswArgLabel(&ab, "Revert");
    IswArgBorderWidth(&ab, 0);
    IswArgWidth(&ab, btn_w);
    IswArgHeight(&ab, btn_h - btn_pad);
    IswArgInternalWidth(&ab, btn_pad);
    IswArgInternalHeight(&ab, btn_pad);
    IswArgResizable(&ab, True);
    IswArgRight(&ab, IswChainRight);
    IswArgLeft(&ab, IswChainRight);
    IswArgBottom(&ab, IswChainBottom);
    IswArgTop(&ab, IswChainBottom);
    s->revert_btn = IswCreateManagedWidget("revertBtn", commandWidgetClass,
                                          s->content_form, ab.args, ab.count);
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
        panel_terminal_set_dbus(s->dbus);
        panel_display_set_dbus(s->dbus);
        panel_desktops_set_dbus(s->dbus);
    }

    IswRealizeWidget(s->toplevel);

    /* Show first panel by default */
    if (s->npanels > 0) {
        settings_switch_panel(s, 0);
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
