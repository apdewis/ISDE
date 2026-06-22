#define _POSIX_C_SOURCE 200809L
/*
 * startmenu.c — custom two-pane start menu
 *
 * OverrideShell with category List + app ListBox:
 *   Left:  category names (List widget) — click to populate right pane
 *   Right: app entries with icons (ListBox + ListBoxRow) — click to launch
 */

#include "panel.h"
#include <ISW/ShellP.h>
#include <ISW/List.h>
#include <ISW/ListBox.h>
#include <ISW/ListBoxRow.h>
#include <ISW/IswArgMacros.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#ifdef __linux__
#include <sys/inotify.h>
#endif

static char *start_icon_path;
static char *shutdown_icon_path;
static char *reboot_icon_path;
static char *logout_icon_path;


/* State-dependent: active inverts to show the menu is open */
static void set_start_btn_active(Panel *p, int active)
{
    const IsdeColorScheme *s = isde_theme_current();
    if (!s) {
        return;
    }
    IswArgBuilder ab = IswArgBuilderInit();
    //if (active) {
    //    IswArgForeground(&ab, start_color_pixel(p, s->taskbar_button.hover_fg));
    //    IswArgBackground(&ab, start_color_pixel(p, s->active));
    //} else {
    //    IswArgForeground(&ab, start_color_pixel(p, s->taskbar_button.fg));
    //    IswArgBackground(&ab, start_color_pixel(p, s->taskbar_button.bg));
    //}
    //IswSetValues(p->start_btn, ab.args, ab.count);
}

#define MENU_WIDTH       600
#define MENU_HEIGHT      350
#define CAT_PANE_WIDTH   150
#define TOOLBAR_HEIGHT   28
#define APP_ICON_SIZE    28

/* Standard freedesktop.org category mapping */
static const struct { const char *key; const char *label; } CAT_MAP[] = {
    { "AudioVideo",  "Multimedia" },
    { "Audio",       "Multimedia" },
    { "Video",       "Multimedia" },
    { "Development", "Development" },
    { "Education",   "Education" },
    { "Game",        "Games" },
    { "Graphics",    "Graphics" },
    { "Network",     "Internet" },
    { "Office",      "Office" },
    { "Settings",    "Settings" },
    { "System",      "System" },
    { "Utility",     "Accessories" },
};
#define NUM_CAT_MAP (sizeof(CAT_MAP) / sizeof(CAT_MAP[0]))

static const char *map_category(const char *categories)
{
    if (!categories) {
        return "Other";
    }
    for (int i = 0; i < (int)NUM_CAT_MAP; i++) {
        const char *cat = CAT_MAP[i].key;
        size_t clen = strlen(cat);
        const char *p = categories;
        while (*p) {
            const char *semi = strchr(p, ';');
            size_t elen = semi ? (size_t)(semi - p) : strlen(p);
            if (elen == clen && strncmp(p, cat, clen) == 0) {
                return CAT_MAP[i].label;
            }
            p = semi ? semi + 1 : p + elen;
        }
    }
    return "Other";
}

/* ---------- build category data ---------- */

static StartMenuCategory *find_or_add_cat(Panel *p, const char *label)
{
    for (int i = 0; i < p->ncategories; i++) {
        if (strcmp(p->categories[i].label, label) == 0) {
            return &p->categories[i];
        }
    }

    p->categories = realloc(p->categories,
                            (p->ncategories + 1) * sizeof(StartMenuCategory));
    StartMenuCategory *c = &p->categories[p->ncategories++];
    memset(c, 0, sizeof(*c));
    c->label = label;
    c->cap = 16;
    c->apps = calloc(c->cap, sizeof(StartMenuApp));
    return c;
}

static void cat_add_app(StartMenuCategory *c, const char *name,
                        IsdeDesktopEntry *entry, const char *icon)
{
    if (c->napps >= c->cap) {
        c->cap *= 2;
        c->apps = realloc(c->apps, c->cap * sizeof(StartMenuApp));
    }
    c->apps[c->napps].name = name;
    c->apps[c->napps].entry = entry;
    c->apps[c->napps].icon = icon;
    c->napps++;
}

static void build_categories(Panel *p)
{
    for (int i = 0; i < p->ndesktop; i++) {
        IsdeDesktopEntry *de = p->desktop_entries[i];
        if (!isde_desktop_should_show(de, "ISDE")) {
            continue;
        }
        if (isde_desktop_no_display(de) || isde_desktop_hidden(de)) {
            continue;
        }
        const char *name = isde_desktop_name(de);
        if (!isde_desktop_exec(de) || !name) {
            continue;
        }

        const char *label = map_category(isde_desktop_categories(de));
        StartMenuCategory *c = find_or_add_cat(p, label);
        cat_add_app(c, name, de, isde_desktop_icon(de));
    }
}

/* ---------- ListBox helpers ---------- */

static void listbox_clear(Widget listbox)
{
    WidgetList children;
    Cardinal num;
    IswArgBuilder qab = IswArgBuilderInit();
    IswArgBuilderAdd(&qab, IswNchildren, (IswArgVal)&children);
    IswArgBuilderAdd(&qab, IswNnumChildren, (IswArgVal)&num);
    IswGetValues(listbox, qab.args, qab.count);

    for (int i = (int)num - 1; i >= 0; i--) {
        IswDestroyWidget(children[i]);
    }
}

static Widget listbox_nth_child(Widget listbox, int n)
{
    CompositeWidget cw = (CompositeWidget)listbox;
    int managed = 0;
    for (Cardinal i = 0; i < cw->composite.num_children; i++) {
        Widget child = cw->composite.children[i];
        if (!IswIsManaged(child)) {
            continue;
        }
        if (managed == n) {
            return child;
        }
        managed++;
    }
    return NULL;
}

static void listbox_select_index(Widget listbox, int index)
{
    IswListBoxClearSelection(listbox);
    Widget child = listbox_nth_child(listbox, index);
    if (child) {
        IswListBoxSelectChild(listbox, child);
    }
}

static void app_row_hover(Widget w, IswPointer client_data,
                          IswEvent *event, Boolean *cont)
{
    (void)cont;
    Panel *p = (Panel *)client_data;
    if (event->kind != IswEnter && event->kind != IswMotion) {
        return;
    }
    Widget listbox = IswParent(IswParent(w));
    CompositeWidget cw = (CompositeWidget)listbox;
    Widget row = IswParent(w);
    int managed = 0;
    for (Cardinal i = 0; i < cw->composite.num_children; i++) {
        Widget child = cw->composite.children[i];
        if (!IswIsManaged(child)) {
            continue;
        }
        if (child == row) {
            p->app_highlight = managed;
            listbox_select_index(listbox, managed);
            return;
        }
        managed++;
    }
}

static void app_row_leave(Widget w, IswPointer client_data,
                          IswEvent *event, Boolean *cont)
{
    (void)w; (void)cont;
    Panel *p = (Panel *)client_data;
    if (event->kind != IswLeave) {
        return;
    }
    p->app_highlight = -1;
    Widget listbox = IswParent(IswParent(w));
    IswListBoxClearSelection(listbox);
}

static void populate_listbox(Panel *p, Widget listbox, int pane_w,
                             StartMenuApp *apps, int napps)
{
    listbox_clear(listbox);

    IswArgBuilder ab = IswArgBuilderInit();

    for (int i = 0; i < napps; i++) {
        IswArgBuilderReset(&ab);
        IswArgBorderWidth(&ab, 0);
        IswArgRowPadding(&ab, 0);
        IswArgListBoxRowHeight(&ab, APP_ICON_SIZE + 6);
        Widget row = IswCreateManagedWidget("appRow", listBoxRowWidgetClass,
                                           listbox, ab.args, ab.count);

        char *icon_path = apps[i].icon
            ? isde_icon_find("apps", apps[i].icon) : NULL;
        if (!icon_path) {
            icon_path = isde_icon_find("apps", "application-default");
        }

        IswArgBuilderReset(&ab);
        IswArgLabel(&ab, "");
        IswArgBorderWidth(&ab, 0);
        IswArgWidth(&ab, APP_ICON_SIZE);
        IswArgHeight(&ab, APP_ICON_SIZE);
        IswArgInternalWidth(&ab, 0);
        IswArgInternalHeight(&ab, 0);
        ISW_ARG(&ab, IswNresize, False);
        if (icon_path) {
            IswArgImage(&ab, icon_path);
        }
        Widget icon_w = IswCreateManagedWidget("appIcon", labelWidgetClass,
                                               row, ab.args, ab.count);
        free(icon_path);

        int text_w = pane_w - APP_ICON_SIZE - 4;
        if (text_w < 1) {
            text_w = 1;
        }
        IswArgBuilderReset(&ab);
        IswArgLabel(&ab, (String)apps[i].name);
        IswArgBorderWidth(&ab, 0);
        IswArgJustify(&ab, IswJustifyLeft);
        IswArgInternalWidth(&ab, 4);
        IswArgInternalHeight(&ab, 0);
        ISW_ARG(&ab, IswNresize, False);
        IswArgWidth(&ab, text_w);
        ISW_ARG(&ab, IswNellipsize, IswEllipsizeEnd);
        Widget label = IswCreateManagedWidget("appLabel", labelWidgetClass,
                                              row, ab.args, ab.count);

        IswAddEventHandler(icon_w,
                           IswEnterWindowMask | IswPointerMotionMask,
                           False, app_row_hover, p);
        IswAddEventHandler(icon_w, IswLeaveWindowMask,
                           False, app_row_leave, p);
        IswAddEventHandler(label,
                           IswEnterWindowMask | IswPointerMotionMask,
                           False, app_row_hover, p);
        IswAddEventHandler(label, IswLeaveWindowMask,
                           False, app_row_leave, p);
    }
}

/* ---------- show apps for selected category ---------- */

static void show_category(Panel *p, int index)
{
    if (index < 0 || index >= p->ncategories) {
        return;
    }
    p->active_cat = index;

    StartMenuCategory *c = &p->categories[index];
    populate_listbox(p, p->app_box, MENU_WIDTH - CAT_PANE_WIDTH,
                     c->apps, c->napps);
    IswViewportSetLocation(p->app_viewport, 0.0, 0.0);
    IswMapWidget(p->app_viewport);
}

static void launch_app(Panel *p, int index)
{
    if (p->active_cat < 0 || p->active_cat >= p->ncategories) {
        return;
    }
    StartMenuCategory *c = &p->categories[p->active_cat];
    if (index < 0 || index >= c->napps) {
        return;
    }

    IsdeDesktopEntry *de = c->apps[index].entry;
    panel_dismiss_popup(p);

    panel_launch_notify(p, de, NULL, 0);
}

/* ---------- callbacks ---------- */

static void category_selected(Widget w, IswPointer client_data,
                              IswPointer call_data)
{
    (void)w;
    Panel *p = (Panel *)client_data;
    IswListReturnStruct *ret = (IswListReturnStruct *)call_data;
    if (ret->list_index == XAW_LIST_NONE) {
        return;
    }
    show_category(p, ret->list_index);
}

static void app_selected(Widget w, IswPointer client_data,
                         IswPointer call_data)
{
    (void)w;
    Panel *p = (Panel *)client_data;
    IswListBoxCallbackData *cb = (IswListBoxCallbackData *)call_data;
    launch_app(p, cb->index);
}

/* ---------- type-ahead search ---------- */

static void search_run_filter(Panel *p)
{
    if (p->search_len == 0) {
        p->nsearch_results = 0;
        listbox_clear(p->search_list);
        return;
    }

    if (!p->search_results) {
        p->cap_search_results = 64;
        p->search_results = malloc(p->cap_search_results * sizeof(StartMenuApp *));
    }
    p->nsearch_results = 0;

    for (int i = 0; i < p->ncategories; i++) {
        StartMenuCategory *c = &p->categories[i];
        for (int j = 0; j < c->napps; j++) {
            StartMenuApp *app = &c->apps[j];
            if (strcasestr(app->name, p->search_buf)) {
                goto match;
            }
            const char *gn = isde_desktop_generic_name(app->entry);
            if (gn && strcasestr(gn, p->search_buf)) {
                goto match;
            }
            continue;
match:
            if (p->nsearch_results >= p->cap_search_results) {
                p->cap_search_results *= 2;
                p->search_results = realloc(p->search_results,
                    p->cap_search_results * sizeof(StartMenuApp *));
            }
            p->search_results[p->nsearch_results++] = app;
        }
    }

    /* Build a temporary StartMenuApp array for populate_listbox */
    StartMenuApp *search_apps = malloc(p->nsearch_results * sizeof(StartMenuApp));
    for (int i = 0; i < p->nsearch_results; i++) {
        search_apps[i] = *p->search_results[i];
    }
    populate_listbox(p, p->search_list, MENU_WIDTH,
                     search_apps, p->nsearch_results);
    free(search_apps);

    IswViewportSetLocation(p->search_viewport, 0.0, 0.0);
    if (p->nsearch_results > 0) {
        p->app_highlight = 0;
        listbox_select_index(p->search_list, 0);
    } else {
        p->app_highlight = -1;
    }
}

static void search_activate(Panel *p)
{
    p->search_active = 1;
    p->menu_focus = 2;
    p->app_highlight = -1;
}

static void search_deactivate(Panel *p)
{
    p->search_active = 0;
    p->search_buf[0] = '\0';
    p->search_len = 0;
    p->menu_focus = 0;

    IswArgBuilder ab = IswArgBuilderInit();
    IswArgLabel(&ab, "");
    IswSetValues(p->search_input, ab.args, ab.count);

    IswUnmapWidget(p->search_viewport);
    IswMapWidget(p->cat_viewport);

    if (p->active_cat >= 0) {
        IswMapWidget(p->app_viewport);
    }

    p->cat_highlight = 0;
    IswListHighlight(p->cat_box, 0);
}

static void search_update_display(Panel *p)
{
    IswArgBuilder ab = IswArgBuilderInit();
    IswArgLabel(&ab, p->search_buf);
    IswSetValues(p->search_input, ab.args, ab.count);
    search_run_filter(p);

    if (p->search_len > 0) {
        IswUnmapWidget(p->cat_viewport);
        IswUnmapWidget(p->app_viewport);
        IswMapWidget(p->search_viewport);
    } else {
        IswUnmapWidget(p->search_viewport);
        IswMapWidget(p->cat_viewport);
        if (p->active_cat >= 0) {
            IswMapWidget(p->app_viewport);
        }
    }
}

static void search_selected(Widget w, IswPointer client_data,
                            IswPointer call_data)
{
    (void)w;
    Panel *p = (Panel *)client_data;
    IswListBoxCallbackData *cb = (IswListBoxCallbackData *)call_data;
    if (cb->index < 0 || cb->index >= p->nsearch_results) {
        return;
    }
    IsdeDesktopEntry *de = p->search_results[cb->index]->entry;
    panel_dismiss_popup(p);
    panel_launch_notify(p, de, NULL, 0);
}

static void launch_search_result(Panel *p, int index)
{
    if (index < 0 || index >= p->nsearch_results) {
        return;
    }
    IsdeDesktopEntry *de = p->search_results[index]->entry;
    panel_dismiss_popup(p);
    panel_launch_notify(p, de, NULL, 0);
}

/* ---------- keyboard navigation ---------- */

static void menu_button_handler(Widget w, IswPointer client_data,
                                IswEvent *event, Boolean *cont)
{
    (void)w; (void)cont;
    Panel *p = (Panel *)client_data;
    if (event->kind != IswButtonDown) {
        return;
    }
    /* With owner_events=True on the pointer grab, clicks inside our own
     * windows deliver to those windows normally. Clicks outside deliver
     * to the grab window (the shell), invoking this handler — dismiss. */
    panel_dismiss_popup(p);
}

static void menu_key_handler(Widget w, IswPointer client_data,
                             IswEvent *event, Boolean *cont)
{
    (void)w; (void)cont;
    Panel *p = (Panel *)client_data;
    if (event->kind != IswKeyDown) {
        return;
    }

    uint32_t sym = event->key.key;

    /* Super always dismisses */
    if (sym == IswKeySuper) {
        panel_dismiss_popup(p);
        return;
    }

    /* Escape: exit search mode first, then dismiss */
    if (sym == IswKeyEscape) {
        if (p->search_active) {
            search_deactivate(p);
        } else {
            panel_dismiss_popup(p);
        }
        return;
    }

    /* Backspace in search mode */
    if (sym == IswKeyBackspace) {
        if (p->search_active) {
            if (p->search_len > 0) {
                /* Remove one UTF-8 character from the end */
                int i = p->search_len - 1;
                while (i > 0 && (p->search_buf[i] & 0xC0) == 0x80) {
                    i--;
                }
                p->search_buf[i] = '\0';
                p->search_len = i;
                if (p->search_len == 0) {
                    search_deactivate(p);
                } else {
                    search_update_display(p);
                }
            }
        }
        return;
    }

    /* Up/Down navigate in search mode */
    if (p->search_active) {
        if (sym == IswKeyArrowDown) {
            int next = p->app_highlight + 1;
            if (next >= p->nsearch_results) {
                next = p->nsearch_results - 1;
            }
            if (next >= 0) {
                p->app_highlight = next;
                listbox_select_index(p->search_list, next);
            }
            return;
        }
        if (sym == IswKeyArrowUp) {
            int next = p->app_highlight - 1;
            if (next < 0) {
                next = 0;
            }
            p->app_highlight = next;
            listbox_select_index(p->search_list, next);
            return;
        }
        if (sym == IswKeyReturn) {
            launch_search_result(p, p->app_highlight);
            return;
        }
    }

    /* Normal category/app navigation */
    switch (sym) {
    case IswKeyArrowDown:
        if (p->menu_focus == 0) {
            int next = p->cat_highlight + 1;
            if (next >= p->ncategories) {
                next = p->ncategories - 1;
            }
            p->cat_highlight = next;
            IswListHighlight(p->cat_box, next);
            show_category(p, next);
            p->app_highlight = -1;
            IswListBoxClearSelection(p->app_box);
        } else {
            if (p->active_cat < 0) {
                break;
            }
            StartMenuCategory *c = &p->categories[p->active_cat];
            int next = p->app_highlight + 1;
            if (next >= c->napps) {
                next = c->napps - 1;
            }
            p->app_highlight = next;
            listbox_select_index(p->app_box, next);
        }
        break;

    case IswKeyArrowUp:
        if (p->menu_focus == 0) {
            int next = p->cat_highlight - 1;
            if (next < 0) {
                next = 0;
            }
            p->cat_highlight = next;
            IswListHighlight(p->cat_box, next);
            show_category(p, next);
            p->app_highlight = -1;
            IswListBoxClearSelection(p->app_box);
        } else {
            int next = p->app_highlight - 1;
            if (next < 0) {
                next = 0;
            }
            p->app_highlight = next;
            listbox_select_index(p->app_box, next);
        }
        break;

    case IswKeyArrowRight:
        if (p->menu_focus == 1 || p->active_cat < 0) {
            break;
        }
        p->menu_focus = 1;
        p->app_highlight = 0;
        listbox_select_index(p->app_box, 0);
        break;

    case IswKeyArrowLeft:
        if (p->menu_focus == 0) {
            break;
        }
        p->menu_focus = 0;
        p->app_highlight = -1;
        IswListBoxClearSelection(p->app_box);
        break;

    case IswKeyReturn:
        if (p->menu_focus == 0) {
            if (p->active_cat < 0) {
                break;
            }
            p->menu_focus = 1;
            p->app_highlight = 0;
            listbox_select_index(p->app_box, 0);
        } else {
            launch_app(p, p->app_highlight);
        }
        break;

    default: {
        /* Printable character — activate search.  The neutral event carries
         * the UTF-8 the key produced ("" for non-text keys). */
        const char *utf8 = event->key.text;
        int charlen = (int)strlen(utf8);
        if (charlen > 0 && (unsigned char)utf8[0] >= 0x20) {
            if (p->search_len + charlen < (int)sizeof(p->search_buf) - 1) {
                if (!p->search_active) {
                    search_activate(p);
                }
                memcpy(p->search_buf + p->search_len, utf8, charlen);
                p->search_len += charlen;
                p->search_buf[p->search_len] = '\0';
                search_update_display(p);
            }
        }
        break;
    }
    }
}

static void shutdown_cb(Widget w, IswPointer client_data, IswPointer call_data)
{
    (void)w; (void)call_data;
    Panel *p = (Panel *)client_data;
    panel_dismiss_popup(p);
    isde_dbus_call_method(p->dbus, ISDE_SESSION_DBUS_SERVICE,
                          ISDE_SESSION_DBUS_PATH, ISDE_SESSION_DBUS_INTERFACE,
                          "Shutdown");
}

static void reboot_cb(Widget w, IswPointer client_data, IswPointer call_data)
{
    (void)w; (void)call_data;
    Panel *p = (Panel *)client_data;
    panel_dismiss_popup(p);
    isde_dbus_call_method(p->dbus, ISDE_SESSION_DBUS_SERVICE,
                          ISDE_SESSION_DBUS_PATH, ISDE_SESSION_DBUS_INTERFACE,
                          "Reboot");
}

static void logout_cb(Widget w, IswPointer client_data, IswPointer call_data)
{
    (void)w; (void)call_data;
    Panel *p = (Panel *)client_data;
    panel_dismiss_popup(p);
    isde_dbus_call_method(p->dbus, ISDE_SESSION_DBUS_SERVICE,
                          ISDE_SESSION_DBUS_PATH, ISDE_SESSION_DBUS_INTERFACE,
                          "Logout");
}

/* ---------- toggle ---------- */

static void toggle_start_menu_cb(Widget w, IswPointer client_data,
                                 IswPointer call_data)
{
    (void)w;
    (void)call_data;
    startmenu_toggle((Panel *)client_data);
}

void startmenu_toggle(Panel *p)
{
    if (p->active_popup == p->start_shell) {
        panel_dismiss_popup(p);
        return;
    }
    set_start_btn_active(p, 1);

    /* Position above the panel at the left edge of primary monitor.
     * All values must be logical — ISW scales to physical internally. */
    double sf = ISWScaleFactor(p->toplevel);
    int log_mon_x = (int)(p->mon_x / sf + 0.5);
    int log_panel_top = (int)((p->mon_y + p->mon_h) / sf + 0.5)
                        - PANEL_HEIGHT;

    if (!IswIsRealized(p->start_shell)) {
        IswRealizeWidget(p->start_shell);
    }
    int menu_w = p->start_shell->core.width;
    int menu_h = p->start_shell->core.height;
    int menu_bw = p->start_shell->core.border_width;
    int menu_y = log_panel_top - menu_h - 2 * menu_bw;
    IswConfigureWidget(p->start_shell, log_mon_x, menu_y,
                      menu_w, menu_h, menu_bw);
    IswPopup(p->start_shell, IswGrabNone);

    /* Force immediate redraw — the shell content may be stale from the
     * previous popdown, causing a blank menu on some redraws. */
    IswExposeProc expose = IswClass(p->start_shell)->core_class.expose;
    if (expose) {
        expose(p->start_shell, NULL, 0);
    }

    panel_show_popup(p, p->start_shell);
    p->active_cat = -1;
    p->cat_highlight = 0;
    p->app_highlight = -1;
    p->menu_focus = 0;

    /* Reset search state and clear the input label */
    p->search_active = 0;
    p->search_buf[0] = '\0';
    p->search_len = 0;
    IswArgBuilder sab = IswArgBuilderInit();
    IswArgLabel(&sab, "");
    IswSetValues(p->search_input, sab.args, sab.count);
    IswUnmapWidget(p->search_viewport);
    IswMapWidget(p->cat_viewport);
    IswUnmapWidget(p->app_viewport);

    /* Highlight first category and grab keyboard */
    IswListHighlight(p->cat_box, 0);
    show_category(p, 0);
    IswGrabKeyboard(p->start_shell, True, ISW_CURRENT_TIME);
    IswGrabPointer(p->start_shell, True,
                   IswButtonPressMask | IswButtonReleaseMask,
                   IswCursorNone, ISW_CURRENT_TIME);
}

/* ---------- refresh / watch ---------- */

/* Backing array for cat_box strings; List widget keeps the pointer, so we
 * retain ownership here and free the previous array on each rebuild. */
static String *cat_names_backing;

static void free_categories(Panel *p)
{
    for (int i = 0; i < p->ncategories; i++) {
        free(p->categories[i].apps);
    }
    free(p->categories);
    p->categories = NULL;
    p->ncategories = 0;
}

static void startmenu_refresh(Panel *p)
{
    /* Menu is closed on refresh so widget pointers stay valid but nothing
     * is currently displayed from the stale data. */
    if (p->active_popup == p->start_shell) {
        panel_dismiss_popup(p);
    }

    free_categories(p);
    panel_reload_desktop_entries(p);
    build_categories(p);

    String *names = malloc((p->ncategories + 1) * sizeof(String));
    for (int i = 0; i < p->ncategories; i++) {
        names[i] = (String)p->categories[i].label;
    }
    names[p->ncategories] = NULL;

    IswListChange(p->cat_box, names, p->ncategories, 0, True);
    free(cat_names_backing);
    cat_names_backing = names;

    /* App pane shows stale pointers into freed categories; clear it. */
    listbox_clear(p->app_box);
    IswUnmapWidget(p->app_viewport);

    /* Search results point into freed category data */
    p->nsearch_results = 0;
    p->search_active = 0;
    p->search_buf[0] = '\0';
    p->search_len = 0;
    listbox_clear(p->search_list);

    p->active_cat = -1;
    p->cat_highlight = -1;
    p->app_highlight = -1;
}

#ifdef __linux__
#define DESKTOP_WATCH_MASK (IN_CREATE | IN_DELETE | IN_MOVED_FROM | \
                            IN_MOVED_TO | IN_MODIFY | IN_ATTRIB)
#define DESKTOP_REFRESH_DEBOUNCE_MS 300

static void desktop_refresh_timer_cb(IswPointer cd, IswIntervalId *id)
{
    (void)id;
    Panel *p = (Panel *)cd;
    p->desktop_refresh_timer = 0;
    startmenu_refresh(p);
}

static void desktop_watch_cb(IswPointer cd, int *fd, IswInputId *id)
{
    (void)id;
    Panel *p = (Panel *)cd;

    char buf[4096]
        __attribute__((aligned(__alignof__(struct inotify_event))));
    ssize_t len;
    int saw_desktop = 0;

    while ((len = read(*fd, buf, sizeof(buf))) > 0) {
        for (char *ptr = buf; ptr < buf + len; ) {
            struct inotify_event *ev = (struct inotify_event *)ptr;
            if (ev->len > 0) {
                /* Only care about .desktop files */
                const char *dot = strrchr(ev->name, '.');
                if (dot && strcmp(dot, ".desktop") == 0) {
                    saw_desktop = 1;
                }
            }
            ptr += sizeof(*ev) + ev->len;
        }
    }

    if (saw_desktop) {
        if (p->desktop_refresh_timer) {
            IswRemoveTimeOut(p->desktop_refresh_timer);
        }
        p->desktop_refresh_timer = IswAppAddTimeOut(
            p->app, DESKTOP_REFRESH_DEBOUNCE_MS,
            desktop_refresh_timer_cb, p);
    }
}

static void desktop_watch_add_dir(Panel *p, const char *path)
{
    /* Non-existent dirs are skipped; directories created later won't be
     * picked up without a restart, but that's the usual tradeoff. */
    inotify_add_watch(p->desktop_inotify_fd, path, DESKTOP_WATCH_MASK);
}

static void desktop_watch_start(Panel *p)
{
    p->desktop_inotify_fd = inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
    if (p->desktop_inotify_fd < 0) {
        return;
    }

    const char *data_dirs = isde_xdg_data_dirs();
    const char *dp = data_dirs;
    while (dp && *dp) {
        const char *colon = strchr(dp, ':');
        size_t dlen = colon ? (size_t)(colon - dp) : strlen(dp);
        if (dlen > 0) {
            char path[512];
            snprintf(path, sizeof(path), "%.*s/applications",
                     (int)dlen, dp);
            desktop_watch_add_dir(p, path);
        }
        dp = colon ? colon + 1 : NULL;
    }

    char home_path[512];
    snprintf(home_path, sizeof(home_path), "%s/applications",
             isde_xdg_data_home());
    desktop_watch_add_dir(p, home_path);

    p->desktop_input_id = IswAppAddInput(p->app, p->desktop_inotify_fd,
                                         (IswPointer)IswInputReadMask,
                                         desktop_watch_cb, p);
}

static void desktop_watch_stop(Panel *p)
{
    if (p->desktop_refresh_timer) {
        IswRemoveTimeOut(p->desktop_refresh_timer);
        p->desktop_refresh_timer = 0;
    }
    if (p->desktop_input_id) {
        IswRemoveInput(p->desktop_input_id);
        p->desktop_input_id = 0;
    }
    if (p->desktop_inotify_fd >= 0) {
        close(p->desktop_inotify_fd);
        p->desktop_inotify_fd = -1;
    }
}
#else
static void desktop_watch_start(Panel *p) { (void)p; }
static void desktop_watch_stop(Panel *p) { (void)p; }
#endif

/* ---------- init / cleanup ---------- */

void startmenu_init(Panel *p)
{
    build_categories(p);
    p->active_cat = -1;
    p->cat_highlight = -1;
    p->app_highlight = -1;
    p->menu_focus = 0;

    /* Resolve start menu and logout icons from theme */
    free(start_icon_path);
    start_icon_path = isde_icon_find("actions", "application-menu");

    free(shutdown_icon_path);
    shutdown_icon_path = isde_icon_find("actions", "system-shutdown");

    free(reboot_icon_path);
    reboot_icon_path = isde_icon_find("actions", "system-reboot");

    free(logout_icon_path);
    logout_icon_path = isde_icon_find("actions", "system-log-out");

    /* Start button — child of form, pinned left */
    IswArgBuilder ab = IswArgBuilderInit();
    if (start_icon_path) {
        IswArgImage(&ab, start_icon_path);
    }
    IswArgLabel(&ab, "");
    IswArgWidth(&ab, PANEL_HEIGHT);
    IswArgHeight(&ab, PANEL_HEIGHT);
    IswArgInternalWidth(&ab, 0);
    IswArgInternalHeight(&ab, 0);
    IswArgFlexBasis(&ab, PANEL_HEIGHT);
    IswArgBorderWidth(&ab, 0);
    p->start_btn = IswCreateManagedWidget("startBtn", commandWidgetClass,
                                         p->form, ab.args, ab.count);
    IswAddCallback(p->start_btn, IswNcallback, toggle_start_menu_cb, p);

    /* Start menu shell — border via theme resources */
    IswArgBuilderReset(&ab);
    IswArgWidth(&ab, MENU_WIDTH);
    IswArgHeight(&ab, MENU_HEIGHT);
    IswArgOverrideRedirect(&ab, True);
    p->start_shell = IswCreatePopupShell("startMenu",
                                        overrideShellWidgetClass,
                                        p->start_btn, ab.args, ab.count);

    /* Form container — single child of the shell */
    IswArgBuilderReset(&ab);
    IswArgDefaultDistance(&ab, 0);
    IswArgBorderWidth(&ab, 0);
    Widget form = IswCreateManagedWidget("menuForm", formWidgetClass,
                                        p->start_shell, ab.args, ab.count);

    /* Viewport for category list (left pane) — vertical scroll */
    IswArgBuilderReset(&ab);
    IswArgWidth(&ab, CAT_PANE_WIDTH);
    IswArgHeight(&ab, MENU_HEIGHT - TOOLBAR_HEIGHT);
    IswArgBorderRight(&ab, 1);
    IswArgAllowVert(&ab, True);
    IswArgAllowHoriz(&ab, False);
    p->cat_viewport = IswCreateManagedWidget("catViewport",
                                            viewportWidgetClass,
                                            form, ab.args, ab.count);

    /* Category list — child of viewport */
    String *cat_names = malloc((p->ncategories + 1) * sizeof(String));
    for (int i = 0; i < p->ncategories; i++) {
        cat_names[i] = (String)p->categories[i].label;
    }
    cat_names[p->ncategories] = NULL;

    IswArgBuilderReset(&ab);
    IswArgList(&ab, cat_names);
    IswArgNumberStrings(&ab, p->ncategories);
    IswArgDefaultColumns(&ab, 1);
    IswArgForceColumns(&ab, True);
    IswArgVerticalList(&ab, True);
    IswArgBorderWidth(&ab, 0);
    IswArgWidth(&ab, CAT_PANE_WIDTH);
    IswArgCursor(&ab, None);
    p->cat_box = IswCreateManagedWidget("catList", listWidgetClass,
                                       p->cat_viewport, ab.args, ab.count);
    IswAddCallback(p->cat_box, IswNcallback, category_selected, p);
    /* List widget holds the pointer — track it for later free on refresh */
    cat_names_backing = cat_names;

    /* Viewport for app list (right pane) — lighter tone, vertical scroll */
    IswArgBuilderReset(&ab);
    IswArgFromHoriz(&ab, p->cat_viewport);
    IswArgWidth(&ab, MENU_WIDTH - CAT_PANE_WIDTH);
    IswArgHeight(&ab, MENU_HEIGHT - TOOLBAR_HEIGHT);
    IswArgBorderWidth(&ab, 0);
    IswArgAllowVert(&ab, True);
    IswArgAllowHoriz(&ab, False);
    IswArgUseRight(&ab, True);
    p->app_viewport = IswCreateManagedWidget("appViewport",
                                            viewportWidgetClass,
                                            form, ab.args, ab.count);

    /* App list — ListBox with icon+label rows */
    IswArgBuilderReset(&ab);
    IswArgSelectionMode(&ab, IswListBoxSelectSingle);
    IswArgRowSpacing(&ab, 0);
    IswArgBorderWidth(&ab, 0);
    p->app_box = IswCreateManagedWidget("appList", listBoxWidgetClass,
                                       p->app_viewport, ab.args, ab.count);
    IswAddCallback(p->app_box, IswNselectCallback, app_selected, p);

    /* Category list: hover highlights and switches category immediately */
    static char catTranslations[] =
        "<EnterWindow>: Set()\n"
        "<LeaveWindow>: Unset()\n"
        "<Motion>:      Set() Notify()\n"
        "<BtnDown>:     Set() Notify()\n"
        "<BtnUp>:       Notify()";
    IswOverrideTranslations(p->cat_box,
                           IswParseTranslationTable(catTranslations));

    /* Search results list — full width, overlaps cat/app viewports */
    IswArgBuilderReset(&ab);
    IswArgWidth(&ab, MENU_WIDTH);
    IswArgHeight(&ab, MENU_HEIGHT - TOOLBAR_HEIGHT);
    IswArgBorderWidth(&ab, 0);
    IswArgAllowVert(&ab, True);
    IswArgAllowHoriz(&ab, False);
    p->search_viewport = IswCreateManagedWidget("searchViewport",
                                               viewportWidgetClass,
                                               form, ab.args, ab.count);

    IswArgBuilderReset(&ab);
    IswArgSelectionMode(&ab, IswListBoxSelectSingle);
    IswArgRowSpacing(&ab, 0);
    IswArgBorderWidth(&ab, 0);
    p->search_list = IswCreateManagedWidget("searchList", listBoxWidgetClass,
                                           p->search_viewport, ab.args, ab.count);
    IswAddCallback(p->search_list, IswNselectCallback, search_selected, p);

    /* Search results hidden until typing starts */
    IswUnmapWidget(p->search_viewport);

    /* Keyboard navigation via event handler on the shell */
    IswAddEventHandler(p->start_shell, IswKeyPressMask, False,
                      menu_key_handler, p);
    IswAddEventHandler(p->start_shell, IswButtonPressMask, False,
                      menu_button_handler, p);

    /* Bottom toolbar — right-aligned action buttons.
     * No defaultDistance override: the Form's default 4px acts as bottom margin,
     * so the natural height = vertDistance + btn_size + 4 = TOOLBAR_HEIGHT. */
    IswArgBuilderReset(&ab);
    IswArgFromVert(&ab, p->cat_viewport);
    IswArgVertDistance(&ab, 0);
    IswArgWidth(&ab, MENU_WIDTH);
    IswArgHeight(&ab, TOOLBAR_HEIGHT);
    IswArgBorderTop(&ab, 1);
    p->menu_toolbar = IswCreateManagedWidget("menuToolbar", formWidgetClass,
                                            form, ab.args, ab.count);

    /* btn_size = TOOLBAR_HEIGHT - top_margin(4) - bottom_margin(4) */
    int btn_margin = 4;
    int btn_size   = TOOLBAR_HEIGHT - btn_margin * 2;
    int btn_x      = MENU_WIDTH - btn_size - btn_margin;

    /* Logout button (rightmost) */
    IswArgBuilderReset(&ab);
    IswArgLabel(&ab, "");
    IswArgWidth(&ab, btn_size);
    IswArgHeight(&ab, btn_size);
    IswArgHorizDistance(&ab, btn_x);
    IswArgVertDistance(&ab, btn_margin);
    IswArgBorderWidth(&ab, 1);
    IswArgInternalWidth(&ab, 0);
    IswArgInternalHeight(&ab, 0);
    IswArgLeft(&ab, IswChainRight);
    IswArgRight(&ab, IswChainRight);
    if (logout_icon_path) {
        IswArgImage(&ab, logout_icon_path);
    }
    p->logout_btn = IswCreateManagedWidget("logoutBtn", commandWidgetClass,
                                          p->menu_toolbar, ab.args, ab.count);
    IswAddCallback(p->logout_btn, IswNcallback, logout_cb, p);

    /* Reboot button (left of logout) */
    btn_x -= btn_size + btn_margin;
    IswArgBuilderReset(&ab);
    IswArgLabel(&ab, "");
    IswArgWidth(&ab, btn_size);
    IswArgHeight(&ab, btn_size);
    IswArgHorizDistance(&ab, btn_x);
    IswArgVertDistance(&ab, btn_margin);
    IswArgBorderWidth(&ab, 1);
    IswArgInternalWidth(&ab, 0);
    IswArgInternalHeight(&ab, 0);
    IswArgLeft(&ab, IswChainRight);
    IswArgRight(&ab, IswChainRight);
    if (reboot_icon_path) {
        IswArgImage(&ab, reboot_icon_path);
    }
    p->reboot_btn = IswCreateManagedWidget("rebootBtn", commandWidgetClass,
                                          p->menu_toolbar, ab.args, ab.count);
    IswAddCallback(p->reboot_btn, IswNcallback, reboot_cb, p);

    /* Shut Down button (left of reboot) */
    btn_x -= btn_size + btn_margin;
    IswArgBuilderReset(&ab);
    IswArgLabel(&ab, "");
    IswArgWidth(&ab, btn_size);
    IswArgHeight(&ab, btn_size);
    IswArgHorizDistance(&ab, btn_x);
    IswArgVertDistance(&ab, btn_margin);
    IswArgBorderWidth(&ab, 1);
    IswArgInternalWidth(&ab, 0);
    IswArgInternalHeight(&ab, 0);
    IswArgLeft(&ab, IswChainRight);
    IswArgRight(&ab, IswChainRight);
    if (shutdown_icon_path) {
        IswArgImage(&ab, shutdown_icon_path);
    }
    p->shutdown_btn = IswCreateManagedWidget("shutdownBtn", commandWidgetClass,
                                            p->menu_toolbar, ab.args, ab.count);
    IswAddCallback(p->shutdown_btn, IswNcallback, shutdown_cb, p);

    IswArgBuilderReset(&ab);
    IswArgLabel(&ab, "Search");
    IswArgBackground(&ab, 0);
    IswArgHeight(&ab, btn_size);
    IswArgCornerRadius(&ab, 0);
    IswArgHorizDistance(&ab, btn_margin);
    IswArgVertDistance(&ab, btn_margin);
    IswArgBorderWidth(&ab, 0);
    IswArgInternalWidth(&ab, 6);
    IswArgInternalHeight(&ab, 0);
    ISW_ARG(&ab, IswNjustify, IswJustifyLeft);
    IswArgLeft(&ab, IswChainLeft);
    IswArgRight(&ab, IswChainLeft);
    p->search_label = IswCreateManagedWidget("searchInputLabel", labelWidgetClass,
                                            p->menu_toolbar, ab.args, ab.count);

    /* Search input — fills the toolbar left of the power buttons */
    int search_w = MENU_WIDTH / 2;
    IswArgBuilderReset(&ab);
    IswArgLabel(&ab, "");
    IswArgWidth(&ab, search_w);
    IswArgHeight(&ab, btn_size);
    IswArgCornerRadius(&ab, 4);
    IswArgRightMargin(&ab, 4);
    IswArgHorizDistance(&ab, btn_margin);
    IswArgVertDistance(&ab, btn_margin);
    IswArgBorderWidth(&ab, 1);
    IswArgInternalWidth(&ab, 6);
    IswArgInternalHeight(&ab, 0);
    IswArgFromHoriz(&ab, p->search_label);
    IswArgJustify(&ab, IswJustifyLeft);
    IswArgLeft(&ab, IswChainLeft);
    IswArgRight(&ab, IswChainLeft);
    p->search_input = IswCreateManagedWidget("searchInput", labelWidgetClass,
                                            p->menu_toolbar, ab.args, ab.count);

    /* Hide app list until a category is hovered */
    IswUnmapWidget(p->app_viewport);

    p->desktop_inotify_fd = -1;
    desktop_watch_start(p);
}

void startmenu_reload_theme(Panel *p)
{
    set_start_btn_active(p, p->active_popup == p->start_shell);
}

void startmenu_cleanup(Panel *p)
{
    desktop_watch_stop(p);

    free_categories(p);
    free(cat_names_backing);
    cat_names_backing = NULL;

    free(p->search_results);
    p->search_results = NULL;
    free(p->search_names);
    p->search_names = NULL;

    free(shutdown_icon_path);
    shutdown_icon_path = NULL;
    free(reboot_icon_path);
    reboot_icon_path = NULL;
    free(logout_icon_path);
    logout_icon_path = NULL;
}
