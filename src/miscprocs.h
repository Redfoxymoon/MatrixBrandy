/*
** This file is part of the Matrix Brandy Basic VI Interpreter.
** Copyright (C) 2000-2014 David Daniels
** Copyright (C) 2018-2021 Michael McConnell and contributors
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
**	Miscellaneous functions
*/

#ifndef __miscprocs_h
#define __miscprocs_h
#include <stdio.h>

#ifdef USE_SDL
#include <SDL.h>
#endif

#include "common.h"
#include "basicdefs.h"

#ifdef TARGET_RISCOS
extern int64 llabs(int64);
#endif
extern boolean read_line(char [], int32);
extern boolean amend_line(char [], int32);
extern boolean isidstart(char);
extern boolean isidchar(char);
extern boolean isident(byte);
extern int32 get_integer(size_t);
extern int64 get_int64(size_t);
extern float64 get_float(size_t);
extern void store_integer(size_t, int32);
extern void store_int64(size_t, int64);
extern void store_float(size_t, float64);
#ifdef USE_SDL
extern size_t m7offset(size_t);
#else
#define m7offset(p) (p)
#endif
extern char *skip_blanks(char *);
extern byte *skip(byte *);
extern char *tocstring(char *, int32);
extern byte *find_line(int32);
extern byte *find_linestart(byte *);
extern library *find_library(byte *);
extern void show_byte(size_t, size_t);
extern void show_word(size_t, size_t);
extern void save_current(void);
extern void restore_current(void);
extern FILE *secure_tmpnam(char []);
extern int32 TOINT(float64);
extern int32 INT64TO32(int64);
extern int64 TOINT64(float64);
extern void set_fpu(void);
extern void decimaltocomma(char *, int32);
#ifdef USE_SDL
extern Uint8 mode7frame[25][40];
#endif
#define ISIDSTART(ch) (isalpha(ch) || ch=='_' || ch=='`')
#define ISIDCHAR(ch) (isalnum(ch) || ch=='_' || ch=='`')

#endif
