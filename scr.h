/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Copyright 2026 Vasyl Samoilov */
/*
 * Text screen and keyboard. DOS: video memory (B800, or B000 in mode 7)
 * and the BIOS keyboard. Host builds: an in-memory screen, keys only from
 * a script. A script ("DOWN F4 y SHOT ESC") can drive both, for tests;
 * SHOT appends the screen as text to the shot file.
 */
#ifndef SCR_H
#define SCR_H

#include <stdio.h>

/* attributes: plain, inverse, bright, bright on inverse */
#define A_NORM 0x07
#define A_INV 0x70
#define A_BRIGHT 0x0F
#define A_INVB 0x7F

/* keys: ASCII, or BIOS scan code << 8 */
#define K_ENTER 0x0D
#define K_ESC 0x1B
#define K_BKSP 0x08
#define K_TAB 0x09
#define K_F(n) (0x3A00 + ((n) << 8)) /* F1..F10 */
#define K_UP 0x4800
#define K_DOWN 0x5000
#define K_LEFT 0x4B00
#define K_RIGHT 0x4D00
#define K_PGUP 0x4900
#define K_PGDN 0x5100
#define K_HOME 0x4700
#define K_END 0x4F00
#define K_INS 0x5200
#define K_DEL 0x5300
#define K_SCRIPT_END 0xFFFF /* the test script has no more keys */

extern int scr_w, scr_h;

int scr_init(const char *script, const char *shot);
void scr_done(void);
/* text at x,y, padded with blanks or cut to width (width < 0: as long as s) */
void scr_put(int x, int y, unsigned char attr, const char *s, int width);
void scr_fill(int x, int y, int w, int h, unsigned char attr, char c);
void scr_box(int x, int y, int w, int h, unsigned char attr, const char *title);
/* next key; 0 after "ticks" timer ticks without one (ticks < 0: wait) */
unsigned scr_key(int ticks);
void scr_beep(void);
/* copy what was drawn to the screen (scr_key does this) */
void scr_flush(void);
/* a key is waiting */
int scr_key_waiting(void);
void scr_shot(void);

#endif
