/* SPDX-License-Identifier: GPL-2.0 */
/* Copyright 2026 Vasyl Samoilov */
#ifndef PCCLOCK_H
#define PCCLOCK_H

/* t = year, month, day, hour, minute */
void pc_get_clock(int *t);
int pc_set_clock(const int *t);

#endif
