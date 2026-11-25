/* SPDX-License-Identifier: GPL-2.0 */
/* Copyright 2026 Vasyl Samoilov */
/* Shared between the command line (ncmain.c) and the full-screen program (ui.c). */
#ifndef APP_H
#define APP_H

#include <stdio.h>
#include "nc.h"

extern struct bus_opts opts;
extern long rics[2];
extern struct nc_msg msgs[NC_MAX_MSGS];
extern char text[NC_MAX_TEXT + 1];

const char *who(const struct nc_msg *m);
const char *when(const u8 *t);
/* message header and text (in text[]) as plain text */
void show(FILE *f, const struct nc_msg *m);
void not_ready_hint(void);
/* run a step with stdout going to a file */
int to_file(const char *name, int (*step)(void));
int step_probe(void);

/* full-screen program; keys = test script, shot = screen dump file */
int ui_run(const char *keys, const char *shot);

#endif
