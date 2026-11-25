/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Copyright 2026 Vasyl Samoilov */
/*
 * Byte access to the card's memory. Two backends: Socket Services
 * (bus_ss.c) and a simulated card over a RAM image (bus_sim.c).
 *
 * Card offsets are 32-bit. Accesses happen only between bus_begin() and
 * bus_end(). bus_begin(off) makes card page 0 (0x0000-0x3FFF: mailbox,
 * reply buffer, directory) and the 16 KB page holding "off" reachable at
 * the same time. Keep a bracket short: no waiting inside it.
 */
#ifndef BUS_H
#define BUS_H

#include <stdio.h>

typedef unsigned long u32;
typedef unsigned short u16;
typedef unsigned char u8;

#define BUS_PAGE 0x4000UL

struct bus_opts {
	const char *sim_file;	/* simulated card over this RAM image */
	int socket;		/* Socket Services socket, 1-based */
	int win[2];		/* window numbers: [0] card page 0, [1] moving */
	unsigned seg[2];	/* host segments of the two windows */
	unsigned speed_ns;	/* access time */
	int attr;		/* window attribute byte, -1 = default */
	long ram_at;		/* card offset where the RAM appears, -1 = find out */
	int verbose;
};

int bus_open(const struct bus_opts *o);
void bus_close(void);
int bus_begin(u32 off);
void bus_end(void);
u8 bus_rd(u32 off);
void bus_wr(u32 off, u8 v);

/* 18.2 Hz ticks; wraps */
u32 bus_ticks(void);
void bus_idle(void);

/* Socket Services card status (GET CARD), -1 if unknown */
int bus_card_status(void);
const char *bus_describe(void);
/* error kept from the last bracket, "" if none */
const char *bus_last_error(void);
/* backend readings for probe; read-only */
void bus_diag(FILE *f);

#endif
