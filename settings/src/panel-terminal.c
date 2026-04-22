#define _POSIX_C_SOURCE 200809L
/*
 * panel-terminal.c — Terminal settings:
 *   - Monospace font (family + size)
 *   - Colour scheme (reuses global theme list)
 *   - Cursor shape
 *   - Scrollback lines
 *
 * Stored under [terminal] in isde.toml.
 */
#include "settings.h"

#include <ISW/List.h>
#include <ISW/SpinBox.h>
#include <ISW/IswArgMacros.h>
#include "isde/isde-dialog.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static IsdeDBus *panel_dbus;

static Widget toplevel_cache;

static char     cur_font_family[128];
static int      cur_font_size;
static char     cur_color_scheme[128];
static int      cur_scrollback;
static char     cur_cursor_shape[16];

static char     saved_font_family[128];
static int      saved_font_size;
static char     saved_color_scheme[128];
static int      saved_scrollback;
static char     saved_cursor_shape[16];

static Widget   font_desc_lbl;
static Widget   scheme_list;
static Widget   cursor_shape_list;
static Widget   scrollback_spin;

static String  *scheme_names_arr;
static int      scheme_count;

static const char *cursor_shapes[] = { "block", "underline", "bar" };
#define NUM_CURSOR_SHAPES 3

static Widget chooser_shell;

static int find_idx(String *arr, int n, const char *name)
{
    if (!name) return 0;
    for (int i = 0; i < n; i++) {
        if (arr[i] && strcmp(arr[i], name) == 0) return i;
    }
    return 0;
}

static void update_font_label(void)
{
    char buf[160];
    char font[160];
    snprintf(buf, sizeof(buf), "%s %dpt", cur_font_family, cur_font_size);
    snprintf(font, sizeof(font), "%s-%d", cur_font_family, cur_font_size);
    IswFontStruct *fs = isde_resolve_font(font_desc_lbl, font);
    IswArgBuilder ab = IswArgBuilderInit();
    IswArgResize(&ab, True);
    IswArgWidth(&ab, 250);
    IswArgLabel(&ab, buf);
    if (fs) { IswArgFont(&ab, fs); }
    IswSetValues(font_desc_lbl, ab.args, ab.count);
}

static void chooser_result_cb(IsdeDialogResult result,
                              const char *family, int size, void *data)
{
    (void)data;
    chooser_shell = NULL;
    if (result != ISDE_DIALOG_OK) return;
    if (family && family[0] && family != cur_font_family) {
        snprintf(cur_font_family, sizeof(cur_font_family), "%s", family);
    }
    if (size > 0) cur_font_size = size;
    update_font_label();
}

static void edit_font_cb(Widget w, IswPointer cd, IswPointer call)
{
    (void)w; (void)cd; (void)call;
    isde_dialog_dismiss(chooser_shell);
    chooser_shell = isde_dialog_font(toplevel_cache, "Terminal Font",
                                     cur_font_family, cur_font_size,
                                     chooser_result_cb, NULL);
}

static Widget terminal_create(Widget parent, IswAppContext app)
{
    (void)app;

    toplevel_cache = parent;
    while (toplevel_cache && !IswIsShell(toplevel_cache)) {
        toplevel_cache = IswParent(toplevel_cache);
    }

    /* Defaults */
    snprintf(cur_font_family, sizeof(cur_font_family), "%s", "Monospace");
    cur_font_size   = 11;
    cur_scrollback  = 10000;
    snprintf(cur_cursor_shape, sizeof(cur_cursor_shape), "%s", "block");
    cur_color_scheme[0] = '\0';

    char errbuf[256];
    IsdeConfig *cfg = isde_config_load_xdg("isde.toml", errbuf, sizeof(errbuf));
    if (cfg) {
        IsdeConfigTable *root = isde_config_root(cfg);
        IsdeConfigTable *term = isde_config_table(root, "terminal");
        if (term) {
            const char *s = isde_config_string(term, "font_family", NULL);
            if (s) snprintf(cur_font_family, sizeof(cur_font_family), "%s", s);
            int n = (int)isde_config_int(term, "font_size", 0);
            if (n > 0) cur_font_size = n;
            s = isde_config_string(term, "color_scheme", NULL);
            if (s) snprintf(cur_color_scheme, sizeof(cur_color_scheme), "%s", s);
            n = (int)isde_config_int(term, "scrollback", -1);
            if (n >= 0) cur_scrollback = n;
            s = isde_config_string(term, "cursor_shape", NULL);
            if (s) snprintf(cur_cursor_shape, sizeof(cur_cursor_shape), "%s", s);
        }
        if (!cur_color_scheme[0]) {
            IsdeConfigTable *ap = isde_config_table(root, "appearance");
            if (ap) {
                const char *s = isde_config_string(ap, "color_scheme", NULL);
                if (s) snprintf(cur_color_scheme, sizeof(cur_color_scheme), "%s", s);
            }
        }
        isde_config_free(cfg);
    }
    memcpy(saved_font_family, cur_font_family, sizeof(saved_font_family));
    saved_font_size = cur_font_size;
    memcpy(saved_color_scheme, cur_color_scheme, sizeof(saved_color_scheme));
    saved_scrollback = cur_scrollback;
    memcpy(saved_cursor_shape, cur_cursor_shape, sizeof(saved_cursor_shape));

    IswArgBuilder ab = IswArgBuilderInit();
    IswArgDefaultDistance(&ab, 8);
    IswArgBorderWidth(&ab, 0);
    Widget form = IswCreateWidget("termForm", formWidgetClass,
                                  parent, ab.args, ab.count);

    int lbl_w = 150;
    Widget prev = NULL;

    /* Font row */
    IswArgBuilderReset(&ab);
    IswArgLabel(&ab, "Font:");
    IswArgBorderWidth(&ab, 0);
    IswArgWidth(&ab, lbl_w);
    IswArgJustify(&ab, IswJustifyRight);
    IswArgLeft(&ab, IswChainLeft);
    IswArgRight(&ab, IswChainLeft);
    Widget font_lbl = IswCreateManagedWidget("lbl", labelWidgetClass,
                                             form, ab.args, ab.count);

    char desc[160];
    char font[160];
    snprintf(desc, sizeof(desc), "%s %dpt", cur_font_family, cur_font_size);
    snprintf(font, sizeof(font), "%s-%d", cur_font_family, cur_font_size);
    IswFontStruct *fs = isde_resolve_font(form, font);
    IswArgBuilderReset(&ab);
    IswArgLabel(&ab, desc);
    IswArgBorderWidth(&ab, 0);
    IswArgWidth(&ab, 250);
    IswArgJustify(&ab, IswJustifyLeft);
    IswArgFromHoriz(&ab, font_lbl);
    IswArgResize(&ab, True);
    IswArgLeft(&ab, IswChainLeft);
    if (fs) { IswArgFont(&ab, fs); }
    font_desc_lbl = IswCreateManagedWidget("fontDesc", labelWidgetClass,
                                           form, ab.args, ab.count);

    IswArgBuilderReset(&ab);
    IswArgLabel(&ab, "Edit...");
    IswArgWidth(&ab, 80);
    IswArgFromHoriz(&ab, font_desc_lbl);
    IswArgLeft(&ab, IswChainLeft);
    Widget edit_btn = IswCreateManagedWidget("fontEdit", commandWidgetClass,
                                             form, ab.args, ab.count);
    IswAddCallback(edit_btn, IswNcallback, edit_font_cb, NULL);
    prev = font_lbl;

    /* Colour scheme list */
    IswArgBuilderReset(&ab);
    IswArgLabel(&ab, "Colour Scheme:");
    IswArgBorderWidth(&ab, 0);
    IswArgWidth(&ab, lbl_w);
    IswArgJustify(&ab, IswJustifyRight);
    IswArgFromVert(&ab, prev);
    IswArgLeft(&ab, IswChainLeft);
    IswArgRight(&ab, IswChainLeft);
    Widget sc_lbl = IswCreateManagedWidget("lbl", labelWidgetClass,
                                           form, ab.args, ab.count);

    char **raw = NULL;
    scheme_count = isde_scheme_list(&raw);
    scheme_names_arr = (String *)raw;
    if (scheme_count == 0) {
        scheme_names_arr = malloc(sizeof(String));
        scheme_names_arr[0] = strdup("default-dark");
        scheme_count = 1;
    }

    IswArgBuilderReset(&ab);
    IswArgList(&ab, scheme_names_arr);
    IswArgNumberStrings(&ab, scheme_count);
    IswArgDefaultColumns(&ab, 1);
    IswArgForceColumns(&ab, True);
    IswArgVerticalList(&ab, True);
    IswArgHeight(&ab, 120);
    IswArgWidth(&ab, 200);
    IswArgBorderWidth(&ab, 0);
    IswArgFromHoriz(&ab, sc_lbl);
    IswArgFromVert(&ab, prev);
    IswArgLeft(&ab, IswChainLeft);
    scheme_list = IswCreateManagedWidget("termSchemeList", listWidgetClass,
                                         form, ab.args, ab.count);
    IswListHighlight(scheme_list, find_idx(scheme_names_arr, scheme_count,
                                           cur_color_scheme));
    prev = scheme_list;

    /* Cursor shape list */
    IswArgBuilderReset(&ab);
    IswArgLabel(&ab, "Cursor Shape:");
    IswArgBorderWidth(&ab, 0);
    IswArgWidth(&ab, lbl_w);
    IswArgJustify(&ab, IswJustifyRight);
    IswArgFromVert(&ab, prev);
    IswArgLeft(&ab, IswChainLeft);
    IswArgRight(&ab, IswChainLeft);
    Widget cs_lbl = IswCreateManagedWidget("lbl", labelWidgetClass,
                                           form, ab.args, ab.count);

    static String shape_names[NUM_CURSOR_SHAPES + 1];
    for (int i = 0; i < NUM_CURSOR_SHAPES; i++) {
        shape_names[i] = (String)cursor_shapes[i];
    }
    shape_names[NUM_CURSOR_SHAPES] = NULL;

    IswArgBuilderReset(&ab);
    IswArgList(&ab, shape_names);
    IswArgNumberStrings(&ab, NUM_CURSOR_SHAPES);
    IswArgDefaultColumns(&ab, 1);
    IswArgForceColumns(&ab, True);
    IswArgVerticalList(&ab, True);
    IswArgHeight(&ab, 80);
    IswArgWidth(&ab, 200);
    IswArgBorderWidth(&ab, 0);
    IswArgFromHoriz(&ab, cs_lbl);
    IswArgFromVert(&ab, prev);
    IswArgLeft(&ab, IswChainLeft);
    cursor_shape_list = IswCreateManagedWidget("termCursorList", listWidgetClass,
                                               form, ab.args, ab.count);
    IswListHighlight(cursor_shape_list,
                     find_idx(shape_names, NUM_CURSOR_SHAPES, cur_cursor_shape));
    prev = cursor_shape_list;

    /* Scrollback spin */
    IswArgBuilderReset(&ab);
    IswArgLabel(&ab, "Scrollback Lines:");
    IswArgBorderWidth(&ab, 0);
    IswArgWidth(&ab, lbl_w);
    IswArgJustify(&ab, IswJustifyRight);
    IswArgFromVert(&ab, prev);
    IswArgLeft(&ab, IswChainLeft);
    IswArgRight(&ab, IswChainLeft);
    Widget sb_lbl = IswCreateManagedWidget("lbl", labelWidgetClass,
                                           form, ab.args, ab.count);

    IswArgBuilderReset(&ab);
    IswArgBorderWidth(&ab, 1);
    IswArgWidth(&ab, 120);
    IswArgFromHoriz(&ab, sb_lbl);
    IswArgFromVert(&ab, prev);
    IswArgLeft(&ab, IswChainLeft);
    scrollback_spin = IswCreateManagedWidget("termScrollback", spinBoxWidgetClass,
                                             form, ab.args, ab.count);
    IswArgBuilderReset(&ab);
    IswArgSpinMinimum(&ab, 0);
    IswArgSpinMaximum(&ab, 100000);
    IswArgSpinIncrement(&ab, 100);
    IswArgSpinValue(&ab, cur_scrollback);
    IswSetValues(scrollback_spin, ab.args, ab.count);

    return form;
}

static void terminal_apply(void)
{
    char *path = isde_xdg_config_path("isde.toml");
    if (!path) return;

    /* Font */
    isde_config_write_string(path, "terminal", "font_family", cur_font_family);
    isde_config_write_int(path, "terminal", "font_size", cur_font_size);

    /* Colour scheme */
    IswListReturnStruct *sr = IswListShowCurrent(scheme_list);
    if (sr && sr->list_index >= 0 && sr->list_index < scheme_count) {
        snprintf(cur_color_scheme, sizeof(cur_color_scheme), "%s",
                 scheme_names_arr[sr->list_index]);
        isde_config_write_string(path, "terminal", "color_scheme",
                                 cur_color_scheme);
    }

    /* Cursor shape */
    sr = IswListShowCurrent(cursor_shape_list);
    if (sr && sr->list_index >= 0 && sr->list_index < NUM_CURSOR_SHAPES) {
        snprintf(cur_cursor_shape, sizeof(cur_cursor_shape), "%s",
                 cursor_shapes[sr->list_index]);
        isde_config_write_string(path, "terminal", "cursor_shape",
                                 cur_cursor_shape);
    }

    /* Scrollback */
    int sv = cur_scrollback;
    IswArgBuilder qb = IswArgBuilderInit();
    IswArgSpinValue(&qb, &sv);
    IswGetValues(scrollback_spin, qb.args, qb.count);
    cur_scrollback = sv;
    isde_config_write_int(path, "terminal", "scrollback", cur_scrollback);

    free(path);

    memcpy(saved_font_family, cur_font_family, sizeof(saved_font_family));
    saved_font_size = cur_font_size;
    memcpy(saved_color_scheme, cur_color_scheme, sizeof(saved_color_scheme));
    saved_scrollback = cur_scrollback;
    memcpy(saved_cursor_shape, cur_cursor_shape, sizeof(saved_cursor_shape));

    if (panel_dbus) {
        isde_dbus_settings_notify(panel_dbus, "terminal", "*");
    }
}

static void terminal_revert(void)
{
    memcpy(cur_font_family, saved_font_family, sizeof(cur_font_family));
    cur_font_size = saved_font_size;
    memcpy(cur_color_scheme, saved_color_scheme, sizeof(cur_color_scheme));
    cur_scrollback = saved_scrollback;
    memcpy(cur_cursor_shape, saved_cursor_shape, sizeof(cur_cursor_shape));
    update_font_label();
    IswListHighlight(scheme_list, find_idx(scheme_names_arr, scheme_count,
                                           cur_color_scheme));
    IswArgBuilder ab = IswArgBuilderInit();
    IswArgSpinValue(&ab, cur_scrollback);
    IswSetValues(scrollback_spin, ab.args, ab.count);
}

static int terminal_has_changes(void)
{
    if (strcmp(cur_font_family, saved_font_family) != 0) return 1;
    if (cur_font_size != saved_font_size) return 1;
    if (strcmp(cur_color_scheme, saved_color_scheme) != 0) return 1;
    if (strcmp(cur_cursor_shape, saved_cursor_shape) != 0) return 1;

    int sv = cur_scrollback;
    IswArgBuilder qb = IswArgBuilderInit();
    IswArgSpinValue(&qb, &sv);
    IswGetValues(scrollback_spin, qb.args, qb.count);
    if (sv != saved_scrollback) return 1;
    return 0;
}

static void terminal_destroy(void)
{
    isde_dialog_dismiss(chooser_shell);
    chooser_shell = NULL;
    font_desc_lbl = NULL;
    scheme_list = NULL;
    cursor_shape_list = NULL;
    scrollback_spin = NULL;
}

void panel_terminal_set_dbus(IsdeDBus *bus) { panel_dbus = bus; }

const IsdeSettingsPanel panel_terminal = {
    .name        = "Terminal",
    .icon        = NULL,
    .section     = "terminal",
    .create      = terminal_create,
    .apply       = terminal_apply,
    .revert      = terminal_revert,
    .has_changes = terminal_has_changes,
    .destroy     = terminal_destroy,
};
