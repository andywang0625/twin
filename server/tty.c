/*
 *  tty.c --  linux terminal emulator
 *
 *  linux terminal control sequences decoder taken from:
 *    linux/drivers/char/console.c
 *    Copyright (C) 1991, 1992  Linus Torvalds
 *
 *  all the rest written by twin author:
 *    Copyright (C) 1993-2000  Massimiliano Ghilardi
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 */

#include "twin.h"
#include "data.h"
#include "methods.h"
#include "draw.h"
#include "resize.h"

#include "remote.h"
#include "hw.h"
#include "common.h"
#include "tty.h"

/*
 * VT102 emulator
 */

/* static variables, common to most functions */

static window *Win;
static ttydata *Data;
static udat *Flags;
static udat kbdFlags = TTY_AUTOWRAP, defaultFlags = TTY_AUTOWRAP;

static dat   dirty[2][4];
static uldat dirtyS[2];
static byte  dirtyN;

#define ColText		Win->ColText
#define State		Data->State
#define Effects		Data->Effects
#define ScrollBack	Data->ScrollBack
#define SizeX		Data->SizeX
#define SizeY		Data->SizeY
#define Top		Data->Top
#define Bottom		Data->Bottom
#define X		Data->X
#define Y		Data->Y
#define saveX		Data->saveX
#define saveY		Data->saveY
#define Base		Win->Contents
#define Start		Data->Start
#define Split		Data->Split
#define Pos		Data->Pos
#define Color		Data->Color
#define DefColor	Data->DefColor
#define saveColor	Data->saveColor
#define Underline	Data->Underline
#define HalfInten	Data->HalfInten
#define TabStop		Data->TabStop
#define Par		Data->Par
#define nPar		Data->nPar
#define currG		Data->currG
#define G		Data->G
#define G0		Data->G0
#define G1		Data->G1
#define saveG		Data->saveG
#define saveG0		Data->saveG0
#define saveG1		Data->saveG1


/* A bitmap for codes <32. A bit of 1 indicates that the code
 * corresponding to that bit number invokes some special action
 * (such as cursor movement) and should not be displayed as a
 * glyph unless the TTY_DISPCTRL mode is explicitly enabled.
 */
#define CTRL_ACTION 0x0d00ff81
#define CTRL_ALWAYS 0x0800f501	/* Cannot be overridden by TTY_DISPCTRL */

#define CHANGE_BIT(bit, on_off) do { \
    if (on_off) *Flags |= (bit); \
    else *Flags &= ~(bit); \
} while(0)

    
/*
 * for better cleannes, dirty_tty()
 * should be used *before* actually touching Win->Contents[]
 */
void dirty_tty(udat x1, udat y1, udat x2, udat y2) {
    byte i;
    uldat S[2] = {0, 0};
    udat xy[2][4];
    
    if (dirtyN == MAXBYTE || x1 > x2 || x1 >= SizeX || y1 > y2 || y1 >= SizeY)
	return;

    x2 = Min2(x2, SizeX-1);
    y2 = Min2(y2, SizeY-1);

    for (i=0; i<dirtyN; i++) {
	xy[i][0] = Min2(dirty[i][0], x1);
	xy[i][1] = Min2(dirty[i][1], y1);
	xy[i][2] = Max2(dirty[i][2], x2);
	xy[i][3] = Max2(dirty[i][3], y2);
	S[i] += (xy[0][2] - xy[0][0] + 1) * (xy[0][3] - xy[0][1] + 1);
	S[!i] += dirtyS[i];
    }
    if (dirtyN < 2)
	S[dirtyN] += (x2-x1+1)*(y2-y1+1);
    
    i = dirtyN && S[0] > S[1];

    if (S[i] >= SizeX*SizeY*3/4) {
	dirtyN = MAXBYTE;
	return;
    } else if (i < dirtyN) {
	CopyMem(xy[i], dirty[i], 4*sizeof(udat));
    } else {
	dirtyN++;
	dirty[i][0] = x1;
	dirty[i][1] = y1;
	dirty[i][2] = x2;
	dirty[i][3] = y2;
    }
    dirtyS[i] = (dirty[i][2]-dirty[i][0]+1)*(dirty[i][3]-dirty[i][1]+1);
}

static void flush_tty(void) {
    byte doupdate = FALSE;
    udat i;
    
    /* first, draw on screen whatever changed in the window */
    if (dirtyN) {
	if (dirtyN == MAXBYTE)
	    DrawTextWindow(Win, 0, ScrollBack, SizeX-1, SizeY-1 + ScrollBack);
	else for (i=0; i<dirtyN; i++)
	    DrawTextWindow(Win, dirty[i][0], dirty[i][1] + ScrollBack, dirty[i][2], dirty[i][3] + ScrollBack);
	dirtyN = 0;
    }
    
    /* then update cursor */
    if (Win->CurX != (uldat)X || Win->CurY != (uldat)Y + ScrollBack) {

	Win->CurX = (uldat)X;
	Win->CurY = (uldat)Y + ScrollBack;
	Pos = Base + Win->CurX + (Win->CurY + Win->NumRowSplit) * SizeX;
	if (Pos >= Split) Pos -= Split - Base;
	
	doupdate = TRUE;
    } else
	doupdate = FALSE;

    if (Win == All->FirstScreen->FocusWindow && (doupdate || (*Flags & TTY_UPDATECURSOR)))
	UpdateCursor();

    *Flags &= ~TTY_UPDATECURSOR;
    
    /* finally, keyboard focus configuration: */
    if (*Flags & TTY_NEEDREFOCUS) {
	*Flags &= ~TTY_NEEDREFOCUS;
	if (Win == All->FirstScreen->FocusWindow)
	    Act(KbdFocus,Win)(Win);
    }
}

/* Note: inverting the screen twice should revert to the original state */

static void invert_screen(void) {
    hwattr a, *p = Start;
    uldat count;
    
    dirty_tty(0, 0, SizeX-1, SizeY-1);
    count = SizeX * SizeY;
    
    while (count--) {
	a = *p;
	*p++ = HWATTR( COL( COLBG(HWCOL(a)), COLFG(HWCOL(a)) ), HWFONT(a) );
	if (p == Split) p = Base;
    }
}

static void insert_char(uldat nr) {
    hwattr *p, *q = Pos;

    p = q + SizeX - X - nr;
    
    dirty_tty(X, Y, SizeX-1, Y);
    
    while (--p >= q)
	p[nr] = *p;
    
    while (nr--)
	*q++ = HWATTR(ColText, ' ');

    *Flags &= ~TTY_NEEDWRAP;
}

INLINE void delete_char(uldat nr) {
    udat i;
    hwattr *p = Pos;
    
    i = SizeX - X - nr;

    dirty_tty(X, Y, SizeX-1, Y);
    
    while (i--) {
	*p = p[nr];
	p++;
    }
    while (nr--)
	*p++ = HWATTR(ColText, ' ');
    
    *Flags &= ~TTY_NEEDWRAP;
}


/*
 * goto_xy() must verify all boundaries, because the arguments
 * might also be negative. If the given position is out of
 * bounds, the cursor is placed at the nearest margin.
 */
static void goto_xy(ldat new_x, ldat new_y) {
    udat min_y, max_y;
    
    if (new_x < 0)
	X = 0;
    else if (new_x >= (ldat)SizeX)
	X = SizeX - 1;
    else
	X = (udat)new_x;
    
    if (*Flags & TTY_ABSORIG) {
	min_y = 0;
	max_y = SizeY - 1;
    } else {
	min_y = Top;
	max_y = Bottom;
    }

    if (new_y < (ldat)min_y)
	Y = min_y;
    else if (new_y >= (ldat)max_y)
	Y = max_y - 1;
    else
	Y = (udat)new_y;
    
    *Flags &= ~TTY_NEEDWRAP;
    
    /* never flush here, just update Pos */
    Pos = Start + X + (uldat)Y * SizeX;
    if (Pos >= Split) Pos -= Split - Base;
}

/* for absolute user moves, when TTY_ABSORIG (decom) is set */
INLINE void goto_axy(ldat new_x, ldat new_y) {
    goto_xy(new_x, *Flags & TTY_ABSORIG ? (Top+new_y) : new_y);
}

/* WARNING: fwd_copy() doesn't call dirty_tty(), you must call it manually! */
static void fwd_copy(hwattr *s, hwattr *d, ldat len) {
    ldat l;
    
    while (s >= Split) s -= Split - Base;
    while (d >= Split) d -= Split - Base;
    
    while (len > 0) {
	l = Min2(len, Split - s); l = Min2(l, Split - d);
	CopyMem(s, d, l * sizeof(hwattr));
	s += l; d += l; len -= l;
	if (s == Split) s = Base;
	if (d == Split) d = Base;
    }
}

/* WARNING: rev_copy() doesn't call dirty_tty(), you must call it manually! */
static void rev_copy(hwattr *s, hwattr *d, ldat len) {
    ldat l;

    s += len; d += len;
    while (s > Split) s -= Split - Base;
    while (d > Split) d -= Split - Base;
    
    while (len > 0) {
	l = Min2(len, s - Base); l = Min2(l, d - Base);
	s -= l; d -= l; len -= l;
	MoveMem(s, d, l * sizeof(hwattr));
	if (s == Base) s = Split;
	if (d == Base) d = Split;
    }
}

/* WARNING: fill() doesn't call dirty_tty(), you must call it manually! */
static void fill(hwattr *s, hwattr c, ldat len) {
    ldat l;
    
    while (s >= Split) s -= Split - Base;

    while (len > 0) {
	l = Min2(len, Split - s);
	len -= l;
	while (l--)
	    *s++ = c;
	if (s == Split) s = Base;
    }
}

static void scrollup(udat t, udat b, dat nr) {
    hwattr *d, *s;
    byte accel = FALSE;
    
    if (t + nr >= b)
	nr = b - t - 1;
    if (b > SizeY || t >= b || nr < 1)
	return;

    /* try to accelerate this */
    if (Win == All->FirstScreen->FirstWindow) {
	accel = TRUE;
	flush_tty();
    } else
	dirty_tty(0, t, SizeX-1, b-1);

    if (t == 0 && b == SizeY) {
	/* full screen scrolls. use splitline+scrollback */
	
	Win->NumRowSplit += nr;
	if (Win->NumRowSplit >= Win->MaxNumRow)
	    Win->NumRowSplit -= Win->MaxNumRow;
	
	Start += nr * SizeX;
	if (Start >= Split) Start -= Split - Base;

	Pos += nr * SizeX;
	if (Pos >= Split) Pos -= Split - Base;
	
	s = Start + SizeX * (t+nr);
	d = Start + SizeX * t;
    } else {
	/* plain memcpy */
	s = Start + SizeX * (t+nr);
	d = Start + SizeX * t;
	fwd_copy(s, d, (b-t-nr) * SizeX);
    }
    
    /* clear the last nr lines */
    fill(d + (b-t-nr) * SizeX, HWATTR(ColText, ' '), nr * SizeX);
    
    if (accel)
	ScrollFirstWindowArea(0, t, SizeX-1, b-1, 0, -nr);
}

static void scrolldown(udat t, udat b, dat nr) {
    hwattr *s;
    ldat step;
    byte accel = FALSE;
    
    if (t+nr >= b)
	nr = b - t - 1;
    if (b > SizeY || t >= b || nr < 1)
	return;

    /* try to accelerate this */
    if (Win == All->FirstScreen->FirstWindow) {
	accel = TRUE;
	flush_tty();
    } else
	dirty_tty(0, t, SizeX-1, b-1);

    s = Start + SizeX * t;
    step = SizeX * nr;

    rev_copy(s, s + step, (b-t-nr)*SizeX);
    fill(s, HWATTR(ColText, ' '), step);

    if (accel)
	ScrollFirstWindowArea(0, t, SizeX-1, b-1, 0, nr);
}

		     
INLINE void lf(void) {
    /* don't scroll if above bottom of scrolling region, or
     * if below scrolling region
     */
    if (Y+1 == Bottom)
	scrollup(Top, Bottom, 1);
    else if (Y < SizeY - 1) {
	Y++;
	Pos += SizeX;
	if (Pos >= Split) Pos -= Split - Base;
    }
    *Flags &= ~TTY_NEEDWRAP;
}

static void ri(void) {
    /* don't scroll if below top of scrolling region, or
     * if above scrolling region
     */
    if (Y == Top)
	scrolldown(Top, Bottom, 1);
    else if (Y > 0) {
	Y--;
	Pos -= SizeX;
	if (Pos < Base)
	    Pos += Split - Base;
    }
    *Flags &= ~TTY_NEEDWRAP;
}

INLINE void cr(void) {
    Pos -= X;
    X = 0;
    *Flags &= ~TTY_NEEDWRAP;
}

INLINE void bs(void) {
    if (X) {
	X--;
	Pos--;
	*Flags &= ~TTY_NEEDWRAP;
    }
}

INLINE void del(void) {
    /* ignored */
}

static void csi_J(int vpar) {
    uldat count;
    hwattr *start;
    
    switch (vpar) {
      case 0:	/* erase from cursor to end of display */
	dirty_tty(0, Y, SizeX-1, SizeY-1);
	count = (SizeY - Y) * (uldat)SizeX + SizeX - X;
	start = Pos;
	break;
      case 1:	/* erase from start to cursor */
	dirty_tty(0, 0, SizeX-1, Y);
	count = Y * (uldat)SizeX + X;
	start = Start;
	break;
      case 2: /* erase whole display */
	dirty_tty(0, 0, SizeX-1, SizeY-1);
	count = (uldat)SizeX * SizeY;
	start = Start;
	break;
      default:
	return;
    }
    fill(start, HWATTR(ColText, ' '), count);
    
    *Flags &= ~TTY_NEEDWRAP;
}

static void csi_K(int vpar) {
    udat count;
    hwattr *start;
    
    switch (vpar) {
      case 0:	/* erase from cursor to end of line */
	dirty_tty(X, Y, SizeX-1, Y);
	count = SizeX - X;
	start = Pos;
	break;
      case 1:	/* erase from start of line to cursor */
	dirty_tty(0, Y, X, Y);
	count = X + 1;
	start = Pos - X;
	break;
      case 2: /* erase whole line */
	dirty_tty(0, Y, SizeX-1, Y);
	count = SizeX;
	start = Pos - X;
	break;
      default:
	return;
    }
    while (count--)
	*start++ = HWATTR(ColText, ' ');

    *Flags &= ~TTY_NEEDWRAP;
}

static void csi_X(int vpar) /* erase the following vpar positions */
{					  /* not vt100? */
    hwattr *start = Pos;
    
    if (!vpar)
	vpar++;
    vpar = Min2(vpar, SizeX - X);
    
    dirty_tty(X, Y, X+vpar-1, Y);
    
    while (vpar--)
	*start++ = HWATTR(ColText, ' ');
    
    *Flags &= ~TTY_NEEDWRAP;
}

static void update_eff(void) {
    udat effects = Effects;
    hwcol fg = COLFG(ColText), bg = COLBG(ColText);
	
    if (effects & EFF_UNDERLINE)
	fg = COLFG(Underline);
    else if (effects & EFF_HALFINTENS)
	fg = COLFG(HalfInten);
    if (!!(effects & EFF_REVERSE) != !!(*Flags & TTY_INVERTSCR)) {
	hwcol tmp = COL(bg & ~HIGH, fg & ~HIGH) | COL(fg & HIGH, bg & HIGH);
	fg = COLFG(tmp);
	bg = COLBG(tmp);
    }
    if (effects & EFF_INTENSITY)
	fg ^= HIGH;
    if (effects & EFF_BLINK)
	bg ^= HIGH;
    Color = COL(fg, bg);
}

INLINE byte applyG(byte c) {
    if (c < 0x80 || currG == IBMPC_MAP)
	return c;
    return All->Gtranslations[currG][c & 0x7F];
}
    
INLINE void csi_m(void) {
    udat i;
    udat effects = Effects;
    hwcol fg = COLFG(ColText), bg = COLBG(ColText);
    
    for (i=0; i<=nPar; i++) switch (Par[i]) {
      case 0:
	/* all attributes off */
	fg = COLFG(DefColor);
	bg = COLBG(DefColor);
	effects = 0;
	break;
      case 1:
	effects &= ~EFF_HALFINTENS;
	effects |= EFF_INTENSITY;
	break;
      case 2:
	effects &= ~EFF_INTENSITY;
	effects |= EFF_HALFINTENS;
	break;
      case 4:
	effects |= EFF_UNDERLINE;
	break;
      case 5:
	effects |= EFF_BLINK;
	break;
      case 7:
	effects |= EFF_REVERSE;
	break;
      case 10: /* ANSI X3.64-1979 (SCO-ish?)
		* Select primary font, don't display
		* control chars if defined, don't set
		* bit 8 on output.
		*/
	currG = G ? G1 : G0;
	*Flags &= ~(TTY_DISPCTRL | TTY_SETMETA);
	break;
      case 11: /* ANSI X3.64-1979 (SCO-ish?)
		* Select first alternate font, lets
		* chars < 32 be displayed as ROM chars.
		*/
	currG = IBMPC_MAP;
	*Flags |= TTY_DISPCTRL;
	*Flags &= ~TTY_SETMETA;
	break;
      case 12: /* ANSI X3.64-1979 (SCO-ish?)
		* Select second alternate font, toggle
		* high bit before displaying as ROM char.
		*/
	currG = IBMPC_MAP;
	*Flags |= TTY_DISPCTRL | TTY_SETMETA;
	break;
      case 21:
      case 22:
	effects &= ~(EFF_HALFINTENS|EFF_INTENSITY);
	break;
      case 24:
	effects &= ~EFF_UNDERLINE;
	break;
      case 25:
	effects &= ~EFF_BLINK;
	break;
      case 27:
	effects &= ~EFF_REVERSE;
	break;
      case 38: /* ANSI X3.64-1979 (SCO-ish?)
		* Enables underscore, white foreground
		* with white underscore
		* (Linux - use default foreground).
		*/
	fg = COLFG(DefColor);
	effects |= EFF_UNDERLINE;
	break;
      case 39: /* ANSI X3.64-1979 (SCO-ish?)
		* Disable underline option.
		* Reset colour to default? It did this
		* before...
		*/
	fg = COLFG(DefColor);
	effects &= ~EFF_UNDERLINE;
	break;
      case 49: /* restore default bg */
	bg = COLBG(DefColor);
	break;
      default:
	if (Par[i] >= 30 && Par[i] <= 37)
	    Par[i] -= 30,
	    fg = ANSI2VGA(Par[i]);
	else if (Par[i] >= 40 && Par[i] <= 47)
	    Par[i] -= 40,
	    bg = ANSI2VGA(Par[i]);
	break;
    }
    Effects = effects;
    ColText = COL(fg, bg);

    update_eff();
}

static void respond_string(byte *p) {
    uldat Len = strlen(p);
    
    /* the remote program may be directly attached to the window */
    if (!RemoteWindowWriteQueue(Win, Len, p)) {
	
	/* or we may need to send a Msg to Win->Menu->MsgPort */
	msg *Msg;
	event_keyboard *Event;
	if ((Msg = Do(Create,Msg)(FnMsg, MSG_WINDOW_KEY, Len + sizeof(event_keyboard)))) {
	    /* this is the same code as in KeyboardEvent() in hw.c */
	    Event = &Msg->Event.EventKeyboard;
	    Event->Window = Win;
	    if (Len == 1 && (p[0] == ENTER || p[0] == ESCAPE))
		Event->Code = p[0];
	    else
		Event->Code = 1;
	    Event->ShiftFlags = (udat)0;
	    Event->SeqLen = Len;
	    CopyMem(p, Event->AsciiSeq, Len);
	    Event->AsciiSeq[Len] = '\0'; /* terminate string with \0 */
	    SendMsg(Win->Menu->MsgPort, Msg);
	}
    }
}

static void cursor_report(void) {
    byte buf[40];
    sprintf(buf, "\033[%d;%dR", Y + (*Flags & TTY_ABSORIG ? Top+1 : 1), X+1);
    respond_string(buf);
}

INLINE void status_report(void) {
    respond_string("\033[0n");	/* Terminal ok */
}

/*
 * this is what the terminal answers to a ESC-Z or csi0c query.
 */
INLINE void respond_ID(void) {
    respond_string("\033[?6;3c" /* VT102ID is "\033[?6c" */ );
}

static void set_mode(byte on_off) {
    udat i;
    
    for (i=0; i<=nPar; i++)
	
	/* DEC private modes set/reset */
	
	if (State & ESques) switch (Par[i]) {
	    
	  case 1:	/* Cursor keys send ^[Ox/^[[x */
	    CHANGE_BIT(TTY_ALTCURSKEYS, on_off);
	    *Flags |= TTY_NEEDREFOCUS;
	    break;
	  case 3:	/* 80/132 mode switch unimplemented */
	    break;
	  case 5:	/* Inverted screen on/off */
	    CHANGE_BIT(TTY_INVERTSCR, on_off);
	    update_eff();
	    invert_screen();
	    break;
	  case 6:			/* Origin relative/absolute */
	    CHANGE_BIT(TTY_ABSORIG, on_off);
	    goto_axy(0,0);
	    break;
	  case 7:			/* Autowrap on/off */
	    CHANGE_BIT(TTY_AUTOWRAP, on_off);
	    break;
	  case 8:			/* Autorepeat on/off */
	    break;
	  case 9:
	    CHANGE_BIT(TTY_REPORTMOUSE, on_off);
	    CHANGE_BIT(TTY_REPORTMOUSE2, FALSE);
	    if (on_off)
		Win->Attrib |= WINDOW_WANT_MOUSE;
	    else
		Win->Attrib &= ~WINDOW_WANT_MOUSE;
	    break;
	  case 25:			/* Cursor on/off */
	    if (on_off)
		Win->Flags |= WINFL_CURSOR_ON;
	    else
		Win->Flags &= ~WINFL_CURSOR_ON;
	    *Flags |= TTY_UPDATECURSOR;
	    break;
	  case 1000:
	    CHANGE_BIT(TTY_REPORTMOUSE, FALSE);
	    CHANGE_BIT(TTY_REPORTMOUSE2, on_off);
	    if (on_off)
		Win->Attrib |= WINDOW_WANT_MOUSE;
	    else
		Win->Attrib &= ~WINDOW_WANT_MOUSE;
	    break;

	/* ANSI modes set/reset */
	} else switch (Par[i]) {
	    
	  case 3:			/* Monitor (display ctrls) */
	    CHANGE_BIT(TTY_DISPCTRL, on_off);
	    break;
	  case 4:			/* Insert Mode on/off */
	    CHANGE_BIT(TTY_INSERT, on_off);
	    break;
	  case 20:			/* Lf, Enter == CrLf/Lf */
	    CHANGE_BIT(TTY_CRLF, on_off);
	    break;
	}
}

static void setterm_command(void) {
    switch (Par[0]) {
	
      case 1:	/* set fg color for underline mode */
	if (Par[1] <= MAXCOL) {
	    Underline = COL(ANSI2VGA(Par[1]), 0);
	    if (Effects & EFF_UNDERLINE)
		update_eff();
	}
	break;
      case 2:	/* set color for half intensity mode */
	if (Par[1] <= MAXCOL) {
	    HalfInten = COL(ANSI2VGA(Par[1]), 0);
	    if (Effects & EFF_HALFINTENS)
		update_eff();
	}
	break;
      case 8:	/* store colors - but not effects - as defaults */
	DefColor = ColText;
	break;
      case 9:	/* set blanking interval */
	break;
      case 10: /* set bell frequency in Hz */
      case 11: /* set bell duration in msec */
	ConfigureHW(Par[0] == 10 ? HW_BELLPITCH : HW_BELLDURATION, nPar == 0, Par[1]);
	break;
      case 12: /* bring specified console to the front */
	break;
      case 13: /* unblank the screen */
	break;
      case 14: /* set vesa powerdown interval */
	break;
      default:
	break;
    }
}

INLINE void insert_line(uldat nr) {
   scrolldown(Y, Bottom, nr);
   *Flags &= ~TTY_NEEDWRAP;
}


INLINE void delete_line(uldat nr) {
   scrollup(Y, Bottom, nr);
   *Flags &= ~TTY_NEEDWRAP;
}

INLINE void csi_at(uldat nr) {
    if (nr > (uldat)SizeX - X)
	nr = (uldat)SizeX - X;
    else if (!nr)
	nr = 1;
    insert_char(nr);
}

INLINE void csi_L(uldat nr) {
    if (nr > (uldat)SizeY - Y)
	nr = (uldat)SizeY - Y;
    else if (!nr)
	nr = 1;
    insert_line(nr);
}

INLINE void csi_P(uldat nr) {
    if (nr > (uldat)SizeX - X)
	nr = (uldat)SizeX - X;
    else if (!nr)
	nr = 1;
    delete_char(nr);
}

INLINE void csi_M(uldat nr) {
    if (nr > (uldat)SizeY - Y)
	nr = (uldat)SizeY - Y;
    else if (!nr)
	nr = 1;
    delete_line(nr);
}

INLINE void save_current(void) {
    saveX = X;
    saveY = Y;
    saveColor = ColText;
    saveG  = G;
    saveG0 = G0;
    saveG1 = G1;
}

INLINE void restore_current(void) {
    goto_xy(saveX, saveY);
    ColText = saveColor;
    update_eff();
    *Flags &= ~TTY_NEEDWRAP;
    G  = saveG;
    G0 = saveG0;
    G1 = saveG1;
    currG = G ? G1 : G0;
}

static void reset_tty(byte do_clear) {

    State = ESnormal;
    if (*Flags & TTY_INVERTSCR)
	invert_screen();
    *Flags = TTY_AUTOWRAP;
    Effects = 0;
    
    Top	 = 0;
    Bottom = SizeY;

    ColText = Color = DefColor = COL(WHITE,BLACK);
    Underline = COL(HIGH|WHITE,BLACK);
    HalfInten = COL(HIGH|BLACK,BLACK);
    
    Win->Flags |= WINFL_CURSOR_ON;
    Win->CursorType = LINECURSOR;

    TabStop[0] = 0x01010100;
    TabStop[1] = TabStop[2] = TabStop[3] = TabStop[4] = 0x01010101;
    
    nPar = 0;
    
    G = saveG = 0;
    /*
     * this probably violates some standard, 
     * but starting with the identity mapping
     * seems the only reasonable choice to me
     */
    currG = G0 = saveG0 = IBMPC_MAP;
    G1 = saveG1 = GRAF_MAP;
    
    /*
    bell_pitch = DEFAULT_BELL_PITCH;
    bell_duration = DEFAULT_BELL_DURATION;

    kbd_table.ledmode = LED_SHOW_FLAGS;
    kbd_table.ledflagstate = kbd_table.default_ledflagstate;
    */
    
    goto_xy(0,0);
    save_current();
    if (do_clear)
	csi_J(2);
}

INLINE void write_ctrl(byte c) {
    /*
     *  Control characters can be used in the _middle_
     *  of an escape sequence.
     */
    switch (c) {
      case 0:
	return;
      case 7:
	BeepHW();
	return;
      case 8:
	bs();
	return;
      case 9: /* TAB */
	Pos -= X;
	while (X < SizeX - 1) {
	    X++;
	    if (TabStop[X >> 5] & (1 << (X & 31)))
		break;
	}
	Pos += X;
	return;
      case 10: case 11: case 12:
	lf();
	if (!(*Flags & TTY_CRLF))
	    return;
	/* FALLTHROUGH */
      case 13:
	cr();
	return;
      case 14:
	G = 1; currG = G1;
	*Flags |= TTY_DISPCTRL;
	return;
      case 15:
	G = 0; currG = G0;
        *Flags &= ~TTY_DISPCTRL;
	return;
      case 24: case 26:
	State = ESnormal;
	return;
      case 27:
	State = ESesc;
	return;
      case 127:
	del();
	return;
      case 128+27:
	State = ESsquare;
	return;
    }
    
    /* State machine */
    
    /* mask out ESques for now */
    switch (State & ESany) {
	
      case ESesc:
	switch (c) {
	  case '[':
	    State = ESsquare;
	    return;
	  case ']':
	    State = ESnonstd;
	    return;
	  case '%':
	    State = ESpercent;
	    return;
	  case 'E':
	    cr();
	    lf();
	    break;
	  case 'M':
	    ri();
	    break;
	  case 'D':
	    lf();
	    break;
	  case 'H':
	    TabStop[X >> 5] |= (1 << (X & 31));
	    break;
	  case 'Z':
	    respond_ID();
	    break;
	  case '7':
	    save_current();
	    break;
	  case '8':
	    restore_current();
	    break;
	  case '(':
	    State = ESsetG0;
	    return;
	  case ')':
	    State = ESsetG1;
	    return;
	  case '#':
	    State = EShash;
	    return;
	  case 'c':
	    reset_tty(TRUE);
	    break;
	  case '>':  /* Numeric keypad */
	    *Flags &= ~TTY_KBDAPPLIC;
	    *Flags |= TTY_NEEDREFOCUS;
	    break;
	  case '=':  /* Appl. keypad */
	    *Flags |= TTY_KBDAPPLIC|TTY_NEEDREFOCUS;
	    break;
	}
	break;
	
      case ESnonstd:
	if (c=='P') {		/* Palette escape sequence */
	    nPar = 0 ;
	    WriteMem((byte *)&Par, 0, NPAR * sizeof(uldat));
	    State = ESpalette;
	    return;
	} else if (c=='R')	/* Reset palette */
	    ResetPaletteHW();
	break;

      case ESpalette:
	if ( (c>='0'&&c<='9') || (c>='A'&&c<='F') || (c>='a'&&c<='f') ) {
	    Par[nPar++] = (c>'9' ? (c&0xDF)-'A'+10 : c-'0') ;
	    if (nPar==7)
		SetPaletteHW(Par[0], Par[1] * 16 + Par[2],
			     Par[3] * 16 + Par[4], Par[5] * 16 + Par[6]);
	    else
		return; /* avoid resetting State */
	}
	break;

      case ESsquare:
	Par[0] = nPar = 0;
	/*WriteMem((byte *)&Par, 0, NPAR * sizeof(uldat));*/
	State = ESgetpars;
	if (c == '[') { /* Function key */
	    State = ESfunckey;
	    return;
	}
	if (c == '?') {
	    State |= ESques;
	    return;
	}
	/* FALLTHROUGH */

      case ESgetpars:
	if (c==';' && nPar<NPAR-1) {
	    Par[++nPar] = 0;
	    return;
	} else if (c>='0' && c<='9') {
	    Par[nPar] *= 10;
	    Par[nPar] += c-'0';
	    return;
	} else
	    State = ESgotpars | (State & ESques);
	/* FALLTHROUGH */
      
      case ESgotpars:
	switch (c) {
	  case 'h':
	    set_mode(1);
	    break;
	  case 'l':
	    set_mode(0);
	    break;
	  case 'c':
	    if (State & ESques) {
		if (!Par[0])
		    Par[1] = Par[2] = Par[0];
		else if (nPar == 1)
		    Par[2] = 0;
		Win->CursorType = Par[0] | (Par[1]<<8) | (Par[2]<<16);
		*Flags |= TTY_UPDATECURSOR;
	    }
	  case 'm':
	    /* selection complement mask */
	    break;
	  case 'n':
	    if (!(State & ESques)) {
		if (Par[0] == 5)
		    status_report();
		else if (Par[0] == 6)
		    cursor_report();
	    }
	    break;
	}
	if (State & ESques)
	    break;

	switch (c) {
	  case 'm':
	    csi_m();
	    break;
	  case 'H': case 'f':
	    if (Par[0]) Par[0]--;
	    if (!nPar)
		Par[1] = nPar;
	    else if (Par[1]) Par[1]--;
	    goto_axy(Par[1],Par[0]);
	    break;
	  case 'J':
	    csi_J(Par[0]);
	    break;
	  case 'K':
	    csi_K(Par[0]);
	    break;
	  case 'G': case '`':
	    if (Par[0]) Par[0]--;
	    goto_xy(Par[0], Y);
	    break;
	  case 'A':
	    if (!Par[0]) Par[0]++;
	    goto_xy(X, Y - Par[0]);
	    break;
	  case 'B': case 'e':
	    if (!Par[0]) Par[0]++;
	    goto_xy(X, Y + Par[0]);
	    break;
	  case 'C': case 'a':
	    if (!Par[0]) Par[0]++;
	    goto_xy(X + Par[0], Y);
	    break;
	  case 'D':
	    if (!Par[0]) Par[0]++;
	    goto_xy(X - Par[0], Y);
	    break;
	  case 'E':
	    if (!Par[0]) Par[0]++;
	    goto_xy(0, Y + Par[0]);
	    break;
	  case 'F':
	    if (!Par[0]) Par[0]++;
	    goto_xy(0, Y - Par[0]);
	    break;
	  case 'd':
	    if (Par[0]) Par[0]--;
	    goto_axy(X, Par[0]);
	    break;
	  case 'L':
	    csi_L(Par[0]);
	    break;
	  case 'M':
	    csi_M(Par[0]);
	    break;
	  case 'P':
	    csi_P(Par[0]);
	    break;
	  case 'c':
	    if (!Par[0])
		respond_ID();
	    break;
	  case 'g':
	    if (!Par[0])
		TabStop[X >> 5] &= ~(1 << (X & 31));
	    else if (Par[0] == 3)
		TabStop[0] = TabStop[1] = TabStop[2] = TabStop[3] = TabStop[4] = 0;
	    break;
	  case 'q': /* DECLL - but only 3 leds */
	    /* map 0,1,2,3 to 0,1,2,4 */
#if 0
	    if (Par[0] < 4)
		setledstate(kbd_table,
			    (Par[0] < 3) ? Par[0] : 4);
#endif
	    break;
	  case 'r':
	    if (!Par[0])
		Par[0]++;
	    if (!nPar || !Par[1])
		Par[1] = SizeY;
	    /* Minimum allowed region is 2 lines */
	    if (Par[0] < Par[1] &&
		Par[1] <= SizeY) {
		Top = Par[0]-1;
		Bottom = Par[1];
		goto_axy(0,0);
	    }
	    break;
	  case 's':
	    save_current();
	    break;
	  case 'u':
	    restore_current();
	    break;
	  case 'X':
	    csi_X(Par[0]);
	    break;
	  case '@':
	    csi_at(Par[0]);
	    break;
	  case ']': /* setterm functions */
	    setterm_command();
	    break;
	}
	break;
	
      case ESpercent:
#if 0
	switch (c) {
	  case '@':  /* defined in ISO 2022 */
	    utf = 0;
	    break;
	  case 'G':  /* prelim official escape code */
	  case '8':  /* retained for compatibility */
	    utf = 1;
	    break;
	}
#endif
	break;
      case ESfunckey:
	break;

      case EShash:
	if (c == '8') {
	    /* DEC screen alignment test */
	    dirty_tty(0, 0, SizeX-1, SizeY-1);
	    fill(Start, HWATTR(ColText, 'E'), (uldat)SizeX * SizeY);
	}
	break;
	
      case ESsetG0:
	switch (c) {
	  case '0': G0 = GRAF_MAP; break;
	  case 'B': G0 = LAT1_MAP; break;
	  case 'U': G0 = IBMPC_MAP; break;
	  case 'K': G0 = USER_MAP; break;
	  default: break;
	}
	if (G == 0)
	    currG = G0;
	break;
	
      case ESsetG1:
	switch (c) {
	  case '0': G1 = GRAF_MAP; break;
	  case 'B': G1 = LAT1_MAP; break;
	  case 'U': G1 = IBMPC_MAP; break;
	  case 'K': G1 = USER_MAP; break;
	  default: break;
	}
	if (G == 1)
	    currG = G1;
	break;
	
      default:
	break;
    }

    State = ESnormal;
}

window *TtyKbdFocus(window *newWin) {
    udat newFlags;
    window *oldWin;
    screen *Screen = newWin ? newWin->Screen : All->FirstScreen;
    
    if (Screen) {
	oldWin = Screen->FocusWindow;
	Screen->FocusWindow = newWin;
    } else
	oldWin = newWin = (window *)0;
	    
    if (Screen == All->FirstScreen) {
	if (!newWin || !newWin->TtyData)
	    newFlags = defaultFlags;
	else
	    newFlags = newWin->TtyData->Flags;
	
	if ((newFlags ^ kbdFlags) & TTY_KBDAPPLIC)
	    ConfigureHW(HW_KBDAPPLIC, FALSE, newFlags & TTY_KBDAPPLIC);
	
	if ((newFlags ^ kbdFlags) & TTY_ALTCURSKEYS)
	    ConfigureHW(HW_ALTCURSKEYS, FALSE, newFlags & TTY_ALTCURSKEYS);
	
	kbdFlags = newFlags;
    }
    return oldWin;
}

void ForceKbdFocus(void) {
    kbdFlags = ~defaultFlags;
    (void)TtyKbdFocus(All->FirstScreen->FocusWindow);
}
    
/* this is the main entry point */
void TtyWriteAscii(window *Window, uldat Len, CONST byte *AsciiSeq) {
    byte c, ok;

    if (!Window || !Len)
	return;
    
    /* initialize global static data */
    Win = Window;
    Data = Win->TtyData;
    Flags = &Data->Flags;

    if (!SizeX || !SizeY)
	return;
    
    /* scroll YLogic to bottom */
    if (Win->YLogic < ScrollBack) {
	if (Win == All->FirstScreen->FirstWindow)
	    ScrollFirstWindow(0, ScrollBack - Win->YLogic, TRUE);
	else {
	    dirty_tty(0, 0, SizeX-1, SizeY-1);
	    Win->YLogic = ScrollBack;
	    DrawBorderWindow(Window, BORDER_RIGHT);
	}
    }
    /* clear any selection */
    if (Win->Attrib & WINDOW_ANYSEL)
	ClearHilight(Win);
    
    while (!(*Flags & TTY_STOPPED) && Len) {
	c = *AsciiSeq;
	AsciiSeq++; Len--;
	
	/* If the original code was a control character we only allow a glyph
	 * to be displayed if the code is not normally used (such as for cursor movement)
	 * or if the TTY_DISPCTRL mode has been explicitly enabled. Certain characters
	 * (as given by the CTRL_ALWAYS bitmap) are always displayed as control characters,
	 * as the console would be pretty useless without them; to display an arbitrary
	 * font position use the direct-to-font zone in UTF-8 mode.
	 */
	if (*Flags & TTY_SETMETA)
	    c |= 0x80;
	
	ok = (c >= 32 || !(((*Flags & TTY_DISPCTRL ? CTRL_ALWAYS : CTRL_ACTION) >> c) & 1))
	    && (c != 127 || (*Flags & TTY_DISPCTRL)) && (c != 128+27)
	    && (c = applyG(c));

	if (State == ESnormal && ok) {

	    /* Now try to find out how to display it */
	    if (*Flags & TTY_NEEDWRAP) {
		cr();
		lf();
	    }
	    if (*Flags & TTY_INSERT)
		insert_char(1);
	    
	    dirty_tty(X, Y, X, Y);
	    *Pos = HWATTR(Color, c);
	    
	    if (X == SizeX - 1) {
		if (*Flags & TTY_AUTOWRAP) 
		    *Flags |= TTY_NEEDWRAP;
	    } else {
		X++;
		Pos++;
	    }
	    continue;
	}
	write_ctrl(c);
	/* don't flush here, it just decreases performance */
	/* flush_tty(); */
    }
    flush_tty();
}


/*
 * this currently wraps at window width
 * so it can write multiple rows at time
 */
void TtyWriteHWAttr(window *Window, udat x, udat y, uldat len, CONST hwattr *text) {
    uldat left, max, chunk;
    hwattr *dst;
    
    if (!Window || Window->Flags & WINFL_INSERT || !len || !text)
	/* WINFL_INSERT not supported */
	return;

    Win = Window;
    Data = Win->TtyData;
    Flags = &Data->Flags;

    /*
     * on-the-fly Contents resize. This is a failsafe check...
     * the real one is in WManager and gets called after a window resize
     */
    if (!CheckResizeWindowContents(Window))
	return;
    
    if (x >= SizeX)
	x = SizeX - 1;
    if (y >= SizeY)
	y = SizeY - 1;

    if (len > (SizeY - y) * SizeX - x)
	len = (SizeY - y) * SizeX - x;

    left = len;
    dst = Start + y * SizeX + x;

    /* scroll YLogic to bottom */
    if (Win->YLogic < ScrollBack) {
	if (Win == All->FirstScreen->FirstWindow)
	    ScrollFirstWindow(0, ScrollBack - Win->YLogic, TRUE);
	else {
	    dirty_tty(0, 0, SizeX-1, SizeY-1);
	    Win->YLogic = ScrollBack;
	    DrawBorderWindow(Window, BORDER_RIGHT);
	}
    }
    /* clear any selection */
    if (Win->Attrib & WINDOW_ANYSEL)
	ClearHilight(Win);

    do {
	if (dst >= Split)
	    dst -= Split - Base;
	max = Split - dst;
	chunk = Min2(left, max);
	CopyMem(text, dst, chunk * sizeof(hwattr));
	text += chunk;
	dst += chunk;
    } while ((left -= chunk) > 0);
    
    if (len > SizeX - x)
	dirty_tty(0, y, SizeX - 1, y + (x + len - 1) / SizeX);
    else
	dirty_tty(x, y, x + len - 1, y);
    
    flush_tty();
}

#if 0
/*
 * Turn the Scroll-Lock LED on when the tty is stopped
 */
static void con_stop(struct tty_struct *tty) {
    int console_num;
    if (!tty)
	return;
    console_num = MINOR(tty->device) - (tty->driver.minor_start);
    if (!vc_cons_allocated(console_num))
	return;
    set_vc_kbd_led(kbd_table + console_num, VC_SCROLLOCK);
    set_leds();
}

/*
 * Turn the Scroll-Lock LED off when the console is started
 */
static void con_start(struct tty_struct *tty) {
    int console_num;
    if (!tty)
	return;
    console_num = MINOR(tty->device) - (tty->driver.minor_start);
    if (!vc_cons_allocated(console_num))
	return;
    clr_vc_kbd_led(kbd_table + console_num, VC_SCROLLOCK);
    set_leds();
}

static void clear_buffer_attributes(window *Window) {
    unsigned short *p = (unsigned short *) origin;
    int count = screenbuf_size/2;
    int mask = hi_font_mask | 0xff;
    
    for (; count > 0; count--, p++) {
	scr_writew((scr_readw(p)&mask) | (video_erase_char&~mask), p);
    }
}

/*
 *	Palettes
 */
void set_palette(window *Window) {
    if (vcmode != KD_GRAPHICS)
	sw->con_set_palette(vc_cons.d, color_table);
}

static int set_get_cmap(unsigned char *arg, int set) {
    int i, j, k;
    
    for (i = 0; i < 16; i++)
	if (set) {
	    get_user(default_red[i], arg++);
	    get_user(default_grn[i], arg++);
	    get_user(default_blu[i], arg++);
	} else {
	    put_user(default_red[i], arg++);
	    put_user(default_grn[i], arg++);
	    put_user(default_blu[i], arg++);
	}
    if (set) {
	for (i = 0; i < MAX_NR_CONSOLES; i++)
	    if (vc_cons_allocated(i)) {
		for (j = k = 0; j < 16; j++) {
		    vc_cons[i].d->vc_palette[k++] = default_red[j];
		    vc_cons[i].d->vc_palette[k++] = default_grn[j];
		    vc_cons[i].d->vc_palette[k++] = default_blu[j];
		}
		set_palette(i);
	    }
    }
    return 0;
}

/*
 * Load palette into the DAC registers. arg points to a colour
 * map, 3 bytes per colour, 16 colours, range from 0 to 255.
 */

int con_set_cmap(unsigned char *arg) {
    return set_get_cmap (arg,1);
}

int con_get_cmap(unsigned char *arg) {
    return set_get_cmap (arg,0);
}

void reset_palette(window *Window) {
    int j, k;
    for (j=k=0; j<16; j++) {
	palette[k++] = default_red[j];
	palette[k++] = default_grn[j];
	palette[k++] = default_blu[j];
    }
    set_palette(currcons);
}
#endif /* 0 */
	
