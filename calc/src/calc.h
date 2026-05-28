/*
 * calc.h - symbolic constants for isde-calc
 *
 * Based on xcalc by Donna Converse, MIT X Consortium.
 * Ported to libISW for the ISDE desktop environment.
 */

#ifndef ISDE_CALC_H
#define ISDE_CALC_H

#include <ISW/Intrinsic.h>
#include <ISW/StringDefs.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <signal.h>
#include <setjmp.h>
#include <errno.h>

#define kRECIP 0
#define kSQR   1
#define kSQRT  2
#define kCLR   3
#define kOFF   4
#define kINV   5
#define kSIN   6
#define kCOS   7
#define kTAN   8
#define kDRG   9
#define kE     10
#define kEE    11
#define kLOG   12
#define kLN    13
#define kPOW   14
#define kPI    15
#define kFACT  16
#define kLPAR  17
#define kRPAR  18
#define kDIV   19
#define kSTO   20
#define kSEVEN 21
#define kEIGHT 22
#define kNINE  23
#define kMUL   24
#define kRCL   25
#define kFOUR  26
#define kFIVE  27
#define kSIX   28
#define kSUB   29
#define kSUM   30
#define kONE   31
#define kTWO   32
#define kTHREE 33
#define kADD   34
#define kEXC   35
#define kZERO  36
#define kDEC   37
#define kNEG   38
#define kEQU   39
#define kENTR  40
#define kXXY   41
#define kEXP   42
#define k10X   43
#define kROLL  44
#define kNOP   45
#define kBKSP  46
#define kAND   47
#define kBASE  48
#define kMOD   49
#define kNOT   50
#define kOR    51
#define kSHL   52
#define kSHR   53
#define kXOR   54
#define kTRUNC 55
#define kxA    56
#define kxB    57
#define kxC    58
#define kxD    59
#define kxE    60
#define kxF    61

#define XCalc_MEMORY    0
#define XCalc_INVERSE   1
#define XCalc_DEGREE    2
#define XCalc_RADIAN    3
#define XCalc_GRADAM    4
#define XCalc_PAREN     5
#define XCalc_HEX       6
#define XCalc_DEC       7
#define XCalc_OCT       8

/* actions.c */
extern IswActionsRec Actions[];
extern int ActionsCount;
extern void calc_do_op(int op);

/* math.c */
extern void fail_op(void);
extern int pre_op(int keynum);
extern void post_op(void);

extern void change_base(void);
extern void numeric(int keynum);
extern void bkspf(void);
extern void decf(void);
extern void eef(void);
extern void clearf(void);
extern void negf(void);
extern void twoop(int keynum);
extern void twof(int keynum);
extern void entrf(void);
extern void equf(void);
extern void lparf(void);
extern void rollf(void);
extern void rparf(void);
extern void drgf(void);
extern void invf(void);
extern void memf(int keynum);
extern void oneop(int keynum);
extern void offf(void);
extern void ResetCalc(void);

#ifndef IEEE
extern jmp_buf env;
extern void fperr(int sig);
extern void illerr(int sig);
#endif

/* main.c */
extern void do_select(xcb_timestamp_t time);
extern void draw(char *string);
extern void Quit(void);
extern void ringbell(void);
extern void setflag(int indicator, Boolean on);

extern int rpn;
#define LCD_STR_LEN 32
extern char dispstr[LCD_STR_LEN];

#endif /* ISDE_CALC_H */
