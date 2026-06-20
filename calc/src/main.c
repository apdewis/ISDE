/*
 * isde-calc - a calculator for the ISDE desktop environment
 *
 * Based on xcalc by John H. Bradley, Mark Rosenstein, and Donna Converse.
 * Ported to libISW with ISDE theming support.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <signal.h>
#include <stdint.h>

#include <ISW/Shell.h>
#include <ISW/Cardinals.h>
#include <ISW/Form.h>
#include <ISW/Label.h>
#include <ISW/Command.h>
#include <ISW/Toggle.h>


#include "isde-theme.h"
#include "dbus.h"
#include <ISW/IswArgMacros.h>
#include <ISW/IswDragDrop.h>
#include <ISW/ISWPlatform.h>

#include "calc.h"

static Boolean convert(Widget w, Atom *selection, Atom *target,
                       Atom *type, IswPointer *value,
                       unsigned long *length, int *format);
static void create_keypad(Widget parent);
static void create_display(Widget parent);
static void create_calculator(Widget shell);
static void done(Widget w, Atom *selection, Atom *target);
static void lose(Widget w, Atom *selection);

/* global data */
int     rpn = 0;
char    dispstr[LCD_STR_LEN];

/* local data */
static Widget       toplevel = NULL;
static Widget       calculator = NULL;
static Widget       LCD = NULL;
static Widget       bevel_w = NULL;
static Widget       ind[9];
static Widget       buttons[55];
static int          nbuttons;
static char         selstr[LCD_STR_LEN];
static IswAppContext app;
static IsdeDBus    *dbus_conn;

static int opt_rpn = 0;

#define COLS_TI  5
#define ROWS_TI  11
#define COLS_HP  10
#define ROWS_HP  4
#define BTN_W    50
#define BTN_H    26
#define BTN_GAP  2
#define BTN_LEFT 4
#define BTN_TOP  12

/* ---------- theme reload ---------- */

static void on_settings_changed(const char *section, const char *key,
                                void *user_data)
{
    (void)key; (void)user_data;
    if (strcmp(section, "appearance") != 0 && strcmp(section, "*") != 0) {
        return;
    }
    isde_theme_reload();
    isde_theme_merge_xrm(toplevel);
    IswReloadResources(toplevel);
}

static void dbus_input_cb(IswPointer client_data, int *fd, IswInputId *id)
{
    (void)fd; (void)id;
    IsdeDBus *bus = (IsdeDBus *)client_data;
    isde_dbus_dispatch(bus);
}


/* ---------- entry point ---------- */

int
main(int argc, char **argv)
{
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-rpn") == 0) {
            opt_rpn = 1;
        }
    }

    toplevel = IswAppInitialize(&app, "ISDE-Calc", NULL, 0,
                                &argc, argv, NULL, NULL, 0);
    isde_theme_merge_xrm(toplevel);

    IswArgBuilder ab = IswArgBuilderInit();
    IswArgInput(&ab, True);
    IswSetValues(toplevel, ab.args, ab.count);

    create_calculator(toplevel);

    IswAppAddActions(app, Actions, ActionsCount);

    IswOverrideTranslations(toplevel,
        IswParseTranslationTable("<Message>WM_PROTOCOLS: quit()\n"));

    {
        static char *ti_keys =
            "Ctrl<Key>c:quit()\n"
            "Ctrl<Key>h:clear()\n"
            "None<Key>0:digit(0)\nNone<Key>1:digit(1)\n"
            "None<Key>2:digit(2)\nNone<Key>3:digit(3)\n"
            "None<Key>4:digit(4)\nNone<Key>5:digit(5)\n"
            "None<Key>6:digit(6)\nNone<Key>7:digit(7)\n"
            "None<Key>8:digit(8)\nNone<Key>9:digit(9)\n"
            "Shift<Key>a:digit(A)\nShift<Key>b:digit(B)\n"
            "Shift<Key>c:digit(C)\nShift<Key>d:digit(D)\n"
            "Shift<Key>e:digit(E)\nShift<Key>f:digit(F)\n"
            "<Key>KP_0:digit(0)\n<Key>KP_1:digit(1)\n"
            "<Key>KP_2:digit(2)\n<Key>KP_3:digit(3)\n"
            "<Key>KP_4:digit(4)\n<Key>KP_5:digit(5)\n"
            "<Key>KP_6:digit(6)\n<Key>KP_7:digit(7)\n"
            "<Key>KP_8:digit(8)\n<Key>KP_9:digit(9)\n"
            "<Key>KP_Enter:equal()\n<Key>KP_Equal:equal()\n"
            "<Key>Return:equal()\n"
            "<Key>KP_Multiply:multiply()\n<Key>KP_Add:add()\n"
            "<Key>KP_Subtract:subtract()\n<Key>KP_Decimal:decimal()\n"
            "<Key>KP_Divide:divide()\n"
            ":<Key>.:decimal()\n:<Key>+:add()\n:<Key>-:subtract()\n"
            ":<Key>*:multiply()\n:<Key>/:divide()\n"
            ":<Key>(:leftParen()\n:<Key>):rightParen()\n"
            ":<Key>!:factorial()\n:<Key>|:or()\n:<Key>&:and()\n"
            ":<Key><:shl()\n:<Key>>:shr()\n:<Key>~:not()\n"
            ":<Key>%:mod()\n<Key>x:xor()\n"
            "<Key>e:e()\n:<Key>^:power()\n<Key>p:pi()\n"
            "<Key>i:inverse()\n<Key>s:sine()\n<Key>c:cosine()\n"
            "<Key>t:tangent()\n<Key>d:degree()\n<Key>l:naturalLog()\n"
            ":<Key>=:equal()\n<Key>n:negate()\n<Key>r:squareRoot()\n"
            "<Key>space:clear()\n<Key>q:quit()\n"
            "<Key>Delete:clear()\n<Key>BackSpace:clear()\n";
        static char *hp_keys =
            "Ctrl<Key>c:quit()\n"
            "Ctrl<Key>h:back()\n"
            "None<Key>0:digit(0)\nNone<Key>1:digit(1)\n"
            "None<Key>2:digit(2)\nNone<Key>3:digit(3)\n"
            "None<Key>4:digit(4)\nNone<Key>5:digit(5)\n"
            "None<Key>6:digit(6)\nNone<Key>7:digit(7)\n"
            "None<Key>8:digit(8)\nNone<Key>9:digit(9)\n"
            "<Key>KP_0:digit(0)\n<Key>KP_1:digit(1)\n"
            "<Key>KP_2:digit(2)\n<Key>KP_3:digit(3)\n"
            "<Key>KP_4:digit(4)\n<Key>KP_5:digit(5)\n"
            "<Key>KP_6:digit(6)\n<Key>KP_7:digit(7)\n"
            "<Key>KP_8:digit(8)\n<Key>KP_9:digit(9)\n"
            "<Key>KP_Enter:enter()\n"
            "<Key>KP_Multiply:multiply()\n<Key>KP_Add:add()\n"
            "<Key>KP_Subtract:subtract()\n<Key>KP_Decimal:decimal()\n"
            "<Key>KP_Divide:divide()\n"
            ":<Key>.:decimal()\n:<Key>+:add()\n:<Key>-:subtract()\n"
            ":<Key>*:multiply()\n:<Key>/:divide()\n"
            ":<Key>!:factorial()\n"
            "<Key>e:e()\n:<Key>^:power()\n<Key>p:pi()\n"
            "<Key>i:inverse()\n<Key>s:sine()\n<Key>c:cosine()\n"
            "<Key>t:tangent()\n<Key>d:degree()\n<Key>l:naturalLog()\n"
            "<Key>n:negate()\n<Key>r:squareRoot()\n"
            "<Key>space:clear()\n<Key>q:quit()\n"
            "<Key>Delete:back()\n<Key>BackSpace:back()\n"
            "<Key>Return:enter()\n<Key>x:XexchangeY()\n";
        IswOverrideTranslations(LCD,
            IswParseTranslationTable(opt_rpn ? hp_keys : ti_keys));
    }

    IswRealizeWidget(toplevel);

    Dimension cur_w = 0, cur_h = 0;
    IswArgBuilderReset(&ab);
    IswArgWidth(&ab, &cur_w);
    IswArgHeight(&ab, &cur_h);
    IswGetValues(toplevel, ab.args, ab.count);
    IswArgBuilderReset(&ab);
    IswArgMinWidth(&ab, cur_w);
    IswArgMinHeight(&ab, cur_h);
    IswArgMaxWidth(&ab, cur_w);
    IswArgMaxHeight(&ab, cur_h);
    IswSetValues(toplevel, ab.args, ab.count);

    /* WM_DELETE_WINDOW — the shell's WM_PROTOCOLS translation (installed
     * above) causes ISW to set this up during realize. */

    /* D-Bus theme watching */
    dbus_conn = isde_dbus_init();
    if (dbus_conn) {
        isde_dbus_settings_subscribe(dbus_conn, on_settings_changed, NULL);
        int dbus_fd = isde_dbus_get_fd(dbus_conn);
        if (dbus_fd >= 0) {
            IswAppAddInput(app, dbus_fd, (IswPointer)IswInputReadMask,
                          dbus_input_cb, dbus_conn);
        }
    }

#ifndef IEEE
    signal(SIGFPE, fperr);
    signal(SIGILL, illerr);
#endif
    ResetCalc();
    IswAppMainLoop(app);

    return 0;
}

static void create_calculator(Widget shell)
{
    rpn = opt_rpn;
    IswArgBuilder ab = IswArgBuilderInit();
    IswArgWidth(&ab, shell->core.width);
    calculator = IswCreateManagedWidget(rpn ? "hp" : "ti", formWidgetClass,
                                        shell, ab.args, ab.count);
    create_display(calculator);
    create_keypad(calculator);
    IswSetKeyboardFocus(calculator, LCD);
}

static void create_display(Widget parent)
{
    Widget screen;
    IswArgBuilder ab = IswArgBuilderInit();

    IswArgJustify(&ab, IswJustifyRight);
    bevel_w = IswCreateManagedWidget("bevel", formWidgetClass, parent,
                                    ab.args, ab.count);

    IswArgBuilderReset(&ab);
    IswArgJustify(&ab, IswJustifyRight);
    screen = IswCreateManagedWidget("screen", formWidgetClass, bevel_w,
                                    ab.args, ab.count);

    IswArgBuilderReset(&ab);
    IswArgBorderWidth(&ab, 0);
    IswArgJustify(&ab, IswJustifyRight);
    ind[XCalc_MEMORY] = IswCreateManagedWidget("M", labelWidgetClass, screen,
                                                ab.args, ab.count);

    IswArgBuilderReset(&ab);
    IswArgRight(&ab, IswChainRight);
    IswArgWidth(&ab, ((BTN_W * 5) - (BTN_GAP * 2)));
    IswArgBorderWidth(&ab, 0);
    IswArgJustify(&ab, IswJustifyRight);
    LCD = IswCreateManagedWidget("LCD", labelWidgetClass, screen, ab.args,
                                 ab.count);

    IswArgBuilderReset(&ab);
    IswArgFromVert(&ab, LCD);
    IswArgBorderWidth(&ab, 0);
    ind[XCalc_INVERSE] = IswCreateManagedWidget("INV", labelWidgetClass,
                                                 screen, ab.args, ab.count);

    IswArgFromHoriz(&ab, ind[XCalc_INVERSE]);
    ind[XCalc_DEGREE] = IswCreateManagedWidget("DEG", labelWidgetClass, screen,
                                                ab.args, ab.count);

    ind[XCalc_RADIAN] = IswCreateManagedWidget("RAD", labelWidgetClass, screen,
                                                ab.args, ab.count);

    ind[XCalc_GRADAM] = IswCreateManagedWidget("GRAD", labelWidgetClass, screen,
                                                ab.args, ab.count);
    IswArgFromHoriz(&ab, ind[XCalc_DEGREE]);
    ind[XCalc_PAREN] = IswCreateManagedWidget("P", labelWidgetClass, screen,
                                               ab.args, ab.count);

    IswArgFromHoriz(&ab, ind[XCalc_PAREN]);

    ind[XCalc_HEX] = IswCreateManagedWidget("HEX", labelWidgetClass, screen,
                                             ab.args, ab.count);

    ind[XCalc_DEC] = IswCreateManagedWidget("DEC", labelWidgetClass, screen,
                                             ab.args, ab.count);

    ind[XCalc_OCT] = IswCreateManagedWidget("OCT", labelWidgetClass, screen,
                                             ab.args, ab.count);
}

static int ti_ops[] = {
    kRECIP, kSQR, kSQRT, kCLR, kOFF,
    kINV, kSIN, kCOS, kTAN, kDRG,
    kE, kEE, kLOG, kLN, kPOW,
    kNOT, kAND, kOR, kXOR, kTRUNC,
    kPI, kFACT, kLPAR, kRPAR, kBASE,
    kSHL, kxD, kxE, kxF, kSHR,
    kMOD, kxA, kxB, kxC, kDIV,
    kSTO, kSEVEN, kEIGHT, kNINE, kMUL,
    kRCL, kFOUR, kFIVE, kSIX, kSUB,
    kSUM, kONE, kTWO, kTHREE, kADD,
    kEXC, kZERO, kDEC, kNEG, kEQU,
};

static int hp_ops[] = {
    kSQRT, kEXP, k10X, kPOW, kRECIP,
    kNEG, kSEVEN, kEIGHT, kNINE, kDIV,
    kFACT, kPI, kSIN, kCOS, kTAN,
    kEE, kFOUR, kFIVE, kSIX, kMUL,
    kNOP, kNOP, kROLL, kXXY, kBKSP,
    kENTR, kONE, kTWO, kTHREE, kSUB,
    kOFF, kDRG, kINV, kSTO, kRCL,
    kZERO, kDEC, kSUM, kADD,
};

static void button_cb(Widget w, IswPointer client_data, IswPointer call_data)
{
    (void)w; (void)call_data;
    int op = (int)(intptr_t)client_data;
    calc_do_op(op);
}

static char *ti_labels[] = {
    "1/x", "x^2", "sqrt", "CE/C", "AC",
    "INV", "sin", "cos", "tan", "DRG",
    "e", "EE", "log", "ln", "y^x",
    "not", "and", "or", "xor", "trunc",
    "pi", "x!", "(", ")", "base",
    "shl", "D", "E", "F", "shr",
    "mod", "A", "B", "C", "/",
    "STO", "7", "8", "9", "*",
    "RCL", "4", "5", "6", "-",
    "SUM", "1", "2", "3", "+",
    "EXC", "0", ".", "+/-", "=",
};

static char *hp_labels[] = {
    "sqrt", "e^x", "10^x", "y^x", "1/x",
    "CHS", "7", "8", "9", "/",
    "x!", "pi", "sin", "cos", "tan",
    "EEX", "4", "5", "6", "*",
    "", "", "Rv", "x:y", "<-",
    "ENTER", "1", "2", "3", "-",
    "ON", "DRG", "INV", "STO", "RCL",
    "0", ".", "SUM", "+",
};



static void create_keypad(Widget parent)
{
    char **labels = opt_rpn ? hp_labels : ti_labels;
    int cols = opt_rpn ? COLS_HP : COLS_TI;
    int total = opt_rpn ? 39 : 55;

    int *ops = opt_rpn ? hp_ops : ti_ops;

    nbuttons = total;
    for (int i = 0; i < total; i++) {
        int row = i / cols;
        int col = i % cols;
        IswArgBuilder ab = IswArgBuilderInit();

        IswArgLabel(&ab, labels[i]);
        IswArgWidth(&ab, BTN_W);
        IswArgHeight(&ab, BTN_H);
        IswArgJustify(&ab, IswJustifyCenter);

        if (col == 0) {
            IswArgHorizDistance(&ab, BTN_LEFT);
        }
        if (col > 0) {
            IswArgFromHoriz(&ab, buttons[i - 1]);
            IswArgHorizDistance(&ab, BTN_GAP);
        }
        if (row == 0) {
            IswArgFromVert(&ab, bevel_w);
            IswArgVertDistance(&ab, BTN_TOP);
        }
        if (row > 0) {
            IswArgFromVert(&ab, buttons[i - cols]);
            IswArgVertDistance(&ab, BTN_GAP);
        }

        char name[16];
        snprintf(name, sizeof(name), "button%d", i + 1);
        buttons[i] = IswCreateManagedWidget(name, commandWidgetClass,
                                            parent, ab.args, ab.count);
        IswAddCallback(buttons[i], IswNcallback, button_cb,
                       (IswPointer)(intptr_t)ops[i]);
    }
}

/* ---------- display helpers ---------- */

void draw(char *string)
{
    IswArgBuilder ab = IswArgBuilderInit();
    IswArgLabel(&ab, string);
    IswSetValues(LCD, ab.args, ab.count);
}

void setflag(int indicator, Boolean on)
{
    if (on) {
        IswMapWidget(ind[indicator]);
    } else {
        IswUnmapWidget(ind[indicator]);
    }
}

void ringbell(void)
{
    /* TODO: needs a neutral ISW bell API */
}

void Quit(void)
{
    if (dbus_conn) {
        isde_dbus_free(dbus_conn);
    }
    IswDestroyApplicationContext(app);
    exit(0);
}

/* ---------- selection support ---------- */

static Atom atom_primary;
static Atom atom_string;

static void ensure_sel_atoms(void)
{
    if (atom_primary) return;
    IswDisplay dpy = IswDisplayOf(toplevel);
    atom_primary = _IswPlatformInternAtomOp(dpy, "PRIMARY", False);
    atom_string  = _IswPlatformInternAtomOp(dpy, "STRING", False);
}

static Boolean convert(Widget w, Atom *selection, Atom *target,
                       Atom *type, IswPointer *value,
                       unsigned long *length, int *format)
{
    (void)w; (void)selection;
    ensure_sel_atoms();
    if (*target == atom_string) {
        *type = atom_string;
        *length = strlen(dispstr);
        memcpy(selstr, dispstr, (size_t)(*length));
        *value = selstr;
        *format = 8;
        return True;
    }
    return False;
}

static void lose(Widget w, Atom *selection)
{
    (void)w; (void)selection;
    IswToggleUnsetCurrent(LCD);
}

static void done(Widget w, Atom *selection, Atom *target)
{
    (void)w; (void)selection; (void)target;
    selstr[0] = '\0';
}

void do_select(IswTime time)
{
    Boolean state;
    IswArgBuilder ab = IswArgBuilderInit();

    IswArgState(&ab, &state);
    IswGetValues(LCD, ab.args, ab.count);

    ensure_sel_atoms();
    if (state) {
        IswOwnSelection(LCD, atom_primary, time, convert, lose, done);
    } else {
        selstr[0] = '\0';
    }
}
