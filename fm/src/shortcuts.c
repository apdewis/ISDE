#define _POSIX_C_SOURCE 200809L
/*
 * shortcuts.c — keyboard action callbacks and translation table
 */
#include "fm.h"

#include <stdlib.h>
#include <string.h>

/* ---------- action callbacks ---------- */

static void act_copy(Widget w, IswEvent *ev, String *p, Cardinal *n)
{
    (void)ev; (void)p; (void)n;
    Fm *fm = fm_from_widget(w);
    if (fm) {
        clipboard_copy(fm);
    }
}

static void act_cut(Widget w, IswEvent *ev, String *p, Cardinal *n)
{
    (void)ev; (void)p; (void)n;
    Fm *fm = fm_from_widget(w);
    if (fm) {
        clipboard_cut(fm);
    }
}

static void act_paste(Widget w, IswEvent *ev, String *p, Cardinal *n)
{
    (void)ev; (void)p; (void)n;
    Fm *fm = fm_from_widget(w);
    if (fm) {
        clipboard_paste(fm);
    }
}

static void act_delete(Widget w, IswEvent *ev, String *p, Cardinal *n)
{
    (void)ev; (void)p; (void)n;
    Fm *fm = fm_from_widget(w);
    if (fm) {
        fm_delete_selected(fm);
    }
}

static void act_delete_permanent(Widget w, IswEvent *ev, String *p, Cardinal *n)
{
    (void)ev; (void)p; (void)n;
    Fm *fm = fm_from_widget(w);
    if (fm) {
        fm_delete_selected_permanent(fm);
    }
}

static void act_rename(Widget w, IswEvent *ev, String *p, Cardinal *n)
{
    (void)ev; (void)p; (void)n;
    Fm *fm = fm_from_widget(w);
    if (fm) {
        show_rename_dialog(fm);
    }
}

static void act_go_up(Widget w, IswEvent *ev, String *p, Cardinal *n)
{
    (void)ev; (void)p; (void)n;
    Fm *fm = fm_from_widget(w);
    if (!fm) {
        return;
    }
    char *parent = strdup(fm->cwd);
    char *slash = strrchr(parent, '/');
    if (slash && slash != parent) {
        *slash = '\0';
    } else {
        free(parent);
        parent = strdup("/");
    }
    fm_navigate(fm, parent);
    free(parent);
}

static void act_go_back(Widget w, IswEvent *ev, String *p, Cardinal *n)
{
    (void)ev; (void)p; (void)n;
    Fm *fm = fm_from_widget(w);
    if (!fm || fm->hist_pos <= 0) {
        return;
    }
    fm->hist_pos--;
    free(fm->cwd);
    fm->cwd = strdup(fm->history[fm->hist_pos]);
    fm_refresh(fm);
}

static void act_go_fwd(Widget w, IswEvent *ev, String *p, Cardinal *n)
{
    (void)ev; (void)p; (void)n;
    Fm *fm = fm_from_widget(w);
    if (!fm || fm->hist_pos >= fm->hist_count - 1) {
        return;
    }
    fm->hist_pos++;
    free(fm->cwd);
    fm->cwd = strdup(fm->history[fm->hist_pos]);
    fm_refresh(fm);
}

static void act_refresh(Widget w, IswEvent *ev, String *p, Cardinal *n)
{
    (void)ev; (void)p; (void)n;
    Fm *fm = fm_from_widget(w);
    if (fm) {
        fm_refresh(fm);
    }
}

static void act_open(Widget w, IswEvent *ev, String *p, Cardinal *n)
{
    (void)ev; (void)p; (void)n;
    Fm *fm = fm_from_widget(w);
    if (!fm) {
        return;
    }
    int sel = fileview_get_selected(fm);
    if (sel >= 0 && sel < fm->nentries) {
        browser_open_entry(fm, sel);
    }
}

static void act_toggle_hidden(Widget w, IswEvent *ev, String *p, Cardinal *n)
{
    (void)ev; (void)p; (void)n;
    Fm *fm = fm_from_widget(w);
    if (!fm) {
        return;
    }
    fm->show_hidden = !fm->show_hidden;
    fm_refresh(fm);
}

static void act_toggle_view(Widget w, IswEvent *ev, String *p, Cardinal *n)
{
    (void)ev; (void)p; (void)n;
    Fm *fm = fm_from_widget(w);
    if (!fm) {
        return;
    }
    fileview_set_mode(fm, fm->view_mode == FM_VIEW_ICON
                         ? FM_VIEW_LIST : FM_VIEW_ICON);
}

static void act_update_status(Widget w, IswEvent *ev,
                              String *p, Cardinal *n)
{
    (void)ev; (void)p; (void)n;
    Fm *fm = fm_from_widget(w);
    if (fm)
        fileview_update_status(fm);
}

static void act_new_window(Widget w, IswEvent *ev,
                           String *p, Cardinal *n)
{
    (void)ev; (void)p; (void)n;
    Fm *fm = fm_from_widget(w);
    if (fm) {
        fm_window_new(fm->app_state, fm->cwd);
    }
}

static void act_close_window(Widget w, IswEvent *ev,
                             String *p, Cardinal *n)
{
    (void)ev; (void)p; (void)n;
    Fm *fm = fm_from_widget(w);
    if (fm) {
        fm_window_destroy(fm);
    }
}

/* ---------- action table and translations ---------- */

static IswActionsRec fm_actions[] = {
    {"fm-update-status", act_update_status},
    {"fm-copy",          act_copy},
    {"fm-cut",           act_cut},
    {"fm-paste",         act_paste},
    {"fm-delete",        act_delete},
    {"fm-delete-permanent", act_delete_permanent},
    {"fm-rename",        act_rename},
    {"fm-go-up",         act_go_up},
    {"fm-go-back",       act_go_back},
    {"fm-go-fwd",        act_go_fwd},
    {"fm-refresh",       act_refresh},
    {"fm-open",          act_open},
    {"fm-toggle-hidden", act_toggle_hidden},
    {"fm-toggle-view", act_toggle_view},
    {"fm-new-window", act_new_window},
    {"fm-close-window", act_close_window},
};

static char fm_translations[] =
    "Ctrl<Key>c:        fm-copy()\n"
    "Ctrl<Key>x:        fm-cut()\n"
    "Ctrl<Key>v:        fm-paste()\n"
    "Shift<Key>Delete:    fm-delete-permanent()\n"
    "Shift<Key>KP_Delete: fm-delete-permanent()\n"
    "<Key>Delete:        fm-delete()\n"
    "<Key>KP_Delete:     fm-delete()\n"
    "<Key>F2:            fm-rename()\n"
    "<Key>BackSpace:     fm-go-up()\n"
    "Alt<Key>Left:       fm-go-back()\n"
    "Alt<Key>Right:      fm-go-fwd()\n"
    "<Key>F5:            fm-refresh()\n"
    "Ctrl<Key>h:        fm-toggle-hidden()\n"
    "Ctrl<Key>l:        fm-toggle-view()\n"
    "Ctrl<Key>n:        fm-new-window()\n";

void fm_register_actions(IswAppContext app)
{
    IswAppAddActions(app, fm_actions, IswNumber(fm_actions));
}

void fm_install_shortcuts(Widget w)
{
    IswOverrideTranslations(w, IswParseTranslationTable(fm_translations));
}
