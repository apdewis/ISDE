#define _POSIX_C_SOURCE 200809L
/*
 * panel-desktops.c — Virtual desktop settings: grid rows/columns and
 *                    desktop background (solid colour or image)
 */
#include "settings.h"
#include <ISW/Slider.h>
#include <ISW/Toggle.h>
#include <ISW/ColorPicker.h>
#include <ISW/Text.h>
#include <ISW/FileChooser.h>
#include <ISW/Command.h>
#include <ISW/IswArgMacros.h>

#include <stdlib.h>
#include <string.h>
#include <stdio.h>

static Widget scale_rows;
static Widget scale_cols;

static int saved_rows;
static int saved_cols;

/* Background state */
static Widget radio_solid;
static Widget radio_image;
static Widget color_picker;
static Widget color_picker_row;
static Widget image_path_label;
static Widget image_browse_btn;
static Widget image_path_row;

static int saved_bg_mode;        /* 0 = solid, 1 = image */
static int saved_bg_r, saved_bg_g, saved_bg_b;
static char saved_bg_image[512];

static IsdeDBus *panel_dbus;

#define SLIDER_W 300
#define LABEL_W 150

/* ---------- helpers ---------- */

static int current_bg_mode(void)
{
    IswArgBuilder ab = IswArgBuilderInit();
    Boolean state = False;
    IswArgState(&ab, &state);
    IswGetValues(radio_image, ab.args, ab.count);
    return state ? 1 : 0;
}

static const char *get_path_label(void)
{
    String str = NULL;
    IswArgBuilder ab = IswArgBuilderInit();
    IswArgLabel(&ab, &str);
    IswGetValues(image_path_label, ab.args, ab.count);
    return str ? str : "";
}

static void set_path_label(const char *str)
{
    IswArgBuilder ab = IswArgBuilderInit();
    IswArgLabel(&ab, str);
    IswSetValues(image_path_label, ab.args, ab.count);
}

/* ---------- file chooser dialog ---------- */

static Widget fc_shell;

static void fc_dismiss(void)
{
    if (fc_shell) {
        IswPopdown(fc_shell);
        IswDestroyWidget(fc_shell);
        fc_shell = NULL;
    }
}

static void fc_selected_cb(Widget w, IswPointer cd, IswPointer call)
{
    (void)w; (void)cd;
    IswFileChooserCallbackData *cb = (IswFileChooserCallbackData *)call;
    if (cb && cb->path)
        set_path_label(cb->path);
    fc_dismiss();
}

static void fc_cancel_cb(Widget w, IswPointer cd, IswPointer call)
{
    (void)w; (void)cd; (void)call;
    fc_dismiss();
}

static void browse_cb(Widget w, IswPointer cd, IswPointer call)
{
    (void)w; (void)cd; (void)call;

    if (fc_shell)
        return;

    Widget parent = image_browse_btn;
    IswArgBuilder ab = IswArgBuilderInit();
    IswArgTitle(&ab, "Select Background Image");
    IswArgWidth(&ab, 500);
    IswArgHeight(&ab, 400);
    fc_shell = IswCreatePopupShell("fileChooserShell", transientShellWidgetClass,
                                   parent, ab.args, ab.count);

    static IswFileFilter filters[] = {
        { "Image Files", "*.png" },
        { "All Files",   "*" },
    };

    IswArgBuilderReset(&ab);
    ISW_ARG(&ab, IswNfileFilters, filters);
    ISW_ARG(&ab, IswNnumFileFilters, IswNumber(filters));
    Widget fc = IswCreateManagedWidget("fileChooser", fileChooserWidgetClass,
                                      fc_shell, ab.args, ab.count);

    IswAddCallback(fc, IswNfileSelected, fc_selected_cb, NULL);
    IswAddCallback(fc, IswNfileCancelled, fc_cancel_cb, NULL);

    IswPopup(fc_shell, IswGrabExclusive);
}

static void update_bg_visibility(void)
{
    int mode = current_bg_mode();
    if (mode == 0) {
        IswManageChild(color_picker_row);
        IswUnmanageChild(image_path_row);
    } else {
        IswUnmanageChild(color_picker_row);
        IswManageChild(image_path_row);
    }
}

static void bg_mode_changed_cb(Widget w, IswPointer cd, IswPointer call)
{
    (void)w; (void)cd; (void)call;
    update_bg_visibility();
}

/* ---------- apply / revert ---------- */

static void desktops_apply(void)
{
    int rows = IswSliderGetValue(scale_rows);
    int cols = IswSliderGetValue(scale_cols);

    char *path = isde_xdg_config_path("isde.toml");
    if (path) {
        isde_config_write_int(path, "wm.desktops", "rows", rows);
        isde_config_write_int(path, "wm.desktops", "columns", cols);

        int mode = current_bg_mode();
        isde_config_write_string(path, "desktop.background", "mode",
                                 mode == 0 ? "solid" : "image");

        int r, g, b;
        IswColorPickerGetColor(color_picker, &r, &g, &b);
        char hex[8];
        snprintf(hex, sizeof(hex), "#%02x%02x%02x", r, g, b);
        isde_config_write_string(path, "desktop.background", "color", hex);

        const char *img = get_path_label();
        isde_config_write_string(path, "desktop.background", "image",
                                 img ? img : "");

        free(path);

        saved_bg_mode = mode;
        saved_bg_r = r;
        saved_bg_g = g;
        saved_bg_b = b;
        snprintf(saved_bg_image, sizeof(saved_bg_image), "%s",
                 img ? img : "");
    }

    saved_rows = rows;
    saved_cols = cols;

    if (panel_dbus) {
        isde_dbus_settings_notify(panel_dbus, "wm.desktops", "*");
        isde_dbus_settings_notify(panel_dbus, "desktop.background", "*");
    }
}

static void desktops_revert(void)
{
    IswSliderSetValue(scale_rows, saved_rows);
    IswSliderSetValue(scale_cols, saved_cols);

    /* Revert background */
    IswColorPickerSetColor(color_picker, saved_bg_r, saved_bg_g, saved_bg_b);
    set_path_label(saved_bg_image);

    IswArgBuilder ab = IswArgBuilderInit();
    IswArgState(&ab, saved_bg_mode == 0 ? True : False);
    IswSetValues(radio_solid, ab.args, ab.count);

    IswArgBuilderReset(&ab);
    IswArgState(&ab, saved_bg_mode == 1 ? True : False);
    IswSetValues(radio_image, ab.args, ab.count);

    update_bg_visibility();
}

/* ---------- UI construction ---------- */

static void make_scale_row(Widget vbox, const char *label_text,
                           int min, int max, int value, Widget *out_scale)
{
    IswArgBuilder ab = IswArgBuilderInit();
    IswArgOrientation(&ab, IswOrientHorizontal);
    IswArgBorderWidth(&ab, 0);
    IswArgSpacing(&ab, 8);
    Widget row = IswCreateManagedWidget("row", flexBoxWidgetClass,
                                       vbox, ab.args, ab.count);

    IswArgBuilderReset(&ab);
    IswArgLabel(&ab, label_text);
    IswArgBorderWidth(&ab, 0);
    IswArgJustify(&ab, IswJustifyRight);
    IswArgFlexBasis(&ab, LABEL_W);
    IswArgFlexAlign(&ab, IswFlexAlignCenter);
    IswCreateManagedWidget("lbl", labelWidgetClass, row, ab.args, ab.count);

    IswArgBuilderReset(&ab);
    IswArgMinimumValue(&ab, min);
    IswArgMaximumValue(&ab, max);
    IswArgSliderValue(&ab, value);
    IswArgOrientation(&ab, IswOrientHorizontal);
    IswArgShowValue(&ab, True);
    IswArgTickInterval(&ab, 1);
    IswArgWidth(&ab, SLIDER_W);
    IswArgBorderWidth(&ab, 0);
    IswArgFlexAlign(&ab, IswFlexAlignCenter);
    *out_scale = IswCreateManagedWidget("slider", sliderWidgetClass,
                                       row, ab.args, ab.count);

    IswArgBuilderReset(&ab);
    IswArgLabel(&ab, "");
    IswArgBorderWidth(&ab, 0);
    IswArgFlexBasis(&ab, 20);
    IswCreateManagedWidget("spacer", labelWidgetClass, row, ab.args, ab.count);
}

static uint32_t parse_hex_color(const char *s)
{
    if (!s) return 0x1a1a2e;
    if (*s == '#') s++;
    char *end;
    unsigned long v = strtoul(s, &end, 16);
    if (end == s) return 0x1a1a2e;
    return (uint32_t)(v & 0xFFFFFF);
}

static Widget desktops_create(Widget parent, IswAppContext app)
{
    (void)app;

    /* Load current values */
    saved_rows = 1;
    saved_cols = 2;
    saved_bg_mode = 0;
    saved_bg_r = 0x1a; saved_bg_g = 0x1a; saved_bg_b = 0x2e;
    saved_bg_image[0] = '\0';

    char errbuf[256];
    IsdeConfig *cfg = isde_config_load_xdg("isde.toml", errbuf, sizeof(errbuf));
    if (cfg) {
        IsdeConfigTable *root = isde_config_root(cfg);
        IsdeConfigTable *wm_tbl = isde_config_table(root, "wm");
        IsdeConfigTable *desk = wm_tbl ? isde_config_table(wm_tbl, "desktops") : NULL;
        if (desk) {
            int r = (int)isde_config_int(desk, "rows", 1);
            int c = (int)isde_config_int(desk, "columns", 2);
            if (r > 0) { saved_rows = r; }
            if (c > 0) { saved_cols = c; }
        }

        IsdeConfigTable *dt = isde_config_table(root, "desktop");
        IsdeConfigTable *bg = dt ? isde_config_table(dt, "background") : NULL;
        if (bg) {
            const char *mode = isde_config_string(bg, "mode", "solid");
            saved_bg_mode = (strcmp(mode, "image") == 0) ? 1 : 0;

            uint32_t color = parse_hex_color(
                isde_config_string(bg, "color", "#1a1a2e"));
            saved_bg_r = (color >> 16) & 0xFF;
            saved_bg_g = (color >> 8) & 0xFF;
            saved_bg_b = color & 0xFF;

            const char *img = isde_config_string(bg, "image", "");
            snprintf(saved_bg_image, sizeof(saved_bg_image), "%s", img);
        }

        isde_config_free(cfg);
    }

    IswArgBuilder ab = IswArgBuilderInit();
    IswArgOrientation(&ab, IswOrientVertical);
    IswArgSpacing(&ab, 8);
    IswArgBorderWidth(&ab, 0);
    Widget vbox = IswCreateWidget("desktopsPanel", flexBoxWidgetClass,
                                 parent, ab.args, ab.count);

    make_scale_row(vbox, "Desktop rows:",
                   1, 4, saved_rows, &scale_rows);
    make_scale_row(vbox, "Desktop columns:",
                   1, 4, saved_cols, &scale_cols);

    /* --- Separator --- */
    IswArgBuilderReset(&ab);
    IswArgLabel(&ab, "Background");
    IswArgBorderWidth(&ab, 0);
    IswArgJustify(&ab, IswJustifyLeft);
    IswArgFlexAlign(&ab, IswFlexAlignCenter);
    IswCreateManagedWidget("bgHeader", labelWidgetClass,
                           vbox, ab.args, ab.count);

    /* --- Mode radio buttons --- */
    IswArgBuilderReset(&ab);
    IswArgOrientation(&ab, IswOrientHorizontal);
    IswArgBorderWidth(&ab, 0);
    IswArgSpacing(&ab, 8);
    Widget mode_row = IswCreateManagedWidget("modeRow", flexBoxWidgetClass,
                                            vbox, ab.args, ab.count);

    IswArgBuilderReset(&ab);
    IswArgLabel(&ab, "Background type:");
    IswArgBorderWidth(&ab, 0);
    IswArgJustify(&ab, IswJustifyRight);
    IswArgFlexBasis(&ab, LABEL_W);
    IswArgFlexAlign(&ab, IswFlexAlignCenter);
    IswCreateManagedWidget("modeLbl", labelWidgetClass,
                           mode_row, ab.args, ab.count);

    IswArgBuilderReset(&ab);
    IswArgLabel(&ab, "Solid Colour");
    IswArgBorderWidth(&ab, 1);
    IswArgToggleShape(&ab, IswToggleShapeRadio);
    if (saved_bg_mode == 0) IswArgState(&ab, True);
    IswArgFlexAlign(&ab, IswFlexAlignCenter);
    radio_solid = IswCreateManagedWidget("radioSolid", toggleWidgetClass,
                                        mode_row, ab.args, ab.count);

    IswArgBuilderReset(&ab);
    IswArgLabel(&ab, "Image");
    IswArgBorderWidth(&ab, 1);
    IswArgToggleShape(&ab, IswToggleShapeRadio);
    IswArgRadioGroup(&ab, radio_solid);
    if (saved_bg_mode == 1) IswArgState(&ab, True);
    IswArgFlexAlign(&ab, IswFlexAlignCenter);
    radio_image = IswCreateManagedWidget("radioImage", toggleWidgetClass,
                                        mode_row, ab.args, ab.count);

    IswAddCallback(radio_solid, IswNcallback, bg_mode_changed_cb, NULL);
    IswAddCallback(radio_image, IswNcallback, bg_mode_changed_cb, NULL);

    /* --- Colour picker row --- */
    IswArgBuilderReset(&ab);
    IswArgOrientation(&ab, IswOrientHorizontal);
    IswArgBorderWidth(&ab, 0);
    IswArgSpacing(&ab, 8);
    color_picker_row = IswCreateWidget("colorRow", flexBoxWidgetClass,
                                       vbox, ab.args, ab.count);

    IswArgBuilderReset(&ab);
    IswArgLabel(&ab, "Colour:");
    IswArgBorderWidth(&ab, 0);
    IswArgJustify(&ab, IswJustifyRight);
    IswArgFlexBasis(&ab, LABEL_W);
    IswArgFlexAlign(&ab, IswFlexAlignCenter);
    IswCreateManagedWidget("colorLbl", labelWidgetClass,
                           color_picker_row, ab.args, ab.count);

    IswArgBuilderReset(&ab);
    ab.args[ab.count].name = IswNcolorRed;
    ab.args[ab.count].value = (IswArgVal)saved_bg_r;
    ab.count++;
    ab.args[ab.count].name = IswNcolorGreen;
    ab.args[ab.count].value = (IswArgVal)saved_bg_g;
    ab.count++;
    ab.args[ab.count].name = IswNcolorBlue;
    ab.args[ab.count].value = (IswArgVal)saved_bg_b;
    ab.count++;
    IswArgFlexAlign(&ab, IswFlexAlignCenter);
    color_picker = IswCreateManagedWidget("colorPicker",
                                          colorPickerWidgetClass,
                                          color_picker_row,
                                          ab.args, ab.count);

    /* --- Image path row --- */
    IswArgBuilderReset(&ab);
    IswArgOrientation(&ab, IswOrientHorizontal);
    IswArgBorderWidth(&ab, 0);
    IswArgSpacing(&ab, 8);
    image_path_row = IswCreateWidget("imageRow", flexBoxWidgetClass,
                                     vbox, ab.args, ab.count);

    IswArgBuilderReset(&ab);
    IswArgLabel(&ab, "Image path:");
    IswArgBorderWidth(&ab, 0);
    IswArgJustify(&ab, IswJustifyRight);
    IswArgFlexBasis(&ab, LABEL_W);
    IswArgFlexAlign(&ab, IswFlexAlignCenter);
    IswCreateManagedWidget("imgLbl", labelWidgetClass,
                           image_path_row, ab.args, ab.count);

    IswArgBuilderReset(&ab);
    IswArgLabel(&ab, saved_bg_image);
    IswArgBorderWidth(&ab, 0);
    IswArgJustify(&ab, IswJustifyLeft);
    IswArgFlexBasis(&ab, SLIDER_W - 80);
    IswArgFlexAlign(&ab, IswFlexAlignCenter);
    image_path_label = IswCreateManagedWidget("imgPath", labelWidgetClass,
                                              image_path_row,
                                              ab.args, ab.count);

    IswArgBuilderReset(&ab);
    IswArgLabel(&ab, "Browse...");
    IswArgBorderWidth(&ab, 1);
    IswArgFlexAlign(&ab, IswFlexAlignCenter);
    image_browse_btn = IswCreateManagedWidget("imgBrowse", commandWidgetClass,
                                              image_path_row,
                                              ab.args, ab.count);
    IswAddCallback(image_browse_btn, IswNcallback, browse_cb, NULL);

    /* Show/hide rows based on current mode */
    if (saved_bg_mode == 0) {
        IswManageChild(color_picker_row);
    } else {
        IswManageChild(image_path_row);
    }

    return vbox;
}

static int desktops_has_changes(void)
{
    if (!scale_rows) { return 0; }
    if (IswSliderGetValue(scale_rows) != saved_rows ||
        IswSliderGetValue(scale_cols) != saved_cols)
        return 1;

    if (current_bg_mode() != saved_bg_mode)
        return 1;

    int r, g, b;
    IswColorPickerGetColor(color_picker, &r, &g, &b);
    if (r != saved_bg_r || g != saved_bg_g || b != saved_bg_b)
        return 1;

    const char *img = get_path_label();
    if (strcmp(img, saved_bg_image) != 0)
        return 1;

    return 0;
}

static void desktops_destroy(void)
{
    scale_rows = NULL;
    scale_cols = NULL;
    radio_solid = NULL;
    radio_image = NULL;
    color_picker = NULL;
    color_picker_row = NULL;
    image_path_label = NULL;
    image_browse_btn = NULL;
    image_path_row = NULL;
    fc_shell = NULL;
}

void panel_desktops_set_dbus(IsdeDBus *bus) { panel_dbus = bus; }

const IsdeSettingsPanel panel_desktops = {
    .name        = "Desktops",
    .icon        = "preferences-desktop-wallpaper",
    .section     = "wm.desktops",
    .create      = desktops_create,
    .apply       = desktops_apply,
    .revert      = desktops_revert,
    .has_changes = desktops_has_changes,
    .destroy     = desktops_destroy,
};
