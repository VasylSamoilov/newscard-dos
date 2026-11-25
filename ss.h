/* SPDX-License-Identifier: GPL-2.0 */
/* Copyright 2026 Vasyl Samoilov */
#ifndef SS_H
#define SS_H

#include <stdio.h>
#include "bus.h"

int ss_open(const struct bus_opts *o);
void ss_close(void);
int ss_begin(u32 off);
void ss_end(void);
u8 ss_rd(u32 off);
void ss_wr(u32 off, u8 v);
int ss_card_status(void);
const char *ss_describe(void);
const char *ss_last_error(void);
void ss_diag(FILE *f);

#endif
