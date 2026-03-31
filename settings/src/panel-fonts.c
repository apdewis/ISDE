#define _POSIX_C_SOURCE 200809L
/*
 * panel-fonts.c — Font settings panel
 *
 * Six font categories: General, Fixed, Small, Toolbar, Menus, Window Title.
 * Each row shows the category label, current font description, and an Edit
 * button that pops up a FontChooser dialog.
 *
 * Config stored in [fonts] section of isde.toml with keys:
 *   general_family, general_size, fixed_family, fixed_size, ...
 */
#include "settings.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <ISW/FontChooser.h>

/* ---------- font slot definitions ---------- */

#define NUM_FONTS 6

static const char *font_labels[NUM_FONTS] = {
    "General:",
    "Fixed Width:",
    "Small:",
    "Toolbar:",
    "Menus:",
    "Window Title:",
};

static const char *font_family_keys[NUM_FONTS] = {
    "general_family",
    "fixed_family",
    "small_family",
    "toolbar_family",
    "menu_family",
    "title_family",
};

static const char *font_size_keys[NUM_FONTS] = {
    "general_size",
    "fixed_size",
    "small_size",
    "toolbar_size",
    "menu_size",
    "title_size",
};

static const char *default_families[NUM_FONTS] = {
    "Sans",
    "Monospace",
    "Sans",
    "Sans",
    "Sans",
    "Sans",
};

static const int default_sizes[NUM_FONTS] = {
    10, 10, 8, 9, 10, 10,
};

/* ---------- state ---------- */

typedef struct {
    char family[128];
    int  size;
} FontSetting;

static FontSetting current[NUM_FONTS];
static FontSetting saved[NUM_FONTS];

static Widget desc_labels[NUM_FONTS];  /* label showing "Family 10pt" */
static Widget toplevel_cache;          /* for popup shell parent */

static IsdeDBus *panel_dbus;

/* ---------- helpers ---------- */

static void format_font_desc(char *buf, size_t bufsz,
                             const char *family, int size)
{
    snprintf(buf, bufsz, "%s %dpt", family, size);
}

static void update_desc_label(int idx)
{
    char buf[160];
    format_font_desc(buf, sizeof(buf), current[idx].family, current[idx].size);
    Arg args[20];
    XtSetArg(args[0], XtNlabel, buf);
    XtSetValues(desc_labels[idx], args, 1);
}

/* ---------- font chooser dialog ---------- */

static Widget chooser_shell;
static Widget chooser_widget;
static int    chooser_slot;  /* which font slot is being edited */

static void dismiss_chooser(void)
{
    if (chooser_shell) {
        XtPopdown(chooser_shell);
        XtDestroyWidget(chooser_shell);
        chooser_shell = NULL;
        chooser_widget = NULL;
    }
}

static void chooser_ok_cb(Widget w, XtPointer cd, XtPointer call)
{
    (void)w; (void)cd; (void)call;
    if (!chooser_widget) { return; }

    String family = IswFontChooserGetFamily(chooser_widget);
    int size = IswFontChooserGetSize(chooser_widget);

    if (family && family[0]) {
        snprintf(current[chooser_slot].family,
                 sizeof(current[chooser_slot].family), "%s", family);
    }
    if (size > 0) {
        current[chooser_slot].size = size;
    }

    update_desc_label(chooser_slot);
    dismiss_chooser();
}

static void chooser_cancel_cb(Widget w, XtPointer cd, XtPointer call)
{
    (void)w; (void)cd; (void)call;
    dismiss_chooser();
}

static void show_chooser(int slot)
{
    dismiss_chooser();

    chooser_slot = slot;

    Arg args[20];
    Cardinal n = 0;
    XtSetArg(args[n], XtNwidth, isde_scale(400));  n++;
    XtSetArg(args[n], XtNheight, isde_scale(350)); n++;
    XtSetArg(args[n], XtNborderWidth, 1);          n++;
    XtSetArg(args[n], XtNtitle, font_labels[slot]); n++;
    chooser_shell = XtCreatePopupShell("fontChooserShell",
                                        transientShellWidgetClass,
                                        toplevel_cache, args, n);

    /* Form to hold chooser + buttons */
    n = 0;
    XtSetArg(args[n], XtNdefaultDistance, isde_scale(8)); n++;
    XtSetArg(args[n], XtNborderWidth, 0);    n++;
    Widget form = XtCreateManagedWidget("fcForm", formWidgetClass,
                                        chooser_shell, args, n);

    /* FontChooser widget */
    n = 0;
    XtSetArg(args[n], XtNfontFamily, current[slot].family); n++;
    XtSetArg(args[n], XtNfontSize, current[slot].size);      n++;
    XtSetArg(args[n], XtNborderWidth, 0);                    n++;
    XtSetArg(args[n], XtNtop, XtChainTop);                   n++;
    XtSetArg(args[n], XtNbottom, XtChainBottom);             n++;
    XtSetArg(args[n], XtNleft, XtChainLeft);                 n++;
    XtSetArg(args[n], XtNright, XtChainRight);               n++;
    XtSetArg(args[n], XtNwidth, isde_scale(390));            n++;
    XtSetArg(args[n], XtNheight, isde_scale(290));           n++;
    chooser_widget = XtCreateManagedWidget("fontChooser",
                                            fontChooserWidgetClass,
                                            form, args, n);

    /* OK / Cancel buttons — bottom-right */
    int btn_w = isde_scale(80);
    int btn_pad = isde_scale(8);

    n = 0;
    XtSetArg(args[n], XtNlabel, "OK");              n++;
    XtSetArg(args[n], XtNfromVert, chooser_widget);  n++;
    XtSetArg(args[n], XtNborderWidth, 0);            n++;
    XtSetArg(args[n], XtNwidth, btn_w);              n++;
    XtSetArg(args[n], XtNinternalWidth, btn_pad);    n++;
    XtSetArg(args[n], XtNinternalHeight, btn_pad);   n++;
    XtSetArg(args[n], XtNleft, XtChainRight);        n++;
    XtSetArg(args[n], XtNright, XtChainRight);       n++;
    XtSetArg(args[n], XtNbottom, XtChainBottom);     n++;
    XtSetArg(args[n], XtNtop, XtChainBottom);        n++;
    XtSetArg(args[n], XtNhorizDistance, isde_scale(390) - btn_w * 2 - btn_pad); n++;
    Widget ok = XtCreateManagedWidget("fcOk", commandWidgetClass,
                                      form, args, n);
    XtAddCallback(ok, XtNcallback, chooser_ok_cb, NULL);

    n = 0;
    XtSetArg(args[n], XtNlabel, "Cancel");           n++;
    XtSetArg(args[n], XtNfromVert, chooser_widget);  n++;
    XtSetArg(args[n], XtNfromHoriz, ok);             n++;
    XtSetArg(args[n], XtNhorizDistance, btn_pad);    n++;
    XtSetArg(args[n], XtNborderWidth, 0);            n++;
    XtSetArg(args[n], XtNwidth, btn_w);              n++;
    XtSetArg(args[n], XtNinternalWidth, btn_pad);    n++;
    XtSetArg(args[n], XtNinternalHeight, btn_pad);   n++;
    XtSetArg(args[n], XtNleft, XtChainRight);        n++;
    XtSetArg(args[n], XtNright, XtChainRight);       n++;
    XtSetArg(args[n], XtNbottom, XtChainBottom);     n++;
    XtSetArg(args[n], XtNtop, XtChainBottom);        n++;
    Widget cancel = XtCreateManagedWidget("fcCancel", commandWidgetClass,
                                          form, args, n);
    XtAddCallback(cancel, XtNcallback, chooser_cancel_cb, NULL);

    XtPopup(chooser_shell, XtGrabExclusive);
}

/* ---------- edit button callbacks ---------- */

static void edit_cb(Widget w, XtPointer cd, XtPointer call)
{
    (void)w; (void)call;
    int slot = (int)(intptr_t)cd;
    show_chooser(slot);
}

/* ---------- panel lifecycle ---------- */

static Widget fonts_create(Widget parent, XtAppContext app)
{
    (void)app;

    /* Find toplevel for popup shells */
    toplevel_cache = parent;
    while (toplevel_cache && !XtIsShell(toplevel_cache)) {
        toplevel_cache = XtParent(toplevel_cache);
    }

    Arg args[20];
    Cardinal n;

    n = 0;
    XtSetArg(args[n], XtNdefaultDistance, isde_scale(8)); n++;
    XtSetArg(args[n], XtNborderWidth, 0);    n++;
    Widget form = XtCreateWidget("fontsForm", formWidgetClass,
                                 parent, args, n);

    /* Load current config */
    for (int i = 0; i < NUM_FONTS; i++) {
        snprintf(current[i].family, sizeof(current[i].family),
                 "%s", default_families[i]);
        current[i].size = default_sizes[i];
    }

    char errbuf[256];
    IsdeConfig *cfg = isde_config_load_xdg("isde.toml", errbuf, sizeof(errbuf));
    if (cfg) {
        IsdeConfigTable *root = isde_config_root(cfg);
        IsdeConfigTable *fonts = isde_config_table(root, "fonts");
        if (fonts) {
            for (int i = 0; i < NUM_FONTS; i++) {
                const char *fam = isde_config_string(fonts,
                    font_family_keys[i], NULL);
                if (fam) {
                    snprintf(current[i].family, sizeof(current[i].family),
                             "%s", fam);
                }
                int sz = (int)isde_config_int(fonts,
                    font_size_keys[i], 0);
                if (sz > 0) {
                    current[i].size = sz;
                }
            }
        }
        isde_config_free(cfg);
    }

    memcpy(saved, current, sizeof(saved));

    /* Build rows */
    Widget prev = NULL;

    for (int i = 0; i < NUM_FONTS; i++) {
        /* Category label */
        n = 0;
        XtSetArg(args[n], XtNlabel, font_labels[i]);  n++;
        XtSetArg(args[n], XtNborderWidth, 0);          n++;
        XtSetArg(args[n], XtNwidth, isde_scale(100));  n++;
        XtSetArg(args[n], XtNjustify, XtJustifyLeft);  n++;
        if (prev) { XtSetArg(args[n], XtNfromVert, prev); n++; }
        Widget lbl = XtCreateManagedWidget("fontCatLbl", labelWidgetClass,
                                            form, args, n);

        /* Font description label */
        char desc[160];
        format_font_desc(desc, sizeof(desc),
                         current[i].family, current[i].size);
        n = 0;
        XtSetArg(args[n], XtNlabel, desc);             n++;
        XtSetArg(args[n], XtNborderWidth, 0);           n++;
        XtSetArg(args[n], XtNwidth, isde_scale(180));   n++;
        XtSetArg(args[n], XtNjustify, XtJustifyLeft);   n++;
        XtSetArg(args[n], XtNfromHoriz, lbl);           n++;
        if (prev) { XtSetArg(args[n], XtNfromVert, prev); n++; }
        desc_labels[i] = XtCreateManagedWidget("fontDescLbl", labelWidgetClass,
                                                form, args, n);

        /* Edit button */
        n = 0;
        XtSetArg(args[n], XtNlabel, "Edit...");          n++;
        XtSetArg(args[n], XtNborderWidth, 0);             n++;
        XtSetArg(args[n], XtNfromHoriz, desc_labels[i]);  n++;
        if (prev) { XtSetArg(args[n], XtNfromVert, prev); n++; }
        Widget btn = XtCreateManagedWidget("fontEditBtn", commandWidgetClass,
                                            form, args, n);
        XtAddCallback(btn, XtNcallback, edit_cb, (XtPointer)(intptr_t)i);

        prev = lbl;
    }

    return form;
}

static void fonts_apply(void)
{
    char *path = isde_xdg_config_path("isde.toml");
    if (!path) { return; }

    for (int i = 0; i < NUM_FONTS; i++) {
        isde_config_write_string(path, "fonts",
                                 font_family_keys[i], current[i].family);
        isde_config_write_int(path, "fonts",
                              font_size_keys[i], current[i].size);
    }

    free(path);
    memcpy(saved, current, sizeof(saved));

    if (panel_dbus) {
        isde_dbus_settings_notify(panel_dbus, "fonts", "*");
    }
}

static void fonts_revert(void)
{
    memcpy(current, saved, sizeof(current));
    for (int i = 0; i < NUM_FONTS; i++) {
        update_desc_label(i);
    }
}

static int fonts_has_changes(void)
{
    return memcmp(current, saved, sizeof(current)) != 0;
}

static void fonts_destroy(void)
{
    dismiss_chooser();
    for (int i = 0; i < NUM_FONTS; i++) {
        desc_labels[i] = NULL;
    }
}

void panel_fonts_set_dbus(IsdeDBus *bus) { panel_dbus = bus; }

const IsdeSettingsPanel panel_fonts = {
    .name        = "Fonts",
    .icon        = NULL,
    .section     = "fonts",
    .create      = fonts_create,
    .apply       = fonts_apply,
    .revert      = fonts_revert,
    .has_changes = fonts_has_changes,
    .destroy     = fonts_destroy,
};
