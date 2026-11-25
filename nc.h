/* SPDX-License-Identifier: GPL-2.0 */
/* Copyright 2026 Vasyl Samoilov */
/* Motorola NewsCard: mailbox commands and the message store (PROTOCOL.md). */
#ifndef NC_H
#define NC_H

#include "bus.h"

#define NC_CMD 0x0C00UL
#define NC_SEM 0x0FF0UL
#define NC_STAT 0x0FFDUL
#define NC_OWN 0x0FFEUL
#define NC_DOORBELL 0x0FFFUL
#define NC_RSP 0x1000UL
#define NC_DIR 0x1400UL
#define NC_DIR_N 16
#define NC_HEAP 0x1C00UL
#define NC_HEAP_END 0x20000UL
#define NC_FLASH 0x02000000UL
#define NC_NONE 0xFFFFFFFFUL

#define NC_SEM_HOST 0x44
#define NC_OWN_HOST 0xAA
#define NC_DOORBELL_RING 0xFF
#define NC_ACK 0x06
#define NC_NAK 0x15

#define NC_MAX_MSGS 1000
#define NC_MAX_BLOCKS 64
#define NC_MAX_TEXT (NC_MAX_BLOCKS * 54)

/* errors */
#define NC_OK 0
#define NC_ETIMEOUT (-1)
#define NC_EIO (-2)
#define NC_ENOTCARD (-3)
#define NC_EBUS (-4)
#define NC_ERANGE (-5)
#define NC_EBLANK (-6) /* card memory reads all 00 or all FF */
#define NC_NAK_BASE (-1000) /* NC_NAK_BASE - code */

struct nc_msg {
	u32 ptr;	/* record offset */
	u32 first;	/* first text block (raw pointer) */
	u32 text_end;	/* last text byte (raw pointer) */
	u16 blocks;
	u16 errors;
	u8 seq;
	u8 source;	/* slot * 8 + function */
	u8 flags;	/* bit 2 = read */
	u8 time[5];	/* YY MM DD hh mm */
};

#define NC_MSG_READ(m) (((m)->flags & 4) != 0)
#define NC_MSG_SLOT(m) ((m)->source >> 3)
#define NC_MSG_FUNC(m) ((m)->source & 7)

struct nc_status {
	int battery_ok;
	int total, unread;
	u8 raw[20];
	int sw;		/* 0x04 on, 0x02 silent, 0x01 off */
	int free_blocks;
	u8 clock[5];
	int alerts;	/* 1 on, 0 off, -1 unknown */
};

extern int nc_timeout_ticks;
extern int nc_event;	/* STAT bit 0 was seen set */

int nc_identify(char *vendor, char *product, int n);
int nc_cmd(u8 cmd, const u8 *payload, unsigned plen, u8 *reply, unsigned rlen);
int nc_locked_read(u32 off, u8 *buf, unsigned n);

int nc_battery(int *ok);
int nc_count(u8 cmd, int *n);
int nc_get_status(u8 *st20);
int nc_get_clock(u8 *t5);
int nc_set_clock(const u8 *t5);
int nc_read_config(unsigned addr, u8 *v);
int nc_write_config(unsigned addr, u8 value, u8 mask);
int nc_mark_read(const struct nc_msg *m);
int nc_delete(const struct nc_msg *m);
int nc_alerts(int *on);
int nc_set_alerts(int on);
int nc_status_all(struct nc_status *s);

/* all records, oldest first; returns the count or an error */
int nc_scan(struct nc_msg *msgs, int max);
/* message text, NUL-terminated; returns its length or an error */
int nc_text(const struct nc_msg *m, char *buf, unsigned max);
/* the start of the text (first block only, up to 54 bytes) */
int nc_preview(const struct nc_msg *m, char *buf, unsigned max);

int nc_year(u8 yy);
const char *nc_strerror(int err);
const char *nc_switch_name(int sw);

#endif
