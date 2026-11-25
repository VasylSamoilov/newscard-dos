/* SPDX-License-Identifier: GPL-2.0 */
/* Copyright 2026 Vasyl Samoilov */
/* Text screen and keyboard (scr.h). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include "scr.h"
#include "bus.h"

#ifdef __DOS__
#include <i86.h>
#include <dos.h>
#include <bios.h>
#include <malloc.h>
#define CELLS_FAR __far
void cpu_halt(void);
#pragma aux cpu_halt = "hlt";
#else
#define CELLS_FAR
#endif

int scr_w = 80, scr_h = 25;

/* drawing goes to cells; scr_flush() copies changed cells to the screen */
static unsigned short CELLS_FAR *cells;
#ifdef __DOS__
static unsigned short __far *front;
#endif
static const char *script;
static const char *shot_path;
#ifdef __DOS__
static unsigned short __far *video;
static unsigned short __far *saved;
static unsigned saved_cursor_shape, saved_cursor_pos;
#else
static unsigned short host_cells[132 * 60];
#endif

/* CP437 box characters */
#define BOX_H 0xC4
#define BOX_V 0xB3
#define BOX_TL 0xDA
#define BOX_TR 0xBF
#define BOX_BL 0xC0
#define BOX_BR 0xD9

static void cell(int x, int y, unsigned char attr, unsigned char c)
{
	if (x >= 0 && y >= 0 && x < scr_w && y < scr_h)
		cells[y * scr_w + x] = (unsigned short)((attr << 8) | c);
}

int scr_init(const char *keys, const char *shot)
{
	script = keys;
	shot_path = shot;
#ifdef __DOS__
	{
		unsigned char mode = *(unsigned char __far *)MK_FP(0x40, 0x49);
		unsigned char rows = *(unsigned char __far *)MK_FP(0x40, 0x84);
		union REGS r;
		unsigned n;

		scr_w = *(unsigned short __far *)MK_FP(0x40, 0x4A);
		scr_h = rows ? rows + 1 : 25;
		if (scr_w < 40 || scr_w > 132 || scr_h < 16 || scr_h > 60) {
			scr_w = 80;
			scr_h = 25;
		}
		video = (unsigned short __far *)MK_FP(mode == 7 ? 0xB000 : 0xB800, 0);
		n = (unsigned)(scr_w * scr_h);
		saved = (unsigned short __far *)_fmalloc(n * 2);
		if (saved)
			_fmemcpy(saved, video, n * 2);
		cells = (unsigned short __far *)_fmalloc(n * 2);
		front = (unsigned short __far *)_fmalloc(n * 2);
		if (!cells || !front) {
			/* no memory for the buffers: draw straight to the screen */
			cells = video;
			front = NULL;
		} else {
			_fmemcpy(front, video, n * 2);
		}
		memset(&r, 0, sizeof(r));
		r.h.ah = 0x03;
		int86(0x10, &r, &r);
		saved_cursor_shape = r.w.cx;
		saved_cursor_pos = r.w.dx;
		memset(&r, 0, sizeof(r));
		r.h.ah = 0x01;
		r.w.cx = 0x2000; /* hide */
		int86(0x10, &r, &r);
	}
#else
	cells = host_cells;
#endif
	scr_fill(0, 0, scr_w, scr_h, A_NORM, ' ');
	return 0;
}

void scr_done(void)
{
#ifdef __DOS__
	union REGS r;

	if (saved) {
		_fmemcpy(video, saved, (unsigned)(scr_w * scr_h) * 2);
		_ffree(saved);
		saved = NULL;
	}
	if (front) {
		_ffree(cells);
		_ffree(front);
		front = NULL;
	}
	memset(&r, 0, sizeof(r));
	r.h.ah = 0x01;
	r.w.cx = saved_cursor_shape;
	int86(0x10, &r, &r);
	memset(&r, 0, sizeof(r));
	r.h.ah = 0x02;
	r.w.dx = saved_cursor_pos;
	int86(0x10, &r, &r);
#endif
}

void scr_flush(void)
{
#ifdef __DOS__
	unsigned i, n = (unsigned)(scr_w * scr_h);

	if (!front)
		return;
	for (i = 0; i < n; i++)
		if (front[i] != cells[i])
			video[i] = front[i] = cells[i];
#endif
}

void scr_put(int x, int y, unsigned char attr, const char *s, int width)
{
	unsigned short CELLS_FAR *p;
	unsigned short a = (unsigned short)(attr << 8);
	int i;

	if (width < 0)
		width = (int)strlen(s);
	if (y < 0 || y >= scr_h)
		return;
	if (x < 0) {
		for (i = 0; i < -x && *s; i++)
			s++;
		width += x;
		x = 0;
	}
	if (x + width > scr_w)
		width = scr_w - x;
	p = cells + y * scr_w + x;
	for (i = 0; i < width; i++) {
		unsigned char c = (unsigned char)(*s ? *s++ : ' ');

		*p++ = (unsigned short)(a | (c < 0x20 ? ' ' : c));
	}
}

void scr_fill(int x, int y, int w, int h, unsigned char attr, char c)
{
	unsigned short v = (unsigned short)((attr << 8) | (unsigned char)c);
	int i, j;

	if (x < 0) {
		w += x;
		x = 0;
	}
	if (x + w > scr_w)
		w = scr_w - x;
	for (j = y < 0 ? 0 : y; j < y + h && j < scr_h; j++) {
		unsigned short CELLS_FAR *p = cells + j * scr_w + x;

		for (i = 0; i < w; i++)
			*p++ = v;
	}
}

void scr_box(int x, int y, int w, int h, unsigned char attr, const char *title)
{
	int i;

	scr_fill(x + 1, y + 1, w - 2, h - 2, attr, ' ');
	for (i = 1; i < w - 1; i++) {
		cell(x + i, y, attr, BOX_H);
		cell(x + i, y + h - 1, attr, BOX_H);
	}
	for (i = 1; i < h - 1; i++) {
		cell(x, y + i, attr, BOX_V);
		cell(x + w - 1, y + i, attr, BOX_V);
	}
	cell(x, y, attr, BOX_TL);
	cell(x + w - 1, y, attr, BOX_TR);
	cell(x, y + h - 1, attr, BOX_BL);
	cell(x + w - 1, y + h - 1, attr, BOX_BR);
	if (title && *title) {
		int n = (int)strlen(title);

		if (n > w - 4)
			n = w - 4;
		scr_put(x + (w - n - 2) / 2, y, attr, " ", 1);
		scr_put(x + (w - n) / 2, y, attr, title, n);
		scr_put(x + (w - n) / 2 + n, y, attr, " ", 1);
	}
}

void scr_shot(void)
{
	FILE *f;
	int x, y;

	if (!shot_path)
		return;
	scr_flush();
	f = strcmp(shot_path, "-") ? fopen(shot_path, "a") : stdout;
	if (!f)
		return;
	for (y = 0; y < scr_h; y++) {
		char line[133];
		int n = 0;

		for (x = 0; x < scr_w; x++) {
#ifdef __DOS__
			unsigned char c = (unsigned char)video[y * scr_w + x];
#else
			unsigned char c = (unsigned char)cells[y * scr_w + x];
#endif

			/* box characters as ASCII */
			if (c == BOX_H)
				c = '-';
			else if (c == BOX_V)
				c = '|';
			else if (c == BOX_TL || c == BOX_TR || c == BOX_BL || c == BOX_BR)
				c = '+';
			else if (c < 0x20 || c > 0x7E)
				c = '?';
			line[n++] = (char)c;
		}
		while (n && line[n - 1] == ' ')
			n--;
		line[n] = 0;
		fprintf(f, "%s\n", line);
	}
	fprintf(f, "========\n");
	if (f != stdout)
		fclose(f);
}

static const struct {
	const char *name;
	unsigned key;
} key_names[] = {
	{ "ENTER", K_ENTER }, { "ESC", K_ESC }, { "BKSP", K_BKSP }, { "TAB", K_TAB },
	{ "UP", K_UP }, { "DOWN", K_DOWN }, { "LEFT", K_LEFT }, { "RIGHT", K_RIGHT },
	{ "PGUP", K_PGUP }, { "PGDN", K_PGDN }, { "HOME", K_HOME }, { "END", K_END },
	{ "INS", K_INS }, { "DEL", K_DEL }, { "SPACE", ' ' },
	{ "F10", K_F(10) }, { "F1", K_F(1) }, { "F2", K_F(2) }, { "F3", K_F(3) },
	{ "F4", K_F(4) }, { "F5", K_F(5) }, { "F6", K_F(6) }, { "F7", K_F(7) },
	{ "F8", K_F(8) }, { "F9", K_F(9) }
};

/* next key from the script; 0 for a pause, K_SCRIPT_END at the end */
static unsigned script_key(void)
{
	char tok[16];
	unsigned i, n = 0;

	for (;;) {
		while (*script == ' ')
			script++;
		if (!*script)
			return K_SCRIPT_END;
		while (*script && *script != ' ' && n < sizeof(tok) - 1)
			tok[n++] = *script++;
		tok[n] = 0;
		if (!strcmp(tok, "SHOT")) {
			scr_shot();
			n = 0;
			continue;
		}
		if (!strcmp(tok, "WAIT"))
			return 0;
		if (n == 1)
			return (unsigned char)tok[0];
		for (i = 0; i < sizeof(key_names) / sizeof(key_names[0]); i++)
			if (!strcmp(tok, key_names[i].name))
				return key_names[i].key;
		/* a word: type it */
		script -= n - 1;
		return (unsigned char)tok[0];
	}
}

int scr_key_waiting(void)
{
	if (script)
		return 0; /* scripts draw after every key */
#ifdef __DOS__
	return _bios_keybrd(_KEYBRD_READY) != 0;
#else
	return 0;
#endif
}

unsigned scr_key(int ticks)
{
	scr_flush();
	if (script)
		return script_key();
#ifdef __DOS__
	{
		u32 start = bus_ticks();

		for (;;) {
			if (_bios_keybrd(_KEYBRD_READY)) {
				unsigned k = _bios_keybrd(_KEYBRD_READ);

				return (k & 0xFF) && (k & 0xFF) != 0xE0 ? (k & 0xFF) : (k & 0xFF00);
			}
			if (ticks >= 0 && (u32)(bus_ticks() - start) >= (u32)ticks)
				return 0;
			cpu_halt(); /* until the next interrupt */
		}
	}
#else
	(void)ticks;
	return K_SCRIPT_END;
#endif
}

void scr_beep(void)
{
#ifdef __DOS__
	union REGS r;

	memset(&r, 0, sizeof(r));
	r.h.ah = 0x0E;
	r.h.al = 0x07;
	int86(0x10, &r, &r);
#endif
}
