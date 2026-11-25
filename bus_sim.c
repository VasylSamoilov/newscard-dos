/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Copyright 2026 Vasyl Samoilov */
/*
 * Simulated NewsCard over a 128 KB RAM image (card offsets 0-0x1FFFF).
 * A write of 0xFF to DOORBELL runs the request at 0xC00 and puts the
 * reply at 0x1000, as the card does. Changes are written back to the
 * image file by bus_close(). Flash reads return 0xFF.
 *
 * NEWSCARD_SIM_FAULT=item,item... makes the card misbehave, for tests:
 *   acq=N   the first N reads of SEM show the card holding the mailbox
 *   own=N   N times, OWN shows the card taking the mailbox during a read
 *   late=N  each request is taken only after OWN was read N times
 *   nak     every request is refused (code 5)
 *   dead    requests are never taken
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "nc.h"
#include "sim.h"

#ifdef __WATCOMC__
#include <malloc.h>
#define SIM_FAR __far
#define sim_alloc(n) _fmalloc(n)
#define sim_free(p) _ffree(p)
#else
#define SIM_FAR
#define sim_alloc(n) malloc(n)
#define sim_free(p) free(p)
#endif

#define CHUNK 0x8000UL
#define NCHUNK 4
#define RAM_SIZE 0x20000UL

static u8 SIM_FAR *ram[NCHUNK];
static const char *sim_path;
static int dirty;
static u8 clock5[5];
static u8 cfg[256];
static int sw = 0x04, battery = 1;
static int in_bracket;
static long f_acq, f_own, f_late, f_late_left;
static int f_nak, f_dead, pending, store_read;

static void parse_faults(const char *s)
{
	f_acq = f_own = f_late = 0;
	f_nak = f_dead = 0;
	while (s && *s) {
		if (!strncmp(s, "acq=", 4))
			f_acq = atol(s + 4);
		else if (!strncmp(s, "own=", 4))
			f_own = atol(s + 4);
		else if (!strncmp(s, "late=", 5))
			f_late = atol(s + 5);
		else if (!strncmp(s, "nak", 3))
			f_nak = 1;
		else if (!strncmp(s, "dead", 4))
			f_dead = 1;
		s = strchr(s, ',');
		if (s)
			s++;
	}
}

static u8 SIM_FAR *at(u32 off)
{
	off &= RAM_SIZE - 1;
	return ram[off / CHUNK] + (unsigned)(off % CHUNK);
}

static u8 get8(u32 off)
{
	return *at(off);
}

static void put8(u32 off, u8 v)
{
	*at(off) = v;
}

static u32 get32(u32 off)
{
	return ((u32)get8(off) << 24) | ((u32)get8(off + 1) << 16) |
	       ((u32)get8(off + 2) << 8) | get8(off + 3);
}

static void put32(u32 off, u32 v)
{
	put8(off, (u8)(v >> 24));
	put8(off + 1, (u8)(v >> 16));
	put8(off + 2, (u8)(v >> 8));
	put8(off + 3, (u8)v);
}

static int ok_ptr(u32 p)
{
	p &= 0x3FFFCUL;
	return p >= NC_HEAP && p + 64 <= NC_HEAP_END;
}

/* count records, unread records and used blocks */
static void walk(int *total, int *unread, long *used)
{
	int k, guard = 0;

	*total = *unread = 0;
	*used = 0;
	for (k = 0; k < NC_DIR_N; k++) {
		u32 p = get32(NC_DIR + (u32)k * 0x80);

		while (p != NC_NONE && ok_ptr(p) && guard++ < 2048) {
			u32 r = p & 0x3FFFCUL;

			(*total)++;
			if (!(get8(r + 38) & 4))
				(*unread)++;
			*used += 1 + ((get8(r + 40) << 8) | get8(r + 41));
			p = get32(r);
		}
	}
}

/* find a record by pointer and sequence number; returns its bucket or -1 */
static int find(u32 ptr, u8 seq, u32 *prev_link)
{
	int k, guard = 0;

	for (k = 0; k < NC_DIR_N; k++) {
		u32 link = NC_DIR + (u32)k * 0x80;
		u32 p = get32(link);

		while (p != NC_NONE && ok_ptr(p) && guard++ < 2048) {
			u32 r = p & 0x3FFFCUL;

			if (r == (ptr & 0x3FFFCUL) && get8(r + 9) == seq) {
				*prev_link = link;
				return k;
			}
			link = r;
			p = get32(r);
		}
	}
	return -1;
}

static void reply(u8 cmd, int status, const u8 *data, unsigned n)
{
	unsigned i, len = 1 + n;

	put8(NC_RSP, cmd);
	put8(NC_RSP + 1, (u8)(len >> 8));
	put8(NC_RSP + 2, (u8)len);
	put8(NC_RSP + 3, (u8)status);
	for (i = 0; i < n; i++)
		put8(NC_RSP + 4 + i, data[i]);
	put8(NC_RSP + 4 + n, 0xFF);
	put8(NC_RSP + 5 + n, 0xAA);
}

static void nak(u8 cmd, u8 code)
{
	reply(cmd, NC_NAK, &code, 1);
}

static void run_request(void)
{
	u8 cmd = get8(NC_CMD), p[16], d[20];
	unsigned plen = (get8(NC_CMD + 1) << 8) | get8(NC_CMD + 2), i;
	int total, unread;
	long used;

	for (i = 0; i < 16; i++)
		p[i] = get8(NC_CMD + 3 + i);
	if (f_nak) {
		nak(cmd, 5);
		put8(NC_OWN, 0x55);
		return;
	}
	switch (cmd) {
	case 0x01: /* mark read */
	case 0x02: { /* delete */
		u32 ptr = ((u32)p[0] << 24) | ((u32)p[1] << 16) | ((u32)p[2] << 8) | p[3];
		u32 link, r;

		if (plen < 5 || find(ptr, p[4], &link) < 0) {
			nak(cmd, 1);
			break;
		}
		r = ptr & 0x3FFFCUL;
		dirty = 1;
		if (cmd == 0x01) {
			put8(r + 38, (u8)(get8(r + 38) | 4));
		} else {
			u32 next = get32(r);

			put32(link, next);
			if (next != NC_NONE && ok_ptr(next))
				put32((next & 0x3FFFCUL) + 4, get32(r + 4));
		}
		reply(cmd, NC_ACK, p, 5);
		break;
	}
	case 0x11: { /* read config */
		unsigned a = (p[0] << 8) | p[1];

		if (a < 0x4A || a > 0xFF) {
			nak(cmd, 8);
			break;
		}
		d[0] = p[0];
		d[1] = p[1];
		d[2] = cfg[a];
		reply(cmd, NC_ACK, d, 3);
		break;
	}
	case 0x12: { /* write config, masked */
		unsigned a = (p[0] << 8) | p[1];

		if (a < 0x4A || a > 0xFF) {
			nak(cmd, 8);
			break;
		}
		cfg[a] = (u8)((cfg[a] & ~p[3]) | (p[2] & p[3]));
		reply(cmd, NC_ACK, p, 2);
		break;
	}
	case 0x13: /* notify: no reply */
		break;
	case 0x31: /* set clock */
		if (p[0] > 99) {
			nak(cmd, 10);
			break;
		}
		memcpy(clock5, p, 5);
		reply(cmd, NC_ACK, NULL, 0);
		break;
	case 0x32:
		reply(cmd, NC_ACK, clock5, 5);
		break;
	case 0x41:
		reply(cmd, NC_ACK, NULL, 0);
		break;
	case 0x44:
		d[0] = (u8)battery;
		reply(cmd, NC_ACK, d, 1);
		break;
	case 0x81:
	case 0x82:
		walk(&total, &unread, &used);
		i = cmd == 0x81 ? (unsigned)unread : (unsigned)total;
		d[0] = (u8)(i >> 8);
		d[1] = (u8)i;
		reply(cmd, NC_ACK, d, 2);
		break;
	case 0x83: {
		long free_blocks;

		walk(&total, &unread, &used);
		free_blocks = (long)((NC_HEAP_END - NC_HEAP) / 64) - used;
		memset(d, 0, 20);
		d[1] = (u8)sw;
		memcpy(d + 12, clock5, 5);
		d[18] = (u8)(free_blocks >> 8);
		d[19] = (u8)free_blocks;
		reply(cmd, NC_ACK, d, 20);
		break;
	}
	default:
		nak(cmd, 2);
	}
	/* the card has taken the request */
	put8(NC_OWN, 0x55);
}

int sim_open(const char *path)
{
	FILE *f;
	unsigned i;
	time_t t;
	struct tm *tm;

	for (i = 0; i < NCHUNK; i++) {
		ram[i] = (u8 SIM_FAR *)sim_alloc((size_t)CHUNK);
		if (!ram[i])
			return NC_EBUS;
	}
	f = fopen(path, "rb");
	if (!f)
		return NC_EBUS;
	for (i = 0; i < NCHUNK; i++) {
		static u8 buf[512];
		unsigned o;

		for (o = 0; o < CHUNK; o += sizeof(buf)) {
			unsigned j;

			if (fread(buf, 1, sizeof(buf), f) != sizeof(buf)) {
				fclose(f);
				return NC_EBUS;
			}
			for (j = 0; j < sizeof(buf); j++)
				ram[i][o + j] = buf[j];
		}
	}
	fclose(f);
	sim_path = path;
	memset(cfg, 0, sizeof(cfg));
	cfg[0xD6] = 0xCF;
	cfg[0xE7] = 0x0F;
	t = time(NULL);
	tm = localtime(&t);
	clock5[0] = (u8)(tm->tm_year % 100);
	clock5[1] = (u8)(tm->tm_mon + 1);
	clock5[2] = (u8)tm->tm_mday;
	clock5[3] = (u8)tm->tm_hour;
	clock5[4] = (u8)tm->tm_min;
	put8(NC_OWN, 0x55);
	dirty = 0;
	pending = 0;
	parse_faults(getenv("NEWSCARD_SIM_FAULT"));
	return NC_OK;
}

void sim_close(void)
{
	unsigned i;

	if (dirty && sim_path) {
		FILE *f = fopen(sim_path, "wb");

		if (f) {
			for (i = 0; i < NCHUNK; i++) {
				static u8 buf[512];
				unsigned o;

				for (o = 0; o < CHUNK; o += sizeof(buf)) {
					unsigned j;

					for (j = 0; j < sizeof(buf); j++)
						buf[j] = ram[i][o + j];
					fwrite(buf, 1, sizeof(buf), f);
				}
			}
			fclose(f);
		}
	}
	for (i = 0; i < NCHUNK; i++) {
		if (ram[i])
			sim_free(ram[i]);
		ram[i] = NULL;
	}
	sim_path = NULL;
}

int sim_begin(u32 off)
{
	(void)off;
	in_bracket = 1;
	return NC_OK;
}

void sim_end(void)
{
	in_bracket = 0;
	store_read = 0;
}

u8 sim_rd(u32 off)
{
	if (off >= NC_FLASH)
		return 0xFF;
	off &= RAM_SIZE - 1;
	if (off == NC_SEM && f_acq > 0) {
		f_acq--;
		return 0x00;
	}
	if (off == NC_OWN) {
		if (pending) {
			if (f_dead || f_late_left-- > 0)
				return get8(NC_OWN);
			pending = 0;
			run_request();
		}
		if (store_read && f_own > 0) {
			f_own--;
			return 0x55;
		}
	}
	if (off >= NC_DIR)
		store_read = 1;
	return get8(off);
}

void sim_wr(u32 off, u8 v)
{
	if (off >= NC_FLASH)
		return;
	off &= RAM_SIZE - 1;
	put8(off, v);
	if (off == NC_DOORBELL && v == NC_DOORBELL_RING) {
		if (f_dead || f_late) {
			pending = 1;
			f_late_left = f_late;
		} else {
			run_request();
		}
	}
}

void sim_set_switch(int v)
{
	sw = v;
}

void sim_set_battery(int ok)
{
	battery = ok;
}
