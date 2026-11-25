/* SPDX-License-Identifier: GPL-2.0 */
/* Copyright 2026 Vasyl Samoilov */
/* Backend selection: the simulated card, or Socket Services (DOS builds). */
#include <string.h>
#include <time.h>
#include "nc.h"
#include "sim.h"
#ifdef __DOS__
#include <i86.h>
#include "ss.h"
#endif

static int use_sim;
static char sim_desc[100];

int bus_open(const struct bus_opts *o)
{
	if (o->sim_file) {
		use_sim = 1;
		strcpy(sim_desc, "simulated card: ");
		strncat(sim_desc, o->sim_file, sizeof(sim_desc) - 17);
		return sim_open(o->sim_file);
	}
	use_sim = 0;
#ifdef __DOS__
	return ss_open(o);
#else
	return NC_EBUS;
#endif
}

void bus_close(void)
{
	if (use_sim)
		sim_close();
#ifdef __DOS__
	else
		ss_close();
#endif
}

int bus_begin(u32 off)
{
#ifdef __DOS__
	if (!use_sim)
		return ss_begin(off);
#endif
	return sim_begin(off);
}

void bus_end(void)
{
#ifdef __DOS__
	if (!use_sim) {
		ss_end();
		return;
	}
#endif
	sim_end();
}

u8 bus_rd(u32 off)
{
#ifdef __DOS__
	if (!use_sim)
		return ss_rd(off);
#endif
	return sim_rd(off);
}

void bus_wr(u32 off, u8 v)
{
#ifdef __DOS__
	if (!use_sim) {
		ss_wr(off, v);
		return;
	}
#endif
	sim_wr(off, v);
}

u32 bus_ticks(void)
{
#ifdef __DOS__
	return *(volatile u32 __far *)MK_FP(0x40, 0x6C);
#else
	return (u32)(clock() * 182L / (10L * CLOCKS_PER_SEC));
#endif
}

void bus_idle(void)
{
#ifdef __DOS__
	delay(1);
#endif
}

int bus_card_status(void)
{
#ifdef __DOS__
	if (!use_sim)
		return ss_card_status();
#endif
	return -1;
}

const char *bus_last_error(void)
{
#ifdef __DOS__
	if (!use_sim)
		return ss_last_error();
#endif
	return "";
}

void bus_diag(FILE *f)
{
#ifdef __DOS__
	if (!use_sim) {
		ss_diag(f);
		return;
	}
#endif
	fprintf(f, "backend     %s\n", sim_desc);
}

const char *bus_describe(void)
{
#ifdef __DOS__
	if (!use_sim)
		return ss_describe();
#endif
	return sim_desc;
}
