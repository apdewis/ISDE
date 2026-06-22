#define _POSIX_C_SOURCE 200809L
/*
 * settings.c — isde-settings UI shell, plugin loading, panel windows
 *
 * Layout: main window = IconView grid of panel icons
 *         each panel opens in its own top-level window
 */
#include "settings.h"

#include <stdio.h>
#include <ISW/ISWPlatform.h>
#include <stdlib.h>
#include <string.h>
#include <dlfcn.h>
#include <dirent.h>

#include <ISW/Viewport.h>
#include <ISW/IswArgMacros.h>

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
    memset(&s->panel_wins[idx], 0, sizeof(PanelWindow));
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

/* ---------- per-panel save/revert callbacks ---------- */

typedef struct {
    Settings *s;
    int       index;
} PanelCbData;

static PanelCbData cb_data[MAX_PANELS];

static void panel_save_cb(Widget w, IswPointer cd, IswPointer call)
{
    (void)w; (void)call;
    PanelCbData *d = (PanelCbData *)cd;
    if (d->s->panels[d->index]->apply) {
        d->s->panels[d->index]->apply();
    }
}

static void panel_revert_cb(Widget w, IswPointer cd, IswPointer call)
{
    (void)w; (void)call;
    PanelCbData *d = (PanelCbData *)cd;
    if (d->s->panels[d->index]->revert) {
        d->s->panels[d->index]->revert();
    }
}

/* ---------- panel window close ---------- */

static void panel_destroy_cb(Widget w, IswPointer cd, IswPointer call)
{
    (void)w; (void)call;
    PanelCbData *d = (PanelCbData *)cd;
    int idx = d->index;
    if (d->s->panels[idx]->destroy) {
        d->s->panels[idx]->destroy();
    }
    memset(&d->s->panel_wins[idx], 0, sizeof(PanelWindow));
}

/* ---------- opening a panel window ---------- */

void settings_open_panel(Settings *s, int index)
{
    if (index < 0 || index >= s->npanels) {
        return;
    }

    PanelWindow *pw = &s->panel_wins[index];

    if (pw->open && pw->shell) {
        IswPopup(pw->shell, IswGrabNone);
        return;
    }

    if (!pw->shell) {
        int win_w = 700;
        int win_h = 450;
        int btn_h = 32;
        int btn_pad = 8;
        int btn_w = 80;
        int sb_w = 14;

        char title[128];
        snprintf(title, sizeof(title), "%s — Settings", s->panels[index]->name);

        IswArgBuilder ab = IswArgBuilderInit();
        IswArgWidth(&ab, win_w);
        IswArgHeight(&ab, win_h);
        IswArgMinWidth(&ab, 400);
        IswArgMinHeight(&ab, 300);
        IswArgTitle(&ab, title);
        pw->shell = IswCreatePopupShell("panelShell",
                                        topLevelShellWidgetClass,
                                        s->toplevel, ab.args, ab.count);

        cb_data[index].s = s;
        cb_data[index].index = index;
        IswAddCallback(pw->shell, IswNdestroyCallback,
                       panel_destroy_cb, &cb_data[index]);

        /* Form layout: viewport + buttons at bottom */
        IswArgBuilderReset(&ab);
        IswArgDefaultDistance(&ab, 0);
        IswArgBorderWidth(&ab, 0);
        Widget form = IswCreateManagedWidget("panelForm", formWidgetClass,
                                            pw->shell, ab.args, ab.count);

        /* Scrollable viewport for panel content */
        IswArgBuilderReset(&ab);
        IswArgAllowVert(&ab, True);
        IswArgAllowHoriz(&ab, True);
        IswArgUseBottom(&ab, True);
        IswArgUseRight(&ab, True);
        IswArgBorderBottom(&ab, 1);
        IswArgWidth(&ab, win_w);
        IswArgHeight(&ab, win_h - btn_h - btn_pad);
        IswArgTop(&ab, IswChainTop);
        IswArgBottom(&ab, IswChainBottom);
        IswArgLeft(&ab, IswChainLeft);
        IswArgRight(&ab, IswChainRight);
        pw->content_vp = IswCreateManagedWidget("contentScroll",
                                                viewportWidgetClass,
                                                form, ab.args, ab.count);

        /* Panel content container inside viewport */
        IswArgBuilderReset(&ab);
        IswArgDefaultDistance(&ab, 0);
        IswArgBorderWidth(&ab, 0);
        IswArgWidth(&ab, win_w);
        IswArgHeight(&ab, win_h - btn_h - btn_pad);
        pw->content_area = IswCreateManagedWidget("content", formWidgetClass,
                                                  pw->content_vp,
                                                  ab.args, ab.count);

        /* Create the panel widget tree */
        pw->panel_widget = s->panels[index]->create(pw->content_area, s->app);

        IswArgBuilderReset(&ab);
        IswArgTop(&ab, IswChainTop);
        IswArgBottom(&ab, IswChainTop);
        IswArgLeft(&ab, IswChainLeft);
        IswArgRight(&ab, IswChainLeft);
        IswSetValues(pw->panel_widget, ab.args, ab.count);

        IswManageChild(pw->panel_widget);

        /* Revert / Apply buttons — fixed at bottom right */
        IswArgBuilderReset(&ab);
        IswArgFromVert(&ab, pw->content_vp);
        IswArgLabel(&ab, "Revert");
        IswArgWidth(&ab, btn_w);
        IswArgHeight(&ab, btn_h - btn_pad);
        IswArgInternalWidth(&ab, btn_pad);
        IswArgInternalHeight(&ab, btn_pad);
        IswArgVertDistance(&ab, btn_pad);
        IswArgHorizDistance(&ab, win_w - btn_pad - ((btn_w + sb_w) * 2));
        IswArgResizable(&ab, True);
        IswArgRight(&ab, IswChainRight);
        IswArgLeft(&ab, IswChainRight);
        IswArgBottom(&ab, IswChainBottom);
        IswArgTop(&ab, IswChainBottom);
        pw->revert_btn = IswCreateManagedWidget("revertBtn",
                                                commandWidgetClass,
                                                form, ab.args, ab.count);
        IswAddCallback(pw->revert_btn, IswNcallback,
                       panel_revert_cb, &cb_data[index]);

        IswArgBuilderReset(&ab);
        IswArgFromVert(&ab, pw->content_vp);
        IswArgLabel(&ab, "Apply");
        IswArgWidth(&ab, btn_w);
        IswArgHeight(&ab, btn_h - btn_pad);
        IswArgInternalWidth(&ab, btn_pad);
        IswArgInternalHeight(&ab, btn_pad);
        IswArgVertDistance(&ab, btn_pad);
        IswArgHorizDistance(&ab, win_w - btn_w - btn_pad - sb_w);
        IswArgResizable(&ab, True);
        IswArgRight(&ab, IswChainRight);
        IswArgLeft(&ab, IswChainRight);
        IswArgBottom(&ab, IswChainBottom);
        IswArgTop(&ab, IswChainBottom);
        pw->save_btn = IswCreateManagedWidget("saveBtn", commandWidgetClass,
                                             form, ab.args, ab.count);
        IswAddCallback(pw->save_btn, IswNcallback,
                       panel_save_cb, &cb_data[index]);
    }

    IswRealizeWidget(pw->shell);
    IswPopup(pw->shell, IswGrabNone);
    pw->open = 1;
}

/* ---------- icon view selection callback ---------- */

static void icon_select_cb(Widget w, IswPointer cd, IswPointer call)
{
    (void)w;
    Settings *s = (Settings *)cd;
    IswIconViewCallbackData *data = (IswIconViewCallbackData *)call;
    if (data->index >= 0 && data->index < s->npanels) {
        settings_open_panel(s, data->index);
    }
}

/* ---------- D-Bus ---------- */

static void settings_theme_changed_cb(void *user_data)
{
    Settings *s = (Settings *)user_data;
    IswReloadResources(s->toplevel);
}

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

    s->toplevel = IswAppInitialize(&s->app, "ISDE-Settings",
                                  NULL, 0, argc, argv,
                                  NULL, NULL, 0);
    isde_theme_merge_xrm(s->toplevel);

    int init_w = 340;

    IswArgBuilder ab = IswArgBuilderInit();
    IswArgWidth(&ab, init_w);
    IswArgMinWidth(&ab, 300);
    IswArgMinHeight(&ab, 250);
    IswArgTitle(&ab, "Settings");
    IswSetValues(s->toplevel, ab.args, ab.count);

    IswAddCallback(s->toplevel, IswNdestroyCallback,
                  settings_destroy_cb, s);

    /* Register core panels */
    register_panel(s, &panel_input, NULL);
    register_panel(s, &panel_keyboard, NULL);
    register_panel(s, &panel_appearance, NULL);
    register_panel(s, &panel_fonts, NULL);
    register_panel(s, &panel_terminal, NULL);
    register_panel(s, &panel_display, NULL);
    register_panel(s, &panel_desktops, NULL);
    register_panel(s, &panel_dm, NULL);
    register_panel(s, &panel_power, NULL);
    load_plugins(s);

    /* Build icon label and path arrays for IconView */
    for (int i = 0; i < s->npanels; i++) {
        s->icon_labels[i] = (String)s->panels[i]->name;
        if (s->panels[i]->icon) {
            s->icon_paths[i] = isde_icon_find("categories",
                                              s->panels[i]->icon);
        } else {
            s->icon_paths[i] = NULL;
        }
    }

    /* Viewport wrapping the icon view for scrolling */
    IswArgBuilderReset(&ab);
    IswArgAllowVert(&ab, True);
    IswArgAllowHoriz(&ab, False);
    IswArgUseRight(&ab, True);
    IswArgBorderWidth(&ab, 0);
    Widget vp = IswCreateManagedWidget("iconScroll", viewportWidgetClass,
                                      s->toplevel, ab.args, ab.count);

    /* IconView grid */
    IswArgBuilderReset(&ab);
    IswArgIconLabels(&ab, s->icon_labels);
    IswArgIconData(&ab, s->icon_paths);
    IswArgNumIcons(&ab, s->npanels);
    IswArgIconSize(&ab, 48);
    IswArgMultiSelect(&ab, False);
    IswArgWidth(&ab, init_w);
    s->icon_view = IswCreateManagedWidget("iconView", iconViewWidgetClass,
                                         vp, ab.args, ab.count);
    IswAddCallback(s->icon_view, IswNselectCallback, icon_select_cb, s);

    /* D-Bus */
    s->dbus = isde_dbus_init();
    if (s->dbus) {
        int fd = isde_dbus_get_fd(s->dbus);
        if (fd >= 0) {
            IswAppAddInput(s->app, fd, (IswPointer)IswInputReadMask,
                          settings_dbus_input_cb, s->dbus);
        }
        isde_theme_watch(s->dbus, s->toplevel,
                         settings_theme_changed_cb, s);
        panel_appearance_set_dbus(s->dbus);
        panel_fonts_set_dbus(s->dbus);
        panel_terminal_set_dbus(s->dbus);
        panel_display_set_dbus(s->dbus);
        panel_desktops_set_dbus(s->dbus);
        panel_power_set_dbus(s->dbus);
    }

    IswRealizeWidget(s->toplevel);

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
        if (s->panel_wins[i].shell && s->panels[i]->destroy) {
            s->panels[i]->destroy();
        }
        if (s->plugin_handles[i]) {
            dlclose(s->plugin_handles[i]);
        }
        free(s->icon_paths[i]);
    }
    isde_dbus_free(s->dbus);
    IswDestroyApplicationContext(s->app);
}
