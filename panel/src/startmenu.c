#define _POSIX_C_SOURCE 200809L
/*
 * startmenu.c — custom two-pane start menu
 *
 * OverrideShell with two List widgets:
 *   Left:  category names — click to populate right pane
 *   Right: app names for selected category — click to launch
 */
#include "panel.h"
#include <X11/ShellP.h>
#include <ISW/List.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static const char *START_ICON_NORMAL =
    "<svg xmlns='http://www.w3.org/2000/svg' viewBox='0 0 16 16'>"
    "<polygon points='3,2 13,8 3,14' fill='black'/>"
    "</svg>";

static const char *START_ICON_ACTIVE =
    "<svg xmlns='http://www.w3.org/2000/svg' viewBox='0 0 16 16'>"
    "<polygon points='3,2 13,8 3,14' fill='white'/>"
    "</svg>";

#define MENU_WIDTH       400
#define MENU_HEIGHT      350
#define CAT_PANE_WIDTH   130

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
    if (!categories) return "Other";
    for (int i = 0; i < (int)NUM_CAT_MAP; i++) {
        const char *cat = CAT_MAP[i].key;
        size_t clen = strlen(cat);
        const char *p = categories;
        while (*p) {
            const char *semi = strchr(p, ';');
            size_t elen = semi ? (size_t)(semi - p) : strlen(p);
            if (elen == clen && strncmp(p, cat, clen) == 0)
                return CAT_MAP[i].label;
            p = semi ? semi + 1 : p + elen;
        }
    }
    return "Other";
}

/* ---------- build category data ---------- */

static StartMenuCategory *find_or_add_cat(Panel *p, const char *label)
{
    for (int i = 0; i < p->ncategories; i++)
        if (strcmp(p->categories[i].label, label) == 0)
            return &p->categories[i];

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
                        const char *exec, const char *icon)
{
    if (c->napps >= c->cap) {
        c->cap *= 2;
        c->apps = realloc(c->apps, c->cap * sizeof(StartMenuApp));
    }
    c->apps[c->napps].name = name;
    c->apps[c->napps].exec = exec;
    c->apps[c->napps].icon = icon;
    c->napps++;
}

static void build_categories(Panel *p)
{
    for (int i = 0; i < p->ndesktop; i++) {
        IsdeDesktopEntry *de = p->desktop_entries[i];
        if (!isde_desktop_should_show(de, "ISDE")) continue;
        if (isde_desktop_no_display(de) || isde_desktop_hidden(de)) continue;
        const char *exec = isde_desktop_exec(de);
        const char *name = isde_desktop_name(de);
        if (!exec || !name) continue;

        const char *label = map_category(isde_desktop_categories(de));
        StartMenuCategory *c = find_or_add_cat(p, label);
        cat_add_app(c, name, exec, isde_desktop_icon(de));
    }
}

/* ---------- show apps for selected category ---------- */

static void show_category(Panel *p, int index)
{
    if (index < 0 || index >= p->ncategories) return;
    p->active_cat = index;

    StartMenuCategory *c = &p->categories[index];

    /* Build string list for the app List widget */
    String *names = malloc((c->napps + 1) * sizeof(String));
    for (int i = 0; i < c->napps; i++)
        names[i] = (String)c->apps[i].name;
    names[c->napps] = NULL;

    IswListChange(p->app_box, names, c->napps, 0, True);
    XtMapWidget(p->app_box);
    /* Don't free names — the List widget holds the pointer.
     * Previous array leaks, but it's small and infrequent. */
}

/* ---------- callbacks ---------- */

static void category_selected(Widget w, XtPointer client_data,
                              XtPointer call_data)
{
    (void)w;
    Panel *p = (Panel *)client_data;
    IswListReturnStruct *ret = (IswListReturnStruct *)call_data;
    if (ret->list_index == XAW_LIST_NONE) return;
    show_category(p, ret->list_index);
}

static void app_selected(Widget w, XtPointer client_data,
                         XtPointer call_data)
{
    (void)w;
    Panel *p = (Panel *)client_data;
    IswListReturnStruct *ret = (IswListReturnStruct *)call_data;
    if (ret->list_index == XAW_LIST_NONE) return;
    if (p->active_cat < 0 || p->active_cat >= p->ncategories) return;

    StartMenuCategory *c = &p->categories[p->active_cat];
    if (ret->list_index >= c->napps) return;

    const char *exec = c->apps[ret->list_index].exec;

    /* Close menu */
    XtPopdown(p->start_shell);

    /* Launch */
    pid_t pid = fork();
    if (pid == 0) {
        execl("/bin/sh", "sh", "-c", exec, (char *)NULL);
        _exit(127);
    }
}

/* ---------- toggle ---------- */

static void toggle_start_menu(Widget w, XtPointer client_data,
                              XtPointer call_data)
{
    (void)w;
    (void)call_data;
    Panel *p = (Panel *)client_data;

    if (!XtIsRealized(p->start_shell))
        XtRealizeWidget(p->start_shell);

    Arg ia[1];
    ShellWidget sw = (ShellWidget)p->start_shell;
    if (sw->shell.popped_up) {
        XtPopdown(p->start_shell);
        XtSetArg(ia[0], XtNsvgData, START_ICON_NORMAL);
        XtSetValues(p->start_btn, ia, 1);
        return;
    }
    XtSetArg(ia[0], XtNsvgData, START_ICON_ACTIVE);
    XtSetValues(p->start_btn, ia, 1);

    Position bx, by;
    XtTranslateCoords(p->start_btn, 0, 0, &bx, &by);

    /* Set position before popup via Xt resources */
    Arg a[2];
    Cardinal na = 0;
    XtSetArg(a[na], XtNx, (Position)bx);              na++;
    XtSetArg(a[na], XtNy, (Position)(by - MENU_HEIGHT)); na++;
    XtSetValues(p->start_shell, a, na);

    XtPopup(p->start_shell, XtGrabNone);
    p->active_cat = -1;
    XtUnmapWidget(p->app_box);
}

/* ---------- init / cleanup ---------- */

void startmenu_init(Panel *p)
{
    build_categories(p);
    p->active_cat = -1;

    /* Start button */
    Arg args[12];
    Cardinal n = 0;
    XtSetArg(args[n], XtNsvgData, START_ICON_NORMAL);  n++;
    XtSetArg(args[n], XtNwidth, PANEL_HEIGHT);       n++;
    XtSetArg(args[n], XtNheight, PANEL_HEIGHT);      n++;
    XtSetArg(args[n], XtNborderWidth, 0);            n++;
    p->start_btn = XtCreateManagedWidget("startBtn", commandWidgetClass,
                                         p->box, args, n);
    XtAddCallback(p->start_btn, XtNcallback, toggle_start_menu, p);

    /* Start menu shell */
    n = 0;
    XtSetArg(args[n], XtNwidth, MENU_WIDTH);          n++;
    XtSetArg(args[n], XtNheight, MENU_HEIGHT);         n++;
    XtSetArg(args[n], XtNoverrideRedirect, True);      n++;
    XtSetArg(args[n], XtNborderWidth, 1);              n++;
    p->start_shell = XtCreatePopupShell("startMenu",
                                        overrideShellWidgetClass,
                                        p->start_btn, args, n);

    /* Form container — single child of the shell */
    n = 0;
    XtSetArg(args[n], XtNdefaultDistance, 0);  n++;
    XtSetArg(args[n], XtNborderWidth, 0);      n++;
    Widget form = XtCreateManagedWidget("menuForm", formWidgetClass,
                                        p->start_shell, args, n);

    /* Category list (left pane) */
    String *cat_names = malloc((p->ncategories + 1) * sizeof(String));
    for (int i = 0; i < p->ncategories; i++)
        cat_names[i] = (String)p->categories[i].label;
    cat_names[p->ncategories] = NULL;

    n = 0;
    XtSetArg(args[n], XtNlist, cat_names);                    n++;
    XtSetArg(args[n], XtNnumberStrings, p->ncategories);     n++;
    XtSetArg(args[n], XtNdefaultColumns, 1);                  n++;
    XtSetArg(args[n], XtNforceColumns, True);                 n++;
    XtSetArg(args[n], XtNverticalList, True);                 n++;
    XtSetArg(args[n], XtNborderWidth, 0);                     n++;
    XtSetArg(args[n], XtNwidth, CAT_PANE_WIDTH);              n++;
    XtSetArg(args[n], XtNheight, MENU_HEIGHT);                n++;
    XtSetArg(args[n], XtNcursor, None);                       n++;
    p->cat_box = XtCreateManagedWidget("catList", listWidgetClass,
                                       form, args, n);
    XtAddCallback(p->cat_box, XtNcallback, category_selected, p);
    /* Don't free cat_names — the List widget holds a pointer to it */

    /* App list (right pane) — starts with placeholder.
     * Must be static since the List widget holds the pointer. */
    static String initial[] = { "Select a category", NULL };
    n = 0;
    XtSetArg(args[n], XtNlist, initial);                          n++;
    XtSetArg(args[n], XtNnumberStrings, 1);                       n++;
    XtSetArg(args[n], XtNdefaultColumns, 1);                      n++;
    XtSetArg(args[n], XtNforceColumns, True);                     n++;
    XtSetArg(args[n], XtNverticalList, True);                     n++;
    XtSetArg(args[n], XtNborderWidth, 0);                         n++;
    XtSetArg(args[n], XtNfromHoriz, p->cat_box);                  n++;
    XtSetArg(args[n], XtNwidth, MENU_WIDTH - CAT_PANE_WIDTH);    n++;
    XtSetArg(args[n], XtNheight, MENU_HEIGHT);                    n++;
    XtSetArg(args[n], XtNcursor, None);                            n++;
    p->app_box = XtCreateManagedWidget("appList", listWidgetClass,
                                       form, args, n);
    XtAddCallback(p->app_box, XtNcallback, app_selected, p);

    /* Category list: hover highlights and switches category immediately */
    static char catTranslations[] =
        "<EnterWindow>: Set()\n"
        "<LeaveWindow>: Unset()\n"
        "<Motion>:      Set() Notify()\n"
        "<BtnDown>:     Set() Notify()\n"
        "<BtnUp>:       Notify()";
    XtOverrideTranslations(p->cat_box,
                           XtParseTranslationTable(catTranslations));

    /* App list: hover highlights, click launches */
    static char appTranslations[] =
        "<EnterWindow>: Set()\n"
        "<LeaveWindow>: Unset()\n"
        "<Motion>:      Set()\n"
        "<BtnDown>:     Set() Notify()\n"
        "<BtnUp>:       Notify()";
    XtOverrideTranslations(p->app_box,
                           XtParseTranslationTable(appTranslations));

    /* Hide app list until a category is hovered */
    XtUnmapWidget(p->app_box);
}

void startmenu_cleanup(Panel *p)
{
    for (int i = 0; i < p->ncategories; i++)
        free(p->categories[i].apps);
    free(p->categories);
    p->categories = NULL;
    p->ncategories = 0;
}
