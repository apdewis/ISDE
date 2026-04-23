#define _POSIX_C_SOURCE 200809L
/*
 * panel-appearance.c — Appearance settings:
 *   - Colour scheme (base16 themes)
 *   - Pointer/cursor theme (Xcursor)
 *   - Icon theme (freedesktop)
 */
#include "settings.h"
#include <ISW/List.h>
#include <ISW/IswArgMacros.h>

#include <stdlib.h>
#include <string.h>

#include "isde/isde-theme.h"

/* --- colour scheme --- */
static Widget  scheme_list;
static String *scheme_names_arr;
static int     scheme_count;
static int     saved_scheme_idx;

/* --- cursor theme --- */
static Widget  cursor_list;
static String *cursor_names_arr;
static int     cursor_count;
static int     saved_cursor_idx;

/* --- icon theme --- */
static Widget  icon_list;
static String *icon_names_arr;  /* display names (shown in List) */
static String *icon_dirs_arr;   /* directory names (written to config) */
static int     icon_count;
static int     saved_icon_idx;

static IsdeDBus *panel_dbus;

/* ---------- save / revert ---------- */

static void appearance_apply(void)
{
    char *path = isde_xdg_config_path("isde.toml");
    if (!path) { return; }

    /* Colour scheme */
    IswListReturnStruct *sr = IswListShowCurrent(scheme_list);
    if (sr && sr->list_index >= 0 && sr->list_index < scheme_count) {
        isde_config_write_string(path, "appearance", "color_scheme",
                                 scheme_names_arr[sr->list_index]);
        saved_scheme_idx = sr->list_index;
    }

    /* Cursor theme */
    sr = IswListShowCurrent(cursor_list);
    if (sr && sr->list_index >= 0 && sr->list_index < cursor_count) {
        isde_config_write_string(path, "appearance", "cursor_theme",
                                 cursor_names_arr[sr->list_index]);
        saved_cursor_idx = sr->list_index;
    }

    /* Icon theme */
    sr = IswListShowCurrent(icon_list);
    if (sr && sr->list_index >= 0 && sr->list_index < icon_count) {
        isde_config_write_string(path, "appearance", "icon_theme",
                                 icon_dirs_arr[sr->list_index]);
        saved_icon_idx = sr->list_index;
    }

    free(path);

    if (panel_dbus) {
        isde_dbus_settings_notify(panel_dbus, "appearance", "*");
    }
}

static void appearance_revert(void)
{
    if (saved_scheme_idx >= 0) {
        IswListHighlight(scheme_list, saved_scheme_idx);
    }
    if (saved_cursor_idx >= 0) {
        IswListHighlight(cursor_list, saved_cursor_idx);
    }
    if (saved_icon_idx >= 0) {
        IswListHighlight(icon_list, saved_icon_idx);
    }
}

/* ---------- find index of a name in an array ---------- */

static int find_index(String *arr, int count, const char *name)
{
    if (!name) { return 0; }
    for (int i = 0; i < count; i++) {
        if (strcmp(arr[i], name) == 0) { return i; }
    }
    return 0;
}

/* ---------- create ---------- */

static Widget appearance_create(Widget parent, IswAppContext app)
{
    (void)app;

    Dimension pw, ph;
    IswArgBuilder qb = IswArgBuilderInit();
    IswArgWidth(&qb, &pw);
    IswArgHeight(&qb, &ph);
    IswGetValues(parent, qb.args, qb.count);

    IswArgBuilder ab = IswArgBuilderInit();
    IswArgDefaultDistance(&ab, 8);
    IswArgBorderWidth(&ab, 0);
    IswArgResizable(&ab, True);

    Widget form = IswCreateWidget("appearForm", formWidgetClass,
                                 parent, ab.args, ab.count);

    /* Load current config */
    char *cur_scheme = NULL, *cur_cursor = NULL, *cur_icon = NULL;
    char errbuf[256];
    IsdeConfig *cfg = isde_config_load_xdg("isde.toml", errbuf, sizeof(errbuf));
    if (cfg) {
        IsdeConfigTable *root = isde_config_root(cfg);
        IsdeConfigTable *appear = isde_config_table(root, "appearance");
        if (appear) {
            const char *s;
            s = isde_config_string(appear, "color_scheme", NULL);
            if (s) { cur_scheme = strdup(s); }
            s = isde_config_string(appear, "cursor_theme", NULL);
            if (s) { cur_cursor = strdup(s); }
            s = isde_config_string(appear, "icon_theme", NULL);
            if (s) { cur_icon = strdup(s); }
        }
        isde_config_free(cfg);
    }

    Widget prev = NULL;
    int list_height = (ph > 0 ? (ph - 80) / 3 : 80);

    int lbl_w = 150;
    int list_w = 250;

    /* --- Colour Scheme --- */
    IswArgBuilderReset(&ab);
    IswArgLabel(&ab, "Colour Scheme:");
    IswArgBorderWidth(&ab, 0);
    IswArgWidth(&ab, lbl_w);
    IswArgJustify(&ab, IswJustifyRight);
    IswArgLeft(&ab, IswChainLeft);
    IswArgRight(&ab, IswChainLeft);
    if (prev) { IswArgFromVert(&ab, prev); }
    Widget scheme_lbl = IswCreateManagedWidget("lbl", labelWidgetClass,
                                              form, ab.args, ab.count);

    char **raw_schemes = NULL;
    scheme_count = isde_scheme_list(&raw_schemes);
    scheme_names_arr = (String *)raw_schemes;
    if (scheme_count == 0) {
        scheme_names_arr = malloc(sizeof(String));
        scheme_names_arr[0] = strdup("(none)");
        scheme_count = 1;
    }
    saved_scheme_idx = find_index(scheme_names_arr, scheme_count, cur_scheme);

    IswArgBuilderReset(&ab);
    IswArgAllowVert(&ab, True);
    IswArgAllowHoriz(&ab, False);
    IswArgUseRight(&ab, True);
    IswArgHeight(&ab, list_height);
    IswArgWidth(&ab, list_w);
    IswArgBorderWidth(&ab, 1);
    IswArgFromHoriz(&ab, scheme_lbl);
    IswArgLeft(&ab, IswChainLeft);
    if (prev) { IswArgFromVert(&ab, prev); }
    Widget scheme_vp = IswCreateManagedWidget("schemeVp", viewportWidgetClass,
                                              form, ab.args, ab.count);

    IswArgBuilderReset(&ab);
    IswArgList(&ab, scheme_names_arr);
    IswArgNumberStrings(&ab, scheme_count);
    IswArgDefaultColumns(&ab, 1);
    IswArgForceColumns(&ab, True);
    IswArgVerticalList(&ab, True);
    IswArgBorderWidth(&ab, 0);
    scheme_list = IswCreateManagedWidget("schemeList", listWidgetClass,
                                        scheme_vp, ab.args, ab.count);
    IswListHighlight(scheme_list, saved_scheme_idx);
    prev = scheme_vp;

    /* --- Cursor Theme --- */
    IswArgBuilderReset(&ab);
    IswArgLabel(&ab, "Cursor Theme:");
    IswArgBorderWidth(&ab, 0);
    IswArgWidth(&ab, lbl_w);
    IswArgJustify(&ab, IswJustifyRight);
    IswArgLeft(&ab, IswChainLeft);
    IswArgRight(&ab, IswChainLeft);
    IswArgFromVert(&ab, prev);
    Widget cursor_lbl = IswCreateManagedWidget("lbl", labelWidgetClass,
                                              form, ab.args, ab.count);

    char **raw_cursors = NULL;
    cursor_count = isde_cursor_theme_list(&raw_cursors);
    cursor_names_arr = (String *)raw_cursors;
    if (cursor_count == 0) {
        cursor_names_arr = malloc(sizeof(String));
        cursor_names_arr[0] = strdup("(none)");
        cursor_count = 1;
    }
    saved_cursor_idx = find_index(cursor_names_arr, cursor_count, cur_cursor);

    IswArgBuilderReset(&ab);
    IswArgAllowVert(&ab, True);
    IswArgAllowHoriz(&ab, False);
    IswArgUseRight(&ab, True);
    IswArgHeight(&ab, list_height);
    IswArgWidth(&ab, list_w);
    IswArgBorderWidth(&ab, 1);
    IswArgFromHoriz(&ab, cursor_lbl);
    IswArgFromVert(&ab, prev);
    IswArgLeft(&ab, IswChainLeft);
    Widget cursor_vp = IswCreateManagedWidget("cursorVp", viewportWidgetClass,
                                              form, ab.args, ab.count);

    IswArgBuilderReset(&ab);
    IswArgList(&ab, cursor_names_arr);
    IswArgNumberStrings(&ab, cursor_count);
    IswArgDefaultColumns(&ab, 1);
    IswArgForceColumns(&ab, True);
    IswArgVerticalList(&ab, True);
    IswArgBorderWidth(&ab, 0);
    cursor_list = IswCreateManagedWidget("cursorList", listWidgetClass,
                                        cursor_vp, ab.args, ab.count);
    IswListHighlight(cursor_list, saved_cursor_idx);
    prev = cursor_vp;

    /* --- Icon Theme --- */
    IswArgBuilderReset(&ab);
    IswArgLabel(&ab, "Icon Theme:");
    IswArgBorderWidth(&ab, 0);
    IswArgWidth(&ab, lbl_w);
    IswArgJustify(&ab, IswJustifyRight);
    IswArgLeft(&ab, IswChainLeft);
    IswArgRight(&ab, IswChainLeft);
    IswArgFromVert(&ab, prev);
    Widget icon_lbl = IswCreateManagedWidget("lbl", labelWidgetClass,
                                            form, ab.args, ab.count);

    char **raw_disp = NULL;
    char **raw_dirs = NULL;
    icon_count = isde_icon_theme_list_full(&raw_disp, &raw_dirs);
    icon_names_arr = (String *)raw_disp;
    icon_dirs_arr  = (String *)raw_dirs;
    if (icon_count == 0) {
        icon_names_arr = malloc(sizeof(String));
        icon_dirs_arr  = malloc(sizeof(String));
        icon_names_arr[0] = strdup("(none)");
        icon_dirs_arr[0]  = strdup("");
        icon_count = 1;
    }
    /* Configured value is the directory name — match against icon_dirs_arr.
     * Fall back to display-name match for configs written before the
     * dir-name convention was adopted. */
    saved_icon_idx = find_index(icon_dirs_arr, icon_count, cur_icon);
    if (saved_icon_idx == 0 && cur_icon &&
        strcmp(icon_dirs_arr[0], cur_icon) != 0) {
        saved_icon_idx = find_index(icon_names_arr, icon_count, cur_icon);
    }

    IswArgBuilderReset(&ab);
    IswArgAllowVert(&ab, True);
    IswArgAllowHoriz(&ab, False);
    IswArgUseRight(&ab, True);
    IswArgHeight(&ab, list_height);
    IswArgWidth(&ab, list_w);
    IswArgBorderWidth(&ab, 1);
    IswArgFromHoriz(&ab, icon_lbl);
    IswArgFromVert(&ab, prev);
    IswArgLeft(&ab, IswChainLeft);
    Widget icon_vp = IswCreateManagedWidget("iconVp", viewportWidgetClass,
                                            form, ab.args, ab.count);

    IswArgBuilderReset(&ab);
    IswArgList(&ab, icon_names_arr);
    IswArgNumberStrings(&ab, icon_count);
    IswArgDefaultColumns(&ab, 1);
    IswArgForceColumns(&ab, True);
    IswArgVerticalList(&ab, True);
    IswArgBorderWidth(&ab, 0);
    icon_list = IswCreateManagedWidget("iconList", listWidgetClass,
                                      icon_vp, ab.args, ab.count);
    IswListHighlight(icon_list, saved_icon_idx);
    prev = icon_vp;

    free(cur_scheme);
    free(cur_cursor);
    free(cur_icon);

    return form;
}

static int appearance_has_changes(void)
{
    /* Check if any list selection differs from saved */
    if (scheme_list) {
        IswListReturnStruct *sr = IswListShowCurrent(scheme_list);
        if (sr && sr->list_index != saved_scheme_idx) return 1;
    }
    if (cursor_list) {
        IswListReturnStruct *sr = IswListShowCurrent(cursor_list);
        if (sr && sr->list_index != saved_cursor_idx) return 1;
    }
    if (icon_list) {
        IswListReturnStruct *sr = IswListShowCurrent(icon_list);
        if (sr && sr->list_index != saved_icon_idx) return 1;
    }
    return 0;
}

static void appearance_destroy(void)
{
    scheme_list = NULL;
    cursor_list = NULL;
    icon_list = NULL;
    /* name arrays stay alive — List widgets hold pointers */
}

void panel_appearance_set_dbus(IsdeDBus *bus) { panel_dbus = bus; }

const IsdeSettingsPanel panel_appearance = {
    .name        = "Appearance",
    .icon        = NULL,
    .section     = "appearance",
    .create      = appearance_create,
    .apply       = appearance_apply,
    .revert      = appearance_revert,
    .has_changes = appearance_has_changes,
    .destroy     = appearance_destroy,
};
