/*
** This file is part of the Brandy Basic V Interpreter.
** Copyright (C) 2000, 2001, 2002, 2003, 2004 David Daniels
**
** SDL additions by Colin Tuckley
**
** Brandy is free software; you can redistribute it and/or modify
** it under the terms of the GNU General Public License as published by
** the Free Software Foundation; either version 2, or (at your option)
** any later version.
**
** Brandy is distributed in the hope that it will be useful,
** but WITHOUT ANY WARRANTY; without even the implied warranty of
** MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
** GNU General Public License for more details.
**
** You should have received a copy of the GNU General Public License
** along with Brandy; see the file COPYING.  If not, write to
** the Free Software Foundation, 59 Temple Place - Suite 330,
** Boston, MA 02111-1307, USA.
**
**
**	This file contains the VDU driver emulation for the interpreter
**	used when graphics output is possible. It uses the SDL graphics library.
**
**	MODE 7 implementation by Michael McConnell. It's rather rudimentary, it
**	supports most codes when output by the VDU driver, however it does NOT
**	support adding codes in front of existing text to change the nature of
**	content already on screen.
**
*/
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <SDL.h>
#include <sys/time.h>
#include "common.h"
#include "target.h"
#include "errors.h"
#include "basicdefs.h"
#include "scrcommon.h"
#include "screen.h"
#include "mos.h"
#include "graphsdl.h"

/*
** Notes
** -----
**  This is one of the four versions of the VDU driver emulation.
**  It is used by versions of the interpreter where graphics are
**  supported as well as text output using the SDL library.
**  The five versions of the VDU driver code are in:
**	riscos.c
**  graphsdl.c
**	textonly.c
**	simpletext.c
**
**  Graphics support for operating systems other than RISC OS is
**  provided using the platform-independent library 'SDL'.
**
**  The most important functions are 'emulate_vdu' and 'emulate_plot'.
**  All text output and any VDU commands go via emulate_vdu. Graphics
**  commands go via emulate_plot. emulate_vdu corresponds to the
**  SWI OS_WriteC and emulate_plot to OS_Plot.
**
**  The program emulates RISC OS graphics in screen modes 0 to
**  46 (the RISC OS 3.1 modes). Both colour and grey scale graphics
**  are possible. Effectively this code supports RISC OS 3.1
**  (Archimedes) graphics with some small extensions.
**
**  The graphics library that was originally used, jlib, limited
**  the range of RISC OS graphics facilities supported quite considerably.
**  The new graphics library SDL overcomes some of those restrictions
**  although some new geometric routines have had to be added. The graphics
**  are written to a virtual screen with a resolution of 800 by 600.
**  RISC OS screen modes that are smaller than this, for example,
**  mode 1 (320 by 256), are scaled to make better use of this. What
**  actually happens in these screen modes is that the output is
**  written to a second virtual screen of a size more appropriate
**  for the screen mode, for example, in mode 1, text and graphics
**  go to a screen with a resolution of 320 by 256. When displaying
**  the output the second virtual screen is copied to the first
**  one, scaling it to fit to first one. In the case of mode 1,
**  everything is scaled by a factor of two in both the X and Y
**  directions (so that the displayed screen is 640 pixels by 512
**  in size).
**
**  To display the graphics the virtual screen has to be copied to
**  the real screen. The code attempts to optimise this by only
**  copying areas of the virtual screen that have changed.
*/

static int displaybank=0;
static int writebank=0;
#define MAXBANKS 4

/*
** SDL related defines, Variables and params
*/
static SDL_Surface *screen0, *screen1, *screen2, *screen2A, *screen3, *screen3A;
static SDL_Surface *screenbank[MAXBANKS];
static SDL_Surface *sdl_fontbuf, *sdl_v5fontbuf, *sdl_m7fontbuf;
static SDL_Surface *modescreen;	/* Buffer used when screen mode is scaled to fit real screen */

static SDL_Rect font_rect, place_rect, scroll_rect, line_rect, scale_rect;

Uint32 tf_colour,       /* text foreground SDL rgb triple */
       tb_colour,       /* text background SDL rgb triple */
       gf_colour,       /* graphics foreground SDL rgb triple */
       gb_colour;       /* graphics background SDL rgb triple */

Uint32 xor_mask;

/*
** function definitions
*/

extern void draw_line(SDL_Surface *, int32, int32, int32, int32, Uint32, int32, Uint32);
extern void filled_triangle(SDL_Surface *, int32, int32, int32, int32, int32, int32, Uint32, Uint32);
extern void draw_ellipse(SDL_Surface *, int32, int32, int32, int32, Uint32, Uint32);
extern void filled_ellipse(SDL_Surface *, int32, int32, int32, int32, Uint32, Uint32);
static void toggle_cursor(void);
static void switch_graphics(void);
static void vdu_cleartext(void);
static void set_text_colour(boolean background, int colnum);
static void set_graphics_colour(boolean background, int colnum);

extern void mode7renderline(int32 ypos);
extern void mode7renderscreen(void);

static Uint8 palette[768];		/* palette for screen */
static Uint8 hardpalette[24];		/* palette for screen */

static Uint8 vdu21state = 0;		/* VDU21 - disable all output until VDU6 received */
static int autorefresh=1;		/* Refresh screen on updates? */

/* From geom.c */
#define MAX_YRES 1280
#define MAX_XRES 16384
static int32 geom_left[MAX_YRES], geom_right[MAX_YRES];
#define FAST_2_MUL(x) ((x)<<1)
#define FAST_3_MUL(x) (((x)<<1)+x)
#define FAST_4_MUL(x) ((x)<<2)
#define FAST_4_DIV(x) ((x)>>2)

/* Flags for controlling MODE 7 operation */
Uint8 mode7frame[25][40];	/* Text frame buffer for Mode 7, akin to BBC screen memory at &7C00 */
static Uint8 vdu141on = 0;		/* Mode 7 VDU141 toggle */
static Uint8 vdu141mode;		/* Mode 7 VDU141 0=top, 1=bottom */
static Uint8 mode7highbit = 0;		/* Use high bits in Mode 7 */
static Uint8 mode7sepgrp = 0;		/* Separated graphics in Mode 7 */
static Uint8 mode7sepreal = 0;		/* Separated graphics in Mode 7 */
static Uint8 mode7conceal = 0;		/* CONCEAL teletext flag */
static Uint8 mode7hold = 0;		/* Hold Graphics flag */
static Uint8 mode7flash = 0;		/* Flash flag */
static int32 mode7prevchar = 0;		/* Placeholder for storing previous char */
static Uint8 mode7bank = 0;		/* Bank switching for Mode 7 Flashing */
static int64 mode7timer = 0;	/* Timer for bank switching */
static Uint8 mode7black = 0;		/* Allow teletext black codes RISC OS 5 */
static Uint8 mode7reveal = 0;		/* RISC OS 5 - reveal content hidden by CONCEAL */
static Uint8 mode7bitmapupdate = 2;	/* RISC OS 5 - do we update bitmap and blit after each character */
static Uint8 vdu141track[27];		/* Track use of Double Height in Mode 7 *
					 * First line is [1] */

static int32
  vscrwidth,			/* Width of virtual screen in pixels */
  vscrheight,			/* Height of virtual screen in pixels */
  screenwidth,			/* RISC OS width of current screen mode in pixels */
  screenheight,			/* RISC OS height of current screen mode in pixels */
  xgraphunits,			/* Screen width in RISC OS graphics units */
  ygraphunits,			/* Screen height in RISC OS graphics units */
  gwinleft,			/* Left coordinate of graphics window in RISC OS graphics units */
  gwinright,			/* Right coordinate of graphics window in RISC OS graphics units */
  gwintop,			/* Top coordinate of graphics window in RISC OS graphics units */
  gwinbottom,			/* Bottom coordinate of graphics window in RISC OS graphics units */
  xgupp,			/* RISC OS graphic units per pixel in X direction */
  ygupp,			/* RISC OS graphic units per pixel in Y direction */
  graph_fore_action,		/* Foreground graphics PLOT action */
  graph_back_action,		/* Background graphics PLOT action (ignored) */
  graph_forecol,		/* Current graphics foreground logical colour number */
  graph_backcol,		/* Current graphics background logical colour number */
  graph_physforecol,		/* Current graphics foreground physical colour number */
  graph_physbackcol,		/* Current graphics background physical colour number */
  graph_foretint,		/* Tint value added to foreground graphics colour in 256 colour modes */
  graph_backtint,		/* Tint value added to background graphics colour in 256 colour modes */
  plot_inverse,			/* PLOT in inverse colour? */
  xlast,			/* Graphics X coordinate of last point visited */
  ylast,			/* Graphics Y coordinate of last point visited */
  xlast2,			/* Graphics X coordinate of last-but-one point visited */
  ylast2,			/* Graphics Y coordinate of last-but-one point visited */
  xorigin,			/* X coordinate of graphics origin */
  yorigin,			/* Y coordinate of graphics origin */
  xscale,			/* X direction scale factor */
  yscale;			/* Y direction scale factor */

static boolean
  scaled,			/* TRUE if screen mode is scaled to fit real screen */
  vdu5mode,			/* TRUE if text output goes to graphics cursor */
  clipping;			/* TRUE if clipping region is not full screen of a RISC OS mode */

/*
** These two macros are used to convert from RISC OS graphics coordinates to
** pixel coordinates
*/
#define GXTOPX(x) ((x) / xgupp)
#define GYTOPY(y) ((ygraphunits - 1 -(y)) / ygupp)

static graphics graphmode;	/* Says whether graphics are possible or not */

/*
** Built-in ISO Latin-1 font for graphics mode. The first character
** in the table is a blank.
*/
static byte sysfont[224][8];
static byte sysfontbase [224][8] = {
/*   */  {0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u},
/* ! */  {0x18u, 0x18u, 0x18u, 0x18u, 0x18u, 0u, 0x18u, 0u},
/* " */  {0x6cu, 0x6cu, 0x6cu, 0u, 0u, 0u, 0u, 0u},
/* # */  {0x36u, 0x36u, 0x7fu, 0x36u, 0x7fu, 0x36u, 0x36u, 0u},
/* $ */  {0x0cu, 0x3fu, 0x68u, 0x3eu, 0x0bu, 0x7eu, 0x18u, 0u},
/* % */  {0x60u, 0x66u, 0x0cu, 0x18u, 0x30u, 0x66u, 0x06u, 0u},
/* & */  {0x38u, 0x6cu, 0x6cu, 0x38u, 0x6du, 0x66u, 0x3bu, 0u},
/* ' */  {0x0cu, 0x18u, 0x30u, 0u, 0u, 0u, 0u, 0u},
/* ( */  {0x0cu, 0x18u, 0x30u, 0x30u, 0x30u, 0x18u, 0x0cu, 0u},
/* ) */  {0x30u, 0x18u, 0x0cu, 0x0cu, 0x0cu, 0x18u, 0x30u, 0u},
/* * */  {0u, 0x18u, 0x7eu, 0x3cu, 0x7eu, 0x18u, 0u, 0u},
/* + */  {0u, 0x18u, 0x18u, 0x7eu, 0x18u, 0x18u, 0u, 0u},
/* , */  {0u, 0u, 0u, 0u, 0u, 0x18u, 0x18u, 0x30u},
/* - */  {0u, 0u, 0u, 0x7eu, 0u, 0u, 0u, 0u},
/* . */  {0u, 0u, 0u, 0u, 0u, 0x18u, 0x18u, 0u},
/* / */  {0u, 0x6u, 0x0cu, 0x18u, 0x30u, 0x60u, 0u, 0u},
/* 0 */  {0x3cu, 0x66u, 0x6eu, 0x7eu, 0x76u, 0x66u, 0x3cu, 0u},
/* 1 */  {0x18u, 0x38u, 0x18u, 0x18u, 0x18u, 0x18u, 0x7eu, 0u},
/* 2 */  {0x3cu, 0x66u, 0x06u, 0x0cu, 0x18u, 0x30u, 0x7eu, 0u},
/* 3 */  {0x3cu, 0x66u, 0x06u, 0x1cu, 0x06u, 0x66u, 0x3cu, 0u},
/* 4 */  {0x0cu, 0x1cu, 0x3cu, 0x6cu, 0x7eu, 0x0cu, 0x0cu, 0u},
/* 5 */  {0x7eu, 0x60u, 0x7cu, 0x6u, 0x6u, 0x66u, 0x3cu, 0u},
/* 6 */  {0x1cu, 0x30u, 0x60u, 0x7cu, 0x66u, 0x66u, 0x3cu, 0u},
/* 7 */  {0x7eu, 0x6u, 0x0cu, 0x18u, 0x30u, 0x30u, 0x30u, 0u},
/* 8 */  {0x3cu, 0x66u, 0x66u, 0x3cu, 0x66u, 0x66u, 0x3cu, 0u},
/* 9 */  {0x3cu, 0x66u, 0x66u, 0x3eu, 0x6u, 0x0cu, 0x38u, 0u},
/* : */  {0u, 0u, 0x18u, 0x18u, 0u, 0x18u, 0x18u, 0u},
/* ; */  {0u, 0u, 0x18u, 0x18u, 0u, 0x18u, 0x18u, 0x30u},
/* < */  {0xcu, 0x18u, 0x30u, 0x60u, 0x30u, 0x18u, 0xcu, 0u},
/* = */  {0u, 0u, 0x7eu, 0u, 0x7eu, 0u, 0u, 0u},
/* > */  {0x30u, 0x18u, 0xcu, 0x6u, 0xcu, 0x18u, 0x30u, 0u},
/* ? */  {0x3cu, 0x66u, 0xcu, 0x18u, 0x18u, 0u, 0x18u, 0u},
/* @ */  {0x3cu, 0x66u, 0x6eu, 0x6au, 0x6eu, 0x60u, 0x3cu, 0u},
/* A */  {0x3cu, 0x66u, 0x66u, 0x7eu, 0x66u, 0x66u, 0x66u, 0u},
/* B */  {0x7cu, 0x66u, 0x66u, 0x7cu, 0x66u, 0x66u, 0x7cu, 0u},
/* C */  {0x3cu, 0x66u, 0x60u, 0x60u, 0x60u, 0x66u, 0x3cu, 0u},
/* D */  {0x78u, 0x6cu, 0x66u, 0x66u, 0x66u, 0x6cu, 0x78u, 0u},
/* E */  {0x7eu, 0x60u, 0x60u, 0x7cu, 0x60u, 0x60u, 0x7eu, 0u},
/* F */  {0x7eu, 0x60u, 0x60u, 0x7cu, 0x60u, 0x60u, 0x60u, 0u},
/* G */  {0x3cu, 0x66u, 0x60u, 0x6eu, 0x66u, 0x66u, 0x3cu, 0u},
/* H */  {0x66u, 0x66u, 0x66u, 0x7eu, 0x66u, 0x66u, 0x66u, 0u},
/* I */  {0x7eu, 0x18u, 0x18u, 0x18u, 0x18u, 0x18u, 0x7eu, 0u},
/* J */  {0x3eu, 0x0cu, 0x0cu, 0x0cu, 0x0cu, 0x6cu, 0x38u, 0u},
/* K */  {0x66u, 0x6cu, 0x78u, 0x70u, 0x78u, 0x6cu, 0x66u, 0u},
/* L */  {0x60u, 0x60u, 0x60u, 0x60u, 0x60u, 0x60u, 0x7eu, 0u},
/* M */  {0x63u, 0x77u, 0x7fu, 0x6bu, 0x6bu, 0x63u, 0x63u, 0u},
/* N */  {0x66u, 0x66u, 0x76u, 0x7eu, 0x6eu, 0x66u, 0x66u, 0u},
/* O */  {0x3cu, 0x66u, 0x66u, 0x66u, 0x66u, 0x66u, 0x3cu, 0u},
/* P */  {0x7cu, 0x66u, 0x66u, 0x7cu, 0x60u, 0x60u, 0x60u, 0u},
/* Q */  {0x3cu, 0x66u, 0x66u, 0x66u, 0x6au, 0x6cu, 0x36u, 0u},
/* R */  {0x7cu, 0x66u, 0x66u, 0x7cu, 0x6cu, 0x66u, 0x66u, 0u},
/* S */  {0x3cu, 0x66u, 0x60u, 0x3cu, 0x06u, 0x66u, 0x3cu, 0u},
/* T */  {0x7eu, 0x18u, 0x18u, 0x18u, 0x18u, 0x18u, 0x18u, 0u},
/* U */  {0x66u, 0x66u, 0x66u, 0x66u, 0x66u, 0x66u, 0x3cu, 0u},
/* V */  {0x66u, 0x66u, 0x66u, 0x66u, 0x66u, 0x3cu, 0x18u, 0u},
/* W */  {0x63u, 0x63u, 0x6bu, 0x6bu, 0x7fu, 0x77u, 0x63u, 0u},
/* X */  {0x66u, 0x66u, 0x3cu, 0x18u, 0x3cu, 0x66u, 0x66u, 0u},
/* Y */  {0x66u, 0x66u, 0x66u, 0x3cu, 0x18u, 0x18u, 0x18u, 0u},
/* Z */  {0x7eu, 0x06u, 0x0cu, 0x18u, 0x30u, 0x60u, 0x7eu, 0u},
/* [ */  {0x7cu, 0x60u, 0x60u, 0x60u, 0x60u, 0x60u, 0x7cu, 0u},
/* \ */  {0u, 0x60u, 0x30u, 0x18u, 0x0cu, 0x6u, 0u, 0u},
/* ] */  {0x3eu, 0x6u, 0x6u, 0x6u, 0x6u, 0x6u, 0x3eu, 0u},
/* ^ */  {0x18u, 0x3cu, 0x66u, 0x42u, 0u, 0u, 0u, 0u},
/* _ */  {0u, 0u, 0u, 0u, 0u, 0u, 0u, 0xffu},
/* ` */  {0x30u, 0x18u, 0xcu, 0u, 0u, 0u, 0u, 0u},
/* a */  {0u, 0u, 0x3cu, 0x6u, 0x3eu, 0x66u, 0x3eu, 0u},
/* b */  {0x60u, 0x60u, 0x7cu, 0x66u, 0x66u, 0x66u, 0x7cu, 0u},
/* c */  {0u, 0u, 0x3cu, 0x66u, 0x60u, 0x66u, 0x3cu, 0u},
/* d */  {0x6u, 0x6u, 0x3eu, 0x66u, 0x66u, 0x66u, 0x3eu, 0u},
/* e */  {0u, 0u, 0x3cu, 0x66u, 0x7eu, 0x60u, 0x3cu, 0u},
/* f */  {0x1cu, 0x30u, 0x30u, 0x7cu, 0x30u, 0x30u, 0x30u, 0u},
/* g */  {0u, 0u, 0x3eu, 0x66u, 0x66u, 0x3eu, 0x6u, 0x3cu},
/* h */  {0x60u, 0x60u, 0x7cu, 0x66u, 0x66u, 0x66u, 0x66u, 0u},
/* i */  {0x18u, 0u, 0x38u, 0x18u, 0x18u, 0x18u, 0x3cu, 0u},
/* j */  {0x18u, 0u, 0x38u, 0x18u, 0x18u, 0x18u, 0x18u, 0x70u},
/* k */  {0x60u, 0x60u, 0x66u, 0x6cu, 0x78u, 0x6cu, 0x66u, 0u},
/* l */  {0x38u, 0x18u, 0x18u, 0x18u, 0x18u, 0x18u, 0x3cu, 0u},
/* m */  {0u, 0u, 0x36u, 0x7fu, 0x6bu, 0x6bu, 0x63u, 0u},
/* n */  {0u, 0u, 0x7cu, 0x66u, 0x66u, 0x66u, 0x66u, 0u},
/* o */  {0u, 0u, 0x3cu, 0x66u, 0x66u, 0x66u, 0x3cu, 0u},
/* p */  {0u, 0u, 0x7cu, 0x66u, 0x66u, 0x7cu, 0x60u, 0x60u},
/* q */  {0u, 0u, 0x3eu, 0x66u, 0x66u, 0x3eu, 0x6u, 0x7u},
/* r */  {0u, 0u, 0x6eu, 0x73u, 0x60u, 0x60u, 0x60u, 0u},
/* s */  {0u, 0u, 0x3eu, 0x60u, 0x3cu, 0x6u, 0x7cu, 0u},
/* t */  {0x30u, 0x30u, 0x7cu, 0x30u, 0x30u, 0x30u, 0x1cu, 0u},
/* u */  {0u, 0u, 0x66u, 0x66u, 0x66u, 0x66u, 0x3eu, 0u},
/* v */  {0u, 0u, 0x66u, 0x66u, 0x66u, 0x3cu, 0x18u, 0u},
/* w */  {0u, 0u, 0x63u, 0x6bu, 0x6bu, 0x7fu, 0x36u, 0u},
/* x */  {0u, 0u, 0x66u, 0x3cu, 0x18u, 0x3cu, 0x66u, 0u},
/* y */  {0u, 0u, 0x66u, 0x66u, 0x66u, 0x3eu, 0x6u, 0x3cu},
/* z */  {0u, 0u, 0x7eu, 0x0cu, 0x18u, 0x30u, 0x7eu, 0u},
/* { */  {0x0cu, 0x18u, 0x18u, 0x70u, 0x18u, 0x18u, 0x0cu, 0u},
/* | */  {0x18u, 0x18u, 0x18u, 0u, 0x18u, 0x18u, 0x18u, 0u},
/* } */  {0x30u, 0x18u, 0x18u, 0xeu, 0x18u, 0x18u, 0x30u, 0u},
/* ~ */  {0x31u, 0x6bu, 0x46u, 0u, 0u, 0u, 0u, 0u},
/* DEL */  {0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u},
	/* 0x80 */
/* � */  {0x3u, 0x3u, 0x6u, 0x6u, 0x76u, 0x1Cu, 0xCu, 0u},
/* � */  {0x1Cu, 0x63u, 0x6Bu, 0x6Bu, 0x7Fu, 0x77u, 0x63u, 0u},
/* � */  {0x1Cu, 0x36u, 0u, 0x6Bu, 0x6Bu, 0x7Fu, 0x36u, 0u},
/* � */  {0xFEu, 0x92u, 0x92u, 0xF2u, 0x82u, 0x82u, 0xFEu, 0u},
/* � */  {0x66u, 0x99u, 0x81u, 0x42u, 0x81u, 0x99u, 0x66u, 0u},
/* � */  {0x18u, 0x66u, 0x42u, 0x66u, 0x3Cu, 0x18u, 0x18u, 0u},
/* � */  {0x18u, 0x66u, 0u, 0x66u, 0x66u, 0x3Eu, 0x6u, 0x3Cu},
/* � */  {0x7u, 0x1u, 0x2u, 0x64u, 0x94u, 0x60u, 0x90u, 0x60u},
/* � */  {0x18u, 0x28u, 0x4Fu, 0x81u, 0x4Fu, 0x28u, 0x18u, 0u},
/* � */  {0x18u, 0x14u, 0xF2u, 0x81u, 0xF2u, 0x14u, 0x18u, 0u},
/* � */  {0x3Cu, 0x24u, 0x24u, 0xE7u, 0x42u, 0x24u, 0x18u, 0u},
/* � */  {0x18u, 0x24u, 0x42u, 0xE7u, 0x24u, 0x24u, 0x3Cu, 0u},
/* � */  {0u, 0u, 0u, 0u, 0u, 0xDBu, 0xDBu, 0u},
/* � */  {0xF1u, 0x5Bu, 0x55u, 0x51u, 0u, 0u, 0u, 0u},
/* � */  {0xC0u, 0xCCu, 0x18u, 0x30u, 0x60u, 0xDBu, 0x1Bu, 0u},
/* � */  {0u, 0u, 0x3Cu, 0x7Eu, 0x7Eu, 0x3Cu, 0u, 0u},
	/* 90 */
/* � */  {0xCu, 0x18u, 0x18u, 0u, 0u, 0u, 0u, 0u},
/* � */  {0xCu, 0xCu, 0x18u, 0u, 0u, 0u, 0u, 0u},
/* � */  {0u, 0xCu, 0x18u, 0x30u, 0x30u, 0x18u, 0xCu, 0u},
/* � */  {0u, 0x30u, 0x18u, 0xCu, 0xCu, 0x18u, 0x30u, 0u},
/* � */  {0x1Bu, 0x36u, 0x36u, 0u, 0u, 0u, 0u, 0u},
/* � */  {0x36u, 0x36u, 0x6Cu, 0u, 0u, 0u, 0u, 0u},
/* � */  {0u, 0u, 0u, 0u, 0u, 0x36u, 0x36u, 0x6Cu},
/* � */  {0u, 0u, 0u, 0x3Cu, 0u, 0u, 0u, 0u},
/* � */  {0u, 0u, 0u, 0xFFu, 0u, 0u, 0u, 0u},
/* � */  {0u, 0u, 0u, 0x7Eu, 0u, 0u, 0u, 0u},
/* � */  {0x77u, 0xCCu, 0xCCu, 0xCFu, 0xCCu, 0xCCu, 0x77u, 0u},
/* � */  {0u, 0u, 0x6Eu, 0xDBu, 0xDFu, 0xD8u, 0x6Eu, 0u},
/* � */  {0x18u, 0x18u, 0x7Eu, 0x18u, 0x18u, 0x18u, 0x18u, 0x18u},
/* � */  {0x18u, 0x18u, 0x7Eu, 0x18u, 0x7Eu, 0x18u, 0x18u, 0x18u},
/* � */  {0x3Cu, 0x66u, 0x60u, 0xF6u, 0x66u, 0x66u, 0x66u, 0u},
/* � */  {0x3Eu, 0x66u, 0x66u, 0xF6u, 0x66u, 0x66u, 0x66u, 0u},
	/* a0 */
/* � */  {0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u},
/* � */  {0x18u, 0u, 0x18u, 0x18u, 0x18u, 0x18u, 0x18u, 0u},
/* � */  {0x8u, 0x3Eu, 0x6Bu, 0x68u, 0x6Bu, 0x3Eu, 0x8u, 0u},
/* � */  {0x1Cu, 0x36u, 0x30u, 0x7Cu, 0x30u, 0x30u, 0x7Eu, 0u},
/* � */  {0u, 0x66u, 0x3Cu, 0x66u, 0x66u, 0x3Cu, 0x66u, 0u},
/* � */  {0x66u, 0x3Cu, 0x18u, 0x18u, 0x7Eu, 0x18u, 0x18u, 0u},
/* � */  {0x18u, 0x18u, 0x18u, 0u, 0x18u, 0x18u, 0x18u, 0u},
/* � */  {0x3Cu, 0x60u, 0x3Cu, 0x66u, 0x3Cu, 0x6u, 0x3Cu, 0u},
/* � */  {0x66u, 0u, 0u, 0u, 0u, 0u, 0u, 0u},
/* � */  {0x3Cu, 0x42u, 0x99u, 0xA1u, 0xA1u, 0x99u, 0x42u, 0x3Cu},
/* � */  {0x1Cu, 0x6u, 0x1Eu, 0x36u, 0x1Eu, 0u, 0x3Eu, 0u},
/* � */  {0u, 0x33u, 0x66u, 0xCCu, 0xCCu, 0x66u, 0x33u, 0u},
/* � */  {0x7Eu, 0x6u, 0u, 0u, 0u, 0u, 0u, 0u},
/* � */  {0u, 0u, 0u, 0x7Eu, 0u, 0u, 0u, 0u},
/* � */  {0x3Cu, 0x42u, 0xB9u, 0xA5u, 0xB9u, 0xA5u, 0x42u, 0x3Cu},
/* � */  {0x7Eu, 0u, 0u, 0u, 0u, 0u, 0u, 0u},
	/* b0 */
/* � */  {0x3Cu, 0x66u, 0x3Cu, 0u, 0u, 0u, 0u, 0u},
/* � */  {0x18u, 0x18u, 0x7Eu, 0x18u, 0x18u, 0u, 0x7Eu, 0u},
/* � */  {0x38u, 0x4u, 0x18u, 0x20u, 0x3Cu, 0u, 0u, 0u},
/* � */  {0x38u, 0x4u, 0x18u, 0x4u, 0x38u, 0u, 0u, 0u},
/* � */  {0xCu, 0x18u, 0u, 0u, 0u, 0u, 0u, 0u},
/* � */  {0u, 0u, 0x33u, 0x33u, 0x33u, 0x33u, 0x3Eu, 0x60u},
/* � */  {0x3u, 0x3Eu, 0x76u, 0x76u, 0x36u, 0x36u, 0x3Eu, 0u},
/* � */  {0u, 0u, 0u, 0x18u, 0x18u, 0u, 0u, 0u},
/* � */  {0u, 0u, 0u, 0u, 0u, 0u, 0x18u, 0x30u},
/* � */  {0x10u, 0x30u, 0x10u, 0x10u, 0x38u, 0u, 0u, 0u},
/* � */  {0x1Cu, 0x36u, 0x36u, 0x36u, 0x1Cu, 0u, 0x3Eu, 0u},
/* � */  {0u, 0xCCu, 0x66u, 0x33u, 0x33u, 0x66u, 0xCCu, 0u},
/* � */  {0x40u, 0xC0u, 0x40u, 0x48u, 0x48u, 0xAu, 0xFu, 0x2u},
/* � */  {0x40u, 0xC0u, 0x40u, 0x4Fu, 0x41u, 0xFu, 0x8u, 0xFu},
/* � */  {0xE0u, 0x20u, 0xE0u, 0x28u, 0xE8u, 0xAu, 0xFu, 0x2u},
/* � */  {0x18u, 0u, 0x18u, 0x18u, 0x30u, 0x66u, 0x3Cu, 0u},
	/* c0 */
/* � */  {0x30u, 0x18u, 0u, 0x3Cu, 0x66u, 0x7Eu, 0x66u, 0u},
/* � */  {0xCu, 0x18u, 0u, 0x3Cu, 0x66u, 0x7Eu, 0x66u, 0u},
/* � */  {0x18u, 0x66u, 0u, 0x3Cu, 0x66u, 0x7Eu, 0x66u, 0u},
/* � */  {0x36u, 0x6Cu, 0u, 0x3Cu, 0x66u, 0x7Eu, 0x66u, 0u},
/* � */  {0x66u, 0x66u, 0u, 0x3Cu, 0x66u, 0x7Eu, 0x66u, 0u},
/* � */  {0x3Cu, 0x66u, 0x3Cu, 0x3Cu, 0x66u, 0x7Eu, 0x66u, 0u},
/* � */  {0x3Fu, 0x66u, 0x66u, 0x7Fu, 0x66u, 0x66u, 0x67u, 0u},
/* � */  {0x3Cu, 0x66u, 0x60u, 0x60u, 0x66u, 0x3Cu, 0x30u, 0x60u},
/* � */  {0x30u, 0x18u, 0x7Eu, 0x60u, 0x7Cu, 0x60u, 0x7Eu, 0u},
/* � */  {0xCu, 0x18u, 0x7Eu, 0x60u, 0x7Cu, 0x60u, 0x7Eu, 0u},
/* � */  {0x3Cu, 0x66u, 0x7Eu, 0x60u, 0x7Cu, 0x60u, 0x7Eu, 0u},
/* � */  {0x66u, 0u, 0x7Eu, 0x60u, 0x7Cu, 0x60u, 0x7Eu, 0u},
/* � */  {0x30u, 0x18u, 0u, 0x7Eu, 0x18u, 0x18u, 0x7Eu, 0u},
/* � */  {0xCu, 0x18u, 0u, 0x7Eu, 0x18u, 0x18u, 0x7Eu, 0u},
/* � */  {0x3Cu, 0x66u, 0u, 0x7Eu, 0x18u, 0x18u, 0x7Eu, 0u},
/* � */  {0x66u, 0x66u, 0u, 0x7Eu, 0x18u, 0x18u, 0x7Eu, 0u},
	/* d0 */
/* � */  {0x78u, 0x6Cu, 0x66u, 0xF6u, 0x66u, 0x6Cu, 0x78u, 0u},
/* � */  {0x36u, 0x6Cu, 0u, 0x66u, 0x76u, 0x6Eu, 0x66u, 0u},
/* � */  {0x30u, 0x18u, 0x3Cu, 0x66u, 0x66u, 0x66u, 0x3Cu, 0u},
/* � */  {0xCu, 0x18u, 0x3Cu, 0x66u, 0x66u, 0x66u, 0x3Cu, 0u},
/* � */  {0x3Cu, 0x66u, 0x3Cu, 0x66u, 0x66u, 0x66u, 0x3Cu, 0u},
/* � */  {0x36u, 0x6Cu, 0x3Cu, 0x66u, 0x66u, 0x66u, 0x3Cu, 0u},
/* � */  {0x66u, 0u, 0x3Cu, 0x66u, 0x66u, 0x66u, 0x3Cu, 0u},
/* � */  {0u, 0x63u, 0x36u, 0x1Cu, 0x1Cu, 0x36u, 0x63u, 0u},
/* � */  {0x3Du, 0x66u, 0x6Eu, 0x7Eu, 0x76u, 0x66u, 0xBCu, 0u},
/* � */  {0x30u, 0x18u, 0x66u, 0x66u, 0x66u, 0x66u, 0x3Cu, 0u},
/* � */  {0xCu, 0x18u, 0x66u, 0x66u, 0x66u, 0x66u, 0x3Cu, 0u},
/* � */  {0x3Cu, 0x66u, 0u, 0x66u, 0x66u, 0x66u, 0x3Cu, 0u},
/* � */  {0x66u, 0u, 0x66u, 0x66u, 0x66u, 0x66u, 0x3Cu, 0u},
/* � */  {0xCu, 0x18u, 0x66u, 0x66u, 0x3Cu, 0x18u, 0x18u, 0u},
/* � */  {0xF0u, 0x60u, 0x7Cu, 0x66u, 0x7Cu, 0x60u, 0xF0u, 0u},
/* � */  {0x3Cu, 0x66u, 0x66u, 0x6Cu, 0x66u, 0x66u, 0x6Cu, 0xC0u},
	/* e0 */
/* � */  {0x30u, 0x18u, 0x3Cu, 0x6u, 0x3Eu, 0x66u, 0x3Eu, 0u},
/* � */  {0xCu, 0x18u, 0x3Cu, 0x6u, 0x3Eu, 0x66u, 0x3Eu, 0u},
/* � */  {0x18u, 0x66u, 0x3Cu, 0x6u, 0x3Eu, 0x66u, 0x3Eu, 0u},
/* � */  {0x36u, 0x6Cu, 0x3Cu, 0x6u, 0x3Eu, 0x66u, 0x3Eu, 0u},
/* � */  {0x66u, 0u, 0x3Cu, 0x6u, 0x3Eu, 0x66u, 0x3Eu, 0u},
/* � */  {0x3Cu, 0x66u, 0x3Cu, 0x6u, 0x3Eu, 0x66u, 0x3Eu, 0u},
/* � */  {0u, 0u, 0x3Fu, 0xDu, 0x3Fu, 0x6Cu, 0x3Fu, 0u},
/* � */  {0u, 0u, 0x3Cu, 0x66u, 0x60u, 0x66u, 0x3Cu, 0x60u},
/* � */  {0x30u, 0x18u, 0x3Cu, 0x66u, 0x7Eu, 0x60u, 0x3Cu, 0u},
/* � */  {0xCu, 0x18u, 0x3Cu, 0x66u, 0x7Eu, 0x60u, 0x3Cu, 0u},
/* � */  {0x3Cu, 0x66u, 0x3Cu, 0x66u, 0x7Eu, 0x60u, 0x3Cu, 0u},
/* � */  {0x66u, 0u, 0x3Cu, 0x66u, 0x7Eu, 0x60u, 0x3Cu, 0u},
/* � */  {0x30u, 0x18u, 0u, 0x38u, 0x18u, 0x18u, 0x3Cu, 0u},
/* � */  {0xCu, 0x18u, 0u, 0x38u, 0x18u, 0x18u, 0x3Cu, 0u},
/* � */  {0x3Cu, 0x66u, 0u, 0x38u, 0x18u, 0x18u, 0x3Cu, 0u},
/* � */  {0x66u, 0u, 0u, 0x38u, 0x18u, 0x18u, 0x3Cu, 0u},
	/* f0 */
/* � */  {0x18u, 0x3Eu, 0xCu, 0x6u, 0x3Eu, 0x66u, 0x3Eu, 0u},
/* � */  {0x36u, 0x6Cu, 0u, 0x7Cu, 0x66u, 0x66u, 0x66u, 0u},
/* � */  {0x30u, 0x18u, 0u, 0x3Cu, 0x66u, 0x66u, 0x3Cu, 0u},
/* � */  {0xCu, 0x18u, 0u, 0x3Cu, 0x66u, 0x66u, 0x3Cu, 0u},
/* � */  {0x3Cu, 0x66u, 0u, 0x3Cu, 0x66u, 0x66u, 0x3Cu, 0u},
/* � */  {0x36u, 0x6Cu, 0u, 0x3Cu, 0x66u, 0x66u, 0x3Cu, 0u},
/* � */  {0x66u, 0u, 0u, 0x3Cu, 0x66u, 0x66u, 0x3Cu, 0u},
/* � */  {0u, 0x18u, 0u, 0xFFu, 0u, 0x18u, 0u, 0u},
/* � */  {0u, 0x2u, 0x3Cu, 0x6Eu, 0x76u, 0x66u, 0xBCu, 0u},
/* � */  {0x30u, 0x18u, 0u, 0x66u, 0x66u, 0x66u, 0x3Eu, 0u},
/* � */  {0xCu, 0x18u, 0u, 0x66u, 0x66u, 0x66u, 0x3Eu, 0u},
/* � */  {0x3Cu, 0x66u, 0u, 0x66u, 0x66u, 0x66u, 0x3Eu, 0u},
/* � */  {0x66u, 0u, 0u, 0x66u, 0x66u, 0x66u, 0x3Eu, 0u},
/* � */  {0xCu, 0x18u, 0x66u, 0x66u, 0x66u, 0x3Eu, 0x6u, 0x3Cu},
/* � */  {0x60u, 0x60u, 0x7Cu, 0x66u, 0x7Cu, 0x60u, 0x60u, 0u},
/* � */  {0x66u, 0u, 0x66u, 0x66u, 0x66u, 0x3Eu, 0x6u, 0x3Cu}
};

static unsigned int mode7font [96][20] = {
/*   */ {0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u},
/* ! */ {0u, 0x0300u, 0x0300u, 0x0300u, 0x0300u, 0x0300u, 0x0300u, 0x0300u, 0x0300u, 0x0300u, 0u, 0u, 0x0300u, 0x0300u, 0x0300u, 0u, 0u, 0u, 0u, 0u},
/* " */ {0u, 0x0CC0u, 0x0CC0u, 0x0CC0u, 0x0CC0u, 0x0CC0u, 0x0CC0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u},
/* # */ {0u, 0x0CC0u, 0x0CC0u, 0x0CC0u, 0x0CC0u, 0x3FF0u, 0x3FF0u, 0x0CC0u, 0x0CC0u, 0x3FF0u, 0x3FF0u, 0x0CC0u, 0x0CC0u, 0x0CC0u, 0x0CC0u, 0u, 0u, 0u, 0u, 0u},
/* $ */ {0u, 0x0FC0u, 0x1FE0u, 0x3B70u, 0x3330u, 0x3300u, 0x3B00u, 0x1FC0u, 0x0FE0u, 0x0370u, 0x0330u, 0x3330u, 0x3B70u, 0x1FE0u, 0x0FC0u, 0u, 0u, 0u, 0u, 0u},
/* % */ {0u, 0x3C00u, 0x3C00u, 0x3C30u, 0x3C70u, 0x00E0u, 0x01C0u, 0x0380u, 0x0700u, 0x0E00u, 0x1C00u, 0x38F0u, 0x30F0u, 0x00F0u, 0x00F0u, 0u, 0u, 0u, 0u, 0u},
/* & */ {0u, 0x0C00u, 0x1E00u, 0x3F00u, 0x3300u, 0x3300u, 0x3F00u, 0x1E00u, 0x1E00u, 0x3F30u, 0x33F0u, 0x31E0u, 0x39E0u, 0x1FF0u, 0x0F30u, 0u, 0u, 0u, 0u, 0u},
/* ' */ {0u, 0x0300u, 0x0300u, 0x0300u, 0x0300u, 0x0300u, 0x0300u, 0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u},
/* ( */ {0u, 0x00C0u, 0x01C0u, 0x0380u, 0x0700u, 0x0E00u, 0x0C00u, 0x0C00u, 0x0C00u, 0x0C00u, 0x0E00u, 0x0700u, 0x0380u, 0x01C0u, 0x00C0u, 0u, 0u, 0u, 0u, 0u},
/* ) */ {0u, 0x0C00u, 0x0E00u, 0x0700u, 0x0380u, 0x01C0u, 0x00C0u, 0x00C0u, 0x00C0u, 0x00C0u, 0x01C0u, 0x0380u, 0x0700u, 0x0E00u, 0x0C00u, 0u, 0u, 0u, 0u, 0u},
/* * */ {0u, 0x0300u, 0x0300u, 0x3330u, 0x3B70u, 0x1FE0u, 0x0FC0u, 0x0300u, 0x0300u, 0x0FC0u, 0x1FE0u, 0x3B70u, 0x3330u, 0x0300u, 0x0300u, 0u, 0u, 0u, 0u, 0u},
/* + */ {0u, 0u, 0u, 0x0300u, 0x0300u, 0x0300u, 0x0300u, 0x3FF0u, 0x3FF0u, 0x0300u, 0x0300u, 0x0300u, 0x0300u, 0u, 0u, 0u, 0u, 0u, 0u, 0u},
/* , */ {0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u, 0x0300u, 0x0300u, 0x0300u, 0x0700u, 0x0E00u, 0x0C00u, 0u, 0u, 0u},
/* - */ {0u, 0u, 0u, 0u, 0u, 0u, 0u, 0x0FC0u, 0x0FC0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u},
/* . */ {0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u, 0x0300u, 0x0300u, 0u, 0u, 0u, 0u, 0u},
/* / */ {0u, 0u, 0u, 0x0030u, 0x0070u, 0x00E0u, 0x01C0u, 0x0380u, 0x0700u, 0x0E00u, 0x1C00u, 0x3800u, 0x3000u, 0u, 0u, 0u, 0u, 0u, 0u, 0u},
/* 0 */ {0u, 0x0300u, 0x0780u, 0x0FC0u, 0x1CE0u, 0x3870u, 0x3030u, 0x3030u, 0x3030u, 0x3030u, 0x3870u, 0x1CE0u, 0x0FC0u, 0x0780u, 0x0300u, 0u, 0u, 0u, 0u, 0u},
/* 1 */ {0u, 0x0300u, 0x0300u, 0x0F00u, 0x0F00u, 0x0300u, 0x0300u, 0x0300u, 0x0300u, 0x0300u, 0x0300u, 0x0300u, 0x0300u, 0x0FC0u, 0x0FC0u, 0u, 0u, 0u, 0u, 0u},
/* 2 */ {0u, 0x0FC0u, 0x1FE0u, 0x3870u, 0x3030u, 0x0030u, 0x0070u, 0x03E0u, 0x07C0u, 0x0E00u, 0x1C00u, 0x3800u, 0x3000u, 0x3FF0u, 0x3FF0u, 0u, 0u, 0u, 0u, 0u},
/* 3 */ {0u, 0x3FF0u, 0x3FF0u, 0x0030u, 0x0070u, 0x00E0u, 0x00C0u, 0x03C0u, 0x03E0u, 0x0070u, 0x0030u, 0x3030u, 0x3870u, 0x1FE0u, 0x0FC0u, 0u, 0u, 0u, 0u, 0u},
/* 4 */ {0u, 0x00C0u, 0x00C0u, 0x03C0u, 0x07C0u, 0x0EC0u, 0x1CC0u, 0x38C0u, 0x30C0u, 0x3FF0u, 0x3FF0u, 0x00C0u, 0x00C0u, 0x00C0u, 0x00C0u, 0u, 0u, 0u, 0u, 0u},
/* 5 */ {0u, 0x3FF0u, 0x3FF0u, 0x3000u, 0x3000u, 0x3FC0u, 0x3FE0u, 0x0070u, 0x0030u, 0x0030u, 0x0030u, 0x3030u, 0x3870u, 0x1FE0u, 0x0FC0u, 0u, 0u, 0u, 0u, 0u},
/* 6 */ {0u, 0x03C0u, 0x07C0u, 0x0E00u, 0x1C00u, 0x3800u, 0x3000u, 0x3FC0u, 0x3FE0u, 0x3070u, 0x3030u, 0x3030u, 0x3870u, 0x1FE0u, 0x0FC0u, 0u, 0u, 0u, 0u, 0u},
/* 7 */ {0u, 0x3FF0u, 0x3FF0u, 0x0030u, 0x0070u, 0x00E0u, 0x01C0u, 0x0380u, 0x0700u, 0x0E00u, 0x0C00u, 0x0C00u, 0x0C00u, 0x0C00u, 0x0C00u, 0u, 0u, 0u, 0u, 0u},
/* 8 */ {0u, 0x0FC0u, 0x1FE0u, 0x3870u, 0x3030u, 0x3030u, 0x3870u, 0x1FE0u, 0x1FE0u, 0x3870u, 0x3030u, 0x3030u, 0x3870u, 0x1FE0u, 0x0FC0u, 0u, 0u, 0u, 0u, 0u},
/* 9 */ {0u, 0x0FC0u, 0x1FE0u, 0x3870u, 0x3030u, 0x3030u, 0x3830u, 0x1FF0u, 0x0FF0u, 0x0030u, 0x0070u, 0x00E0u, 0x01C0u, 0x0F80u, 0x0F00u, 0u, 0u, 0u, 0u, 0u},
/* : */ {0u, 0u, 0u, 0u, 0u, 0x0300u, 0x0300u, 0u, 0u, 0u, 0u, 0x0300u, 0x0300u, 0u, 0u, 0u, 0u, 0u, 0u, 0u},
/* ; */ {0u, 0u, 0u, 0u, 0u, 0x0300u, 0x0300u, 0u, 0u, 0u, 0u, 0x0300u, 0x0300u, 0x0300u, 0x0700u, 0x0E00u, 0x0C00u, 0u, 0u, 0u},
/* < */ {0u, 0x00C0u, 0x01C0u, 0x0380u, 0x0700u, 0x0E00u, 0x1C00u, 0x3800u, 0x3800u, 0x1C00u, 0x0E00u, 0x0700u, 0x0380u, 0x01C0u, 0x00C0u, 0u, 0u, 0u, 0u, 0u},
/* = */ {0u, 0u, 0u, 0u, 0u, 0x3FF0u, 0x3FF0u, 0u, 0u, 0x3FF0u, 0x3FF0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u},
/* > */ {0u, 0x0C00u, 0x0E00u, 0x0700u, 0x0380u, 0x01C0u, 0x00E0u, 0x0070u, 0x0070u, 0x00E0u, 0x01C0u, 0x0380u, 0x0700u, 0x0E00u, 0x0C00u, 0u, 0u, 0u, 0u, 0u},
/* ? */ {0u, 0xFC0u, 0x1FE0u, 0x3870u, 0x3070u, 0xE0u, 0x1C0u, 0x380u, 0x300u, 0x300u, 0x300u, 0u, 0u, 0x300u, 0x300u, 0u, 0u, 0u, 0u, 0u},
/* @ */ {0u, 0x0FC0u, 0x1FE0u, 0x3870u, 0x3030u, 0x33F0u, 0x33F0u, 0x3330u, 0x3330u, 0x33F0u, 0x33F0u, 0x3000u, 0x3800u, 0x1FC0u, 0x0FC0u, 0u, 0u, 0u, 0u, 0u},
/* A */ {0u, 0x0300u, 0x0780u, 0x0FC0u, 0x1CE0u, 0x3870u, 0x3030u, 0x3030u, 0x3030u, 0x3FF0u, 0x3FF0u, 0x3030u, 0x3030u, 0x3030u, 0x3030u, 0u, 0u, 0u, 0u, 0u},
/* B */ {0u, 0x3FC0u, 0x3FE0u, 0x3070u, 0x3030u, 0x3030u, 0x3070u, 0x3FE0u, 0x3FE0u, 0x3070u, 0x3030u, 0x3030u, 0x3070u, 0x3FE0u, 0x3FC0u, 0u, 0u, 0u, 0u, 0u},
/* C */ {0u, 0xFC0u, 0x1FE0u, 0x3870u, 0x3030u, 0x3000u, 0x3000u, 0x3000u, 0x3000u, 0x3000u, 0x3000u, 0x3030u, 0x3870u, 0x1FE0u, 0xFC0u, 0u, 0u, 0u, 0u, 0u },
/* D */ {0u, 0x3FC0u, 0x3FE0u, 0x3070u, 0x3030u, 0x3030u, 0x3030u, 0x3030u, 0x3030u, 0x3030u, 0x3030u, 0x3030u, 0x3070u, 0x3FE0u, 0x3FC0u, 0u, 0u, 0u, 0u, 0u },
/* E */ {0u, 0x3FF0u, 0x3FF0u, 0x3000u, 0x3000u, 0x3000u, 0x3000u, 0x3FC0u, 0x3FC0u, 0x3000u, 0x3000u, 0x3000u, 0x3000u, 0x3FF0u, 0x3FF0u, 0u, 0u, 0u, 0u, 0u },
/* F */ {0u, 0x3FF0u, 0x3FF0u, 0x3000u, 0x3000u, 0x3000u, 0x3000u, 0x3FC0u, 0x3FC0u, 0x3000u, 0x3000u, 0x3000u, 0x3000u, 0x3000u, 0x3000u, 0u, 0u, 0u, 0u, 0u },
/* G */ {0u, 0xFC0u, 0x1FE0u, 0x3870u, 0x3030u, 0x3000u, 0x3000u, 0x3000u, 0x3000u, 0x30F0u, 0x30F0u, 0x3030u, 0x3830u, 0x1FF0u, 0xFF0u, 0u, 0u, 0u, 0u, 0u },
/* H */ {0u, 0x3030u, 0x3030u, 0x3030u, 0x3030u, 0x3030u, 0x3030u, 0x3FF0u, 0x3FF0u, 0x3030u, 0x3030u, 0x3030u, 0x3030u, 0x3030u, 0x3030u, 0u, 0u, 0u, 0u, 0u },
/* I */ {0u, 0xFC0u, 0xFC0u, 0x300u, 0x300u, 0x300u, 0x300u, 0x300u, 0x300u, 0x300u, 0x300u, 0x300u, 0x300u, 0xFC0u, 0xFC0u, 0u, 0u, 0u, 0u, 0u },
/* J */ {0u, 0x30u, 0x30u, 0x30u, 0x30u, 0x30u, 0x30u, 0x30u, 0x30u, 0x30u, 0x30u, 0x3030u, 0x3870u, 0x1FE0u, 0xFC0u, 0u, 0u, 0u, 0u, 0u },
/* K */ {0u, 0x3030u, 0x3070u, 0x30E0u, 0x31C0u, 0x3380u, 0x3700u, 0x3E00u, 0x3E00u, 0x3700u, 0x3380u, 0x31C0u, 0x30E0u, 0x3070u, 0x3030u, 0u, 0u, 0u, 0u, 0u },
/* L */ {0u, 0x3000u, 0x3000u, 0x3000u, 0x3000u, 0x3000u, 0x3000u, 0x3000u, 0x3000u, 0x3000u, 0x3000u, 0x3000u, 0x3000u, 0x3FF0u, 0x3FF0u, 0u, 0u, 0u, 0u, 0u },
/* M */ {0u, 0x3030u, 0x3030u, 0x3CF0u, 0x3FF0u, 0x37B0u, 0x3330u, 0x3330u, 0x3330u, 0x3030u, 0x3030u, 0x3030u, 0x3030u, 0x3030u, 0x3030u, 0u, 0u, 0u, 0u, 0u },
/* N */ {0u, 0x3030u, 0x3030u, 0x3030u, 0x3030u, 0x3C30u, 0x3E30u, 0x3730u, 0x33B0u, 0x31F0u, 0x30F0u, 0x3030u, 0x3030u, 0x3030u, 0x3030u, 0u, 0u, 0u, 0u, 0u },
/* O */ {0u, 0xFC0u, 0x1FE0u, 0x3870u, 0x3030u, 0x3030u, 0x3030u, 0x3030u, 0x3030u, 0x3030u, 0x3030u, 0x3030u, 0x3870u, 0x1FE0u, 0xFC0u, 0u, 0u, 0u, 0u, 0u },
/* P */ {0u, 0x3FC0u, 0x3FE0u, 0x3070u, 0x3030u, 0x3030u, 0x3070u, 0x3FE0u, 0x3FC0u, 0x3000u, 0x3000u, 0x3000u, 0x3000u, 0x3000u, 0x3000u, 0u, 0u, 0u, 0u, 0u },
/* Q */ {0u, 0xFC0u, 0x1FE0u, 0x3870u, 0x3030u, 0x3030u, 0x3030u, 0x3030u, 0x3030u, 0x3330u, 0x33F0u, 0x31E0u, 0x39E0u, 0x1FF0u, 0xF30u, 0u, 0u, 0u, 0u, 0u },
/* R */ {0u, 0x3FC0u, 0x3FE0u, 0x3070u, 0x3030u, 0x3030u, 0x3070u, 0x3FE0u, 0x3FC0u, 0x3300u, 0x3380u, 0x31C0u, 0x30E0u, 0x3070u, 0x3030u, 0u, 0u, 0u, 0u, 0u },
/* S */ {0u, 0xFC0u, 0x1FE0u, 0x3870u, 0x3030u, 0x3000u, 0x3800u, 0x1FC0u, 0xFE0u, 0x70u, 0x30u, 0x3030u, 0x3870u, 0x1FE0u, 0xFC0u, 0u, 0u, 0u, 0u, 0u },
/* T */ {0u, 0x3FF0u, 0x3FF0u, 0x300u, 0x300u, 0x300u, 0x300u, 0x300u, 0x300u, 0x300u, 0x300u, 0x300u, 0x300u, 0x300u, 0x300u, 0u, 0u, 0u, 0u, 0u },
/* U */ {0u, 0x3030u, 0x3030u, 0x3030u, 0x3030u, 0x3030u, 0x3030u, 0x3030u, 0x3030u, 0x3030u, 0x3030u, 0x3030u, 0x3870u, 0x1FE0u, 0xFC0u, 0u, 0u, 0u, 0u, 0u },
/* V */ {0u, 0x3030u, 0x3030u, 0x3030u, 0x3030u, 0x3030u, 0x3870u, 0x1CE0u, 0xCC0u, 0xCC0u, 0xFC0u, 0x780u, 0x300u, 0x300u, 0x300u, 0u, 0u, 0u, 0u, 0u },
/* W */ {0u, 0x3030u, 0x3030u, 0x3030u, 0x3030u, 0x3030u, 0x3030u, 0x3030u, 0x3030u, 0x3330u, 0x3330u, 0x3330u, 0x3FF0u, 0x1FE0u, 0xCC0u, 0u, 0u, 0u, 0u, 0u },
/* X */ {0u, 0x3030u, 0x3030u, 0x3030u, 0x3870u, 0x1CE0u, 0xFC0u, 0x780u, 0x780u, 0xFC0u, 0x1CE0u, 0x3870u, 0x3030u, 0x3030u, 0x3030u, 0u, 0u, 0u, 0u, 0u },
/* Y */ {0u, 0x3030u, 0x3030u, 0x3030u, 0x3870u, 0x1CE0u, 0xFC0u, 0x780u, 0x300u, 0x300u, 0x300u, 0x300u, 0x300u, 0x300u, 0x300u, 0u, 0u, 0u, 0u, 0u },
/* Z */ {0u, 0x3FF0u, 0x3FF0u, 0x30u, 0x70u, 0xE0u, 0x1C0u, 0x380u, 0x700u, 0xE00u, 0x1C00u, 0x3800u, 0x3000u, 0x3FF0u, 0x3FF0u, 0u, 0u, 0u, 0u, 0u },
/* [ */ {0u, 0u, 0u, 0x300u, 0x700u, 0xE00u, 0xC00u, 0x3FF0u, 0x3FF0u, 0xC00u, 0xE00u, 0x700u, 0x300u, 0u, 0u, 0u, 0u, 0u, 0u, 0u },
/* \ */ {0u, 0x3000u, 0x3000u, 0x3000u, 0x3000u, 0x3000u, 0x3000u, 0x3000u, 0x3000u, 0x33C0u, 0x33E0u, 0x70u, 0x70u, 0xE0u, 0x1C0u, 0x380u, 0x300u, 0x3F0u, 0x3F0u, 0u},
/* ] */ {0u, 0u, 0u, 0x300u, 0x380u, 0x1C0u, 0xC0u, 0x3FF0u, 0x3FF0u, 0xC0u, 0x1C0u, 0x380u, 0x300u, 0u, 0u, 0u, 0u, 0u, 0u, 0u },
/* ^ */ {0u, 0u, 0u, 0x300u, 0x300u, 0xFC0u, 0x1FE0u, 0x3B70u, 0x3330u, 0x300u, 0x300u, 0x300u, 0x300u, 0u, 0u, 0u, 0u, 0u, 0u, 0u },
/* _ */ {0u, 0u, 0u, 0u, 0u, 0u, 0u, 0x3FF0u, 0x3FF0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u },
/* ` */ {0u, 0x3C0u, 0x7E0u, 0xE70u, 0xC30u, 0xC00u, 0xC00u, 0x3F00u, 0x3F00u, 0xC00u, 0xC00u, 0xC00u, 0xC00u, 0x3FF0u, 0x3FF0u, 0u, 0u, 0u, 0u, 0u },
/* a */ {0u, 0u, 0u, 0u, 0u, 0xFC0u, 0xFE0u, 0x70u, 0x30u, 0xFF0u, 0x1FF0u, 0x3830u, 0x3830u, 0x1FF0u, 0xFF0u, 0u, 0u, 0u, 0u, 0u },
/* b */ {0u, 0x3000u, 0x3000u, 0x3000u, 0x3000u, 0x3FC0u, 0x3FE0u, 0x3070u, 0x3030u, 0x3030u, 0x3030u, 0x3030u, 0x3070u, 0x3FE0u, 0x3FC0u, 0u, 0u, 0u, 0u, 0u },
/* c */ {0u, 0u, 0u, 0u, 0u, 0xFF0u, 0x1FF0u, 0x3800u, 0x3000u, 0x3000u, 0x3000u, 0x3000u, 0x3800u, 0x1FF0u, 0xFF0u, 0u, 0u, 0u, 0u, 0u },
/* d */ {0u, 0x30u, 0x30u, 0x30u, 0x30u, 0xFF0u, 0x1FF0u, 0x3830u, 0x3030u, 0x3030u, 0x3030u, 0x3030u, 0x3830u, 0x1FF0u, 0xFF0u, 0u, 0u, 0u, 0u, 0u },
/* e */ {0u, 0u, 0u, 0u, 0u, 0xFC0u, 0x1FE0u, 0x3870u, 0x3030u, 0x3FF0u, 0x3FF0u, 0x3000u, 0x3800u, 0x1FC0u, 0xFC0u, 0u, 0u, 0u, 0u, 0u },
/* f */ {0u, 0xC0u, 0x1C0u, 0x380u, 0x300u, 0x300u, 0x300u, 0xFC0u, 0xFC0u, 0x300u, 0x300u, 0x300u, 0x300u, 0x300u, 0x300u, 0u, 0u, 0u, 0u, 0u },
/* g */ {0u, 0u, 0u, 0u, 0u, 0xFF0u, 0x1FF0u, 0x3830u, 0x3030u, 0x3030u, 0x3030u, 0x3030u, 0x3830u, 0x1FF0u, 0xFF0u, 0x30u, 0x70u, 0xFE0u, 0xFC0u, 0u},
/* h */ {0u, 0x3000u, 0x3000u, 0x3000u, 0x3000u, 0x3FC0u, 0x3FE0u, 0x3070u, 0x3030u, 0x3030u, 0x3030u, 0x3030u, 0x3030u, 0x3030u, 0x3030u, 0u, 0u, 0u, 0u, 0u },
/* i */ {0u, 0x300u, 0x300u, 0u, 0u, 0xF00u, 0xF00u, 0x300u, 0x300u, 0x300u, 0x300u, 0x300u, 0x300u, 0xFC0u, 0xFC0u, 0u, 0u, 0u, 0u, 0u },
/* j */ {0u, 0x300u, 0x300u, 0u, 0u, 0x300u, 0x300u, 0x300u, 0x300u, 0x300u, 0x300u, 0x300u, 0x300u, 0x300u, 0x300u, 0x300u, 0x700u, 0xE00u, 0xC00u, 0u},
/* k */ {0u, 0xC00u, 0xC00u, 0xC00u, 0xC00u, 0xC30u, 0xC70u, 0xCE0u, 0xDC0u, 0xF80u, 0xF80u, 0xDC0u, 0xCE0u, 0xC70u, 0xC30u, 0u, 0u, 0u, 0u, 0u },
/* l */ {0u, 0xF00u, 0xF00u, 0x300u, 0x300u, 0x300u, 0x300u, 0x300u, 0x300u, 0x300u, 0x300u, 0x300u, 0x300u, 0xFC0u, 0xFC0u, 0u, 0u, 0u, 0u, 0u },
/* m */ {0u, 0u, 0u, 0u, 0u, 0x3CC0u, 0x3FE0u, 0x37F0u, 0x3330u, 0x3330u, 0x3330u, 0x3330u, 0x3330u, 0x3330u, 0x3330u, 0u, 0u, 0u, 0u, 0u },
/* n */ {0u, 0u, 0u, 0u, 0u, 0x3FC0u, 0x3FE0u, 0x3070u, 0x3030u, 0x3030u, 0x3030u, 0x3030u, 0x3030u, 0x3030u, 0x3030u, 0u, 0u, 0u, 0u, 0u },
/* o */ {0u, 0u, 0u, 0u, 0u, 0xFC0u, 0x1FE0u, 0x3870u, 0x3030u, 0x3030u, 0x3030u, 0x3030u, 0x3870u, 0x1FE0u, 0xFC0u, 0u, 0u, 0u, 0u, 0u },
/* p */ {0u, 0u, 0u, 0u, 0u, 0x3FC0u, 0x3FE0u, 0x3070u, 0x3030u, 0x3030u, 0x3030u, 0x3030u, 0x3070u, 0x3FE0u, 0x3FC0u, 0x3000u, 0x3000u, 0x3000u, 0x3000u, 0u},
/* q */ {0u, 0u, 0u, 0u, 0u, 0xFF0u, 0x1FF0u, 0x3830u, 0x3030u, 0x3030u, 0x3030u, 0x3030u, 0x3830u, 0x1FF0u, 0xFF0u, 0x30u, 0x30u, 0x30u, 0x30u, 0u},
/* r */ {0u, 0u, 0u, 0u, 0u, 0xCF0u, 0xDF0u, 0xF80u, 0xF00u, 0xC00u, 0xC00u, 0xC00u, 0xC00u, 0xC00u, 0xC00u, 0u, 0u, 0u, 0u, 0u },
/* s */ {0u, 0u, 0u, 0u, 0u, 0xFF0u, 0x1FF0u, 0x3800u, 0x3800u, 0x1FC0u, 0xFE0u, 0x70u, 0x70u, 0x3FE0u, 0x3FC0u, 0u, 0u, 0u, 0u, 0u },
/* t */ {0u, 0x300u, 0x300u, 0x300u, 0x300u, 0xFC0u, 0xFC0u, 0x300u, 0x300u, 0x300u, 0x300u, 0x300u, 0x380u, 0x1C0u, 0xC0u, 0u, 0u, 0u, 0u, 0u },
/* u */ {0u, 0u, 0u, 0u, 0u, 0x3030u, 0x3030u, 0x3030u, 0x3030u, 0x3030u, 0x3030u, 0x3030u, 0x3830u, 0x1FF0u, 0xFF0u, 0u, 0u, 0u, 0u, 0u },
/* v */ {0u, 0u, 0u, 0u, 0u, 0x3030u, 0x3030u, 0x3030u, 0x3870u, 0x1CE0u, 0xCC0u, 0xCC0u, 0xFC0u, 0x780u, 0x300u, 0u, 0u, 0u, 0u, 0u },
/* w */ {0u, 0u, 0u, 0u, 0u, 0x3030u, 0x3030u, 0x3030u, 0x3030u, 0x3330u, 0x3330u, 0x3330u, 0x3FF0u, 0x1FE0u, 0xCC0u, 0u, 0u, 0u, 0u, 0u },
/* x */ {0u, 0u, 0u, 0u, 0u, 0x3030u, 0x3870u, 0x1CE0u, 0xFC0u, 0x780u, 0x780u, 0xFC0u, 0x1CE0u, 0x3870u, 0x3030u, 0u, 0u, 0u, 0u, 0u },
/* y */ {0u, 0u, 0u, 0u, 0u, 0x3030u, 0x3030u, 0x3030u, 0x3030u, 0x3030u, 0x3030u, 0x3030u, 0x3830u, 0x1FF0u, 0xFF0u, 0x30u, 0x70u, 0xFE0u, 0xFC0u, 0u},
/* z */ {0u, 0u, 0u, 0u, 0u, 0x3FF0u, 0x3FF0u, 0xC0u, 0x1C0u, 0x380u, 0x700u, 0xE00u, 0xC00u, 0x3FF0u, 0x3FF0u, 0u, 0u, 0u, 0u, 0u },
/* { */ {0u, 0xC00u, 0xC00u, 0xC00u, 0xC00u, 0xC00u, 0xC00u, 0xC00u, 0xC00u, 0xC30u, 0xC30u, 0xF0u, 0x1F0u, 0x3B0u, 0x330u, 0x3F0u, 0x3F0u, 0x30u, 0x30u, 0u},
/* | */ {0u, 0xCC0u, 0xCC0u, 0xCC0u, 0xCC0u, 0xCC0u, 0xCC0u, 0xCC0u, 0xCC0u, 0xCC0u, 0xCC0u, 0xCC0u, 0xCC0u, 0xCC0u, 0xCC0u, 0u, 0u, 0u, 0u, 0u },
/* } */ {0u, 0x3C00u, 0x3E00u, 0x700u, 0x700u, 0x3E00u, 0x3E00u, 0x700u, 0x700u, 0x3E30u, 0x3C30u, 0xF0u, 0x1F0u, 0x3B0u, 0x330u, 0x3F0u, 0x3F0u, 0x30u, 0x30u, 0u},
/* ~ */ {0u, 0u, 0u, 0x300u, 0x300u, 0u, 0u, 0x3FF0u, 0x3FF0u, 0u, 0u, 0x300u, 0x300u, 0u, 0u, 0u, 0u, 0u, 0u, 0u },
/*255 */  {0u, 0x3FF0u, 0x3FF0u, 0x3FF0u,0x3FF0u,0x3FF0u,0x3FF0u,0x3FF0u,0x3FF0u,0x3FF0u,0x3FF0u,0x3FF0u,0x3FF0u,0x3FF0u,0x3FF0u, 0u, 0u, 0u, 0u, 0u}
};

unsigned int XPPC=8;		/* Size of character in pixels in X direction */
unsigned int YPPC=8;		/* Size of character in pixels in Y direction */
unsigned int M7XPPC=16;		/* Size of Mode 7 characters in X direction */
unsigned int M7YPPC=20;		/* Size of Mode 7 characters in Y direction */

static void reset_mode7() {
  int p, q;
  vdu141mode = 1;
  vdu141on = 0;
  mode7highbit = 0;
  mode7sepgrp = 0;
  mode7sepreal = 0;
  mode7conceal = 0;
  mode7hold = 0;
  mode7flash = 0;
  mode7bank = 0;
  mode7timer=0;
  mode7prevchar=32;
  place_rect.h=M7YPPC;
  font_rect.h=M7YPPC;

  for(p=0;p<26;p++) vdu141track[p]=0;
  for (p=0; p<25; p++) {
    for (q=0; q<40; q++) mode7frame[p][q]=32;
  }
}

void reset_sysfont(int x) {
  int p, c, i;

  if (!x) {
    memcpy(sysfont, sysfontbase, sizeof(sysfont));
    return;
  }
  if ((x>=1) && (x<= 7)) {
    p=(x-1)*32;
    for (c=0; c<= 31; c++) 
      for (i=0; i<= 7; i++) sysfont[p+c][i]=sysfontbase[p+c][i];
  }
  if (x ==8){
    for (c=0; c<=95; c++)
      for (i=0; i<= 7; i++) sysfont[c][i]=sysfontbase[c][i];
  }
}

static void do_sdl_flip(SDL_Surface *layer) {
  if (autorefresh==1) SDL_Flip(layer);
}

static void do_sdl_updaterect(SDL_Surface *layer, Sint32 x, Sint32 y, Sint32 w, Sint32 h) {
  if (autorefresh==1) SDL_UpdateRect(layer, x, y, w, h);
}

static int istextonly(void) {
  return ((screenmode == 3 || screenmode == 6 || screenmode == 7));
}

static int32 riscoscolour(colour) {
  return (((colour & 0xFF) <<16) + (colour & 0xFF00) + ((colour & 0xFF0000) >> 16));
}

static int32 tint24bit(colour, tint) {
  colour=(colour & 0xC0C0C0);
  colour += ((colour & 0xF0) ? (tint << 4) : 0)+((colour & 0xF000) ? (tint << 12) : 0)+((colour & 0xF00000) ? (tint << 20) : 0);
  if (colour == 0) colour+=(tint << 4)+(tint << 12)+(tint << 20);
  return colour + (colour >> 4);
}

static int32 colour24bit(colour, tint) {
  int32 col=(((colour & 1) << 6) + ((colour & 2) << 6)) +
	 (((colour & 4) << 12) + ((colour & 8) << 12)) +
	 (((colour & 16) << 18) + ((colour & 32) << 18));
  col = tint24bit(col, tint);
  return col;
}

/*
** 'find_cursor' locates the cursor on the text screen and ensures that
** its position is valid, that is, lies within the text window
*/
void find_cursor(void) {
//  if (graphmode!=FULLSCREEN) {
//    xtext = wherex()-1;
//    ytext = wherey()-1;
//  }
}

void set_rgb(void) {
  int j;
  if (colourdepth == COL24BIT) {
    tf_colour = SDL_MapRGB(sdl_fontbuf->format, (text_physforecol & 0xFF), ((text_physforecol & 0xFF00) >> 8), ((text_physforecol & 0xFF0000) >> 16));
    tb_colour = SDL_MapRGB(sdl_fontbuf->format, (text_physbackcol & 0xFF), ((text_physbackcol & 0xFF00) >> 8), ((text_physbackcol & 0xFF0000) >> 16));
    gf_colour = SDL_MapRGB(sdl_fontbuf->format, (graph_physforecol & 0xFF), ((graph_physforecol & 0xFF00) >> 8), ((graph_physforecol & 0xFF0000) >> 16));
    gb_colour = SDL_MapRGB(sdl_fontbuf->format, (graph_physbackcol & 0xFF), ((graph_physbackcol & 0xFF00) >> 8), ((graph_physbackcol & 0xFF0000) >> 16));
  } else {
    j = text_physforecol*3;
    tf_colour = SDL_MapRGB(sdl_fontbuf->format, palette[j], palette[j+1], palette[j+2]);
    j = text_physbackcol*3;
    tb_colour = SDL_MapRGB(sdl_fontbuf->format, palette[j], palette[j+1], palette[j+2]);

    j = graph_physforecol*3;
    gf_colour = SDL_MapRGB(sdl_fontbuf->format, palette[j], palette[j+1], palette[j+2]);
    j = graph_physbackcol*3;
    gb_colour = SDL_MapRGB(sdl_fontbuf->format, palette[j], palette[j+1], palette[j+2]);
  }
}

void sdlchar(int32 ch) {
  int32 y, line, mxppc, myppc;
  if (cursorstate == ONSCREEN) cursorstate = SUSPENDED;
  if (screenmode == 7) {
    mxppc=M7XPPC;
    myppc=M7YPPC;
  } else {
    mxppc=XPPC;
    myppc=YPPC;
  }
  place_rect.x = xtext * mxppc;
  place_rect.y = ytext * myppc;
  SDL_FillRect(sdl_fontbuf, NULL, tb_colour);
  for (y = 0; y < YPPC; y++) {
    if (screenmode == 7) {
      line = mode7font[ch-' '][y];
    } else {
      line = sysfont[ch-' '][y];
    }
    if (line != 0) {
      if (line & 0x80) *((Uint32*)sdl_fontbuf->pixels + 0 + y*mxppc) = tf_colour;
      if (line & 0x40) *((Uint32*)sdl_fontbuf->pixels + 1 + y*mxppc) = tf_colour;
      if (line & 0x20) *((Uint32*)sdl_fontbuf->pixels + 2 + y*mxppc) = tf_colour;
      if (line & 0x10) *((Uint32*)sdl_fontbuf->pixels + 3 + y*mxppc) = tf_colour;
      if (line & 0x08) *((Uint32*)sdl_fontbuf->pixels + 4 + y*mxppc) = tf_colour;
      if (line & 0x04) *((Uint32*)sdl_fontbuf->pixels + 5 + y*mxppc) = tf_colour;
      if (line & 0x02) *((Uint32*)sdl_fontbuf->pixels + 6 + y*mxppc) = tf_colour;
      if (line & 0x01) *((Uint32*)sdl_fontbuf->pixels + 7 + y*mxppc) = tf_colour;
    }
  }
  SDL_BlitSurface(sdl_fontbuf, &font_rect, screen0, &place_rect);
  if (echo) do_sdl_updaterect(screen0, xtext * XPPC, ytext * YPPC, XPPC, YPPC);
}

/*
** 'scroll_text' is called to move the text window up or down a line.
** Note that the coordinates here are in RISC OS text coordinates which
** start at (0, 0) whereas conio's start with (1, 1) at the top left-hand
** corner of the screen.
*/
static void scroll_text(updown direction) {
  int n, xx, yy;
  if (!textwin && direction == SCROLL_UP) {	/* Text window is the whole screen and scrolling upwards */
    scroll_rect.x = 0;
    scroll_rect.y = YPPC;
    scroll_rect.w = vscrwidth;
    scroll_rect.h = YPPC * textheight-1;
    SDL_BlitSurface(screen0, &scroll_rect, screen1, NULL);
    line_rect.x = 0;
    line_rect.y = YPPC * textheight-1;
    line_rect.w = vscrwidth;
    line_rect.h = YPPC;
    SDL_FillRect(screen1, &line_rect, tb_colour);
    SDL_BlitSurface(screen1, NULL, screen0, NULL);
    do_sdl_flip(screen0);
  }
  else {
    xx = xtext; yy = ytext;
    scroll_rect.x = XPPC * twinleft;
    scroll_rect.w = XPPC * (twinright - twinleft +1);
    scroll_rect.h = YPPC * (twinbottom - twintop);
    line_rect.x = 0;
    if (twintop != twinbottom) {	/* Text window is more than one line high */
      if (direction == SCROLL_UP) {	/* Scroll text up a line */
        scroll_rect.y = YPPC * (twintop + 1);
        line_rect.y = 0;
      }
      else {	/* Scroll text down a line */
        scroll_rect.y = YPPC * twintop;
        line_rect.y = YPPC;
      }
      SDL_BlitSurface(screen0, &scroll_rect, screen1, &line_rect);
      scroll_rect.x = 0;
      scroll_rect.y = 0;
      scroll_rect.w = XPPC * (twinright - twinleft +1);
      scroll_rect.h = YPPC * (twinbottom - twintop +1);
      line_rect.x = twinleft * XPPC;
      line_rect.y = YPPC * twintop;
      SDL_BlitSurface(screen1, &scroll_rect, screen0, &line_rect);
    }
    xtext = twinleft;
    echo_off();
    for (n=twinleft; n<=twinright; n++) sdlchar(' ');	/* Clear the vacated line of the window */
    xtext = xx; ytext = yy;	/* Put the cursor back where it should be */
    echo_on();
  }
}

/*
** 'vdu_2317' deals with various flavours of the sequence VDU 23,17,...
*/
static void vdu_2317(void) {
  int32 temp;
  switch (vduqueue[1]) {	/* vduqueue[1] is the byte after the '17' and says what to change */
  case TINT_FORETEXT:
    text_foretint = (vduqueue[2] & TINTMASK)>>TINTSHIFT;	/* Third byte in queue is new TINT value */
    if (colourdepth==256) text_physforecol = (text_forecol<<COL256SHIFT)+text_foretint;
    if (colourdepth==COL24BIT) text_physforecol=tint24bit(text_forecol, text_foretint);
    break;
  case TINT_BACKTEXT:
    text_backtint = (vduqueue[2] & TINTMASK)>>TINTSHIFT;
    if (colourdepth==256) text_physbackcol = (text_backcol<<COL256SHIFT)+text_backtint;
    if (colourdepth==COL24BIT) text_physbackcol=tint24bit(text_backcol, text_backtint);
    break;
  case TINT_FOREGRAPH:
    graph_foretint = (vduqueue[2] & TINTMASK)>>TINTSHIFT;
    if (colourdepth==256) graph_physforecol = (graph_forecol<<COL256SHIFT)+graph_foretint;
    if (colourdepth==COL24BIT) graph_physforecol=tint24bit(graph_forecol, graph_foretint);
    break;
  case TINT_BACKGRAPH:
    graph_backtint = (vduqueue[2] & TINTMASK)>>TINTSHIFT;
    if (colourdepth==256) graph_physbackcol = (graph_backcol<<COL256SHIFT)+graph_backtint;
    if (colourdepth==COL24BIT) graph_physbackcol=tint24bit(graph_backcol, graph_backtint);
    break;
  case EXCH_TEXTCOLS:	/* Exchange text foreground and background colours */
    temp = text_forecol; text_forecol = text_backcol; text_backcol = temp;
    temp = text_physforecol; text_physforecol = text_physbackcol; text_physbackcol = temp;
    temp = text_foretint; text_foretint = text_backtint; text_backtint = temp;
    break;
  default:		/* Ignore bad value */
    break;
  }
  set_rgb();
}

/* RISC OS 5 - Set Teletext characteristics */
static void vdu_2318(void) {
  if (vduqueue[1] == 1) {
    mode7bitmapupdate=vduqueue[2] & 2;
  }
  if (vduqueue[1] == 2) {
    mode7reveal=vduqueue[2] & 1;
  }
  if (vduqueue[1] == 3) {
    mode7black = vduqueue[2] & 1;
  }
  if (vduqueue[1] == 255) { /* Brandy extension - render glyphs 12, 14 or 16 glyphs wide */
    if ((vduqueue[2] == 12) || (vduqueue[2]== 14) || (vduqueue[2] == 16)) {
      SDL_Surface *m7fontbuf;
      M7XPPC = vduqueue[2];
      SDL_FreeSurface(sdl_m7fontbuf);
      m7fontbuf = SDL_CreateRGBSurface(SDL_SWSURFACE, M7XPPC, M7YPPC, 32, 0xff000000, 0x00ff0000, 0x0000ff00, 0x000000ff);
      sdl_m7fontbuf = SDL_ConvertSurface(m7fontbuf, screen0->format, 0);
      SDL_FreeSurface(m7fontbuf);
      modetable[7].xres = 40*M7XPPC;
      modetable[7].xgraphunits = 80*M7XPPC;
      if (screenmode == 7) {
	screenwidth = modetable[7].xres;
	screenheight = modetable[7].yres;
	xgraphunits = modetable[7].xgraphunits;
	gwinright = xgraphunits-1;
	line_rect.x = 0;
	line_rect.y = 0;
	line_rect.w = vscrwidth;
	line_rect.h = vscrheight;
	SDL_FillRect(modescreen, NULL, tb_colour);
	SDL_FillRect(screen0, NULL, tb_colour);
	SDL_FillRect(screen2, NULL, tb_colour);
	SDL_FillRect(screen3, NULL, tb_colour);
	do_sdl_flip(screen0);
	SDL_SetClipRect(screen0, &line_rect);
      }
    }
  }
  mode7renderscreen();
}

/* BB4W/BBCSDL - Define and select custom mode */
/* Implementation not likely to be exact, char width and height are fixed in Brandy so are /8 to generate xscale and yscale */
static void vdu_2322(void) {
  int32 mwidth, mheight, mxscale, myscale, cols, charset;
  
  mwidth=(vduqueue[1] + (vduqueue[2]<<8));
  mheight=(vduqueue[3] + (vduqueue[4]<<8));
  mxscale=vduqueue[5]/8;
  myscale=vduqueue[6]/8;
  cols=vduqueue[7];
  charset=vduqueue[8];
  if ((cols != 0) && (cols != 2) && (cols != 4) && (cols != 16)) return; /* Invalid colours, do nothing */
  if (0 == cols) cols=256;
  if (0 == mxscale) mxscale=1;
  if (0 == myscale) myscale=1;
  setupnewmode(126,mwidth/mxscale,mheight/myscale,cols,mxscale,myscale,1,1);
  emulate_mode(126);
  if (charset & 0x80) {
    text_physforecol = text_forecol = 0;
    if(cols==256) {
      text_backcol = 63;
      text_physbackcol = (text_backcol << COL256SHIFT)+text_foretint;
    } else {
      text_physbackcol = text_backcol = 63 & colourmask;
    }
    set_rgb();
    vdu_cleartext();
  }
}

/*
** 'vdu_23command' emulates some of the VDU 23 command sequences
*/
static void vdu_23command(void) {
  int codeval, n;
  switch (vduqueue[0]) {	/* First byte in VDU queue gives the command type */
  case 0:       /* More cursor stuff - this only handles VDU23;{8202,29194};0;0;0; */
    if (vduqueue[1] == 10) {
      if (vduqueue[2] == 32) {
	if (graphmode == FULLSCREEN) hide_cursor();
        cursorstate = HIDDEN;	/* 0 = hide, 1 = show */
      } else if (vduqueue[2] == 114) {
        cursorstate = SUSPENDED;
	if (graphmode == FULLSCREEN) toggle_cursor();
        cursorstate = ONSCREEN;
      }
    }
    break;
  case 1:	/* Control the appear of the text cursor */
    if (graphmode == FULLSCREEN) {
      if (vduqueue[1] == 0) {
        hide_cursor();
        cursorstate = HIDDEN;	/* 0 = hide, 1 = show */
      }
      if (vduqueue[1] == 1 && cursorstate != NOCURSOR) cursorstate = ONSCREEN;
    }
    if (vduqueue[1] == 1) cursorstate = ONSCREEN;
    else cursorstate = HIDDEN;
    break;
  case 8:	/* Clear part of the text window */
    break;
  case 17:	/* Set the tint value for a colour in 256 colour modes, etc */
    vdu_2317();
    break;
  case 18:	/* RISC OS 5 set Teletext characteristics */
    vdu_2318();
    break;
  case 22:	/* BB4W/BBCSDL Custom Mode */
    vdu_2322();
    break;
  default:
    codeval = vduqueue[0] & 0x00FF;
    if ((codeval < 32) || (codeval == 127)) break;   /* Ignore unhandled commands */
    /* codes 32 to 255 are user-defined character setup commands */
    for (n=0; n < 8; n++) sysfont[codeval-32][n] = vduqueue[n+1];
  }
}

void hide_cursor() {
  if (cursorstate == ONSCREEN) toggle_cursor();
}

void reveal_cursor() {
  if (cursorstate==SUSPENDED) toggle_cursor();
}

/*
** 'toggle_cursor' draws the text cursor at the current text position
** in graphics modes.
** It draws (and removes) the cursor by inverting the colours of the
** pixels at the current text cursor position. Two different styles
** of cursor can be drawn, an underline and a block
*/
static void toggle_cursor(void) {
  int32 left, right, top, bottom, x, y, mxppc, myppc;

  if (screenmode==7) {
    mxppc=M7XPPC;
    myppc=M7YPPC;
  } else {
    mxppc=XPPC;
    myppc=YPPC;
  }
  if (displaybank != writebank) return;
  curstate instate=cursorstate;
  if ((cursorstate != SUSPENDED) && (cursorstate != ONSCREEN)) return;	/* Cursor is not being displayed so give up now */
  if (cursorstate == ONSCREEN)	/* Toggle the cursor state */
    cursorstate = SUSPENDED;
  else
    if (!vdu5mode) cursorstate = ONSCREEN;
  left = xtext*xscale*mxppc;	/* Calculate pixel coordinates of ends of cursor */
  right = left + xscale*mxppc -1;
  if (cursmode == UNDERLINE) {
    y = ((ytext+1)*yscale*myppc - yscale) * vscrwidth;
    for (x=left; x <= right; x++) {
      *((Uint32*)screen0->pixels + x + y) ^= xor_mask;
      if (yscale != 1) *((Uint32*)screen0->pixels + x + y + vscrwidth) ^= xor_mask;
    }
  }
  else if (cursmode == BLOCK) {
    top = ytext*yscale*myppc;
    bottom = top + myppc*yscale -1;
    for (y = top; y <= bottom; y++) {
      for (x = left; x <= right; x++)
        *((Uint32*)screen0->pixels + x + y*vscrwidth) ^= xor_mask;
    }
  }
  if (echo && (instate != cursorstate)) do_sdl_updaterect(screen0, xtext*xscale*mxppc, ytext*yscale*myppc, xscale*mxppc, yscale*myppc);
}

static void toggle_tcursor(void) {
  int32 x, y, top, bottom, left, right, mxppc, myppc;

  if (screenmode==7) {
    mxppc=M7XPPC;
    myppc=M7YPPC;
  } else {
    mxppc=XPPC;
    myppc=YPPC;
  }

  if (cursorstate == ONSCREEN)	/* Toggle the cursor state */
    cursorstate = SUSPENDED;
  else
    cursorstate = ONSCREEN;
  left = xtext*mxppc;
  right = left + mxppc -1;
  if (cursmode == UNDERLINE) {
    y = ((ytext+1)*myppc -1) * vscrwidth;
    for (x=left; x <= right; x++)
      *((Uint32*)screen0->pixels + x + y) ^= xor_mask;
  }
  else if (cursmode == BLOCK) {
    top = ytext*myppc;
    bottom = top + myppc -1;
    for (y = top; y <= bottom; y++) {
      for (x = left; x <= right; x++)
        *((Uint32*)screen0->pixels + x + y*vscrwidth) ^= xor_mask;
    }
  }
  if (echo) do_sdl_updaterect(screen0, xtext * mxppc, ytext * myppc, mxppc, myppc);
}

/*
** 'blit_scaled' is called when working in one of the 'scaled'
** screen modes to copy the scaled rectangle defined by (x1, y1) and
** (x2, y2) to the screen buffer and then to display it.
** This function is used when one of the RISC OS screen modes that has
** to be scaled to fit the screen is being used, for example, mode 0.
** Everything is written to a buffer of a size appropriate to the
** resolution of that screen mode, for example, mode 0 is written to a
** 640 by 256 buffer and mode 1 to a 320 by 256 buffer. When the buffer
** is displayed it is scaled to fit the screen. This means that the
** buffer being used for that screen mode has to be copied in an
** enlarged form to the main screen buffer.
** (x1, y1) and (x2, y2) define the rectangle to be displayed. These
** are given in terms of what could be called the pseudo pixel
** coordinates of the buffer. These pseudo pixel coordinates are
** converted to real pixel coordinates by multiplying them by 'xscale'
** and 'yscale'.
*/
static void blit_scaled(int32 left, int32 top, int32 right, int32 bottom) {
  int32 dleft, dtop, xx, yy, i, j, ii, jj;
/*
** Start by clipping the rectangle to be blit'ed if it extends off the
** screen.
** Note that 'screenwidth' and 'screenheight' give the dimensions of the
** RISC OS screen mode in pixels
*/
  if(!scaled) {
    scale_rect.x = left;
    scale_rect.y = top;
    scale_rect.w = (right+1 - left);
    scale_rect.h = (bottom+1 - top);
    SDL_BlitSurface(modescreen, &scale_rect, screenbank[writebank], &scale_rect);
    if ((autorefresh==1) && (displaybank == writebank)) SDL_BlitSurface(modescreen, &scale_rect, screen0, &scale_rect);
  } else {
    if (left >= screenwidth || right < 0 || top >= screenheight || bottom < 0) return;	/* Is off screen completely */
    if (left < 0) left = 0;		/* Clip the rectangle as necessary */
    if (right >= screenwidth) right = screenwidth-1;
    if (top < 0) top = 0;
    if (bottom >= screenheight) bottom = screenheight-1;
    dleft = left*xscale;			/* Calculate pixel coordinates in the */
    dtop  = top*yscale;			/* screen buffer of the rectangle */
    yy = dtop;
    for (j = top; j <= bottom; j++) {
      for (jj = 1; jj <= yscale; jj++) {
	xx = dleft;
	for (i = left; i <= right; i++) {
          for (ii = 1; ii <= xscale; ii++) {
            *((Uint32*)screenbank[writebank]->pixels + xx + yy*vscrwidth) = *((Uint32*)modescreen->pixels + i + j*vscrwidth);
            if ((autorefresh==1) && (displaybank == writebank)) {
	      *((Uint32*)screen0->pixels + xx + yy*vscrwidth) = *((Uint32*)modescreen->pixels + i + j*vscrwidth);
            }
	    xx++;
          }
	}
	yy++;
      } 
    }
    scale_rect.x = dleft;
    scale_rect.y = dtop;
    scale_rect.w = (right+1 - left) * xscale;
    scale_rect.h = (bottom+1 - top) * yscale;
  }
  if ((screenmode == 3) || (screenmode == 6)) {
    int p;
    hide_cursor();
    scroll_rect.x=0;
    scroll_rect.w=screenwidth*xscale;
    scroll_rect.h=4;
    for (p=0; p<25; p++) {
      scroll_rect.y=16+(p*20);
      SDL_FillRect(screen0, &scroll_rect, 0);
    }
  }
  if ((autorefresh==1) && (displaybank == writebank)) SDL_UpdateRect(screen0, scale_rect.x, scale_rect.y, scale_rect.w, scale_rect.h);
}

#define COLOURSTEP 68		/* RGB colour value increment used in 256 colour modes */
#define TINTSTEP 17		/* RGB colour value increment used for tints */

/*
** 'init_palette' is called to initialise the palette used for the
** screen. This is just a 768 byte block of memory with
** three bytes for each colour. The table is initialised with RGB
** values so that it corresponds directly to the RISC OS default
** palettes in 2, 4, 16 and 256 colour screen modes. This means we
** can go directly from a RISC OS GCOL or COLOUR number to the
** physical colour without an extra layer of mapping to convert a
** RISC OS physical colour to its equivalent under foreign operating
** systems
*/
static void init_palette(void) {
  hardpalette[0] = hardpalette[1] = hardpalette[2] = 0;		    /* Black */
  hardpalette[3] = 255; hardpalette[4] = hardpalette[5] = 0;	    /* Red */
  hardpalette[6] = 0; hardpalette[7] = 255; hardpalette[8] = 0;	/* Green */
  hardpalette[9] = hardpalette[10] = 255; hardpalette[11] = 0;	/* Yellow */
  hardpalette[12] = hardpalette[13] = 0; hardpalette[14] = 255;	/* Blue */
  hardpalette[15] = 255; hardpalette[16] = 0; hardpalette[17] = 255;	/* Magenta */
  hardpalette[18] = 0; hardpalette[19] = hardpalette[20] = 255;	/* Cyan */
  hardpalette[21] = hardpalette[22] = hardpalette[23] = 255;	    /* White */
  switch (colourdepth) {
  case 2:	/* Two colour - Black and white only */
    palette[0] = palette[1] = palette[2] = 0;
    palette[3] = palette[4] = palette[5] = 255;
    break;
  case 4:	/* Four colour - Black, red, yellow and white */
    palette[0] = palette[1] = palette[2] = 0;		/* Black */
    palette[3] = 255; palette[4] = palette[5] = 0;	/* Red */
    palette[6] = palette[7] = 255; palette[8] = 0;	/* Yellow */
    palette[9] = palette[10] = palette[11] = 255;	/* White */
    break;
  case 16:	/* Sixteen colour */
    palette[0] = palette[1] = palette[2] = 0;		    /* Black */
    palette[3] = 255; palette[4] = palette[5] = 0;	    /* Red */
    palette[6] = 0; palette[7] = 255; palette[8] = 0;	/* Green */
    palette[9] = palette[10] = 255; palette[11] = 0;	/* Yellow */
    palette[12] = palette[13] = 0; palette[14] = 255;	/* Blue */
    palette[15] = 255; palette[16] = 0; palette[17] = 255;	/* Magenta */
    palette[18] = 0; palette[19] = palette[20] = 255;	/* Cyan */
    palette[21] = palette[22] = palette[23] = 255;	    /* White */
    palette[24] = palette[25] = palette[26] = 0;	    /* Black */
    palette[27] = 160; palette[28] = palette[29] = 0;	/* Dark red */
    palette[30] = 0; palette[31] = 160; palette[32] = 0;/* Dark green */
    palette[33] = palette[34] = 160; palette[35] = 0;	/* Khaki */
    palette[36] = palette[37] = 0; palette[38] = 160;	/* Navy blue */
    palette[39] = 160; palette[40] = 0; palette[41] = 160;	/* Purple */
    palette[42] = 0; palette[43] = palette[44] = 160;	/* Cyan */
    palette[45] = palette[46] = palette[47] = 160;	    /* Grey */
    break;
  case 256:
  case COL15BIT:
  case COL24BIT: {	/* >= 256 colour */
    int red, green, blue, tint, colour;
/*
** The colour number in 256 colour modes can be seen as a bit map as
** follows:
**	bb gg rr tt
** where 'rr' is a two-bit red component, 'gg' is the green component,
** 'bb' is the blue component and 'tt' is the 'tint', a value that
** affects the brightness of the three component colours. The two-bit
** component numbers correspond to RGB values of 0, 68, 136 and 204
** for the brightness of that component. The tint values increase the
** RGB values by 0, 17, 34 or 51. Note that the tint value is added
** to *all three* colour components. An example colour number where
** rr = 2, gg = 0, bb = 3 and tt = 1 would have colour components
** red: 136+17 = 153, green: 0+17 = 17 and blue: 204+17 = 221.
** The RISC OS logical colour number provides the 'rr gg bb' bits.
** THe tint value can be supplied at the same time as the colour (via
** the 'TINT' parameter of the COLOUR and GCOL statements) or changed
** separated by using 'TINT' as a statement in its own right.
*/
    colour = 0;
    for (blue = 0; blue <= COLOURSTEP * 3; blue += COLOURSTEP) {
      for (green = 0; green <= COLOURSTEP * 3; green += COLOURSTEP) {
        for (red = 0; red <= COLOURSTEP * 3; red += COLOURSTEP) {
          for (tint = 0; tint <= TINTSTEP * 3; tint += TINTSTEP) {
            palette[colour] = red+tint;
            palette[colour+1] = green+tint;
            palette[colour+2] = blue+tint;
            colour += 3;
          }
        }
      }
    }
    break;
  }
  default:	/* 32K and 16M colour modes are not supported */
    error(ERR_UNSUPPORTED);
  }
  if (colourdepth >= 256) {
    text_physforecol = (text_forecol<<COL256SHIFT)+text_foretint;
    text_physbackcol = (text_backcol<<COL256SHIFT)+text_backtint;
    graph_physforecol = (graph_forecol<<COL256SHIFT)+graph_foretint;
    graph_physbackcol = (graph_backcol<<COL256SHIFT)+graph_backtint;
  }
  else {
    text_physforecol = text_forecol;
    text_physbackcol = text_backcol;
    graph_physforecol = graph_forecol;
    graph_physbackcol = graph_backcol;
  }
  set_rgb();
}

/*
** 'change_palette' is called to change the palette entry for physical
** colour 'colour' to the colour defined by the RGB values red, green
** and blue. The screen is updated by this call
*/
static void change_palette(int32 colour, int32 red, int32 green, int32 blue) {
  if (graphmode != FULLSCREEN) return;	/* There be no palette to change */
  palette[colour*3] = red;	/* The palette is not structured */
  palette[colour*3+1] = green;
  palette[colour*3+2] = blue;
}

/*
 * emulate_colourfn - This performs the function COLOUR(). It
 * Returns the entry in the palette for the current screen mode
 * that most closely matches the colour with red, green and
 * blue components passed to it.
 * It is assumed that this function will be used for graphics
 * so the screen is switched to graphics mode if it is used
 */
int32 emulate_colourfn(int32 red, int32 green, int32 blue) {
  int32 n, distance, test, best, dr, dg, db;

  if (graphmode < TEXTMODE)
    return colourdepth - 1;	/* There is no palette */
  else if (graphmode == TEXTMODE) {
    switch_graphics();
  }
  if (colourdepth == COL24BIT) return (red + (green << 8) + (blue << 16));
  distance = 0x7fffffff;
  best = 0;
  for (n = 0; n < colourdepth && distance != 0; n++) {
    dr = palette[n * 3] - red;
    dg = palette[n * 3 + 1] - green;
    db = palette[n * 3 + 2] - blue;
    test = 2 * dr * dr + 4 * dg * dg + db * db;
    if (test < distance) {
      distance = test;
      best = n;
    }
  }
  return best;
}

/*
 * set_text_colour - Set either the text foreground colour
 * or the background colour to the supplied colour number
 * (palette entry number). This is used when a colour has
 * been matched with an entry in the palette via COLOUR()
 */
static void set_text_colour(boolean background, int colnum) {
  if (background)
    text_physbackcol = text_backcol = (colnum & (colourdepth - 1));
  else {
    text_physforecol = text_forecol = (colnum & (colourdepth - 1));
  }
  set_rgb();
}

/*
 * set_graphics_colour - Set either the graphics foreground
 * colour or the background colour to the supplied colour
 * number (palette entry number). This is used when a colour
 * has been matched with an entry in the palette via COLOUR()
 */
static void set_graphics_colour(boolean background, int colnum) {
  if (background)
    graph_physbackcol = graph_backcol = (colnum & (colourdepth - 1));
  else {
    graph_physforecol = graph_forecol = (colnum & (colourdepth - 1));
  }
  graph_fore_action = graph_back_action = 0;
  set_rgb();
}

/*
** 'switch_graphics' switches from text output mode to fullscreen graphics
** mode. Unless the option '-graphics' is specified on the command line,
** the interpreter remains in fullscreen mode until the next mode change
** when it switches back to text output mode. If '-graphics' is given
** then it remains in fullscreen mode
*/
static void switch_graphics(void) {
  SDL_SetClipRect(screen0, NULL);
  SDL_SetClipRect(modescreen, NULL);
  SDL_FillRect(screen0, NULL, tb_colour);
  SDL_FillRect(screen1, NULL, tb_colour);
  SDL_FillRect(modescreen, NULL, tb_colour);
  init_palette();
  graphmode = FULLSCREEN;
  xtext = twinleft;		/* Send the text cursor to the home position */
  ytext = twintop;
#if defined(TARGET_DJGPP) | defined(TARGET_MACOSX)
  textwidth = modetable[screenmode & MODEMASK].xtext;	/* Hack to set the depth of the graphics screen */
  textheight = modetable[screenmode & MODEMASK].ytext;
  if (!textwin) {	/* Text window is the whole screen */
    twinright = textwidth-1;
    twinbottom = textheight-1;
  }
#endif
  vdu_cleartext();	/* Clear the graphics screen */
  if (cursorstate == NOCURSOR) {	/* 'cursorstate' might be set to 'HIDDEN' if OFF used */
    cursorstate = SUSPENDED;
    toggle_cursor();
  }
}

/*
** 'switch_text' switches from fullscreen graphics back to text output mode.
** It does this on a mode change
*/
static void switch_text(void) {
  SDL_SetClipRect(screen0, NULL);
  SDL_SetClipRect(modescreen, NULL);
  SDL_FillRect(screen0, NULL, tb_colour);
  SDL_FillRect(screen1, NULL, tb_colour);
  SDL_FillRect(modescreen, NULL, tb_colour);
}

/*
** 'scroll' scrolls the graphics screen up or down by the number of
** rows equivalent to one line of text on the screen. Depending on
** the RISC OS mode being used, this can be either eight or sixteen
** rows. 'direction' says whether the screen is moved up or down.
** The screen is redrawn by this call
*/
static void scroll(updown direction) {
  int left, right, top, dest, topwin, m, n, mxppc, myppc;
  if (screenmode == 7) {
    mxppc = M7XPPC;
    myppc = M7YPPC;
  } else {
    mxppc=XPPC;
    myppc=YPPC;
  }
  topwin = twintop*myppc;		/* Y coordinate of top of text window */
  if (direction == SCROLL_UP) {	/* Shifting screen up */
    dest = twintop*myppc;		/* Move screen up to this point */
    left = twinleft*mxppc;
    right = twinright*mxppc+mxppc-1;
    top = dest+myppc;				/* Top of block to move starts here */
    scroll_rect.x = twinleft*mxppc;
    scroll_rect.y = myppc * (twintop + 1);
    scroll_rect.w = mxppc * (twinright - twinleft +1);
    scroll_rect.h = myppc * (twinbottom - twintop);
    if (screenmode != 7) SDL_BlitSurface(modescreen, &scroll_rect, screen1, NULL);
    if (screenmode == 7 && mode7bitmapupdate) {
      SDL_BlitSurface(screen0, &scroll_rect, screen1, NULL);
      SDL_BlitSurface(screen3, &scroll_rect, screen3A, NULL);
      SDL_BlitSurface(screen2, &scroll_rect, screen2A, NULL);
    }
    line_rect.x = 0;
    line_rect.y = myppc * (twinbottom - twintop);
    line_rect.w = mxppc * (twinright - twinleft +1);
    line_rect.h = myppc;
    if (screenmode != 7 || mode7bitmapupdate) SDL_FillRect(screen1, &line_rect, tb_colour);
    if (screenmode == 7) {
      if (mode7bitmapupdate) {
        SDL_FillRect(screen2A, &line_rect, tb_colour);
        SDL_FillRect(screen3A, &line_rect, tb_colour);
      }
      for(n=2; n<=25; n++) vdu141track[n-1]=vdu141track[n];
      vdu141track[25]=0;
      vdu141track[0]=0;
      /* Scroll the Mode 7 text buffer */
      for (m=twintop+1; m<=twinbottom; m++) {
	for (n=twinleft; n<=twinright; n++) mode7frame[m-1][n] = mode7frame[m][n];
      }
      /* Blank the bottom line */
      for (n=twinleft; n<=twinright; n++) mode7frame[twinbottom][n] = 32;
    }
  }
  else {	/* Shifting screen down */
    dest = (twintop+1)*myppc;
    left = twinleft*mxppc;
    right = (twinright+1)*mxppc-1;
    top = twintop*myppc;
    scroll_rect.x = left;
    scroll_rect.y = top;
    scroll_rect.w = mxppc * (twinright - twinleft +1);
    scroll_rect.h = myppc * (twinbottom - twintop);
    line_rect.x = 0;
    line_rect.y = myppc;
    if (screenmode != 7) SDL_BlitSurface(modescreen, &scroll_rect, screen1, &line_rect);
    if (screenmode == 7 && mode7bitmapupdate) {
      SDL_BlitSurface(screen0, &scroll_rect, screen1, &line_rect);
      SDL_BlitSurface(screen3, &scroll_rect, screen3A, NULL);
      SDL_BlitSurface(screen2, &scroll_rect, screen2A, NULL);
    }
    line_rect.x = 0;
    line_rect.y = 0;
    line_rect.w = mxppc * (twinright - twinleft +1);
    line_rect.h = myppc;
    if (screenmode != 7 || mode7bitmapupdate) SDL_FillRect(screen1, &line_rect, tb_colour);
    if (screenmode == 7) {
      if (mode7bitmapupdate) {
        SDL_FillRect(screen2A, &line_rect, tb_colour);
        SDL_FillRect(screen3A, &line_rect, tb_colour);
      }
      for(n=0; n<=24; n++) vdu141track[n+1]=vdu141track[n];
      vdu141track[0]=0; vdu141track[1]=0;
      /* Scroll the Mode 7 text buffer */
      for (m=twintop; m<=twinbottom-1; m++) {
	for (n=twinleft; n<=twinright; n++) mode7frame[m+1][n] = mode7frame[m][n];
      }
      /* Blank the bottom line */
      for (n=twinleft; n<=twinright; n++) mode7frame[twintop][n] = 32;
    }
  }
  line_rect.x = 0;
  line_rect.y = 0;
  line_rect.w = mxppc * (twinright - twinleft +1);
  line_rect.h = myppc * (twinbottom - twintop +1);
  scroll_rect.x = left;
  scroll_rect.y = dest;
  if (screenmode != 7) SDL_BlitSurface(screen1, &line_rect, modescreen, &scroll_rect);
  if (screenmode == 7 && mode7bitmapupdate) {
    SDL_BlitSurface(screen2A, &line_rect, screen2, &scroll_rect);
    SDL_BlitSurface(screen3A, &line_rect, screen3, &scroll_rect);
  }
  if (screenmode == 7) {
    if (mode7bitmapupdate) SDL_BlitSurface(screen1, &line_rect, screen0, &scroll_rect);
  } else {
    blit_scaled(left, topwin, right, twinbottom*myppc+myppc-1);
  }
  do_sdl_flip(screen0);
}

/*
** 'echo_ttext' is called to display text held in the screen buffer on the
** text screen when working in 'no echo' mode. If does the buffer flip.
*/
static void echo_ttext(void) {
  if (xtext != 0) do_sdl_updaterect(screen0, 0, ytext*YPPC, xtext*XPPC, YPPC);
}

/*
** 'echo_text' is called to display text held in the screen buffer on the
** graphics screen when working in 'no echo' mode. If displays from the
** start of the line to the current value of the text cursor
*/
static void echo_text(void) {
  if (xtext == 0) return;	/* Return if nothing has changed */
  if (screenmode == 7) {
    do_sdl_flip(screen0);
    return;
  }
    blit_scaled(0, ytext*YPPC, xtext*XPPC-1, ytext*YPPC+YPPC-1);
}

void mode7flipbank() {
  if (screenmode == 7) {
    if ((mode7timer - mos_centiseconds()) <= 0) {
      hide_cursor();
      if (!mode7bitmapupdate) mode7renderscreen();
      if (mode7bank) {
	SDL_BlitSurface(screen2, NULL, screen0, NULL);
	mode7bank=0;
	mode7timer=mos_centiseconds() + 100;
      } else {
	SDL_BlitSurface(screen3, NULL, screen0, NULL);
	mode7bank=1;
	mode7timer=mos_centiseconds() + 33;
      }
      do_sdl_updaterect(screen0, 0, 0, 0, 0);
      reveal_cursor();
    }
  }
}

/*
** 'write_char' draws a character when in fullscreen graphics mode
** when output is going to the text cursor. It assumes that the
** screen in is fullscreen graphics mode.
** The line or block representing the text cursor is overwritten
** by this code so the cursor state is automatically set to
** 'suspended' (if the cursor is being displayed)
*/
static void write_char(int32 ch) {
  int32 y, topx, topy, line;

  if (cursorstate == ONSCREEN) cursorstate = SUSPENDED;
  topx = xtext*XPPC;
  topy = ytext*YPPC;
  place_rect.x = topx;
  place_rect.y = topy;
  SDL_FillRect(sdl_fontbuf, NULL, tb_colour);
  for (y=0; y < 8; y++) {
    line = sysfont[ch-' '][y];
    if (line!=0) {
      if (line & 0x80) *((Uint32*)sdl_fontbuf->pixels + 0 + y*XPPC) = tf_colour;
      if (line & 0x40) *((Uint32*)sdl_fontbuf->pixels + 1 + y*XPPC) = tf_colour;
      if (line & 0x20) *((Uint32*)sdl_fontbuf->pixels + 2 + y*XPPC) = tf_colour;
      if (line & 0x10) *((Uint32*)sdl_fontbuf->pixels + 3 + y*XPPC) = tf_colour;
      if (line & 0x08) *((Uint32*)sdl_fontbuf->pixels + 4 + y*XPPC) = tf_colour;
      if (line & 0x04) *((Uint32*)sdl_fontbuf->pixels + 5 + y*XPPC) = tf_colour;
      if (line & 0x02) *((Uint32*)sdl_fontbuf->pixels + 6 + y*XPPC) = tf_colour;
      if (line & 0x01) *((Uint32*)sdl_fontbuf->pixels + 7 + y*XPPC) = tf_colour;
    }
  }
  SDL_BlitSurface(sdl_fontbuf, &font_rect, modescreen, &place_rect);
  if (echo) {
    blit_scaled(topx, topy, topx+XPPC-1, topy+YPPC-1);
  }
  xtext++;
  if (xtext > twinright) {
    if (!echo) echo_text();	/* Line is full so flush buffered characters */
    xtext = twinleft;
    ytext++;
    if (ytext > twinbottom) {	/* Text cursor was on the last line of the text window */
      scroll(SCROLL_UP);	/* So scroll window up */
      ytext--;
    }
  }
}

/*
** 'plot_char' draws a character when in fullscreen graphics mode
** when output is going to the graphics cursor. It will scale the
** character if necessary. It assumes that the screen in is
** fullscreen graphics mode.
** Note that characters can be scaled in the 'y' direction or the
** 'x' and 'y' direction but never in just the 'x' direction.
*/
static void plot_char(int32 ch) {
  int32 y, topx, topy, line;
  topx = GXTOPX(xlast);		/* X and Y coordinates are those of the */
  topy = GYTOPY(ylast);	/* top left-hand corner of the character */
  place_rect.x = topx;
  place_rect.y = topy;
  SDL_FillRect(sdl_v5fontbuf, NULL, gb_colour);
  for (y=0; y<YPPC; y++) {
    line = sysfont[ch-' '][y];
    if (line!=0) {
      if (line & 0x80) *((Uint32*)sdl_v5fontbuf->pixels + 0 + y*XPPC) = gf_colour;
      if (line & 0x40) *((Uint32*)sdl_v5fontbuf->pixels + 1 + y*XPPC) = gf_colour;
      if (line & 0x20) *((Uint32*)sdl_v5fontbuf->pixels + 2 + y*XPPC) = gf_colour;
      if (line & 0x10) *((Uint32*)sdl_v5fontbuf->pixels + 3 + y*XPPC) = gf_colour;
      if (line & 0x08) *((Uint32*)sdl_v5fontbuf->pixels + 4 + y*XPPC) = gf_colour;
      if (line & 0x04) *((Uint32*)sdl_v5fontbuf->pixels + 5 + y*XPPC) = gf_colour;
      if (line & 0x02) *((Uint32*)sdl_v5fontbuf->pixels + 6 + y*XPPC) = gf_colour;
      if (line & 0x01) *((Uint32*)sdl_v5fontbuf->pixels + 7 + y*XPPC) = gf_colour;
    }
  }
  SDL_BlitSurface(sdl_v5fontbuf, &font_rect, modescreen, &place_rect);
  blit_scaled(topx, topy, topx+XPPC-1, topy+YPPC-1);

  cursorstate = SUSPENDED; /* because we just overwrote it */
  xlast += XPPC*xgupp;	/* Move to next character position in X direction */
  if (xlast > gwinright) {	/* But position is outside the graphics window */
    xlast = gwinleft;
    ylast -= YPPC*ygupp;
    if (ylast < gwinbottom) ylast = gwintop;	/* Below bottom of graphics window - Wrap around to top */
  }
}

static void plot_space_opaque(void) {
  int32 topx, topy;
  topx = GXTOPX(xlast);		/* X and Y coordinates are those of the */
  topy = GYTOPY(ylast);	/* top left-hand corner of the character */
  place_rect.x = topx;
  place_rect.y = topy;
  SDL_FillRect(sdl_fontbuf, NULL, gb_colour);
  SDL_BlitSurface(sdl_fontbuf, &font_rect, modescreen, &place_rect);
  blit_scaled(topx, topy, topx+XPPC-1, topy+YPPC-1);

  cursorstate = SUSPENDED; /* because we just overwrote it */
  xlast += XPPC*xgupp;	/* Move to next character position in X direction */
  if (xlast > gwinright) {	/* But position is outside the graphics window */
    xlast = gwinleft;
    ylast -= YPPC*ygupp;
    if (ylast < gwinbottom) ylast = gwintop;	/* Below bottom of graphics window - Wrap around to top */
  }
}

/*
** 'echo_on' turns on cursor (if in graphics mode) and the immediate
** echo of characters to the screen
*/
void echo_on(void) {
  echo = TRUE;
  if (graphmode == FULLSCREEN) {
    echo_text();	/* Flush what is in the graphics buffer */
    reveal_cursor();	/* Display cursor again */
  }
  else {
    echo_ttext();	/* Flush what is in the text buffer */
  }
}

/*
** 'echo_off' turns off the cursor (if in graphics mode) and the
** immediate echo of characters to the screen. This is used to
** make character output more efficient
*/
void echo_off(void) {
  echo = FALSE;
  if (graphmode == FULLSCREEN) hide_cursor();	/* Remove the cursor if it is being displayed */
}

/*
** 'move_cursor' sends the text cursor to the position (column, row)
** on the screen.  The function updates the cursor position as well.
** The column and row are given in RISC OS text coordinates, that
** is, (0,0) is the top left-hand corner of the screen. These values
** are the true coordinates on the screen. The code that uses this
** function has to allow for the text window.
*/
static void move_cursor(int32 column, int32 row) {
  if (graphmode == FULLSCREEN) {
    hide_cursor();	/* Remove cursor if in graphics mode */
    xtext = column;
    ytext = row;
    reveal_cursor();	/* Redraw cursor if in graphics mode */
  }
  else {
    toggle_tcursor();
    xtext = column;
    ytext = row;
  }
}

/*
** 'set_cursor' sets the type of the text cursor used on the graphic
** screen to either a block or an underline. 'underline' is set to
** TRUE if an underline is required. Underline is used by the program
** when keyboard input is in 'insert' mode and a block when it is in
** 'overwrite'.
*/
void set_cursor(boolean underline) {
    hide_cursor();	/* Remove old style cursor */
    cursmode = underline ? UNDERLINE : BLOCK;
    reveal_cursor();	/* Draw new style cursor */
}

/*
** 'vdu_setpalette' changes one of the logical to physical colour map
** entries (VDU 19). When the interpreter is in full screen mode it
** can also redefine colours for in the palette.
** Note that when working in text mode, this function should have the
** side effect of changing all pixels of logical colour number 'logcol'
** to the physical colour given by 'mode' but the code does not do this.
*/
static void vdu_setpalette(void) {
  int32 logcol, pmode, mode;
  logcol = vduqueue[0] & colourmask;
  mode = vduqueue[1];
  pmode = mode % 16;
  if (mode < 16 && colourdepth <= 16) {	/* Just change the RISC OS logical to physical colour mapping */
    logtophys[logcol] = mode;
    palette[logcol*3] = hardpalette[pmode*3];
    palette[1+logcol*3] = hardpalette[1+pmode*3];
    palette[2+logcol*3] = hardpalette[2+pmode*3];
  } else if (mode == 16)	/* Change the palette entry for colour 'logcol' */
    change_palette(logcol, vduqueue[2], vduqueue[3], vduqueue[4]);
  else {
    if (basicvars.runflags.flag_cosmetic) error(ERR_UNSUPPORTED);
  }
  set_rgb();
}

/*
** 'move_down' moves the text cursor down a line within the text
** window, scrolling the window up if the cursor is on the last line
** of the window. This only works in full screen graphics mode.
*/
static void move_down(void) {
  ytext++;
  if (ytext > twinbottom) {	/* Cursor was on last line in window - Scroll window up */
    ytext--;
    scroll(SCROLL_UP);
  }
}

/*
** 'move_up' moves the text cursor up a line within the text window,
** scrolling the window down if the cursor is on the top line of the
** window
*/
static void move_up(void) {
  ytext--;
  if (ytext < twintop) {	/* Cursor was on top line in window - Scroll window down */
    ytext++;
    scroll(SCROLL_DOWN);
  }
}

/*
** 'move_curback' moves the cursor back one character on the screen (VDU 8)
*/
static void move_curback(void) {
  if (vdu5mode) {	/* VDU 5 mode - Move graphics cursor back one character */
    xlast -= XPPC*xgupp;
    if (xlast < gwinleft) {		/* Cursor is outside the graphics window */
      xlast = gwinright-XPPC*xgupp+1;	/* Move back to right edge of previous line */
      ylast += YPPC*ygupp;
      if (ylast > gwintop) {		/* Move above top of window */
        ylast = gwinbottom+YPPC*ygupp-1;	/* Wrap around to bottom of window */
      }
    }
  }
  else if (graphmode == FULLSCREEN) {
    hide_cursor();	/* Remove cursor */
    xtext--;
    if (xtext < twinleft) {	/* Cursor is at left-hand edge of text window so move up a line */
      xtext = twinright;
      move_up();
    }
    reveal_cursor();	/* Redraw cursor */
  }
  else {	/* Writing to the text screen */
    toggle_tcursor();
    xtext--;
    if (xtext < twinleft) {	/* Cursor is outside window */
      xtext = twinright;
      ytext--;
      if (ytext < twintop) {	/* Cursor is outside window */
        ytext++;
        scroll_text(SCROLL_DOWN);
      }
    }
    if (!vdu5mode) toggle_tcursor();
  }
}

/*
** 'move_curforward' moves the cursor forwards one character on the screen (VDU 9)
*/
static void move_curforward(void) {
  if (vdu5mode) {	/* VDU 5 mode - Move graphics cursor back one character */
    xlast += XPPC*xgupp;
    if (xlast > gwinright) {	/* Cursor is outside the graphics window */
      xlast = gwinleft;		/* Move to left side of window on next line */
      ylast -= YPPC*ygupp;
      if (ylast < gwinbottom) ylast = gwintop;	/* Moved below bottom of window - Wrap around to top */
    }
  }
  else if (graphmode == FULLSCREEN) {
    hide_cursor();	/* Remove cursor */
    xtext++;
    if (xtext > twinright) {	/* Cursor is at right-hand edge of text window so move down a line */
      xtext = twinleft;
      move_down();
    }
    reveal_cursor();	/* Redraw cursor */
  }
  else {	/* Writing to text screen */
    xtext++;
    if (xtext > twinright) {	/* Cursor has moved outside window - Move to next line */
      ytext++;
      if (ytext > twinbottom) {	/* Cursor is outside the window - Move the text window up one line */
        ytext--;
        scroll_text(SCROLL_UP);
      }
    }
  }
}

/*
** 'move_curdown' moves the cursor down the screen, that is, it
** performs the linefeed operation (VDU 10)
*/
static void move_curdown(void) {
  if (vdu5mode) {
    ylast -= YPPC*ygupp;
    if (ylast < gwinbottom) ylast = gwintop;	/* Moved below bottom of window - Wrap around to top */
  }
  else if (graphmode == FULLSCREEN) {
    hide_cursor();	/* Remove cursor */
    move_down();
    reveal_cursor();	/* Redraw cursor */
  }
  else {		/* Writing to a text window */
    ytext++;
    if (ytext > twinbottom)	{ /* Cursor is outside the confines of the window */
      ytext--;
      scroll_text(SCROLL_UP);
    }
  }
}

/*
** 'move_curup' moves the cursor up a line on the screen (VDU 11)
*/
static void move_curup(void) {
  if (vdu5mode) {
    ylast += YPPC*ygupp;
    if (ylast > gwintop) ylast = gwinbottom+YPPC*ygupp-1;	/* Move above top of window - Wrap around to bottow */
  }
  else if (graphmode == FULLSCREEN) {
    hide_cursor();	/* Remove cursor */
    move_up();
    reveal_cursor();	/* Redraw cursor */
  }
  else {	/* Writing to text screen */
    ytext--;
    if (ytext < twintop) {		/* Cursor lies above the text window */
      ytext++;
      scroll_text(SCROLL_DOWN);	/* Scroll the screen down a line */
    }
  }
}

/*
** 'vdu_cleartext' clears the text window. Normally this is the
** entire screen (VDU 12). This is the version of the function used
** when the interpreter supports graphics
*/
static void vdu_cleartext(void) {
  int32 left, right, top, bottom, mxppc, myppc, lx, ly;
  if (screenmode == 7) {
    mxppc=M7XPPC;
    myppc=M7YPPC;
  } else {
    mxppc=XPPC;
    myppc=YPPC;
  }
  if (graphmode == FULLSCREEN) {
    hide_cursor();	/* Remove cursor if it is being displayed */
    if (textwin) {	/* Text window defined that does not occupy the whole screen */
      for (ly=twintop; ly <= twinbottom; ly++) {
        for (lx=twinleft; lx <=twinright; lx++) {
	  mode7frame[ly][lx]=32;
	}
      }
      left = twinleft*mxppc;
      right = twinright*mxppc+mxppc-1;
      top = twintop*myppc;
      bottom = twinbottom*myppc+myppc-1;
      line_rect.x = left;
      line_rect.y = top;
      line_rect.w = right - left +1;
      line_rect.h = bottom - top +1;
      SDL_FillRect(modescreen, &line_rect, tb_colour);
      SDL_FillRect(screen2, &line_rect, tb_colour);
      SDL_FillRect(screen3, &line_rect, tb_colour);
      blit_scaled(0,0,screenwidth-1,screenheight-1);
      mode7renderscreen();
    }
    else {	/* Text window is not being used */
      reset_mode7();
      left = twinleft*mxppc;
      right = twinright*mxppc+mxppc-1;
      top = twintop*myppc;
      bottom = twinbottom*myppc+myppc-1;
      SDL_FillRect(modescreen, NULL, tb_colour);
      blit_scaled(left, top, right, bottom);
      SDL_FillRect(screen2, NULL, tb_colour);
      SDL_FillRect(screen3, NULL, tb_colour);
      xtext = twinleft;
      ytext = twintop;
      reveal_cursor();	/* Redraw cursor */
    }
  }
  else if (textwin) {	/* Text window defined that does not occupy the whole screen */
    int32 column, row;
    echo_off();
    for (row = twintop; row <= twinbottom; row++) {
      xtext = twinleft;  /* Go to start of line on screen */
      ytext = row;
      for (column = twinleft; column <= twinright;  column++) sdlchar(' ');
    }
    echo_on();
    xtext = twinleft;
    ytext = twintop;
  }
  else {
    SDL_FillRect(screen0, NULL, tb_colour);
    xtext = twinleft;
    ytext = twintop;
  }
  do_sdl_flip(screen0);
}

/*
** 'vdu_return' deals with the carriage return character (VDU 13)
*/
static void vdu_return(void) {
  if (vdu5mode)
    xlast = gwinleft;
  else if (graphmode==FULLSCREEN) {
    hide_cursor();	/* Remove cursor */
    xtext = twinleft;
    reveal_cursor();	/* Redraw cursor */
  }
  else {
    move_cursor(twinleft, ytext);
  }
  if (screenmode == 7) {
    vdu141on = 0;
    mode7highbit=0;
    mode7flash=0;
    mode7sepgrp=0;
    mode7sepreal=0;
    mode7prevchar=32;
    text_physforecol = text_forecol = 7;
    text_physbackcol = text_backcol = 0;
    set_rgb();
  }
}

static void fill_rectangle(Uint32 left, Uint32 top, Uint32 right, Uint32 bottom, Uint32 colour, Uint32 action) {
  Uint32 xloop, yloop, pxoffset, prevcolour, altcolour = 0;

  colour=emulate_colourfn((colour >> 16) & 0xFF, (colour >> 8) & 0xFF, (colour & 0xFF));
  for (yloop=top;yloop<=bottom; yloop++) {
    for (xloop=left; xloop<=right; xloop++) {
      pxoffset = xloop + yloop*vscrwidth;
      prevcolour=*((Uint32*)modescreen->pixels + pxoffset);
      prevcolour=emulate_colourfn((prevcolour >> 16) & 0xFF, (prevcolour >> 8) & 0xFF, (prevcolour & 0xFF));
      if (colourdepth == 256) prevcolour = prevcolour >> COL256SHIFT;
      switch (graph_back_action) {
	case 0:
	  altcolour=colour;
	  break;
	case 1:
	  altcolour=(prevcolour | colour);
	  break;
	case 2:
	  altcolour=(prevcolour & colour);
	  break;
	case 3:
	  altcolour=(prevcolour ^ colour);
	  break;
      }
      if (colourdepth == COL24BIT) {
        altcolour = altcolour & 0xFFFFFF;
      } else {
        altcolour=altcolour*3;
        altcolour=SDL_MapRGB(sdl_fontbuf->format, palette[altcolour], palette[altcolour+1], palette[altcolour+2]);
      }
      *((Uint32*)modescreen->pixels + pxoffset) = altcolour;
    }
  }

}

/*
** 'vdu_cleargraph' set the entire graphics window to the current graphics
** background colour (VDU 16)
*/
static void vdu_cleargraph(void) {
  if (istextonly()) return;
  if (graphmode == TEXTONLY) return;	/* Ignore command in text-only modes */
  if (graphmode == TEXTMODE) switch_graphics();
  hide_cursor();	/* Remove cursor */
  if (graph_back_action == 0) {
    SDL_FillRect(modescreen, NULL, gb_colour);
  } else {
    fill_rectangle(GXTOPX(gwinleft), GYTOPY(gwintop), GXTOPX(gwinright), GYTOPY(gwinbottom), graph_physbackcol, graph_back_action);
  }
  blit_scaled(GXTOPX(gwinleft), GYTOPY(gwintop), GXTOPX(gwinright), GYTOPY(gwinbottom));
  reveal_cursor();	/* Redraw cursor */
  do_sdl_flip(screen0);
}

/*
** 'vdu_textcol' changes the text colour to the value in the VDU queue
** (VDU 17). It handles both foreground and background colours at any
** colour depth. The RISC OS physical colour number is mapped to the
** equivalent as used by conio
*/
static void vdu_textcol(void) {
  int32 colnumber;
  if (screenmode == 7) return;
  colnumber = vduqueue[0];
  if (colnumber < 128) {	/* Setting foreground colour */
    if (graphmode == FULLSCREEN) {	/* Operating in full screen graphics mode */
      if (colourdepth == 256) {
        text_forecol = colnumber & COL256MASK;
        text_physforecol = (text_forecol << COL256SHIFT)+text_foretint;
      } else if (colourdepth == COL24BIT) {
	text_physforecol = text_forecol = colour24bit(colnumber, text_foretint);
      } else {
        text_physforecol = text_forecol = colnumber & colourmask;
      }
    }
    else {	/* Operating in text mode */
      text_physforecol = text_forecol = colnumber & colourmask;
    }
  }
  else {	/* Setting background colour */
    if (graphmode == FULLSCREEN) {	/* Operating in full screen graphics mode */
      if (colourdepth == 256) {
        text_backcol = colnumber & COL256MASK;
        text_physbackcol = (text_backcol << COL256SHIFT)+text_backtint;
      } else if (colourdepth == COL24BIT) {
	text_physbackcol = text_backcol = colour24bit(colnumber, text_backtint);
      } else {	/* Operating in text mode */
        text_physbackcol = text_backcol = colnumber & colourmask;
      }
    }
    else {
      text_physbackcol = text_backcol = (colnumber-128) & colourmask;
    }
  }
  set_rgb();
}

/*
** 'reset_colours' initialises the RISC OS logical to physical colour
** map for the current screen mode and sets the default foreground
** and background text and graphics colours to white and black
** respectively (VDU 20)
*/
static void reset_colours(void) {
  switch (colourdepth) {	/* Initialise the text mode colours */
  case 2:
    logtophys[0] = VDU_BLACK;
    logtophys[1] = VDU_WHITE;
    text_forecol = graph_forecol = 1;
    break;
  case 4:
    logtophys[0] = VDU_BLACK;
    logtophys[1] = VDU_RED;
    logtophys[2] = VDU_YELLOW;
    logtophys[3] = VDU_WHITE;
    text_forecol = graph_forecol = 3;
    break;
  case 16:
    logtophys[0] = VDU_BLACK;
    logtophys[1] = VDU_RED;
    logtophys[2] = VDU_GREEN;
    logtophys[3] = VDU_YELLOW;
    logtophys[4] = VDU_BLUE;
    logtophys[5] = VDU_MAGENTA;
    logtophys[6] = VDU_CYAN;
    logtophys[7]  = VDU_WHITE;
    logtophys[8] = FLASH_BLAWHITE;
    logtophys[9] = FLASH_REDCYAN;
    logtophys[10] = FLASH_GREENMAG;
    logtophys[11] = FLASH_YELBLUE;
    logtophys[12] = FLASH_BLUEYEL;
    logtophys[13] = FLASH_MAGREEN;
    logtophys[14] = FLASH_CYANRED;
    logtophys[15]  = FLASH_WHITEBLA;
    text_forecol = graph_forecol = 7;
    break;
  case 256:
    text_forecol = graph_forecol = 63;
    graph_foretint = text_foretint = MAXTINT;
    graph_backtint = text_backtint = 0;
    break;
  case COL24BIT:
    text_forecol = graph_forecol = 0xFFFFFF;
    graph_foretint = text_foretint = MAXTINT;
    graph_backtint = text_backtint = 0;
    break;
  default:
    error(ERR_UNSUPPORTED);
  }
  if (colourdepth==256)
    colourmask = COL256MASK;
  else {
    colourmask = colourdepth-1;
  }
  text_backcol = graph_backcol = 0;
  init_palette();
}

/*
** 'vdu_graphcol' sets the graphics foreground or background colour and
** changes the type of plotting action to be used for graphics (VDU 18).
*/
static void vdu_graphcol(void) {
  int32 colnumber;
  if (graphmode == NOGRAPHICS) error(ERR_NOGRAPHICS);
  colnumber = vduqueue[1];
  if (colnumber < 128) {	/* Setting foreground graphics colour */
      graph_fore_action = vduqueue[0];
      if (colourdepth == 256) {
        graph_forecol = colnumber & COL256MASK;
        graph_physforecol = (graph_forecol<<COL256SHIFT)+graph_foretint;
      } else if (colourdepth == COL24BIT) {
        graph_physforecol = graph_forecol = colour24bit(colnumber, graph_foretint);
      } else {
        graph_physforecol = graph_forecol = colnumber & colourmask;
      }
  }
  else {	/* Setting background graphics colour */
    graph_back_action = vduqueue[0];
    if (colourdepth == 256) {
      graph_backcol = colnumber & COL256MASK;
      graph_physbackcol = (graph_backcol<<COL256SHIFT)+graph_backtint;
    } else if (colourdepth == COL24BIT) {
      graph_physbackcol = graph_backcol = colour24bit(colnumber, graph_backtint);
    } else {	/* Operating in text mode */
      graph_physbackcol = graph_backcol = colnumber & colourmask;
    }
  }
  set_rgb();
}

/*
** 'vdu_graphwind' defines a graphics clipping region (VDU 24)
*/
static void vdu_graphwind(void) {
  int32 left, right, top, bottom;
  if (graphmode != FULLSCREEN) return;
  left = vduqueue[0]+vduqueue[1]*256;		/* Left-hand coordinate */
  if (left > 0x7FFF) left = -(0x10000-left);	/* Coordinate is negative */
  bottom = vduqueue[2]+vduqueue[3]*256;		/* Bottom coordinate */
  if (bottom > 0x7FFF) bottom = -(0x10000-bottom);
  right = vduqueue[4]+vduqueue[5]*256;		/* Right-hand coordinate */
  if (right > 0x7FFF) right = -(0x10000-right);
  top = vduqueue[6]+vduqueue[7]*256;		/* Top coordinate */
  if (top > 0x7FFF) top = -(0x10000-top);
  left += xorigin;
  right += xorigin;
  top += yorigin;
  bottom += yorigin;
  if (left > right) {	/* Ensure left < right */
    int32 temp = left;
    left = right;
    right = temp;
  }
  if (bottom > top) {	/* Ensure bottom < top */
    int32 temp = bottom;
    bottom = top;
    top = temp;
  }
/* Ensure clipping region is entirely within the screen area */
  if (right < 0 || top < 0 || left >= xgraphunits || bottom >= ygraphunits) return;
  gwinleft = left;
  gwinright = right;
  gwintop = top;
  gwinbottom = bottom;
  line_rect.x = GXTOPX(left);
  line_rect.y = GYTOPY(top);
  line_rect.w = right - left +1;
  line_rect.h = bottom - top +1;
  SDL_SetClipRect(modescreen, &line_rect);
  clipping = TRUE;
}

/*
** 'vdu_plot' handles the VDU 25 graphics sequences
*/
static void vdu_plot(void) {
  int32 x, y;
  x = vduqueue[1]+vduqueue[2]*256;
  if (x > 0x7FFF) x = -(0x10000-x);	/* X is negative */
  y = vduqueue[3]+vduqueue[4]*256;
  if (y > 0x7FFF) y = -(0x10000-y);	/* Y is negative */
  emulate_plot(vduqueue[0], x, y);	/* vduqueue[0] gives the plot code */
}

/*
** 'vdu_restwind' restores the default (full screen) text and
** graphics windows (VDU 26)
*/
static void vdu_restwind(void) {
  if (clipping) {	/* Restore graphics clipping region to entire screen area for mode */
    SDL_SetClipRect(modescreen, NULL);
    clipping = FALSE;
  }
  mode7highbit = 0;
  xorigin = yorigin = 0;
  xlast = ylast = xlast2 = ylast2 = 0;
  gwinleft = 0;
  gwinright = xgraphunits-1;
  gwintop = ygraphunits-1;
  gwinbottom = 0;
  if (graphmode == FULLSCREEN) {
    hide_cursor();	/* Remove cursor if in graphics mode */
    xtext = ytext = 0;
    reveal_cursor();	/* Redraw cursor if in graphics mode */
  }
  else {
    xtext = ytext = 0;
    move_cursor(0, 0);
  }
  textwin = FALSE;
  twinleft = 0;
  twinright = textwidth-1;
  twintop = 0;
  twinbottom = textheight-1;
}

/*
** 'vdu_textwind' defines a text window (VDU 28)
*/
static void vdu_textwind(void) {
  int32 left, right, top, bottom;
  mode7highbit = 0;
  left = vduqueue[0];
  bottom = vduqueue[1];
  right = vduqueue[2];
  top = vduqueue[3];
  if (left > right) {	/* Ensure right column number > left */
    int32 temp = left;
    left = right;
    right = temp;
  }
  if (bottom < top) {	/* Ensure bottom line number > top */
    int32 temp = bottom;
    bottom = top;
    top = temp;
  }
  if (left >= textwidth || top >= textheight) return;	/* Ignore bad parameters */
  twinleft = left;
  twinright = right;
  twintop = top;
  twinbottom = bottom;
/* Set flag to say if text window occupies only a part of the screen */
  textwin = left > 0 || right < textwidth-1 || top > 0 || bottom < textheight-1;
  move_cursor(twinleft, twintop);	/* Move text cursor to home position in new window */
}

/*
** 'vdu_origin' sets the graphics origin (VDU 29)
*/
static void vdu_origin(void) {
  int32 x, y;
  x = vduqueue[0]+vduqueue[1]*256;
  y = vduqueue[2]+vduqueue[3]*256;
  xorigin = x<=32767 ? x : -(0x10000-x);
  yorigin = y<=32767 ? y : -(0x10000-y);
}

/*
** 'vdu_hometext' sends the text cursor to the top left-hand corner of
** the text window (VDU 30)
*/
static void vdu_hometext(void) {
  if (vdu5mode) {	/* Send graphics cursor to top left-hand corner of graphics window */
    xlast = gwinleft;
    ylast = gwintop;
  }
  else {	/* Send text cursor to the top left-hand corner of the text window */
    move_cursor(twinleft, twintop);
  }
}

/*
** 'vdu_movetext' moves the text cursor to the given column and row in
** the text window (VDU 31)
*/
static void vdu_movetext(void) {
  int32 column, row;
  if (vdu5mode) {	/* Text is going to the graphics cursor */
    xlast = gwinleft+vduqueue[0]*XPPC*xgupp;
    ylast = gwintop-vduqueue[1]*YPPC*ygupp+1;
  }
  else {	/* Text is going to the graphics cursor */
    column = vduqueue[0] + twinleft;
    row = vduqueue[1] + twintop;
    if (column > twinright || row > twinbottom) return;	/* Ignore command if values are out of range */
    move_cursor(column, row);
  }
  if (screenmode == 7) {
    vdu141on=0;
    mode7highbit=0;
    mode7sepgrp=0;
    mode7conceal=0;
    mode7hold=0;
    mode7flash=0;
    text_physforecol = text_forecol = 7;
    text_physbackcol = text_backcol = 0;
    set_rgb();
  }
}

/*
** 'emulate_vdu' is a simple emulation of the RISC OS VDU driver. It
** accepts characters as per the RISC OS driver and uses them to imitate
** some of the VDU commands. Some of them are not supported and flagged
** as errors but others, for example, the 'page mode on' and 'page mode
** off' commands, are silently ignored.
*/
void emulate_vdu(int32 charvalue) {
  charvalue = charvalue & BYTEMASK;	/* Deal with any signed char type problems */
  if (vduneeded == 0) {			/* VDU queue is empty */
    if (vdu21state) {
      if (charvalue == VDU_ENABLE) vdu21state=0;
      return;
    }
    if (charvalue >= ' ') {		/* Most common case - print something */
      /* Handle Mode 7 */
      if (screenmode == 7) {
	if (charvalue == 127) {
	  mode7frame[ytext][xtext]=32;
	  move_curback();
	  move_curback();
	} else {
	  mode7frame[ytext][xtext]=charvalue;
	}
	mode7renderline(ytext);
	/* Set At codes go here, Set After codes are further down. */
        xtext++;
        if (xtext > twinright) {		/* Have reached edge of text window. Skip to next line  */
          xtext = twinleft;
          ytext++;
          if (ytext > twinbottom) {
            ytext--;
            if (textwin) {
              scroll_text(SCROLL_UP);
	    } else {
	      scroll(SCROLL_UP);
	    }
          }
        }
	return;
      } else {
	if (vdu5mode)			    /* Sending text output to graphics cursor */
          if (charvalue == 127) {
	    move_curback();
	    plot_space_opaque();
	    move_curback();
	  } else {
	    plot_char(charvalue);
	  }
	else {
	  if (charvalue == 127) {
	    move_curback();
	    write_char(32);
	    move_curback();
	  } else {
	    write_char(charvalue);
	    reveal_cursor();	/* Redraw the cursor */
	  }
	}
      }
      return;
    }
    else {	/* Control character - Found start of new VDU command */
      if (graphmode==FULLSCREEN) {	/* Flush any buffered text to the screen */
        if (!echo) echo_text();
      }
      else {
        if (!echo) echo_ttext();
      }
      vducmd = charvalue;
      vduneeded = vdubytes[charvalue];
      vdunext = 0;
    }
  }
  else {	/* Add character to VDU queue for current command */
    vduqueue[vdunext] = charvalue;
    vdunext++;
  }
  if (vdunext < vduneeded) return;
  vduneeded = 0;

/* There are now enough entries in the queue for the current command */

  switch (vducmd) {	/* Emulate the various control codes */
  case VDU_NULL:  	/* 0 - Do nothing */
    break;
  case VDU_PRINT:	/* 1 - Send next character to the print stream */
  case VDU_ENAPRINT: 	/* 2 - Enable the sending of characters to the printer */
  case VDU_DISPRINT:	/* 3 - Disable the sending of characters to the printer */
    break;
  case VDU_TEXTCURS:	/* 4 - Print text at text cursor */
    vdu5mode = FALSE;
    if (cursorstate == HIDDEN) {	/* Start displaying the cursor */
      cursorstate = SUSPENDED;
      toggle_cursor();
    }
    break;
  case VDU_GRAPHICURS:	/* 5 - Print text at graphics cursor */
    if (!istextonly()) {
      if (graphmode == TEXTMODE) switch_graphics();		/* Use VDU 5 as a way of switching to graphics mode */
      if (graphmode == FULLSCREEN) {
        vdu5mode = TRUE;
        toggle_cursor();	/* Remove the cursor if it is being displayed */
        cursorstate = HIDDEN;
      }
    }
    break;
  case VDU_ENABLE:	/* 6 - Enable the VDU driver (ignored) */
    enable_vdu = TRUE;
    vdu21state=0;
    break;
  case VDU_BEEP:	/* 7 - Sound the bell */
    putchar('\7');
    if (echo) fflush(stdout);
    break;
  case VDU_CURBACK:	/* 8 - Move cursor left one character */
    move_curback();
    break;
  case VDU_CURFORWARD:	/* 9 - Move cursor right one character */
    move_curforward();
    break;
  case VDU_CURDOWN:	/* 10 - Move cursor down one line (linefeed) */
    move_curdown();
    break;
  case VDU_CURUP:	/* 11 - Move cursor up one line */
    move_curup();
    break;
  case VDU_CLEARTEXT:	/* 12 - Clear text window (formfeed) */
    if (vdu5mode)	/* In VDU 5 mode, clear the graphics window */
      vdu_cleargraph();
    else		/* In text mode, clear the text window */
      vdu_cleartext();
    vdu_hometext();
    break;
  case VDU_RETURN:	/* 13 - Carriage return */
    vdu_return();
    break;
  case VDU_ENAPAGE:	/* 14 - Enable page mode (ignored) */
  case VDU_DISPAGE:	/* 15 - Disable page mode (ignored) */
    break;
  case VDU_CLEARGRAPH:	/* 16 - Clear graphics window */
    vdu_cleargraph();
    break;
  case VDU_TEXTCOL:	/* 17 - Change current text colour */
    vdu_textcol();
    break;
  case VDU_GRAPHCOL:	/* 18 - Change current graphics colour */
    vdu_graphcol();
    break;
  case VDU_LOGCOL:	/* 19 - Map logical colour to physical colour */
    vdu_setpalette();
    break;
  case VDU_RESTCOL:	/* 20 - Restore logical colours to default values */
    reset_colours();
    break;
  case VDU_DISABLE:	/* 21 - Disable the VDU driver */
    vdu21state = 1;
    break;
  case VDU_SCRMODE:	/* 22 - Change screen mode */
    emulate_mode(vduqueue[0]);
    break;
  case VDU_COMMAND:	/* 23 - Assorted VDU commands */
    vdu_23command();
    break;
  case VDU_DEFGRAPH:	/* 24 - Define graphics window */
    vdu_graphwind();
    break;
  case VDU_PLOT:	/* 25 - Issue graphics command */
    vdu_plot();
    break;
  case VDU_RESTWIND:	/* 26 - Restore default windows */
    vdu_restwind();
    break;
  case VDU_ESCAPE:	/* 27 - Do nothing (character is sent to output stream) */
//    putch(vducmd);
    break;
  case VDU_DEFTEXT:	/* 28 - Define text window */
    vdu_textwind();
    break;
  case VDU_ORIGIN:	/* 29 - Define graphics origin */
    vdu_origin();
    break;
  case VDU_HOMETEXT:	/* 30 - Send cursor to top left-hand corner of screen */
    vdu_hometext();
    break;
  case VDU_MOVETEXT:	/* 31 - Send cursor to column x, row y on screen */
    vdu_movetext();
  }
}

/*
** 'emulate_vdustr' is called to print a string via the 'VDU driver'
*/
void emulate_vdustr(char string[], int32 length) {
  int32 n;
  if (length == 0) length = strlen(string);
  echo_off();
  for (n = 0; n < length-1; n++) emulate_vdu(string[n]);	/* Send the string to the VDU driver */
  echo_on();
  emulate_vdu(string[length-1]);        /* last char sent after echo turned back on */
}

/*
** 'emulate_printf' provides a more flexible way of displaying formatted
** output than calls to 'emulate_vdustr'. It is used in the same way as
** 'printf' and can take any number of parameters. The text is sent directly
** to the screen
*/
void emulate_printf(char *format, ...) {
  int32 length;
  va_list parms;
  char text [MAXSTRING];
  va_start(parms, format);
  length = vsprintf(text, format, parms);
  va_end(parms);
  emulate_vdustr(text, length);
}

/*
** emulate_vdufn - Emulates the Basic VDU function. This
** returns the value of the specified VDU variable. Only a
** small subset of the possible values available under
** RISC OS are returned
*/
int32 emulate_vdufn(int variable) {
  switch (variable) {
  case 0: /* ModeFlags */	return graphmode >= TEXTMODE ? 0 : 1;
  case 1: /* ScrRCol */		return textwidth - 1;
  case 2: /* ScrBRow */		return textheight - 1;
  case 3: /* NColour */		return colourdepth - 1;
  case 11: /* XWindLimit */	return screenwidth - 1;
  case 12: /* YWindLimit */	return screenheight - 1;
  case 128: /* GWLCol */	return gwinleft / xgupp;
  case 129: /* GWBRow */	return gwinbottom / ygupp;
  case 130: /* GWRCol */	return gwinright / xgupp;
  case 131: /* GWTRow */	return gwintop / ygupp;
  case 132: /* TWLCol */	return twinleft;
  case 133: /* TWBRow */	return twinbottom;
  case 134: /* TWRCol */	return twinright;
  case 135: /* TWTRow */	return twintop;
  case 136: /* OrgX */		return xorigin;
  case 137: /* OrgY */		return yorigin;
  case 153: /* GFCOL */		return graph_forecol;
  case 154: /* GBCOL */		return graph_backcol;
  case 155: /* TForeCol */	return text_forecol;
  case 156: /* TBackCol */	return text_backcol;
  case 157: /* GFTint */	return graph_foretint;
  case 158: /* GBTint */	return graph_backtint;
  case 159: /* TFTint */	return text_foretint;
  case 160: /* TBTint */	return text_backtint;
  case 161: /* MaxMode */	return HIGHMODE;
  default:
    return 0;
  }
}

/*
** 'emulate_pos' returns the number of the column in which the text cursor
** is located in the text window
*/
int32 emulate_pos(void) {
  return xtext-twinleft;
}

/*
** 'emulate_vpos' returns the number of the row in which the text cursor
** is located in the text window
*/
int32 emulate_vpos(void) {
  return ytext-twintop;
}

/*
** 'setup_mode' is called to set up the details of mode 'mode'
*/
static void setup_mode(int32 mode) {
  int32 modecopy;
  Uint32 sx, sy, ox, oy;
  int flags = screen0->flags;
  int p;
  SDL_Surface *m7fontbuf;

  if (mode == 7) { /* Reset width to 16 */
    M7XPPC=16;
    m7fontbuf = SDL_CreateRGBSurface(SDL_SWSURFACE, M7XPPC, M7YPPC, 32, 0xff000000, 0x00ff0000, 0x0000ff00, 0x000000ff);
    sdl_m7fontbuf = SDL_ConvertSurface(m7fontbuf, screen0->format, 0);
    SDL_FreeSurface(m7fontbuf);
    modetable[7].xres = 40*M7XPPC;
    modetable[7].xgraphunits = 80*M7XPPC;
  }
  modecopy = mode;
  mode = mode & MODEMASK;	/* Lose 'shadow mode' bit */
  if (mode > HIGHMODE) mode = modecopy = 0;	/* Out of range modes are mapped to MODE 0 */
  ox=vscrwidth;
  oy=vscrheight;
  /* Try to catch an undefined mode */
  hide_cursor();
  if (modetable[mode].xres == 0) error(ERR_BADMODE);
  sx=(modetable[mode].xres * modetable[mode].xscale);
  sy=(modetable[mode].yres * modetable[mode].yscale);
  SDL_BlitSurface(screen0, NULL, screen1, NULL);
  SDL_FreeSurface(screen0);
  screen0 = SDL_SetVideoMode(sx, sy, 32, flags);
  if (!screen0) {
    /* Reinstate previous display mode */
    sx=ox; sy=oy;
    screen0 = SDL_SetVideoMode(ox, oy, 32, flags);
    SDL_BlitSurface(screen1, NULL, screen0, NULL);
    do_sdl_updaterect(screen0, 0, 0, 0, 0);
    error(ERR_BADMODE);
  }
  autorefresh=1;
  vscrwidth = sx;
  vscrheight = sy;
  for (p=0; p<4; p++) {
    SDL_FreeSurface(screenbank[p]);
    screenbank[p]=SDL_DisplayFormat(screen0);
  }
  modescreen = SDL_DisplayFormat(screen0);
  displaybank=0;
  writebank=0;
  SDL_FreeSurface(screen1);
  screen1 = SDL_DisplayFormat(screen0);
  SDL_FreeSurface(screen2);
  screen2 = SDL_DisplayFormat(screen0);
  SDL_FreeSurface(screen2A);
  screen2A = SDL_DisplayFormat(screen0);
  SDL_FreeSurface(screen3);
  screen3 = SDL_DisplayFormat(screen0);
  SDL_FreeSurface(screen3A);
  screen3A = SDL_DisplayFormat(screen0);
/* Set up VDU driver parameters for mode */
  screenmode = modecopy;
  YPPC=8; if ((mode == 3) || (mode == 6)) YPPC=10;
  place_rect.h = font_rect.h = YPPC;
  reset_mode7();
  screenwidth = modetable[mode].xres;
  screenheight = modetable[mode].yres;
  xgraphunits = modetable[mode].xgraphunits;
  ygraphunits = modetable[mode].ygraphunits;
  colourdepth = modetable[mode].coldepth;
  textwidth = modetable[mode].xtext;
  textheight = modetable[mode].ytext;
  xscale = modetable[mode].xscale;
  yscale = modetable[mode].yscale;
  scaled = yscale != 1 || xscale != 1;	/* TRUE if graphics screen is scaled to fit real screen */
/* If running in text mode, ignore the screen depth */
  enable_vdu = TRUE;
  echo = TRUE;
  vdu5mode = FALSE;
  cursmode = UNDERLINE;
  cursorstate = NOCURSOR;	/* Graphics mode text cursor is not being displayed */
  clipping = FALSE;		/* A clipping region has not been defined for the screen mode */
  xgupp = xgraphunits/screenwidth;	/* Graphics units per pixel in X direction */
  ygupp = ygraphunits/screenheight;	/* Graphics units per pixel in Y direction */
  xorigin = yorigin = 0;
  xlast = ylast = xlast2 = ylast2 = 0;
  gwinleft = 0;
  gwinright = xgraphunits-1;
  gwintop = ygraphunits-1;
  gwinbottom = 0;
  textwin = FALSE;		/* A text window has not been created yet */
  twinleft = 0;			/* Set up initial text window to whole screen */
  twinright = textwidth-1;
  twintop = 0;
  twinbottom = textheight-1;
  xtext = ytext = 0;
  graph_fore_action = graph_back_action = 0;
  if (graphmode == FULLSCREEN && (!basicvars.runflags.start_graphics)) {
    switch_text();
    graphmode = TEXTONLY;
  }
  if (graphmode != NOGRAPHICS && graphmode != FULLSCREEN) {	/* Decide on current graphics mode */
    graphmode = TEXTMODE;	/* Output to text screen but can switch to graphics */
  }
  reset_colours();
  init_palette();
  if (cursorstate == NOCURSOR) cursorstate = ONSCREEN;
  SDL_FillRect(screen0, NULL, tb_colour);
  SDL_FillRect(modescreen, NULL, tb_colour);
  SDL_FillRect(screen2, NULL, tb_colour);
  SDL_FillRect(screen3, NULL, tb_colour);
  SDL_SetClipRect(screen0, NULL);
  sdl_mouse_onoff(0);
  if (screenmode == 7) {
    font_rect.w = place_rect.w = M7XPPC;
    font_rect.h = place_rect.h = M7YPPC;
  } else {
    font_rect.w = place_rect.w = XPPC;
    font_rect.h = place_rect.h = YPPC;
  }
}

/*
** 'emulate_mode' deals with the Basic 'MODE' statement when the
** parameter is a number. This version of the function is used when
** the interpreter supports graphics.
*/
void emulate_mode(int32 mode) {
  setup_mode(mode);
/* Reset colours, clear screen and home cursor */
  SDL_FillRect(screen0, NULL, tb_colour);
  SDL_FillRect(modescreen, NULL, tb_colour);
  xtext = twinleft;
  ytext = twintop;
  do_sdl_flip(screen0);
  emulate_vdu(VDU_CLEARGRAPH);
}

/*
 * emulate_newmode - Change the screen mode using specific mode
 * parameters for the screen size and so on. This is for the new
 * form of the MODE statement
 */
void emulate_newmode(int32 xres, int32 yres, int32 bpp, int32 rate) {
  int32 coldepth, n;
  if (xres == 0 || yres == 0 || rate == 0 || bpp == 0) error(ERR_BADMODE);
  switch (bpp) {
  case 1: coldepth = 2; break;
  case 2: coldepth = 4; break;
  case 4: coldepth = 16; break;
  case 24: coldepth = COL24BIT; break;
  default:
    coldepth = 256;
  }
  for (n=0; n<=HIGHMODE; n++) {
    if (modetable[n].xres == xres && modetable[n].yres == yres && modetable[n].coldepth == coldepth) break;
  }
  if (n > HIGHMODE) {
    /* Mode isn't predefined. So, let's make it. */
    n=126;
    setupnewmode(n, xres, yres, coldepth, 1, 1, 1, 1);
  }
  emulate_mode(n);
}

/*
** 'emulate_modestr' deals with the Basic 'MODE' command when the
** parameter is a string. This code is restricted to the standard
** RISC OS screen modes but can be used to define a grey scale mode
** instead of a colour one
*/
void emulate_modestr(int32 xres, int32 yres, int32 colours, int32 greys, int32 xeig, int32 yeig, int32 rate) {
  int32 coldepth, n;
  if (xres == 0 || yres == 0 || rate == 0 || (colours == 0 && greys == 0)) error(ERR_BADMODE);
  coldepth = colours!=0 ? colours : greys;
  for (n=0; n <= HIGHMODE; n++) {
    if (xeig==1 && yeig==1 && modetable[n].xres == xres && modetable[n].yres == yres && modetable[n].coldepth == coldepth) break;
  }
  if (n > HIGHMODE) {
    /* Mode isn't predefined. So, let's make it. */
    n=126;
    setupnewmode(n, xres, yres, coldepth, 1, 1, xeig, yeig);
  }
  emulate_mode(n);
  if (colours == 0) {	/* Want a grey scale palette  - Reset all the colours */
    int32 step, intensity;
    step = 255/(greys-1);
    intensity = 0;
    for (n=0; n < greys; n++) {
      change_palette(n, intensity, intensity, intensity);
      intensity+=step;
    }
  }
}

/*
** 'emulate_modefn' emulates the Basic function 'MODE'
*/
int32 emulate_modefn(void) {
  return screenmode;
}

#define FILLSTACK 500

/*
** 'flood_fill' floods fills an area of screen with the colour 'colour'.
** x and y are the coordinates of the point at which to start. All
** points that have the same colour as the one at (x, y) that can be
** reached from (x, y) are set to colour 'colour'.
** Note that the coordinates are *pixel* coordinates, that is, they are
** not expressed in graphics units.
**
** This code is slow but does the job
*/
static void flood_fill(int32 x, int y, int colour, Uint32 action) {
  int32 sp, fillx[FILLSTACK], filly[FILLSTACK];
  int32 left, right, top, bottom, lleft, lright, pwinleft, pwinright, pwintop, pwinbottom;
  boolean above, below;
  pwinleft = GXTOPX(gwinleft);		/* Calculate extent of graphics window in pixels */
  pwinright = GXTOPX(gwinright);
  pwintop = GYTOPY(gwintop);
  pwinbottom = GYTOPY(gwinbottom);
  if (x < pwinleft || x > pwinright || y < pwintop || y > pwinbottom
   || *((Uint32*)modescreen->pixels + x + y*vscrwidth) != gb_colour) return;
  left = right = x;
  top = bottom = y;
  sp = 0;
  fillx[sp] = x;
  filly[sp] = y;
  sp++;
  do {
    sp--;
    y = filly[sp];
    lleft = fillx[sp];
    lright = lleft+1;
    if (y < top) top = y;
    if (y > bottom) bottom = y;
    above = below = FALSE;
    while (lleft >= pwinleft && *((Uint32*)modescreen->pixels + lleft + y*vscrwidth) == gb_colour) {
      if (y > pwintop) {	/* Check if point above current point is set to the background colour */
        if (*((Uint32*)modescreen->pixels + lleft + (y-1)*vscrwidth) != gb_colour)
          above = FALSE;
        else if (!above) {
          above = TRUE;
          if (sp == FILLSTACK) return;	/* Shape is too complicated so give up */
          fillx[sp] = lleft;
          filly[sp] = y-1;
          sp++;
        }
      }
      if (y < pwinbottom) {	/* Check if point below current point is set to the background colour */
        if (*((Uint32*)modescreen->pixels + lleft + (y+1)*vscrwidth) != gb_colour)
          below = FALSE;
        else if (!below) {
          below = TRUE;
          if (sp == FILLSTACK) return;	/* Shape is too complicated so give up */
          fillx[sp] = lleft;
          filly[sp] = y+1;
          sp++;
        }
      }
      lleft--;
    }
    lleft++;	/* Move back to first column set to background colour */
    above = below = FALSE;
    while (lright <= pwinright && *((Uint32*)modescreen->pixels + lright + y*vscrwidth) == gb_colour) {
      if (y > pwintop) {
        if (*((Uint32*)modescreen->pixels + lright + (y-1)*vscrwidth) != gb_colour)
          above = FALSE;
        else if (!above) {
          above = TRUE;
          if (sp == FILLSTACK) return;	/* Shape is too complicated so give up */
          fillx[sp] = lright;
          filly[sp] = y-1;
          sp++;
        }
      }
      if (y < pwinbottom) {
        if (*((Uint32*)modescreen->pixels + lright + (y+1)*vscrwidth) != gb_colour)
          below = FALSE;
        else if (!below) {
          below = TRUE;
          if (sp == FILLSTACK) return;	/* Shape is too complicated so give up */
          fillx[sp] = lright;
          filly[sp] = y+1;
          sp++;
        }
      }
      lright++;
    }
    lright--;
    draw_line(modescreen, lleft, y, lright, y, colour, 0, action);
    if (lleft < left) left = lleft;
    if (lright > right) right = lright;
  } while (sp != 0);
    hide_cursor();
    blit_scaled(left, top, right, bottom);
    reveal_cursor();
}

/* The plot_pixel function plots pixels for the drawing functions, and
   takes into account the GCOL foreground action code */
void plot_pixel(SDL_Surface *surface, int64 offset, Uint32 colour, Uint32 action) {
  Uint32 altcolour = 0, prevcolour = 0, drawcolour;
  
  if (plot_inverse ==1) {
    action=3;
    drawcolour=(colourdepth-1);
  } else {
    drawcolour=graph_physforecol;
  }

  if ((action==0) && (plot_inverse == 0)) {
    altcolour = colour;
  } else {
    prevcolour=*((Uint32*)surface->pixels + offset);
    prevcolour=emulate_colourfn((prevcolour >> 16) & 0xFF, (prevcolour >> 8) & 0xFF, (prevcolour & 0xFF));
    if (colourdepth == 256) prevcolour = prevcolour >> COL256SHIFT;
    switch (action) {
      case 1:
	altcolour=(prevcolour | drawcolour);
	break;
      case 2:
	altcolour=(prevcolour & drawcolour);
	break;
      case 3:
	altcolour=(prevcolour ^ drawcolour);
	break;
    }
    if (colourdepth == COL24BIT) {
      altcolour = altcolour & 0xFFFFFF;
    } else {
      altcolour=altcolour*3;
      altcolour=SDL_MapRGB(sdl_fontbuf->format, palette[altcolour], palette[altcolour+1], palette[altcolour+2]);
    }
  }
  *((Uint32*)surface->pixels + offset) = altcolour;
}

/*
** 'emulate_plot' emulates the Basic statement 'PLOT'. It also represents
** the heart of the graphics emulation functions as most of the other
** graphics functions are just pre-packaged calls to this one.
** The way the graphics support works is that objects are drawn on
** a virtual screen and then copied to the real screen. The code tries
** to minimise the size of the area of the real screen updated each time
** for speed as updating the entire screen each time is too slow
*/
void emulate_plot(int32 code, int32 x, int32 y) {
  int32 xlast3, ylast3, sx, sy, ex, ey, action;
  Uint32 colour = 0;
  SDL_Rect plot_rect, temp_rect;
  if (istextonly()) return;
  if (graphmode == TEXTONLY) return;
  if (graphmode == TEXTMODE) switch_graphics();
/* Decode the command */
  plot_inverse = 0;
  action = graph_fore_action;
  xlast3 = xlast2;
  ylast3 = ylast2;
  xlast2 = xlast;
  ylast2 = ylast;
  if ((code & ABSCOORD_MASK) != 0 ) {		/* Coordinate (x,y) is absolute */
    xlast = x+xorigin;	/* These probably have to be treated as 16-bit values */
    ylast = y+yorigin;
  }
  else {	/* Coordinate (x,y) is relative */
    xlast+=x;	/* These probably have to be treated as 16-bit values */
    ylast+=y;
  }
  if ((code & PLOT_COLMASK) == PLOT_MOVEONLY) return;	/* Just moving graphics cursor, so finish here */
  sx = GXTOPX(xlast2);
  sy = GYTOPY(ylast2);
  ex = GXTOPX(xlast);
  ey = GYTOPY(ylast);
  if ((code & GRAPHOP_MASK) != SHIFT_RECTANGLE) {		/* Move and copy rectangle are a special case */
    switch (code & PLOT_COLMASK) {
    case PLOT_FOREGROUND:	/* Use graphics foreground colour */
      colour = gf_colour;
      break;
    case PLOT_INVERSE:		/* Use logical inverse of colour at each point */
      plot_inverse=1;
      break;
    case PLOT_BACKGROUND:	/* Use graphics background colour */
      colour = gb_colour;
      action = graph_back_action;
    }
  }
/* Now carry out the operation */
  switch (code & GRAPHOP_MASK) {
  case DRAW_SOLIDLINE:
  case DRAW_SOLIDLINE+8:
  case DRAW_DOTLINE:
  case DRAW_DOTLINE+8:
  case DRAW_SOLIDLINE2:
  case DRAW_SOLIDLINE2+8:
  case DRAW_DOTLINE2:
  case DRAW_DOTLINE2+8: {	/* Draw line */
    int32 top, left;
    left = sx;	/* Find top left-hand corner of rectangle containing line */
    top = sy;
    if (ex < sx) left = ex;
    if (ey < sy) top = ey;
    draw_line(modescreen, sx, sy, ex, ey, colour, (code & DRAW_STYLEMASK), action);
    hide_cursor();
    blit_scaled(left, top, sx+ex-left, sy+ey-top);
    reveal_cursor();
    break;
  }
  case PLOT_POINT:	/* Plot a single point */
    hide_cursor();
    if ((ex < 0) || (ex >= screenwidth) || (ey < 0) || (ey >= screenheight)) break;
    plot_pixel(modescreen, ex + ey*vscrwidth, colour, action);
    blit_scaled(ex, ey, ex, ey);
    reveal_cursor();
    break;
  case FILL_TRIANGLE: {		/* Plot a filled triangle */
    int32 left, right, top, bottom;
    filled_triangle(modescreen, GXTOPX(xlast3), GYTOPY(ylast3), sx, sy, ex, ey, colour, action);
/*  Now figure out the coordinates of the rectangle that contains the triangle */
    left = right = xlast3;
    top = bottom = ylast3;
    if (xlast2 < left) left = xlast2;
    if (xlast < left) left = xlast;
    if (xlast2 > right) right = xlast2;
    if (xlast > right) right = xlast;
    if (ylast2 > top) top = ylast2;
    if (ylast > top) top = ylast;
    if (ylast2 < bottom) bottom = ylast2;
    if (ylast < bottom) bottom = ylast;
    hide_cursor();
    blit_scaled(GXTOPX(left), GYTOPY(top), GXTOPX(right), GYTOPY(bottom));
    reveal_cursor();
    break;
  }
  case FILL_RECTANGLE: {		/* Plot a filled rectangle */
    int32 left, right, top, bottom;
    left = sx;
    top = sy;
    if (ex < sx) left = ex;
    if (ey < sy) top = ey;
    right = sx+ex-left;
    bottom = sy+ey-top;
/* sx and sy give the bottom left-hand corner of the rectangle */
/* x and y are its width and height */
    plot_rect.x = left;
    plot_rect.y = top;
    plot_rect.w = right - left +1;
    plot_rect.h = bottom - top +1;
    if (action==0) {
    SDL_FillRect(modescreen, &plot_rect, colour);
    } else {
      fill_rectangle(left, top, right, bottom, colour, action);
    }
    hide_cursor();
    blit_scaled(left, top, right, bottom);
    reveal_cursor();
    break;
  }
  case FILL_PARALLELOGRAM: {	/* Plot a filled parallelogram */
    int32 vx, vy, left, right, top, bottom;
    filled_triangle(modescreen, GXTOPX(xlast3), GYTOPY(ylast3), sx, sy, ex, ey, colour, action);
    vx = xlast3-xlast2+xlast;
    vy = ylast3-ylast2+ylast;
    filled_triangle(modescreen, ex, ey, GXTOPX(vx), GYTOPY(vy), GXTOPX(xlast3), GYTOPY(ylast3), colour, action);
/*  Now figure out the coordinates of the rectangle that contains the parallelogram */
    left = right = xlast3;
    top = bottom = ylast3;
    if (xlast2 < left) left = xlast2;
    if (xlast < left) left = xlast;
    if (vx < left) left = vx;
    if (xlast2 > right) right = xlast2;
    if (xlast > right) right = xlast;
    if (vx > right) right = vx;
    if (ylast2 > top) top = ylast2;
    if (ylast > top) top = ylast;
    if (vy > top) top = vy;
    if (ylast2 < bottom) bottom = ylast2;
    if (ylast < bottom) bottom = ylast;
    if (vy < bottom) bottom = vy;
    hide_cursor();
    blit_scaled(GXTOPX(left), GYTOPY(top), GXTOPX(right), GYTOPY(bottom));
    reveal_cursor();
    break;
  }
  case FLOOD_BACKGROUND:	/* Flood fill background with graphics foreground colour */
    flood_fill(ex, ey, colour, action);
    break;
  case PLOT_CIRCLE:		/* Plot the outline of a circle */
  case FILL_CIRCLE: {		/* Plot a filled circle */
    int32 xradius, yradius, xr;
/*
** (xlast2, ylast2) is the centre of the circle. (xlast, ylast) is a
** point on the circumference, specifically the left-most point of the
** circle.
*/
    xradius = abs(xlast2-xlast)/xgupp;
    yradius = abs(xlast2-xlast)/ygupp;
    xr=xlast2-xlast;
    if ((code & GRAPHOP_MASK) == PLOT_CIRCLE)
      draw_ellipse(modescreen, sx, sy, xradius, yradius, colour, action);
    else {
      filled_ellipse(modescreen, sx, sy, xradius, yradius, colour, action);
    }
    /* To match RISC OS, xlast needs to be the right-most point not left-most. */
    xlast+=(xr*2);
    ex = sx-xradius;
    ey = sy-yradius;
/* (ex, ey) = coordinates of top left hand corner of the rectangle that contains the ellipse */
    hide_cursor();
    blit_scaled(ex, ey, ex+2*xradius, ey+2*yradius);
    reveal_cursor();
    break;
  }
  case SHIFT_RECTANGLE: {	/* Move or copy a rectangle */
    int32 destleft, destop, left, right, top, bottom;
    if (xlast3 < xlast2) {	/* Figure out left and right hand extents of rectangle */
      left = GXTOPX(xlast3);
      right = GXTOPX(xlast2);
    }
    else {
      left = GXTOPX(xlast2);
      right = GXTOPX(xlast3);
    }
    if (ylast3 > ylast2) {	/* Figure out upper and lower extents of rectangle */
      top = GYTOPY(ylast3);
      bottom = GYTOPY(ylast2);
    }
    else {
      top = GYTOPY(ylast2);
      bottom = GYTOPY(ylast3);
    }
    destleft = GXTOPX(xlast);		/* X coordinate of top left-hand corner of destination */
    destop = GYTOPY(ylast)-(bottom-top);	/* Y coordinate of top left-hand corner of destination */
    plot_rect.x = destleft;
    plot_rect.y = destop;
    temp_rect.x = left;
    temp_rect.y = top;
    temp_rect.w = plot_rect.w = right - left +1;
    temp_rect.h = plot_rect.h = bottom - top +1;
    SDL_BlitSurface(modescreen, &temp_rect, screen1, &plot_rect); /* copy to temp buffer */
    SDL_BlitSurface(screen1, &plot_rect, modescreen, &plot_rect);
    hide_cursor();
    blit_scaled(destleft, destop, destleft+(right-left), destop+(bottom-top));
    reveal_cursor();
    if (code == MOVE_RECTANGLE) {	/* Move rectangle - Set original rectangle to the background colour */
      int32 destright, destbot;
      destright = destleft+right-left;
      destbot = destop+bottom-top;
/* Check if source and destination rectangles overlap */
      if (((destleft >= left && destleft <= right) || (destright >= left && destright <= right)) &&
       ((destop >= top && destop <= bottom) || (destbot >= top && destbot <= bottom))) {	/* Overlap found */
        int32 xdiff, ydiff;
/*
** The area of the original rectangle that is not overlapped can be
** broken down into one or two smaller rectangles. Figure out the
** coordinates of those rectangles and plot filled rectangles over
** them set to the graphics background colour
*/
        xdiff = left-destleft;
        ydiff = top-destop;
        if (ydiff > 0) {	/* Destination area is higher than the original area on screen */
          if (xdiff > 0) {
            plot_rect.x = destright+1;
            plot_rect.y = top;
            plot_rect.w = right - (destright+1) +1;
            plot_rect.h = destbot - top +1;
            SDL_FillRect(modescreen, &plot_rect, gb_colour);
          }
          else if (xdiff < 0) {
            plot_rect.x = left;
            plot_rect.y = top;
            plot_rect.w = (destleft-1) - left +1;
            plot_rect.h = destbot - top +1;
            SDL_FillRect(modescreen, &plot_rect, gb_colour);
          }
          plot_rect.x = left;
          plot_rect.y = destbot+1;
          plot_rect.w = right - left +1;
          plot_rect.h = bottom - (destbot+1) +1;
          SDL_FillRect(modescreen, &plot_rect, gb_colour);
        }
        else if (ydiff == 0) {	/* Destination area is on same level as original area */
          if (xdiff > 0) {	/* Destination area lies to left of original area */
            plot_rect.x = destright+1;
            plot_rect.y = top;
            plot_rect.w = right - (destright+1) +1;
            plot_rect.h = bottom - top +1;
            SDL_FillRect(modescreen, &plot_rect, gb_colour);
          }
          else if (xdiff < 0) {
            plot_rect.x = left;
            plot_rect.y = top;
            plot_rect.w = (destleft-1) - left +1;
            plot_rect.h = bottom - top +1;
            SDL_FillRect(modescreen, &plot_rect, gb_colour);
          }
        }
        else {	/* Destination area is lower than original area on screen */
          if (xdiff > 0) {
            plot_rect.x = destright+1;
            plot_rect.y = destop;
            plot_rect.w = right - (destright+1) +1;
            plot_rect.h = bottom - destop +1;
            SDL_FillRect(modescreen, &plot_rect, gb_colour);
          }
          else if (xdiff < 0) {
            plot_rect.x = left;
            plot_rect.y = destop;
            plot_rect.w = (destleft-1) - left +1;
            plot_rect.h = bottom - destop +1;
            SDL_FillRect(modescreen, &plot_rect, gb_colour);
          }
          plot_rect.x = left;
          plot_rect.y = top;
          plot_rect.w = right - left +1;
          plot_rect.h = (destop-1) - top +1;
          SDL_FillRect(modescreen, &plot_rect, gb_colour);
        }
      }
      else {	/* No overlap - Simple case */
        plot_rect.x = left;
        plot_rect.y = top;
        plot_rect.w = right - left +1;
        plot_rect.h = bottom - top +1;
        SDL_FillRect(modescreen, &plot_rect, gb_colour);
      }
      hide_cursor();
      blit_scaled(left, top, right, bottom);
      reveal_cursor();
    }
    break;
  }
  case PLOT_ELLIPSE:		/* Draw an ellipse outline */
  case FILL_ELLIPSE: {		/* Draw a filled ellipse */
    int32 semimajor, semiminor;
/*
** (xlast3, ylast3) is the centre of the ellipse. (xlast2, ylast2) is a
** point on the circumference in the +ve X direction and (xlast, ylast)
** is a point on the circumference in the +ve Y direction
*/
    semimajor = abs(xlast2-xlast3)/xgupp;
    semiminor = abs(ylast-ylast3)/ygupp;
    sx = GXTOPX(xlast3);
    sy = GYTOPY(ylast3);
    if ((code & GRAPHOP_MASK) == PLOT_ELLIPSE)
      draw_ellipse(modescreen, sx, sy, semimajor, semiminor, colour, action);
    else {
      filled_ellipse(modescreen, sx, sy, semimajor, semiminor, colour, action);
    }
    ex = sx-semimajor;
    ey = sy-semiminor;
/* (ex, ey) = coordinates of top left hand corner of the rectangle that contains the ellipse */
    hide_cursor();
    blit_scaled(ex, ey, ex+2*semimajor, ey+2*semiminor);
    reveal_cursor();
    break;
  }
  //default:
    //error(ERR_UNSUPPORTED); /* switch this off, make unhandled plots a no-op*/
  }
}

/*
** 'emulate_pointfn' emulates the Basic function 'POINT', returning
** the colour number of the point (x,y) on the screen
*/
int32 emulate_pointfn(int32 x, int32 y) {
  int32 colour, colnum;
  if (graphmode == FULLSCREEN) {
    colour = *((Uint32*)modescreen->pixels + GXTOPX(x+xorigin) + GYTOPY(y+yorigin)*vscrwidth);
    if (colourdepth == COL24BIT) return riscoscolour(colour);
    colnum = emulate_colourfn((colour >> 16) & 0xFF, (colour >> 8) & 0xFF, (colour & 0xFF));
    if (colourdepth == 256) colnum = colnum >> COL256SHIFT;
    return colnum;
  }
  else {
    return 0;
  }
}

/*
** 'emulate_tintfn' deals with the Basic keyword 'TINT' when used as
** a function. It returns the 'TINT' value of the point (x, y) on the
** screen. This is one of 0, 0x40, 0x80 or 0xC0
*/
int32 emulate_tintfn(int32 x, int32 y) {
  if (graphmode != FULLSCREEN || colourdepth < 256) return 0;
  return *((Uint32*)modescreen->pixels + GXTOPX(x+xorigin) + GYTOPY(y+yorigin)*vscrwidth)<<TINTSHIFT;
}

/*
** 'emulate_pointto' emulates the 'POINT TO' statement
*/
void emulate_pointto(int32 x, int32 y) {
  error(ERR_UNSUPPORTED);
}

/*
** 'emulate_wait' deals with the Basic 'WAIT' statement
*/
void emulate_wait(void) {
  if (basicvars.runflags.flag_cosmetic) error(ERR_UNSUPPORTED);
}

/*
** 'emulate_tab' moves the text cursor to the position column 'x' row 'y'
** in the current text window
*/
void emulate_tab(int32 x, int32 y) {
  emulate_vdu(VDU_MOVETEXT);
  emulate_vdu(x);
  emulate_vdu(y);
}

/*
** 'emulate_newline' skips to a new line on the screen.
*/
void emulate_newline(void) {
  emulate_vdu(CR);
  emulate_vdu(LF);
}

/*
** 'emulate_off' deals with the Basic 'OFF' statement which turns
** off the text cursor
*/
void emulate_off(void) {
  int32 n;
  emulate_vdu(VDU_COMMAND);
  emulate_vdu(1);
  emulate_vdu(0);
  for (n=1; n<=7; n++) emulate_vdu(0);
}

/*
** 'emulate_on' emulates the Basic 'ON' statement, which turns on
** the text cursor
*/
void emulate_on(void) {
  int32 n;
  emulate_vdu(VDU_COMMAND);
  emulate_vdu(1);
  emulate_vdu(1);
  for (n=1; n<=7; n++) emulate_vdu(0);
}

/*
** 'exec_tint' is called to handle the Basic 'TINT' statement which
** sets the 'tint' value for the current text or graphics foreground
** or background colour to 'tint'.
** 'Tint' has to be set to 0, 0x40, 0x80 or 0xC0, that is, the tint
** value occupies the most significant two bits of the one byte tint
** value. This code also allows it to be specified in the lower two
** bits as well (I can never remember where it goes)
*/
void emulate_tint(int32 action, int32 tint) {
  int32 n;
  emulate_vdu(VDU_COMMAND);		/* Use VDU 23,17 */
  emulate_vdu(17);
  emulate_vdu(action);	/* Says which colour to modify */
  if (tint<=MAXTINT) tint = tint<<TINTSHIFT;	/* Assume value is in the wrong place */
  emulate_vdu(tint);
  for (n=1; n<=7; n++) emulate_vdu(0);
}

/*
** 'emulate_gcol' deals with both forms of the Basic 'GCOL' statement,
** where is it used to either set the graphics colour or to define
** how the VDU drivers carry out graphics operations.
*/
void emulate_gcol(int32 action, int32 colour, int32 tint) {
  emulate_vdu(VDU_GRAPHCOL);
  emulate_vdu(action);
  emulate_vdu(colour);
  emulate_tint(colour < 128 ? TINT_FOREGRAPH : TINT_BACKGRAPH, tint);
}

/*
** emulate_gcolrgb - Called to deal with the 'GCOL <red>,<green>,
** <blue>' version of the GCOL statement. 'background' is set
** to true if the graphics background colour is to be changed
** otherwise the foreground colour is altered
*/
int emulate_gcolrgb(int32 action, int32 background, int32 red, int32 green, int32 blue) {
  int32 colnum = emulate_colourfn(red & 0xFF, green & 0xFF, blue & 0xFF);
  emulate_gcolnum(action, background, colnum);
  return(colnum);
}

/*
** emulate_gcolnum - Called to set the graphics foreground or
** background colour to the colour number 'colnum'. This code
** is a bit of a hack
*/
void emulate_gcolnum(int32 action, int32 background, int32 colnum) {
  if (background)
    graph_back_action = action;
  else {
    graph_fore_action = action;
  }
  set_graphics_colour(background, colnum);
}

/*
** 'emulate_colourtint' deals with the Basic 'COLOUR <colour> TINT' statement
*/
void emulate_colourtint(int32 colour, int32 tint) {
  emulate_vdu(VDU_TEXTCOL);
  emulate_vdu(colour);
  emulate_tint(colour<128 ? TINT_FORETEXT : TINT_BACKTEXT, tint);
}

/*
** 'emulate_mapcolour' handles the Basic 'COLOUR <colour>,<physical colour>'
** statement.
*/
void emulate_mapcolour(int32 colour, int32 physcolour) {
  emulate_vdu(VDU_LOGCOL);
  emulate_vdu(colour);
  emulate_vdu(physcolour);	/* Set logical logical colour to given physical colour */
  emulate_vdu(0);
  emulate_vdu(0);
  emulate_vdu(0);
}

/*
** 'emulate_setcolour' handles the Basic 'COLOUR <red>,<green>,<blue>'
** statement
*/
int32 emulate_setcolour(int32 background, int32 red, int32 green, int32 blue) {
  int32 colnum = emulate_colourfn(red & 0xFF, green & 0xFF, blue & 0xFF);
  set_text_colour(background, colnum);
  return(colnum);
}

/*
** emulate_setcolnum - Called to set the text forground or
** background colour to the colour number 'colnum'
*/
void emulate_setcolnum(int32 background, int32 colnum) {
  set_text_colour(background, colnum);
}

/*
** 'emulate_defcolour' handles the Basic 'COLOUR <colour>,<red>,<green>,<blue>'
** statement
*/
void emulate_defcolour(int32 colour, int32 red, int32 green, int32 blue) {
  emulate_vdu(VDU_LOGCOL);
  emulate_vdu(colour);
  emulate_vdu(16);	/* Set both flash palettes for logical colour to given colour */
  emulate_vdu(red);
  emulate_vdu(green);
  emulate_vdu(blue);
}

/*
** 'emulate_move' moves the graphics cursor to the absolute
** position (x,y) on the screen
*/
void emulate_move(int32 x, int32 y) {
  emulate_plot(DRAW_SOLIDLINE+MOVE_ABSOLUTE, x, y);
}

/*
** 'emulate_moveby' move the graphics cursor by the offsets 'x'
** and 'y' relative to its current position
*/
void emulate_moveby(int32 x, int32 y) {
  emulate_plot(DRAW_SOLIDLINE+MOVE_RELATIVE, x, y);
}

/*
** 'emulate_draw' draws a solid line from the current graphics
** cursor position to the absolute position (x,y) on the screen
*/
void emulate_draw(int32 x, int32 y) {
  emulate_plot(DRAW_SOLIDLINE+DRAW_ABSOLUTE, x, y);
}

/*
** 'emulate_drawby' draws a solid line from the current graphics
** cursor position to that at offsets 'x' and 'y' relative to that
** position
*/
void emulate_drawby(int32 x, int32 y) {
  emulate_plot(DRAW_SOLIDLINE+DRAW_RELATIVE, x, y);
}

/*
** 'emulate_line' draws a line from the absolute position (x1,y1)
** on the screen to (x2,y2)
*/
void emulate_line(int32 x1, int32 y1, int32 x2, int32 y2) {
  emulate_plot(DRAW_SOLIDLINE+MOVE_ABSOLUTE, x1, y1);
  emulate_plot(DRAW_SOLIDLINE+DRAW_ABSOLUTE, x2, y2);
}

/*
** 'emulate_point' plots a single point at the absolute position
** (x,y) on the screen
*/
void emulate_point(int32 x, int32 y) {
  emulate_plot(PLOT_POINT+DRAW_ABSOLUTE, x, y);
}

/*
** 'emulate_pointby' plots a single point at the offsets 'x' and
** 'y' from the current graphics position
*/
void emulate_pointby(int32 x, int32 y) {
  emulate_plot(PLOT_POINT+DRAW_RELATIVE, x, y);
}

/*
** 'emulate_ellipse' handles the Basic statement 'ELLIPSE'. This one is
** a little more complex than a straight call to a SWI as it plots the
** ellipse with the semi-major axis at any angle. However, as the graphics
** library used only supports drawing an ellipse whose semimajor axis is
** parallel to the X axis, values of angle other 0.0 radians are not
** supported by this version of the code. Angle!=0.0 could be supported
** under RISC OS if I knew the maths...
*/
void emulate_ellipse(int32 x, int32 y, int32 majorlen, int32 minorlen, float64 angle, boolean isfilled) {
  if (angle != 0.0) error(ERR_UNSUPPORTED);	/* Graphics library limitation */
  emulate_plot(DRAW_SOLIDLINE+MOVE_ABSOLUTE, x, y);	   /* Move to centre of ellipse */
  emulate_plot(DRAW_SOLIDLINE+MOVE_ABSOLUTE, x+majorlen, y);	/* Find a point on the circumference */
  if (isfilled)
    emulate_plot(FILL_ELLIPSE+DRAW_ABSOLUTE, x, y+minorlen);
  else {
    emulate_plot(PLOT_ELLIPSE+DRAW_ABSOLUTE, x, y+minorlen);
  }
}

void emulate_circle(int32 x, int32 y, int32 radius, boolean isfilled) {
  emulate_plot(DRAW_SOLIDLINE+MOVE_ABSOLUTE, x, y);	   /* Move to centre of circle */
  if (isfilled)
    emulate_plot(FILL_CIRCLE+DRAW_ABSOLUTE, x-radius, y);	/* Plot to a point on the circumference */
  else {
    emulate_plot(PLOT_CIRCLE+DRAW_ABSOLUTE, x-radius, y);
  }
}

/*
** 'emulate_drawrect' draws either an outline of a rectangle or a
** filled rectangle
*/
void emulate_drawrect(int32 x1, int32 y1, int32 width, int32 height, boolean isfilled) {
  emulate_plot(DRAW_SOLIDLINE+MOVE_ABSOLUTE, x1, y1);
  if (isfilled)
    emulate_plot(FILL_RECTANGLE+DRAW_RELATIVE, width, height);
  else {
    emulate_plot(DRAW_SOLIDLINE+DRAW_RELATIVE, width, 0);
    emulate_plot(DRAW_SOLIDLINE+DRAW_RELATIVE, 0, height);
    emulate_plot(DRAW_SOLIDLINE+DRAW_RELATIVE, -width, 0);
    emulate_plot(DRAW_SOLIDLINE+DRAW_RELATIVE, 0, -height);
  }
}

/*
** 'emulate_moverect' is called to either copy an area of the graphics screen
** from one place to another or to move it, clearing its old location to the
** current background colour
*/
void emulate_moverect(int32 x1, int32 y1, int32 width, int32 height, int32 x2, int32 y2, boolean ismove) {
  emulate_plot(DRAW_SOLIDLINE+MOVE_ABSOLUTE, x1, y1);
  emulate_plot(DRAW_SOLIDLINE+MOVE_RELATIVE, width, height);
  if (ismove)	/* Move the area just marked */
    emulate_plot(MOVE_RECTANGLE, x2, y2);
  else {
    emulate_plot(COPY_RECTANGLE, x2, y2);
  }
}

/*
** 'emulate_fill' flood-fills an area of the graphics screen in
** the current foreground colour starting at position (x,y) on the
** screen
*/
void emulate_fill(int32 x, int32 y) {
  emulate_plot(FLOOD_BACKGROUND+DRAW_ABSOLUTE, x, y);
}

/*
** 'emulate_fillby' flood-fills an area of the graphics screen in
** the current foreground colour starting at the position at offsets
** 'x' and 'y' relative to the current graphics cursor position
*/
void emulate_fillby(int32 x, int32 y) {
  emulate_plot(FLOOD_BACKGROUND+DRAW_RELATIVE, x, y);
}

/*
** 'emulate_origin' emulates the Basic statement 'ORIGIN' which
** sets the absolute location of the origin on the graphics screen
*/
void emulate_origin(int32 x, int32 y) {
  emulate_vdu(VDU_ORIGIN);
  emulate_vdu(x & BYTEMASK);
  emulate_vdu((x>>BYTESHIFT) & BYTEMASK);
  emulate_vdu(y & BYTEMASK);
  emulate_vdu((y>>BYTESHIFT) & BYTEMASK);
}

/*
** 'init_screen' is called to initialise the RISC OS VDU driver
** emulation code for the versions of this program that do not run
** under RISC OS. It returns 'TRUE' if initialisation was okay or
** 'FALSE' if it failed (in which case it is not safe for the
** interpreter to run)
*/
boolean init_screen(void) {
  static SDL_Surface *fontbuf, *v5fontbuf, *m7fontbuf;
  int flags = SDL_DOUBLEBUF | SDL_HWSURFACE;
  int p;

  if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_TIMER) < 0) {
    fprintf(stderr, "Unable to init SDL: %s\n", SDL_GetError());
    return FALSE;
  }

  reset_sysfont(0);
  if (basicvars.runflags.startfullscreen) flags |= SDL_FULLSCREEN;
  screen0 = SDL_SetVideoMode(640, 512, 32, flags); /* MODE 0 */
  if (!screen0) {
    fprintf(stderr, "Failed to open screen: %s\n", SDL_GetError());
    return FALSE;
  }
  for (p=0; p<4; p++) {
    SDL_FreeSurface(screenbank[p]);
    screenbank[p]=SDL_DisplayFormat(screen0);
  }
  modescreen = SDL_DisplayFormat(screen0);
  displaybank=0;
  writebank=0;
  screen1 = SDL_DisplayFormat(screen0);
  screen2 = SDL_DisplayFormat(screen0);
  screen2A = SDL_DisplayFormat(screen0);
  screen3 = SDL_DisplayFormat(screen0);
  screen3A = SDL_DisplayFormat(screen0);
  fontbuf = SDL_CreateRGBSurface(SDL_SWSURFACE,   XPPC,   YPPC, 32, 0xff000000, 0x00ff0000, 0x0000ff00, 0x000000ff);
  v5fontbuf = SDL_CreateRGBSurface(SDL_SWSURFACE,   XPPC,   YPPC, 32, 0xff000000, 0x00ff0000, 0x0000ff00, 0x000000ff);
  m7fontbuf = SDL_CreateRGBSurface(SDL_SWSURFACE, M7XPPC, M7YPPC, 32, 0xff000000, 0x00ff0000, 0x0000ff00, 0x000000ff);
  sdl_fontbuf = SDL_ConvertSurface(fontbuf, screen0->format, 0);  /* copy surface to get same format as main windows */
  sdl_v5fontbuf = SDL_ConvertSurface(v5fontbuf, screen0->format, 0);  /* copy surface to get same format as main windows */
  sdl_m7fontbuf = SDL_ConvertSurface(m7fontbuf, screen0->format, 0);  /* copy surface to get same format as main windows */
  SDL_SetColorKey(sdl_v5fontbuf, SDL_SRCCOLORKEY, 0);
  SDL_FreeSurface(fontbuf);
  SDL_FreeSurface(v5fontbuf);
  SDL_FreeSurface(m7fontbuf);

  vdunext = 0;
  vduneeded = 0;
  enable_print = FALSE;
  graphmode = TEXTMODE;         /* Say mode is capable of graphics output but currently set to text */
  xgupp = ygupp = 1;
  SDL_WM_SetCaption("Matrix Brandy Basic V Interpreter", "Matrix Brandy");
  SDL_EnableUNICODE(SDL_ENABLE);
  SDL_EnableKeyRepeat(SDL_DEFAULT_REPEAT_DELAY, SDL_DEFAULT_REPEAT_INTERVAL);
  if (basicvars.runflags.start_graphics) {
    setup_mode(0);
    switch_graphics();
  }
  else {
    setup_mode(46);	/* Mode 46 - 80 columns by 25 lines by 16 colours */
  }

  xor_mask = SDL_MapRGB(sdl_fontbuf->format, 0xff, 0xff, 0xff);

  font_rect.x = font_rect.y = 0;

  font_rect.w = place_rect.w = XPPC;
  font_rect.h = place_rect.h = YPPC;
  place_rect.x = place_rect.y = 0;
  scale_rect.x = scale_rect.y = 0;
  scale_rect.w = 1;
  scale_rect.h = 1;

  return TRUE;
}

/*
** 'end_screen' is called to tidy up the VDU emulation at the end
** of the run
*/
void end_screen(void) {
  SDL_EnableUNICODE(SDL_DISABLE);
  SDL_Quit();
}

static unsigned int teletextgraphic(unsigned int ch, unsigned int y) {
  unsigned int left, right, hmask, val;

  if (y > 19) return(0); /* out of range */
  val = 0;
  switch (M7XPPC) {
    case 12:
      left=0xFC00u;
      right=0x03F0u;
      hmask=0x79E0u;
      break;
    case 14:
      left=0xFE00u;
      right=0x01FCu;
      hmask=0x7CF8u;
      break;
    default:
      left=0xFF00u;
      right=0x00FFu;
      hmask=0x7E7Eu;
  }

/* Row sets - 0 to 6, 7 to 13, and 14 to 19 */
  if ((y >= 0) && (y <= 6)) {
    if (ch & 1) val=left; 
    if (ch & 2) val+=right;
  } else if ((y >= 7) && (y <= 13)) {
    if (ch & 4) val=left;
    if (ch & 8) val+=right;
  } else if ((y >= 14) && (y <= 19)) {
    if (ch & 16) val=left;
    if (ch & 64) val+=right;
  }
  if (mode7sepreal) {
    if (y == 0 || y == 6 || y == 7 || y == 13 || y==14 || y == 19) val = 0;
    val = val & hmask;
  }
  return(val);
}

void mode7renderline(int32 ypos) {
  int32 ch, l_text_physbackcol, l_text_backcol, l_text_physforecol, l_text_forecol, xt, yt;
  int32 y, yy, topx, topy, line, xch;
  int32 vdu141used = 0;
  
  if (!mode7bitmapupdate || (screenmode != 7)) return;
  /* Preserve values */
  l_text_physbackcol=text_physbackcol;
  l_text_backcol=text_backcol;
  l_text_physforecol=text_physforecol;
  l_text_forecol=text_forecol;
  xt=xtext;
  yt=ytext;

  text_physbackcol=text_backcol=0;
  text_physforecol=text_forecol=7;
  set_rgb();

  vdu141mode = 1;
  vdu141on=0;
  mode7highbit=0;
  mode7sepgrp=0;
  mode7conceal=0;
  mode7hold = 0;
  mode7flash=0;
  mode7prevchar=32;

  if (cursorstate == ONSCREEN) cursorstate = SUSPENDED;
  for (xtext=0; xtext<=39; xtext++) {
    ch=mode7frame[ypos][xtext];
    /* Check the Set At codes here */
    switch (ch) {
      case TELETEXT_FLASH_OFF:
	mode7flash=0;
	break;
      case TELETEXT_SIZE_NORMAL:
	vdu141on=0;
	break;
      case TELETEXT_CONCEAL:
	mode7conceal=1;
	break;
      case TELETEXT_GRAPHICS_CONTIGUOUS:
	mode7sepgrp=0;
	break;
      case TELETEXT_GRAPHICS_SEPARATE:
	mode7sepgrp=1;
	break;
      case TELETEXT_BACKGROUND_BLACK:
	text_physbackcol = text_backcol = 0;
	set_rgb();
	break;
      case TELETEXT_BACKGROUND_SET:
	text_physbackcol = text_backcol = text_physforecol;
	set_rgb();
	break;
      case TELETEXT_GRAPHICS_HOLD:
	mode7hold=1;
	break;
    }
    /* Now we write the character. Copied and optimised from write_char() above */
    topx = xtext*M7XPPC;
    topy = ypos*M7YPPC;
    place_rect.x = topx;
    place_rect.y = topy;
    SDL_FillRect(sdl_m7fontbuf, NULL, tb_colour);
    xch=ch;
    if (mode7hold && ((ch >= 128 && ch <= 140) || (ch >= 142 && ch <= 151 ) || (ch == 152 && mode7reveal) || (ch >= 153 && ch <= 159))) {
      ch=mode7prevchar;
    } else {
      if (mode7highbit) {
	ch = ch | 0x80;
	if (ch==223) ch=35;
	if ((ch >= 0xC0) && (ch <= 0xDF)) ch = ch & 0x7F;
	mode7sepreal=mode7sepgrp;
      } else {
        if (ch==163) ch=96;
	if (ch==223) ch=35;
	if (ch==224) ch=95;
	ch = ch & 0x7F;
	if (ch < 32) ch=32;
      }
    }
    for (y=0; y < M7YPPC; y++) {
      if (mode7conceal && !mode7reveal) {
	line=0;
      } else {
	if (vdu141on) {
	  yy=((y/2)+(M7YPPC*vdu141mode/2));
	  if ((ch >= 160 && ch <= 191) || (ch >= 224 && ch <= 255)) {
	    line = teletextgraphic(ch, yy);
	  } else if ((ch >= 128) && (ch <= 159)) line = 0;
	  else line = mode7font[ch-' '][yy];
	} else {
	  if (vdu141track[ypos] == 2) line = 0;
	    else {
	    if ((ch >= 160 && ch <= 191) || (ch >= 224 && ch <= 255)) {
	      line = teletextgraphic(ch, y);
	    } else if ((ch >= 128) && (ch <= 159)) line = 0;
	    else line = mode7font[ch-' '][y];
	  }
	}
	if (mode7highbit && ((ch >= 160 && ch <= 191) || (ch >= 224 && ch <= 255)))
	  mode7prevchar=ch;
      }
      if (line!=0) {
	if (line & 0x8000) *((Uint32*)sdl_m7fontbuf->pixels +  0 + y*M7XPPC) = tf_colour;
	if (line & 0x4000) *((Uint32*)sdl_m7fontbuf->pixels +  1 + y*M7XPPC) = tf_colour;
	if (line & 0x2000) *((Uint32*)sdl_m7fontbuf->pixels +  2 + y*M7XPPC) = tf_colour;
	if (line & 0x1000) *((Uint32*)sdl_m7fontbuf->pixels +  3 + y*M7XPPC) = tf_colour;
	if (line & 0x0800) *((Uint32*)sdl_m7fontbuf->pixels +  4 + y*M7XPPC) = tf_colour;
	if (line & 0x0400) *((Uint32*)sdl_m7fontbuf->pixels +  5 + y*M7XPPC) = tf_colour;
	if (line & 0x0200) *((Uint32*)sdl_m7fontbuf->pixels +  6 + y*M7XPPC) = tf_colour;
	if (line & 0x0100) *((Uint32*)sdl_m7fontbuf->pixels +  7 + y*M7XPPC) = tf_colour;
	if (line & 0x0080) *((Uint32*)sdl_m7fontbuf->pixels +  8 + y*M7XPPC) = tf_colour;
	if (line & 0x0040) *((Uint32*)sdl_m7fontbuf->pixels +  9 + y*M7XPPC) = tf_colour;
	if (line & 0x0020) *((Uint32*)sdl_m7fontbuf->pixels + 10 + y*M7XPPC) = tf_colour;
	if (line & 0x0010) *((Uint32*)sdl_m7fontbuf->pixels + 11 + y*M7XPPC) = tf_colour;
	if (line & 0x0008) *((Uint32*)sdl_m7fontbuf->pixels + 12 + y*M7XPPC) = tf_colour;
	if (line & 0x0004) *((Uint32*)sdl_m7fontbuf->pixels + 13 + y*M7XPPC) = tf_colour;
	if (line & 0x0002) *((Uint32*)sdl_m7fontbuf->pixels + 14 + y*M7XPPC) = tf_colour;
	if (line & 0x0001) *((Uint32*)sdl_m7fontbuf->pixels + 15 + y*M7XPPC) = tf_colour;
      }
    }
    if(!mode7bank || !mode7flash) SDL_BlitSurface(sdl_m7fontbuf, &font_rect, screen0, &place_rect);
    SDL_BlitSurface(sdl_m7fontbuf, &font_rect, screen2, &place_rect);
    if (mode7flash) SDL_FillRect(sdl_m7fontbuf, NULL, tb_colour);
    SDL_BlitSurface(sdl_m7fontbuf, &font_rect, screen3, &place_rect);
    ch=xch; /* restore value */
    /* And now handle the Set After codes */
    switch (ch) {
      case TELETEXT_ALPHA_BLACK:
        if (mode7black) {
	  mode7highbit=0;
	  mode7conceal=0;
	  mode7prevchar=32;
	  text_physforecol = text_forecol = 0;
	  set_rgb();
	}
	break;
      case TELETEXT_ALPHA_RED:
      case TELETEXT_ALPHA_GREEN:
      case TELETEXT_ALPHA_YELLOW:
      case TELETEXT_ALPHA_BLUE:
      case TELETEXT_ALPHA_MAGENTA:
      case TELETEXT_ALPHA_CYAN:
      case TELETEXT_ALPHA_WHITE:
	mode7highbit=0;
	mode7conceal=0;
	mode7prevchar=32;
	text_physforecol = text_forecol = (ch - 128);
	set_rgb();
	break;
      case TELETEXT_FLASH_ON:
	mode7flash=1;
	break;
      case TELETEXT_SIZE_DOUBLEHEIGHT:
	vdu141on = 1;
	vdu141used=1;
	if (vdu141track[ypos] < 2) {
	  vdu141track[ypos] = 1;
	  vdu141track[ypos+1]=2;
	  vdu141mode = 0;
	} else {
	  vdu141mode = 1;
	}
	break;
      case TELETEXT_GRAPHICS_BLACK:
	if (mode7black) {
	  mode7highbit=1;
	  mode7conceal=0;
	  text_physforecol = text_forecol = 0;
	  set_rgb();
	}
	break;
      case TELETEXT_GRAPHICS_RED:
      case TELETEXT_GRAPHICS_GREEN:
      case TELETEXT_GRAPHICS_YELLOW:
      case TELETEXT_GRAPHICS_BLUE:
      case TELETEXT_GRAPHICS_MAGENTA:
      case TELETEXT_GRAPHICS_CYAN:
      case TELETEXT_GRAPHICS_WHITE:
	mode7highbit=1;
	mode7conceal=0;
	text_physforecol = text_forecol = (ch - 144);
	set_rgb();
	 break;
      /* These two break the teletext spec, but matches the behaviour in the SAA5050 and RISC OS */
      case TELETEXT_BACKGROUND_BLACK:
      case TELETEXT_BACKGROUND_SET:
	mode7prevchar=32;
	break;
      case TELETEXT_GRAPHICS_RELEASE:
	mode7hold=0;
	break;
    }
  }
  do_sdl_updaterect(screen0, 0, topy, 640, M7YPPC);

  vdu141on=0;
  mode7highbit=0;
  mode7sepgrp=0;
  mode7sepreal=0;
  mode7conceal=0;
  mode7hold=0;
  mode7flash=0;
  text_physbackcol=l_text_physbackcol;
  text_backcol=l_text_backcol;
  text_physforecol=l_text_physforecol;
  text_forecol=l_text_forecol;
  set_rgb();
  xtext=xt;
  ytext=yt;

  /* Cascade VDU141 changes */
  if ((!vdu141used) && vdu141track[ypos]==1) vdu141track[ypos]=0;
  if ((ypos < 24) && vdu141track[ypos+1]) {
    if ((vdu141track[ypos] == 0) || (vdu141track[ypos] == 2)) vdu141track[ypos+1]=1;
    mode7renderline(ypos+1);
  }
}

void mode7renderscreen(void) {
  int32 ypos;
  Uint8 bmpstate=mode7bitmapupdate;
  
  if (screenmode != 7) return;
  
  mode7bitmapupdate=1;
  for (ypos=0; ypos < 26;ypos++) vdu141track[ypos]=0;
  for (ypos=0; ypos<=24; ypos++) mode7renderline(ypos);
  mode7bitmapupdate=bmpstate;
}

void trace_edge(int32 x1, int32 y1, int32 x2, int32 y2)
{
  int32 dx, dy, xf, yf, a, b, t, i;

  if (x1 == x2 && y1 == y2) return;

  if (x2 > x1) {
    dx = x2 - x1;
    xf = 1;
  }
  else {
    dx = x1 - x2;
    xf = -1;
  }

  if (y2 > y1) {
    dy = y2 - y1;
    yf = 1;
  }
  else {
    dy = y1 - y2;
    yf = -1;
  }

  if (dx > dy) {
    a = dy + dy;
    t = a - dx;
    b = t - dx;
    for (i = 0; i <= dx; i++) {
      if (x1 < geom_left[y1]) geom_left[y1] = x1;
      if (x1 > geom_right[y1]) geom_right[y1] = x1;
      x1 += xf;
      if (t < 0)
        t += a;
      else {
        t += b;
        y1 += yf;
      }
    }
  }
  else {
    a = dx + dx;
    t = a - dy;
    b = t - dy;
    for (i = 0; i <= dy; i++) {
      if (x1 < geom_left[y1]) geom_left[y1] = x1;
      if (x1 > geom_right[y1]) geom_right[y1] = x1;
      y1 += yf;
      if (t < 0)
        t += a;
      else {
        t += b;
        x1 += xf;
      }
    }
  }
}

/*
** Draw a horizontal line
*/
void draw_h_line(SDL_Surface *sr, int32 x1, int32 y, int32 x2, Uint32 col, Uint32 action) {
  int32 tt, i;
  if (x1 > x2) {
    tt = x1; x1 = x2; x2 = tt;
  }
  if ( y >= 0 && y < vscrheight ) {
    if (x1 < 0) x1 = 0;
    if (x1 >= vscrwidth) x1 = vscrwidth-1;
    if (x2 < 0) x2 = 0;
    if (x2 >= vscrwidth) x2 = vscrwidth-1;
    for (i = x1; i <= x2; i++)
      plot_pixel(sr, i + y*vscrwidth, col, action);
  }
}

/*
** Draw a filled polygon of n vertices
*/
void buff_convex_poly(SDL_Surface *sr, int32 n, int32 *x, int32 *y, Uint32 col, Uint32 action) {
  int32 i, iy;
  int32 low = MAX_YRES, high = 0;

  /* set highest and lowest points to visit */
  for (i = 0; i < n; i++) {
    if (y[i] > MAX_YRES)
      y[i] = high = MAX_YRES;
    else if (y[i] > high)
      high = y[i];
    if (y[i] < 0)
      y[i] = low = 0;
    else if (y[i] < low)
      low = y[i];
  }

  /* reset the minumum amount of the edge tables */
  for (iy = low; iy <= high; iy++) {
    geom_left[iy] = MAX_XRES + 1;
    geom_right[iy] = - 1;
  }

  /* define edges */
  trace_edge(x[n - 1], y[n - 1], x[0], y[0]);

  for (i = 0; i < n - 1; i++)
    trace_edge(x[i], y[i], x[i + 1], y[i + 1]);

  /* fill horizontal spans of pixels from geom_left[] to geom_right[] */
  for (iy = low; iy <= high; iy++)
    draw_h_line(sr, geom_left[iy], iy, geom_right[iy], col, action);
}

/*
** 'draw_line' draws an arbitary line in the graphics buffer 'sr'.
** clipping for x & y is implemented.
** Style is the bitmasked with 0x38 from the PLOT code:
** Bit 0x08: Don't plot end point
** Bit 0x10: Draw a dotted line, skipping every other point.
** Bit 0x20: Don't plot the start point.
*/
void draw_line(SDL_Surface *sr, int32 x1, int32 y1, int32 x2, int32 y2, Uint32 col, int32 style, Uint32 action) {
  int d, x, y, ax, ay, sx, sy, dx, dy, tt, skip=0;
  if (x1 > x2) {
    tt = x1; x1 = x2; x2 = tt;
    tt = y1; y1 = y2; y2 = tt;
  }
  dx = x2 - x1;
  ax = abs(dx) << 1;
  sx = ((dx < 0) ? -1 : 1);
  dy = y2 - y1;
  ay = abs(dy) << 1;
  sy = ((dy < 0) ? -1 : 1);

  x = x1;
  y = y1;
  if (style & 0x20) skip=1;

  if (ax > ay) {
    d = ay - (ax >> 1);
    while (x != x2) {
      if (skip) {
        skip=0;
      } else {
	if ((x >= 0) && (x < screenwidth) && (y >= 0) && (y < screenheight))
	  plot_pixel(sr, x + y*vscrwidth, col, action);
	if (style & 0x10) skip=1;
      }
      if (d >= 0) {
        y += sy;
        d -= ax;
      }
      x += sx;
      d += ay;
    }
  } else {
    d = ax - (ay >> 1);
    while (y != y2) {
      if (skip) {
        skip=0;
      } else {
	if ((x >= 0) && (x < screenwidth) && (y >= 0) && (y < screenheight))
	  plot_pixel(sr, x + y*vscrwidth, col, action);
	if (style & 0x10) skip=1;
      }
      if (d >= 0) {
        x += sx;
        d -= ay;
      }
      y += sy;
      d += ax;
    }
  }
  if ( ! (style & 0x08)) {
    if ((x >= 0) && (x < screenwidth) && (y >= 0) && (y < screenheight))
      plot_pixel(sr, x + y*vscrwidth, col, action);
  }
}

/*
** 'filled_triangle' draws a filled triangle in the graphics buffer 'sr'.
*/
void filled_triangle(SDL_Surface *sr, int32 x1, int32 y1, int32 x2, int32 y2,
                     int32 x3, int32 y3, Uint32 col, Uint32 action)
{
  int x[3], y[3];

  x[0]=x1;
  x[1]=x2;
  x[2]=x3;

  y[0]=y1;
  y[1]=y2;
  y[2]=y3;

  buff_convex_poly(sr, 3, x, y, col, action);
}

/*
** Draw an ellipse into a buffer
*/
void draw_ellipse(SDL_Surface *sr, int32 x0, int32 y0, int32 a, int32 b, Uint32 c, Uint32 action) {
  int32 x, y, y1, aa, bb, d, g, h;

  aa = a * a;
  bb = b * b;

  h = (FAST_4_DIV(aa)) - b * aa + bb;
  g = (FAST_4_DIV(9 * aa)) - (FAST_3_MUL(b * aa)) + bb;
  x = 0;
  y = b;

  while (g < 0) {
    if (((y0 - y) >= 0) && ((y0 - y) < vscrheight)) {
      if (((x0 - x) >= 0) && ((x0 - x) < vscrwidth)) plot_pixel(sr, x0 + (y0 - y)*vscrwidth - x, c, action);
      if (((x0 + x) >= 0) && ((x0 + x) < vscrwidth)) plot_pixel(sr, x0 + (y0 - y)*vscrwidth + x, c, action);
    }
    if (((y0 + y) >= 0) && ((y0 + y) < vscrheight)) {
      if (((x0 - x) >= 0) && ((x0 - x) < vscrwidth)) plot_pixel(sr, x0 + (y0 + y)*vscrwidth - x, c, action);
      if (((x0 + x) >= 0) && ((x0 + x) < vscrwidth)) plot_pixel(sr, x0 + (y0 + y)*vscrwidth + x, c, action);
    }

    if (h < 0) {
      d = ((FAST_2_MUL(x)) + 3) * bb;
      g += d;
    }
    else {
      d = ((FAST_2_MUL(x)) + 3) * bb - FAST_2_MUL((y - 1) * aa);
      g += (d + (FAST_2_MUL(aa)));
      --y;
    }

    h += d;
    ++x;
  }

  y1 = y;
  h = (FAST_4_DIV(bb)) - a * bb + aa;
  x = a;
  y = 0;

  while (y <= y1) {
    if (((y0 - y) >= 0) && ((y0 - y) < vscrheight)) {
      if (((x0 - x) >= 0) && ((x0 - x) < vscrwidth)) plot_pixel(sr, x0 + (y0 - y)*vscrwidth - x, c, action);
      if (((x0 + x) >= 0) && ((x0 + x) < vscrwidth)) plot_pixel(sr, x0 + (y0 - y)*vscrwidth + x, c, action);
    } 
    if (((y0 + y) >= 0) && ((y0 + y) < vscrheight)) {
      if (((x0 - x) >= 0) && ((x0 - x) < vscrwidth)) plot_pixel(sr, x0 + (y0 + y)*vscrwidth - x, c, action);
      if (((x0 + x) >= 0) && ((x0 + x) < vscrwidth)) plot_pixel(sr, x0 + (y0 + y)*vscrwidth + x, c, action);
    }

    if (h < 0)
      h += ((FAST_2_MUL(y)) + 3) * aa;
    else {
      h += (((FAST_2_MUL(y) + 3) * aa) - (FAST_2_MUL(x - 1) * bb));
      --x;
    }
    ++y;
  }
}

/*
** Draw a filled ellipse into a buffer
*/
void filled_ellipse(SDL_Surface *sr, int32 x0, int32 y0, int32 a, int32 b, Uint32 c, Uint32 action) {
  int32 x, y, y1, aa, bb, d, g, h;

  aa = a * a;
  bb = b * b;

  h = (FAST_4_DIV(aa)) - b * aa + bb;
  g = (FAST_4_DIV(9 * aa)) - (FAST_3_MUL(b * aa)) + bb;
  x = 0;
  y = b;

  while (g < 0) {
    draw_h_line(sr, x0 - x, y0 + y, x0 + x, c, action);
    draw_h_line(sr, x0 - x, y0 - y, x0 + x, c, action);

    if (h < 0) {
      d = ((FAST_2_MUL(x)) + 3) * bb;
      g += d;
    }
    else {
      d = ((FAST_2_MUL(x)) + 3) * bb - FAST_2_MUL((y - 1) * aa);
      g += (d + (FAST_2_MUL(aa)));
      --y;
    }

    h += d;
    ++x;
  }

  y1 = y;
  h = (FAST_4_DIV(bb)) - a * bb + aa;
  x = a;
  y = 0;

  while (y <= y1) {
    draw_h_line(sr, x0 - x, y0 + y, x0 + x, c, action);
    draw_h_line(sr, x0 - x, y0 - y, x0 + x, c, action);

    if (h < 0)
      h += ((FAST_2_MUL(y)) + 3) * aa;
    else {
      h += (((FAST_2_MUL(y) + 3) * aa) - (FAST_2_MUL(x - 1) * bb));
      --x;
    }
    ++y;
  }
}

void get_sdl_mouse(int32 values[]) {
  int x, y, xo, yo;
  Uint8 b, xb;

  SDL_PumpEvents();
  b=SDL_GetMouseState(&x, &y);
  xo = ((2*vscrwidth) - xgraphunits)/2;
  yo = ((2*vscrheight) - ygraphunits)/2;
  x=(x*2)-xo;
  if (x < 0) x = 0;
  if (x >= xgraphunits) x = (xgraphunits - 1);

  y=(2*(vscrheight-y))-yo;
  if (y < 0) y = 0;
  if (y >= ygraphunits) y = (ygraphunits - 1);

  /* Swap button bits around */
  xb = FAST_4_DIV(b & 4) + (b & 2) + FAST_4_MUL(b & 1);

  values[0]=x;
  values[1]=y;
  values[2]=xb;
  values[3]=mos_rdtime();
}

void sdl_mouse_onoff(int state) {
  if (state) SDL_ShowCursor(SDL_ENABLE);
  else SDL_ShowCursor(SDL_DISABLE);
}

void set_wintitle(char *title) {
  SDL_WM_SetCaption(title, title);
}

void fullscreenmode(int onoff) {
  Uint32 flags = screen0->flags;
  if (onoff == 1) {
    flags |= SDL_FULLSCREEN;
  } else if (onoff == 2) {
    flags ^= SDL_FULLSCREEN;
  } else {
    flags &= ~SDL_FULLSCREEN;
  }
  SDL_BlitSurface(screen0, NULL, screen1, NULL);
  SDL_SetVideoMode(screen0->w, screen0->h, screen0->format->BitsPerPixel, flags);
  SDL_BlitSurface(screen1, NULL, screen0, NULL);
  do_sdl_updaterect(screen0, 0, 0, 0, 0);
}

void setupnewmode(int32 mode, int32 xres, int32 yres, int32 cols, int32 mxscale, int32 myscale, int32 xeig, int32 yeig) {
  if ((mode < 64) || (mode > HIGHMODE)) {
    emulate_printf("Warning: *NewMode can only define modes in the range 64 to %d.\r\n", HIGHMODE);
    return;
  }
  if ((cols != 2) && (cols != 4) && (cols != 16) && (cols != 256) && (cols != COL24BIT)) {
    emulate_printf("Warning: Can only define modes with 2, 4, 16, 256 or 16777216 colours.\r\n");
    return;
  }
  if ((mxscale==0) || (myscale==0)) {
    emulate_printf("Warning: pixel scaling can't be zero.\r\n");
    return;
  }
  if ((xres < 8) || (yres < 8)) {
    emulate_printf("Warning: Display size can't be smaller than 8x8 pixels.\r\n");
    return;
  }
  modetable[mode].xres = xres;
  modetable[mode].yres = yres;
  modetable[mode].coldepth = cols;
  modetable[mode].xgraphunits = (xres * (1<<xeig) * mxscale);
  modetable[mode].ygraphunits = (yres * (1<<yeig) * myscale);
  modetable[mode].xtext = (xres / 8);
  modetable[mode].ytext = (yres / 8);
  modetable[mode].xscale = mxscale;
  modetable[mode].yscale = myscale;
  modetable[mode].graphics = TRUE;
}

void star_refresh(int flag) {
  if ((flag == 0) || (flag == 1) || (flag==2)) {
    autorefresh=flag;
  }
  if (flag & 1) {
    SDL_BlitSurface(screenbank[displaybank], NULL, screen0, NULL);
    SDL_Flip(screen0);
  }
}

int get_refreshmode(void) {
  return autorefresh;
}

int32 osbyte42(int x) {
  int fullscreen=0, ref=(x & 3), fsc=((x & 12) >> 2);
  int outx;
  
  if (screen0->flags & SDL_FULLSCREEN) fullscreen=8;
  if (x == 0) {
    outx = fullscreen + (autorefresh+1);
    return ((outx << 8) + 42);
  }
  if (x == 255) {
    star_refresh(1);
    osbyte112(1);
    osbyte113(1);
    emulate_vdu(6);
    return 0xFF2A;
  }
  /* Handle the lowest 2 bits - REFRESH state */
  if (ref) star_refresh(ref-1);
  /* Handle the next 2 bits - FULLSCREEN state */
  if (fsc) fullscreenmode(fsc-1);
  return((x << 8) + 42);
}

void osbyte112(int x) {
  /* OSBYTE 112 selects which bank of video memory is to be written to */
  if (screenmode == 7) return;
  if (x==0) x=1;
  if (x <= MAXBANKS) writebank=(x-1);
}

void osbyte113(int x) {
  /* OSBYTE 113 selects which bank of video memory is to be displayed */
  if (screenmode == 7) return;
  if (x==0) x=1;
  if (x <= MAXBANKS) displaybank=(x-1);
  SDL_BlitSurface(screenbank[displaybank], NULL, screen0, NULL);
  SDL_Flip(screen0);
}

int32 osbyte134_165(int32 a) {
  return ((ytext << 16) + (xtext << 8) + a);
}

int32 osbyte135() {
  if (screenmode == 7) {
    printf("Mode 7\n");
    return ((screenmode << 16) + (mode7frame[ytext][xtext] << 8) + 135);
  } else {
    return ((screenmode << 16) + 135);
  }
}

int32 osbyte250() {
  return (((displaybank+1) << 16) + ((writebank+1) << 8) + 250);
}

int32 osbyte251() {
  return (((displaybank+1) << 8) + 251);
}

void osword10(int32 x) {
  char *block;
  int32 offset, i;
  
  block=(char *)(basicvars.offbase+x);
  offset = block[0]-32;
  if (offset < 0) return;
  for (i=0; i<= 7; i++) block[i+1]=sysfont[offset][i];
}

void sdl_screensave(char *fname) {
  if (screenmode == 7) {
    if (SDL_SaveBMP(screen2, fname)) {
      error(ERR_CANTWRITE);
    }
  } else {
    SDL_BlitSurface(screenbank[displaybank], NULL, screen1, NULL);
    if (SDL_SaveBMP(screen1, fname)) {
      error(ERR_CANTWRITE);
    }
  }
}

void sdl_screenload(char *fname) {
  SDL_Surface *placeholder;
  
  placeholder=SDL_LoadBMP(fname);
  if(!placeholder) {
    error(ERR_CANTREAD);
  } else {
    SDL_BlitSurface(placeholder, NULL, screenbank[writebank], NULL);
    if (displaybank == writebank) {
      SDL_BlitSurface(placeholder, NULL, screen0, NULL);
      SDL_Flip(screen0);
    }
    SDL_FreeSurface(placeholder);
  }
}
