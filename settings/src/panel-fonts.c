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
#include "isde/isde-dialog.h"

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
    IswSetArg(args[0], IswNlabel, buf);
    IswSetValues(desc_labels[idx], args, 1);
}

/* ---------- font chooser dialog ---------- */

static Widget chooser_shell;
static int    chooser_slot;  /* which font slot is being edited */

static void chooser_result_cb(IsdeDialogResult result,
                              const char *family, int size, void *data)
{
    (void)data;
    chooser_shell = NULL;
    if (result != ISDE_DIALOG_OK)
        return;

    if (family && family[0]) {
        snprintf(current[chooser_slot].family,
                 sizeof(current[chooser_slot].family), "%s", family);
    }
    if (size > 0) {
        current[chooser_slot].size = size;
    }
    update_desc_label(chooser_slot);
}

static void show_chooser(int slot)
{
    isde_dialog_dismiss(chooser_shell);
    chooser_slot = slot;
    chooser_shell = isde_dialog_font(toplevel_cache, font_labels[slot],
                                     current[slot].family,
                                     current[slot].size,
                                     chooser_result_cb, NULL);
}

/* ---------- edit button callbacks ---------- */

static void edit_cb(Widget w, IswPointer cd, IswPointer call)
{
    (void)w; (void)call;
    int slot = (int)(intptr_t)cd;
    show_chooser(slot);
}

/* ---------- panel lifecycle ---------- */

static Widget fonts_create(Widget parent, IswAppContext app)
{
    (void)app;

    /* Find toplevel for popup shells */
    toplevel_cache = parent;
    while (toplevel_cache && !IswIsShell(toplevel_cache)) {
        toplevel_cache = IswParent(toplevel_cache);
    }

    Arg args[20];
    Cardinal n;

    Dimension pw;
    Arg qa[20];
    IswSetArg(qa[0], IswNwidth, &pw);
    IswGetValues(parent, qa, 1);

    int cat_w = 100;
    int btn_w = 60;
    int desc_w = (pw > 0 ? (int)pw - cat_w - btn_w - 8 * 4 : 180);
    if (desc_w < 80) { desc_w = 80; }

    n = 0;
    IswSetArg(args[n], IswNdefaultDistance, 8); n++;
    IswSetArg(args[n], IswNborderWidth, 0);    n++;
    Widget form = IswCreateWidget("fontsForm", formWidgetClass,
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
        IswSetArg(args[n], IswNlabel, font_labels[i]);  n++;
        IswSetArg(args[n], IswNborderWidth, 0);          n++;
        IswSetArg(args[n], IswNwidth, 100);              n++;
        IswSetArg(args[n], IswNjustify, IswJustifyRight); n++;
        IswSetArg(args[n], IswNleft, IswChainLeft);       n++;
        IswSetArg(args[n], IswNright, IswChainLeft);      n++;
        if (prev) { IswSetArg(args[n], IswNfromVert, prev); n++; }
        Widget lbl = IswCreateManagedWidget("fontCatLbl", labelWidgetClass,
                                            form, args, n);

        /* Font description label */
        char desc[160];
        format_font_desc(desc, sizeof(desc),
                         current[i].family, current[i].size);
        n = 0;
        IswSetArg(args[n], IswNlabel, desc);             n++;
        IswSetArg(args[n], IswNborderWidth, 0);           n++;
        IswSetArg(args[n], IswNwidth, desc_w);              n++;
        IswSetArg(args[n], IswNjustify, IswJustifyLeft);   n++;
        IswSetArg(args[n], IswNfromHoriz, lbl);           n++;
        IswSetArg(args[n], IswNresizable, True);          n++;
        IswSetArg(args[n], IswNleft, IswChainLeft);        n++;
        IswSetArg(args[n], IswNright, IswChainLeft);       n++;
        if (prev) { IswSetArg(args[n], IswNfromVert, prev); n++; }
        desc_labels[i] = IswCreateManagedWidget("fontDescLbl", labelWidgetClass,
                                                form, args, n);

        /* Edit button */
        n = 0;
        IswSetArg(args[n], IswNlabel, "Edit...");          n++;
        IswSetArg(args[n], IswNborderWidth, 0);             n++;
        IswSetArg(args[n], IswNfromHoriz, desc_labels[i]);  n++;
        IswSetArg(args[n], IswNresizable, True);            n++;
        IswSetArg(args[n], IswNleft, IswChainRight);         n++;
        IswSetArg(args[n], IswNright, IswChainRight);        n++;
        if (prev) { IswSetArg(args[n], IswNfromVert, prev); n++; }
        Widget btn = IswCreateManagedWidget("fontEditBtn", commandWidgetClass,
                                            form, args, n);
        IswAddCallback(btn, IswNcallback, edit_cb, (IswPointer)(intptr_t)i);

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
    isde_dialog_dismiss(chooser_shell);
    chooser_shell = NULL;
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
