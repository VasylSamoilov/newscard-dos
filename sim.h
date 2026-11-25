/* SPDX-License-Identifier: GPL-2.0 */
/* Copyright 2026 Vasyl Samoilov */
#ifndef SIM_H
#define SIM_H

#include "bus.h"

int sim_open(const char *path);
void sim_close(void);
int sim_begin(u32 off);
void sim_end(void);
u8 sim_rd(u32 off);
void sim_wr(u32 off, u8 v);
void sim_set_switch(int v);
void sim_set_battery(int ok);

#endif
