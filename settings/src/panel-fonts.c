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
#include <ISW/IswArgMacros.h>
#include <fontconfig/fontconfig.h>
#include "isde-dialog.h"

/* ---------- font slot definitions ---------- */

#define NUM_FONTS 6
#define SELECTED_W 300
#define LABEL_W 250
#define SLIDER_W 300
#define BUTTON_W 60


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

static const char *font_weight_keys[NUM_FONTS] = {
    "general_weight",
    "fixed_weight",
    "small_weight",
    "toolbar_weight",
    "menu_weight",
    "title_weight",
};

static const char *font_slant_keys[NUM_FONTS] = {
    "general_slant",
    "fixed_slant",
    "small_slant",
    "toolbar_slant",
    "menu_slant",
    "title_slant",
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
    int  weight;
    int  slant;
} FontSetting;

static FontSetting current[NUM_FONTS];
static FontSetting saved[NUM_FONTS];

static Widget desc_labels[NUM_FONTS];  /* label showing "Family 10pt" */
static Widget toplevel_cache;          /* for popup shell parent */

static IsdeDBus *panel_dbus;

/* ---------- helpers ---------- */

static void fc_style_name(const char *family, int weight, int slant,
                          char *out, size_t outsz)
{
    FcPattern *pat = FcPatternCreate();
    FcPatternAddString(pat, FC_FAMILY, (const FcChar8 *)family);
    FcPatternAddInteger(pat, FC_WEIGHT, weight);
    FcPatternAddInteger(pat, FC_SLANT, slant);
    FcConfigSubstitute(NULL, pat, FcMatchPattern);
    FcDefaultSubstitute(pat);
    FcResult result;
    FcPattern *match = FcFontMatch(NULL, pat, &result);
    FcChar8 *style = NULL;
    if (match && FcPatternGetString(match, FC_STYLE, 0, &style) == FcResultMatch
        && style) {
        snprintf(out, outsz, "%s", (const char *)style);
    } else {
        snprintf(out, outsz, "Regular");
    }
    if (match) FcPatternDestroy(match);
    FcPatternDestroy(pat);
}

static void format_font_desc(char *buf, size_t bufsz,
                             const char *family, int size,
                             int weight, int slant)
{
    char style[64];
    fc_style_name(family, weight, slant, style, sizeof(style));
    snprintf(buf, bufsz, "%s %s %dpt", family, style, size);
}

static void format_fc_pattern(char *out, size_t outsz,
                              const char *family, int size,
                              int weight, int slant)
{
    int n = snprintf(out, outsz, "%s-%d", family, size);
    if (weight != FC_WEIGHT_REGULAR) {
        n += snprintf(out + n, outsz - n, ":weight=%d", weight);
    }
    if (slant != FC_SLANT_ROMAN) {
        snprintf(out + n, outsz - n, ":slant=%d", slant);
    }
}

static void update_desc_label(int idx)
{
    char buf[160];
    char font[160];
    format_font_desc(buf, sizeof(buf), current[idx].family,
                     current[idx].size, current[idx].weight,
                     current[idx].slant);
    format_fc_pattern(font, sizeof(font), current[idx].family,
                      current[idx].size, current[idx].weight,
                      current[idx].slant);
    IswFontStruct *fs = isde_resolve_font(desc_labels[idx], font);
    IswArgBuilder ab = IswArgBuilderInit();
    IswArgResize(&ab, True);
    IswArgWidth(&ab, SELECTED_W);
    IswArgLabel(&ab, buf);
    if (fs) { IswArgFont(&ab, fs); }
    IswSetValues(desc_labels[idx], ab.args, ab.count);
}

/* ---------- font chooser dialog ---------- */

static Widget chooser_shell;
static int    chooser_slot;  /* which font slot is being edited */

static void chooser_result_cb(IsdeDialogResult result,
                              const char *family, int size,
                              int weight, int slant, void *data)
{
    (void)data;
    chooser_shell = NULL;
    if (result != ISDE_DIALOG_OK)
        return;

    if (family && family[0] && family != current[chooser_slot].family) {
        snprintf(current[chooser_slot].family,
                 sizeof(current[chooser_slot].family), "%s", family);
    }
    if (size > 0) {
        current[chooser_slot].size = size;
    }
    current[chooser_slot].weight = weight;
    current[chooser_slot].slant = slant;
    update_desc_label(chooser_slot);
}

static void show_chooser(int slot)
{
    isde_dialog_dismiss(chooser_shell);
    chooser_slot = slot;
    chooser_shell = isde_dialog_font(toplevel_cache, font_labels[slot],
                                     current[slot].family,
                                     current[slot].size,
                                     current[slot].weight,
                                     current[slot].slant,
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

    Dimension pw;
    IswArgBuilder qb = IswArgBuilderInit();
    IswArgWidth(&qb, &pw);
    IswGetValues(parent, qb.args, qb.count);

    IswArgBuilder ab = IswArgBuilderInit();
    IswArgDefaultDistance(&ab, 8);
    IswArgBorderWidth(&ab, 0);
    Widget form = IswCreateWidget("fontsForm", formWidgetClass,
                                 parent, ab.args, ab.count);

    /* Load current config */
    for (int i = 0; i < NUM_FONTS; i++) {
        snprintf(current[i].family, sizeof(current[i].family),
                 "%s", default_families[i]);
        current[i].size = default_sizes[i];
        current[i].weight = FC_WEIGHT_REGULAR;
        current[i].slant = FC_SLANT_ROMAN;
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
                int wt = (int)isde_config_int(fonts,
                    font_weight_keys[i], -1);
                if (wt >= 0) {
                    current[i].weight = wt;
                }
                int sl = (int)isde_config_int(fonts,
                    font_slant_keys[i], -1);
                if (sl >= 0) {
                    current[i].slant = sl;
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
        IswArgBuilderReset(&ab);
        IswArgLabel(&ab, font_labels[i]);
        IswArgBorderWidth(&ab, 0);
        IswArgWidth(&ab, LABEL_W);
        IswArgJustify(&ab, IswJustifyRight);
        IswArgLeft(&ab, IswChainLeft);
        IswArgRight(&ab, IswChainLeft);
        if (prev) { IswArgFromVert(&ab, prev); }
        Widget lbl = IswCreateManagedWidget("fontCatLbl", labelWidgetClass,
                                            form, ab.args, ab.count);

        /* Font description label — rendered in the configured font */
        char desc[160];
        char font[160];
        format_font_desc(desc, sizeof(desc),
                         current[i].family, current[i].size,
                         current[i].weight, current[i].slant);
        format_fc_pattern(font, sizeof(font), current[i].family,
                          current[i].size, current[i].weight,
                          current[i].slant);
        IswFontStruct *fs = isde_resolve_font(form, font);
        IswArgBuilderReset(&ab);
        IswArgLabel(&ab, desc);
        IswArgBorderWidth(&ab, 0);
        IswArgWidth(&ab, SELECTED_W);
        IswArgJustify(&ab, IswJustifyLeft);
        IswArgFromHoriz(&ab, lbl);
        IswArgResize(&ab, True);
        IswArgResizable(&ab, True);
        IswArgLeft(&ab, IswChainLeft);
        IswArgRight(&ab, IswChainLeft);
        if (prev) { IswArgFromVert(&ab, prev); }
        if (fs) { IswArgFont(&ab, fs); }
        desc_labels[i] = IswCreateManagedWidget("fontDescLbl",
                                                labelWidgetClass,
                                                form, ab.args, ab.count);

        /* Edit button */
        IswArgBuilderReset(&ab);
        IswArgLabel(&ab, "Edit...");
        IswArgWidth(&ab, BUTTON_W);
        IswArgFromHoriz(&ab, desc_labels[i]);
        IswArgLeft(&ab, IswChainLeft);
        if (prev) { IswArgFromVert(&ab, prev); }
        Widget btn = IswCreateManagedWidget("fontEditBtn", commandWidgetClass,
                                            form, ab.args, ab.count);
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
        isde_config_write_int(path, "fonts",
                              font_weight_keys[i], current[i].weight);
        isde_config_write_int(path, "fonts",
                              font_slant_keys[i], current[i].slant);
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
