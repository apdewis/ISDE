/*
 * actions.c - action handlers for isde-calc
 *
 * Based on xcalc by Donna Converse, MIT X Consortium.
 * Ported to libISW.
 */

#include <ISW/Intrinsic.h>
#include <setjmp.h>
#include "calc.h"

#ifndef IEEE
#define XCALC_PRE_OP(keynum) do { if (pre_op(keynum)) return; \
                       if (setjmp (env)) {fail_op(); return;}} while (0)
#else
#define XCALC_PRE_OP(keynum) do { if (pre_op(keynum)) return; } while (0)
#endif

static void add(Widget w, IswEvent *ev, String *vector, Cardinal *count);
static void and(Widget w, IswEvent *ev, String *vector, Cardinal *count);
static void back(Widget w, IswEvent *ev, String *vector, Cardinal *count);
static void base(Widget w, IswEvent *ev, String *vector, Cardinal *count);
static void bell(Widget w, IswEvent *ev, String *vector, Cardinal *count);
static void clearit(Widget w, IswEvent *ev, String *vector, Cardinal *count);
static void cosine(Widget w, IswEvent *ev, String *vector, Cardinal *count);
static void decimal(Widget w, IswEvent *ev, String *vector, Cardinal *count);
static void degree(Widget w, IswEvent *ev, String *vector, Cardinal *count);
static void digit(Widget w, IswEvent *ev, String *vector, Cardinal *count);
static void divide(Widget w, IswEvent *ev, String *vector, Cardinal *count);
static void e(Widget w, IswEvent *ev, String *vector, Cardinal *count);
static void enter(Widget w, IswEvent *ev, String *vector, Cardinal *count);
static void epower(Widget w, IswEvent *ev, String *vector, Cardinal *count);
static void equal(Widget w, IswEvent *ev, String *vector, Cardinal *count);
static void exchange(Widget w, IswEvent *ev, String *vector, Cardinal *count);
static void factorial(Widget w, IswEvent *ev, String *vector, Cardinal *count);
static void inverse(Widget w, IswEvent *ev, String *vector, Cardinal *count);
static void leftParen(Widget w, IswEvent *ev, String *vector, Cardinal *count);
static void logarithm(Widget w, IswEvent *ev, String *vector, Cardinal *count);
static void mod(Widget w, IswEvent *ev, String *vector, Cardinal *count);
static void multiply(Widget w, IswEvent *ev, String *vector, Cardinal *count);
static void naturalLog(Widget w, IswEvent *ev, String *vector, Cardinal *count);
static void negate(Widget w, IswEvent *ev, String *vector, Cardinal *count);
static void nop(Widget w, IswEvent *ev, String *vector, Cardinal *count);
static void not(Widget w, IswEvent *ev, String *vector, Cardinal *count);
static void off(Widget w, IswEvent *ev, String *vector, Cardinal *count);
static void or(Widget w, IswEvent *ev, String *vector, Cardinal *count);
static void pi(Widget w, IswEvent *ev, String *vector, Cardinal *count);
static void power(Widget w, IswEvent *ev, String *vector, Cardinal *count);
static void quit(Widget w, IswEvent *ev, String *vector, Cardinal *count);
static void recall(Widget w, IswEvent *ev, String *vector, Cardinal *count);
static void reciprocal(Widget w, IswEvent *ev, String *vector, Cardinal *count);
static void rightParen(Widget w, IswEvent *ev, String *vector, Cardinal *count);
static void roll(Widget w, IswEvent *ev, String *vector, Cardinal *count);
static void scientific(Widget w, IswEvent *ev, String *vector, Cardinal *count);
static void selection(Widget w, IswEvent *ev, String *vector, Cardinal *count);
static void shl(Widget w, IswEvent *ev, String *vector, Cardinal *count);
static void shr(Widget w, IswEvent *ev, String *vector, Cardinal *count);
static void sine(Widget w, IswEvent *ev, String *vector, Cardinal *count);
static void square(Widget w, IswEvent *ev, String *vector, Cardinal *count);
static void squareRoot(Widget w, IswEvent *ev, String *vector, Cardinal *count);
static void store(Widget w, IswEvent *ev, String *vector, Cardinal *count);
static void subtract(Widget w, IswEvent *ev, String *vector, Cardinal *count);
static void sum(Widget w, IswEvent *ev, String *vector, Cardinal *count);
static void tangent(Widget w, IswEvent *ev, String *vector, Cardinal *count);
static void tenpower(Widget w, IswEvent *ev, String *vector, Cardinal *count);
static void xtrunc(Widget w, IswEvent *ev, String *vector, Cardinal *count);
static void xor(Widget w, IswEvent *ev, String *vector, Cardinal *count);
static void XexchangeY(Widget w, IswEvent *ev, String *vector, Cardinal *count);

IswActionsRec Actions[] = {
    {"add",         add},
    {"and",         and},
    {"back",        back},
    {"base",        base},
    {"bell",        bell},
    {"clear",       clearit},
    {"cosine",      cosine},
    {"decimal",     decimal},
    {"degree",      degree},
    {"digit",       digit},
    {"divide",      divide},
    {"e",           e},
    {"enter",       enter},
    {"epower",      epower},
    {"equal",       equal},
    {"exchange",    exchange},
    {"factorial",   factorial},
    {"inverse",     inverse},
    {"leftParen",   leftParen},
    {"logarithm",   logarithm},
    {"mod",         mod},
    {"multiply",    multiply},
    {"naturalLog",  naturalLog},
    {"negate",      negate},
    {"nop",         nop},
    {"not",         not},
    {"off",         off},
    {"or",          or},
    {"pi",          pi},
    {"power",       power},
    {"quit",        quit},
    {"recall",      recall},
    {"reciprocal",  reciprocal},
    {"rightParen",  rightParen},
    {"roll",        roll},
    {"scientific",  scientific},
    {"selection",   selection},
    {"shl",         shl},
    {"shr",         shr},
    {"sine",        sine},
    {"square",      square},
    {"squareRoot",  squareRoot},
    {"store",       store},
    {"subtract",    subtract},
    {"sum",         sum},
    {"tangent",     tangent},
    {"tenpower",    tenpower},
    {"trunc",       xtrunc},
    {"xor",         xor},
    {"XexchangeY",  XexchangeY}
};

int ActionsCount = IswNumber(Actions);

void calc_do_op(int op)
{
    XCALC_PRE_OP(op);
    switch (op) {
    case kADD: case kSUB: case kMUL: case kDIV: case kPOW:
    case kMOD: case kAND: case kOR: case kXOR: case kSHL: case kSHR:
        rpn ? twof(op) : twoop(op);
        break;
    case kXXY:
        twof(op);
        break;
    case kRECIP: case kSQR: case kSQRT: case kSIN: case kCOS: case kTAN:
    case kLOG: case kLN: case kEXP: case k10X: case kE: case kPI:
    case kFACT: case kNOT: case kTRUNC:
        oneop(op);
        break;
    case kSTO:
        rpn ? memf(kSTO) : oneop(kSTO);
        break;
    case kRCL:
        rpn ? memf(kRCL) : oneop(kRCL);
        break;
    case kSUM:
        rpn ? memf(kSUM) : oneop(kSUM);
        break;
    case kEXC:
        oneop(kEXC);
        break;
    case kCLR:   clearf();   break;
    case kOFF:   offf();     break;
    case kINV:   invf();     break;
    case kDRG:   drgf();     break;
    case kEE:    eef();      break;
    case kDEC:   decf();     break;
    case kNEG:   negf();     break;
    case kBKSP:  bkspf();    break;
    case kLPAR:  lparf();    break;
    case kRPAR:  rparf();    break;
    case kENTR:  entrf();    break;
    case kEQU:   equf();     break;
    case kROLL:  rollf();    break;
    case kBASE:  change_base(); break;
    case kZERO: case kONE: case kTWO: case kTHREE: case kFOUR:
    case kFIVE: case kSIX: case kSEVEN: case kEIGHT: case kNINE:
    case kxA: case kxB: case kxC: case kxD: case kxE: case kxF:
        numeric(op);
        break;
    case kNOP:
        ringbell();
        break;
    }
    post_op();
}

static void add(Widget w, IswEvent *ev, String *vector, Cardinal *count)
{
    (void)w; (void)ev; (void)vector; (void)count;
    XCALC_PRE_OP(kADD);
    rpn ? twof(kADD) : twoop(kADD);
    post_op();
}

static void and(Widget w, IswEvent *ev, String *vector, Cardinal *count)
{
    (void)w; (void)ev; (void)vector; (void)count;
    XCALC_PRE_OP(kAND);
    rpn ? twof(kAND) : twoop(kAND);
    post_op();
}

static void back(Widget w, IswEvent *ev, String *vector, Cardinal *count)
{
    (void)w; (void)ev; (void)vector; (void)count;
    XCALC_PRE_OP(kBKSP);
    bkspf();
    post_op();
}

static void base(Widget w, IswEvent *ev, String *vector, Cardinal *count)
{
    (void)w; (void)ev; (void)vector; (void)count;
    XCALC_PRE_OP(kBASE);
    change_base();
    post_op();
}

static void bell(Widget w, IswEvent *ev, String *vector, Cardinal *count)
{
    (void)w; (void)ev; (void)vector; (void)count;
    ringbell();
}

static void clearit(Widget w, IswEvent *ev, String *vector, Cardinal *count)
{
    (void)w; (void)ev; (void)vector; (void)count;
    XCALC_PRE_OP(kCLR);
    clearf();
    post_op();
}

static void cosine(Widget w, IswEvent *ev, String *vector, Cardinal *count)
{
    (void)w; (void)ev; (void)vector; (void)count;
    XCALC_PRE_OP(kCOS);
    oneop(kCOS);
    post_op();
}

static void decimal(Widget w, IswEvent *ev, String *vector, Cardinal *count)
{
    (void)w; (void)ev; (void)vector; (void)count;
    XCALC_PRE_OP(kDEC);
    decf();
    post_op();
}

static void degree(Widget w, IswEvent *ev, String *vector, Cardinal *count)
{
    (void)w; (void)ev; (void)vector; (void)count;
    XCALC_PRE_OP(kDRG);
    drgf();
    post_op();
}

static void digit(Widget w, IswEvent *ev, String *vector, Cardinal *count)
{
    (void)w; (void)ev; (void)count;
    switch (vector[0][0]) {
    case '0': XCALC_PRE_OP(kZERO); numeric(kZERO); break;
    case '1': XCALC_PRE_OP(kONE); numeric(kONE); break;
    case '2': XCALC_PRE_OP(kTWO); numeric(kTWO); break;
    case '3': XCALC_PRE_OP(kTHREE); numeric(kTHREE); break;
    case '4': XCALC_PRE_OP(kFOUR); numeric(kFOUR); break;
    case '5': XCALC_PRE_OP(kFIVE); numeric(kFIVE); break;
    case '6': XCALC_PRE_OP(kSIX); numeric(kSIX); break;
    case '7': XCALC_PRE_OP(kSEVEN); numeric(kSEVEN); break;
    case '8': XCALC_PRE_OP(kEIGHT); numeric(kEIGHT); break;
    case '9': XCALC_PRE_OP(kNINE); numeric(kNINE); break;
    case 'A': XCALC_PRE_OP(kxA); numeric(kxA); break;
    case 'B': XCALC_PRE_OP(kxB); numeric(kxB); break;
    case 'C': XCALC_PRE_OP(kxC); numeric(kxC); break;
    case 'D': XCALC_PRE_OP(kxD); numeric(kxD); break;
    case 'E': XCALC_PRE_OP(kxE); numeric(kxE); break;
    case 'F': XCALC_PRE_OP(kxF); numeric(kxF); break;
    }
    post_op();
}

static void divide(Widget w, IswEvent *ev, String *vector, Cardinal *count)
{
    (void)w; (void)ev; (void)vector; (void)count;
    XCALC_PRE_OP(kDIV);
    rpn ? twof(kDIV) : twoop(kDIV);
    post_op();
}

static void e(Widget w, IswEvent *ev, String *vector, Cardinal *count)
{
    (void)w; (void)ev; (void)vector; (void)count;
    XCALC_PRE_OP(kE);
    oneop(kE);
    post_op();
}

static void enter(Widget w, IswEvent *ev, String *vector, Cardinal *count)
{
    (void)w; (void)ev; (void)vector; (void)count;
    XCALC_PRE_OP(kENTR);
    entrf();
    post_op();
}

static void epower(Widget w, IswEvent *ev, String *vector, Cardinal *count)
{
    (void)w; (void)ev; (void)vector; (void)count;
    XCALC_PRE_OP(kEXP);
    oneop(kEXP);
    post_op();
}

static void equal(Widget w, IswEvent *ev, String *vector, Cardinal *count)
{
    (void)w; (void)ev; (void)vector; (void)count;
    XCALC_PRE_OP(kEQU);
    equf();
    post_op();
}

static void exchange(Widget w, IswEvent *ev, String *vector, Cardinal *count)
{
    (void)w; (void)ev; (void)vector; (void)count;
    XCALC_PRE_OP(kEXC);
    oneop(kEXC);
    post_op();
}

static void factorial(Widget w, IswEvent *ev, String *vector, Cardinal *count)
{
    (void)w; (void)ev; (void)vector; (void)count;
    XCALC_PRE_OP(kFACT);
    oneop(kFACT);
    post_op();
}

static void inverse(Widget w, IswEvent *ev, String *vector, Cardinal *count)
{
    (void)w; (void)ev; (void)vector; (void)count;
    XCALC_PRE_OP(kINV);
    invf();
    post_op();
}

static void leftParen(Widget w, IswEvent *ev, String *vector, Cardinal *count)
{
    (void)w; (void)ev; (void)vector; (void)count;
    XCALC_PRE_OP(kLPAR);
    lparf();
    post_op();
}

static void logarithm(Widget w, IswEvent *ev, String *vector, Cardinal *count)
{
    (void)w; (void)ev; (void)vector; (void)count;
    XCALC_PRE_OP(kLOG);
    oneop(kLOG);
    post_op();
}

static void mod(Widget w, IswEvent *ev, String *vector, Cardinal *count)
{
    (void)w; (void)ev; (void)vector; (void)count;
    XCALC_PRE_OP(kMOD);
    rpn ? twof(kMOD) : twoop(kMOD);
    post_op();
}

static void multiply(Widget w, IswEvent *ev, String *vector, Cardinal *count)
{
    (void)w; (void)ev; (void)vector; (void)count;
    XCALC_PRE_OP(kMUL);
    rpn ? twof(kMUL) : twoop(kMUL);
    post_op();
}

static void naturalLog(Widget w, IswEvent *ev, String *vector, Cardinal *count)
{
    (void)w; (void)ev; (void)vector; (void)count;
    XCALC_PRE_OP(kLN);
    oneop(kLN);
    post_op();
}

static void negate(Widget w, IswEvent *ev, String *vector, Cardinal *count)
{
    (void)w; (void)ev; (void)vector; (void)count;
    XCALC_PRE_OP(kNEG);
    negf();
    post_op();
}

static void nop(Widget w, IswEvent *ev, String *vector, Cardinal *count)
{
    (void)w; (void)ev; (void)vector; (void)count;
    ringbell();
}

static void not(Widget w, IswEvent *ev, String *vector, Cardinal *count)
{
    (void)w; (void)ev; (void)vector; (void)count;
    XCALC_PRE_OP(kNOT);
    oneop(kNOT);
    post_op();
}

static void off(Widget w, IswEvent *ev, String *vector, Cardinal *count)
{
    (void)w; (void)ev; (void)vector; (void)count;
    XCALC_PRE_OP(kOFF);
    offf();
    post_op();
}

static void or(Widget w, IswEvent *ev, String *vector, Cardinal *count)
{
    (void)w; (void)ev; (void)vector; (void)count;
    XCALC_PRE_OP(kOR);
    rpn ? twof(kOR) : twoop(kOR);
    post_op();
}

static void pi(Widget w, IswEvent *ev, String *vector, Cardinal *count)
{
    (void)w; (void)ev; (void)vector; (void)count;
    XCALC_PRE_OP(kPI);
    oneop(kPI);
    post_op();
}

static void power(Widget w, IswEvent *ev, String *vector, Cardinal *count)
{
    (void)w; (void)ev; (void)vector; (void)count;
    XCALC_PRE_OP(kPOW);
    rpn ? twof(kPOW) : twoop(kPOW);
    post_op();
}

static void quit(Widget w, IswEvent *ev, String *vector, Cardinal *count)
{
    (void)w; (void)ev; (void)vector; (void)count;
    Quit();
}

static void recall(Widget w, IswEvent *ev, String *vector, Cardinal *count)
{
    (void)w; (void)ev; (void)vector; (void)count;
    XCALC_PRE_OP(kRCL);
    rpn ? memf(kRCL) : oneop(kRCL);
    post_op();
}

static void reciprocal(Widget w, IswEvent *ev, String *vector, Cardinal *count)
{
    (void)w; (void)ev; (void)vector; (void)count;
    XCALC_PRE_OP(kRECIP);
    oneop(kRECIP);
    post_op();
}

static void rightParen(Widget w, IswEvent *ev, String *vector, Cardinal *count)
{
    (void)w; (void)ev; (void)vector; (void)count;
    XCALC_PRE_OP(kRPAR);
    rparf();
    post_op();
}

static void roll(Widget w, IswEvent *ev, String *vector, Cardinal *count)
{
    (void)w; (void)ev; (void)vector; (void)count;
    XCALC_PRE_OP(kROLL);
    rollf();
    post_op();
}

static void scientific(Widget w, IswEvent *ev, String *vector, Cardinal *count)
{
    (void)w; (void)ev; (void)vector; (void)count;
    XCALC_PRE_OP(kEE);
    eef();
    post_op();
}

static void selection(Widget w, IswEvent *ev, String *vector, Cardinal *count)
{
    (void)w; (void)vector; (void)count;
    do_select(ev->any.time);
}

static void shl(Widget w, IswEvent *ev, String *vector, Cardinal *count)
{
    (void)w; (void)ev; (void)vector; (void)count;
    XCALC_PRE_OP(kSHL);
    rpn ? twof(kSHL) : twoop(kSHL);
    post_op();
}

static void shr(Widget w, IswEvent *ev, String *vector, Cardinal *count)
{
    (void)w; (void)ev; (void)vector; (void)count;
    XCALC_PRE_OP(kSHR);
    rpn ? twof(kSHR) : twoop(kSHR);
    post_op();
}

static void sine(Widget w, IswEvent *ev, String *vector, Cardinal *count)
{
    (void)w; (void)ev; (void)vector; (void)count;
    XCALC_PRE_OP(kSIN);
    oneop(kSIN);
    post_op();
}

static void square(Widget w, IswEvent *ev, String *vector, Cardinal *count)
{
    (void)w; (void)ev; (void)vector; (void)count;
    XCALC_PRE_OP(kSQR);
    oneop(kSQR);
    post_op();
}

static void squareRoot(Widget w, IswEvent *ev, String *vector, Cardinal *count)
{
    (void)w; (void)ev; (void)vector; (void)count;
    XCALC_PRE_OP(kSQRT);
    oneop(kSQRT);
    post_op();
}

static void store(Widget w, IswEvent *ev, String *vector, Cardinal *count)
{
    (void)w; (void)ev; (void)vector; (void)count;
    XCALC_PRE_OP(kSTO);
    rpn ? memf(kSTO) : oneop(kSTO);
    post_op();
}

static void subtract(Widget w, IswEvent *ev, String *vector, Cardinal *count)
{
    (void)w; (void)ev; (void)vector; (void)count;
    XCALC_PRE_OP(kSUB);
    rpn ? twof(kSUB) : twoop(kSUB);
    post_op();
}

static void sum(Widget w, IswEvent *ev, String *vector, Cardinal *count)
{
    (void)w; (void)ev; (void)vector; (void)count;
    XCALC_PRE_OP(kSUM);
    rpn ? memf(kSUM) : oneop(kSUM);
    post_op();
}

static void tangent(Widget w, IswEvent *ev, String *vector, Cardinal *count)
{
    (void)w; (void)ev; (void)vector; (void)count;
    XCALC_PRE_OP(kTAN);
    oneop(kTAN);
    post_op();
}

static void tenpower(Widget w, IswEvent *ev, String *vector, Cardinal *count)
{
    (void)w; (void)ev; (void)vector; (void)count;
    XCALC_PRE_OP(k10X);
    oneop(k10X);
    post_op();
}

static void xtrunc(Widget w, IswEvent *ev, String *vector, Cardinal *count)
{
    (void)w; (void)ev; (void)vector; (void)count;
    XCALC_PRE_OP(kTRUNC);
    oneop(kTRUNC);
    post_op();
}

static void xor(Widget w, IswEvent *ev, String *vector, Cardinal *count)
{
    (void)w; (void)ev; (void)vector; (void)count;
    XCALC_PRE_OP(kXOR);
    rpn ? twof(kXOR) : twoop(kXOR);
    post_op();
}

static void XexchangeY(Widget w, IswEvent *ev, String *vector, Cardinal *count)
{
    (void)w; (void)ev; (void)vector; (void)count;
    XCALC_PRE_OP(kXXY);
    twof(kXXY);
    post_op();
}
