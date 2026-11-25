/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Copyright 2026 Vasyl Samoilov */
/*
 * The computer's own clock. DOS: INT 21h get/set date and time, and the
 * real-time clock with INT 1Ah AH=03h/05h so the setting survives a power
 * cycle. Host builds keep a set value in memory, for tests.
 */
#include <string.h>
#include <time.h>
#include "pcclock.h"
#ifdef __DOS__
#include <i86.h>
#include <dos.h>
#endif

#ifndef __DOS__
static int host_set;
static int host_t[5];
#endif

void pc_get_clock(int *t)
{
#ifdef __DOS__
	union REGS r;

	memset(&r, 0, sizeof(r));
	r.h.ah = 0x2A;
	int86(0x21, &r, &r);
	t[0] = r.w.cx;
	t[1] = r.h.dh;
	t[2] = r.h.dl;
	memset(&r, 0, sizeof(r));
	r.h.ah = 0x2C;
	int86(0x21, &r, &r);
	t[3] = r.h.ch;
	t[4] = r.h.cl;
#else
	time_t now = time(NULL);
	struct tm *tm = localtime(&now);

	if (host_set) {
		memcpy(t, host_t, sizeof(host_t));
		return;
	}
	t[0] = tm->tm_year + 1900;
	t[1] = tm->tm_mon + 1;
	t[2] = tm->tm_mday;
	t[3] = tm->tm_hour;
	t[4] = tm->tm_min;
#endif
}

#ifdef __DOS__
static unsigned char bcd(int v)
{
	return (unsigned char)(((v / 10) << 4) | (v % 10));
}
#endif

int pc_set_clock(const int *t)
{
#ifdef __DOS__
	union REGS r;

	memset(&r, 0, sizeof(r));
	r.h.ah = 0x2B;
	r.w.cx = (unsigned)t[0];
	r.h.dh = (unsigned char)t[1];
	r.h.dl = (unsigned char)t[2];
	int86(0x21, &r, &r);
	if (r.h.al)
		return -1;
	memset(&r, 0, sizeof(r));
	r.h.ah = 0x2D;
	r.h.ch = (unsigned char)t[3];
	r.h.cl = (unsigned char)t[4];
	int86(0x21, &r, &r);
	if (r.h.al)
		return -1;
	/* the real-time clock, in BCD */
	memset(&r, 0, sizeof(r));
	r.h.ah = 0x05;
	r.h.ch = bcd(t[0] / 100);
	r.h.cl = bcd(t[0] % 100);
	r.h.dh = bcd(t[1]);
	r.h.dl = bcd(t[2]);
	int86(0x1A, &r, &r);
	memset(&r, 0, sizeof(r));
	r.h.ah = 0x03;
	r.h.ch = bcd(t[3]);
	r.h.cl = bcd(t[4]);
	r.h.dh = 0;
	r.h.dl = 0;
	int86(0x1A, &r, &r);
	return 0;
#else
	memcpy(host_t, t, sizeof(host_t));
	host_set = 1;
	return 0;
#endif
}
