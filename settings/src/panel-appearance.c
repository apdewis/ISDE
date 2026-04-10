#define _POSIX_C_SOURCE 200809L
/*
 * panel-appearance.c — Appearance settings:
 *   - Colour scheme (base16 themes)
 *   - Pointer/cursor theme (Xcursor)
 *   - Icon theme (freedesktop)
 */
#include "settings.h"
#include <ISW/List.h>

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
static String *icon_names_arr;
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
                                 icon_names_arr[sr->list_index]);
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

static Widget appearance_create(Widget parent, XtAppContext app)
{
    (void)app;

    Arg args[20];
    Cardinal n;
    Dimension pw, ph;
    Arg qargs[20];
    XtSetArg(qargs[0], XtNwidth, &pw);
    XtSetArg(qargs[1], XtNheight, &ph);
    XtGetValues(parent, qargs, 2);

    n = 0;
    XtSetArg(args[n], XtNdefaultDistance, 8); n++;
    XtSetArg(args[n], XtNborderWidth, 0);    n++;
    Widget form = XtCreateWidget("appearForm", formWidgetClass,
                                 parent, args, n);

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

    int lbl_w = 60;
    int list_max = 200;
    int list_w = (pw > 0 ? pw - lbl_w - 24 : list_max);
    if (list_w > list_max) list_w = list_max;

    /* --- Colour Scheme --- */
    n = 0;
    XtSetArg(args[n], XtNlabel, "Colour Scheme:");    n++;
    XtSetArg(args[n], XtNborderWidth, 0);               n++;
    XtSetArg(args[n], XtNwidth, lbl_w);                  n++;
    XtSetArg(args[n], XtNjustify, XtJustifyRight);       n++;
    if (prev) { XtSetArg(args[n], XtNfromVert, prev); n++; }
    Widget scheme_lbl = XtCreateManagedWidget("lbl", labelWidgetClass,
                                              form, args, n);

    char **raw_schemes = NULL;
    scheme_count = isde_scheme_list(&raw_schemes);
    scheme_names_arr = (String *)raw_schemes;
    if (scheme_count == 0) {
        scheme_names_arr = malloc(sizeof(String));
        scheme_names_arr[0] = strdup("(none)");
        scheme_count = 1;
    }
    saved_scheme_idx = find_index(scheme_names_arr, scheme_count, cur_scheme);

    n = 0;
    XtSetArg(args[n], XtNlist, scheme_names_arr);      n++;
    XtSetArg(args[n], XtNnumberStrings, scheme_count);  n++;
    XtSetArg(args[n], XtNdefaultColumns, 1);            n++;
    XtSetArg(args[n], XtNforceColumns, True);           n++;
    XtSetArg(args[n], XtNverticalList, True);           n++;
    XtSetArg(args[n], XtNheight, list_height);          n++;
    XtSetArg(args[n], XtNwidth, list_w);                n++;
    XtSetArg(args[n], XtNborderWidth, 0);               n++;
    XtSetArg(args[n], XtNfromHoriz, scheme_lbl);        n++;
    if (prev) { XtSetArg(args[n], XtNfromVert, prev); n++; }
    scheme_list = XtCreateManagedWidget("schemeList", listWidgetClass,
                                        form, args, n);
    IswListHighlight(scheme_list, saved_scheme_idx);
    prev = scheme_list;

    /* --- Cursor Theme --- */
    n = 0;
    XtSetArg(args[n], XtNlabel, "Cursor Theme:");     n++;
    XtSetArg(args[n], XtNborderWidth, 0);               n++;
    XtSetArg(args[n], XtNwidth, lbl_w);                  n++;
    XtSetArg(args[n], XtNjustify, XtJustifyRight);       n++;
    XtSetArg(args[n], XtNfromVert, prev);                 n++;
    Widget cursor_lbl = XtCreateManagedWidget("lbl", labelWidgetClass,
                                              form, args, n);

    char **raw_cursors = NULL;
    cursor_count = isde_cursor_theme_list(&raw_cursors);
    cursor_names_arr = (String *)raw_cursors;
    if (cursor_count == 0) {
        cursor_names_arr = malloc(sizeof(String));
        cursor_names_arr[0] = strdup("(none)");
        cursor_count = 1;
    }
    saved_cursor_idx = find_index(cursor_names_arr, cursor_count, cur_cursor);

    n = 0;
    XtSetArg(args[n], XtNlist, cursor_names_arr);      n++;
    XtSetArg(args[n], XtNnumberStrings, cursor_count);  n++;
    XtSetArg(args[n], XtNdefaultColumns, 1);            n++;
    XtSetArg(args[n], XtNforceColumns, True);           n++;
    XtSetArg(args[n], XtNverticalList, True);           n++;
    XtSetArg(args[n], XtNheight, list_height);          n++;
    XtSetArg(args[n], XtNwidth, list_w);                n++;
    XtSetArg(args[n], XtNborderWidth, 0);               n++;
    XtSetArg(args[n], XtNfromHoriz, cursor_lbl);        n++;
    XtSetArg(args[n], XtNfromVert, prev);                n++;
    cursor_list = XtCreateManagedWidget("cursorList", listWidgetClass,
                                        form, args, n);
    IswListHighlight(cursor_list, saved_cursor_idx);
    prev = cursor_list;

    /* --- Icon Theme --- */
    n = 0;
    XtSetArg(args[n], XtNlabel, "Icon Theme:");        n++;
    XtSetArg(args[n], XtNborderWidth, 0);               n++;
    XtSetArg(args[n], XtNwidth, lbl_w);                  n++;
    XtSetArg(args[n], XtNjustify, XtJustifyRight);       n++;
    XtSetArg(args[n], XtNfromVert, prev);                 n++;
    Widget icon_lbl = XtCreateManagedWidget("lbl", labelWidgetClass,
                                            form, args, n);

    char **raw_icons = NULL;
    icon_count = isde_icon_theme_list(&raw_icons);
    icon_names_arr = (String *)raw_icons;
    if (icon_count == 0) {
        icon_names_arr = malloc(sizeof(String));
        icon_names_arr[0] = strdup("(none)");
        icon_count = 1;
    }
    saved_icon_idx = find_index(icon_names_arr, icon_count, cur_icon);

    n = 0;
    XtSetArg(args[n], XtNlist, icon_names_arr);        n++;
    XtSetArg(args[n], XtNnumberStrings, icon_count);    n++;
    XtSetArg(args[n], XtNdefaultColumns, 1);            n++;
    XtSetArg(args[n], XtNforceColumns, True);           n++;
    XtSetArg(args[n], XtNverticalList, True);           n++;
    XtSetArg(args[n], XtNheight, list_height);          n++;
    XtSetArg(args[n], XtNwidth, list_w);                n++;
    XtSetArg(args[n], XtNborderWidth, 0);               n++;
    XtSetArg(args[n], XtNfromHoriz, icon_lbl);          n++;
    XtSetArg(args[n], XtNfromVert, prev);                n++;
    icon_list = XtCreateManagedWidget("iconList", listWidgetClass,
                                      form, args, n);
    IswListHighlight(icon_list, saved_icon_idx);
    prev = icon_list;

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
