/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Copyright 2026 Vasyl Samoilov */
/* Motorola NewsCard: mailbox handshake, commands, message store. */
#include <stdlib.h>
#include <string.h>
#include "nc.h"

int nc_timeout_ticks = 364; /* 20 s */
int nc_event;

/*
 * Tries per bracket, small enough that interrupts stay off for well under
 * one timer tick; between brackets the caller waits.
 */
#define ACQUIRE_TRIES 500

static u32 be32(const u8 *p)
{
	return ((u32)p[0] << 24) | ((u32)p[1] << 16) | ((u32)p[2] << 8) | p[3];
}

static u16 be16(const u8 *p)
{
	return (u16)((p[0] << 8) | p[1]);
}

static int expired(u32 start)
{
	return (u32)(bus_ticks() - start) > (u32)nc_timeout_ticks;
}

static int valid_ptr(u32 p)
{
	p &= 0x3FFFCUL;
	return p >= NC_HEAP && p + 64 <= NC_HEAP_END;
}

/* Inside a bracket: SEM = 0x44 and OWN = 0xAA, both read back. */
static int acquire(void)
{
	int i;

	for (i = 0; i < ACQUIRE_TRIES; i++) {
		bus_wr(NC_SEM, NC_SEM_HOST);
		bus_wr(NC_OWN, NC_OWN_HOST);
		if (bus_rd(NC_SEM) == NC_SEM_HOST && bus_rd(NC_OWN) == NC_OWN_HOST)
			return 1;
	}
	return 0;
}

/* Inside a bracket, holding the mailbox: clear STAT bit 0 (new page). */
static void clear_status(void)
{
	if (bus_rd(NC_STAT) & 1) {
		bus_wr(NC_STAT, 0);
		nc_event = 1;
	}
}

/*
 * The card's CIS strings at common memory 0x00-0x7F must name the
 * NewsCard. Read-only; called before anything is written to the card.
 */
int nc_identify(char *vendor, char *product, int n)
{
	static const char v[] = "Motorola, Inc.";
	static const char p[] = "NewsCard Data Receiver";
	u8 cis[0x81];
	int i, hv = 0, hp = 0;

	if (bus_begin(0))
		return NC_EBUS;
	for (i = 0; i < 0x80; i++)
		cis[i] = bus_rd((u32)i);
	bus_end();
	cis[0x80] = 0;
	for (i = 1; i < 0x80 && cis[i] == cis[0]; i++)
		;
	if (i == 0x80 && (cis[0] == 0x00 || cis[0] == 0xFF))
		return NC_EBLANK;
	for (i = 0; i + (int)sizeof(p) <= 0x81; i++) {
		if (!memcmp(cis + i, v, sizeof(v)))
			hv = 1;
		if (!memcmp(cis + i, p, sizeof(p)))
			hp = 1;
	}
	if (vendor && product && n > 0) {
		/* the VERS_1 strings follow the tuple code 0x15 */
		vendor[0] = product[0] = 0;
		for (i = 0; i + 4 < 0x80; i++) {
			if (cis[i] == 0x15 && cis[i + 2] == 0x04) {
				int j = i + 4, k = 0;

				while (j < 0x80 && cis[j] && k < n - 1)
					vendor[k++] = (char)cis[j++];
				vendor[k] = 0;
				j++;
				k = 0;
				while (j < 0x80 && cis[j] && k < n - 1)
					product[k++] = (char)cis[j++];
				product[k] = 0;
				break;
			}
		}
	}
	return hv && hp ? NC_OK : NC_ENOTCARD;
}

/* Write the request (cmd, lenHi, lenLo, payload) and ring the doorbell. */
static int send(const u8 *pkt, unsigned n)
{
	u32 start = bus_ticks();
	unsigned i;
	int ok;
	u8 own;

	for (;;) {
		if (bus_begin(0))
			return NC_EBUS;
		ok = acquire();
		if (ok) {
			clear_status();
			for (i = 0; i < n; i++)
				bus_wr(NC_CMD + i, pkt[i]);
			bus_wr(NC_CMD + n, 0xFF);
			bus_wr(NC_CMD + n + 1, 0xAA);
			ok = bus_rd(NC_CMD) == pkt[0] && bus_rd(NC_OWN) == NC_OWN_HOST;
			if (ok)
				bus_wr(NC_DOORBELL, NC_DOORBELL_RING);
		}
		bus_end();
		if (ok)
			break;
		if (expired(start))
			return NC_ETIMEOUT;
		bus_idle();
	}
	/* the card changes OWN when it takes the request */
	for (;;) {
		if (bus_begin(0))
			return NC_EBUS;
		own = bus_rd(NC_OWN);
		bus_end();
		if (own != NC_OWN_HOST)
			return NC_OK;
		if (expired(start))
			return NC_ETIMEOUT;
		bus_idle();
	}
}

static int receive(u8 cmd, u8 *buf, unsigned n)
{
	u32 start = bus_ticks();
	unsigned i;
	int ok;

	for (;;) {
		if (bus_begin(0))
			return NC_EBUS;
		ok = acquire();
		if (ok) {
			clear_status();
			for (i = 0; i < n; i++)
				buf[i] = bus_rd(NC_RSP + i);
			ok = buf[0] == cmd && bus_rd(NC_OWN) == NC_OWN_HOST;
		}
		bus_end();
		if (ok)
			break;
		if (expired(start))
			return NC_ETIMEOUT;
		bus_idle();
	}
	if (n >= 5 && buf[3] == NC_NAK)
		return NC_NAK_BASE - buf[4];
	return NC_OK;
}

/* Read card memory while holding the mailbox; n must not cross a 16 KB page. */
int nc_locked_read(u32 off, u8 *buf, unsigned n)
{
	u32 start = bus_ticks();
	unsigned i;
	int ok;

	if (n == 0 || (off & ~(BUS_PAGE - 1)) != ((off + n - 1) & ~(BUS_PAGE - 1)))
		return NC_ERANGE;
	for (;;) {
		if (bus_begin(off))
			return NC_EBUS;
		ok = acquire();
		if (ok) {
			clear_status();
			for (i = 0; i < n; i++)
				buf[i] = bus_rd(off + i);
			ok = bus_rd(NC_OWN) == NC_OWN_HOST;
		}
		bus_end();
		if (ok)
			return NC_OK;
		if (expired(start))
			return NC_ETIMEOUT;
		bus_idle();
	}
}

int nc_cmd(u8 cmd, const u8 *payload, unsigned plen, u8 *reply, unsigned rlen)
{
	u8 pkt[3 + 16];
	int r;

	if (plen > 16)
		return NC_ERANGE;
	pkt[0] = cmd;
	pkt[1] = (u8)(plen >> 8);
	pkt[2] = (u8)plen;
	if (plen)
		memcpy(pkt + 3, payload, plen);
	r = send(pkt, 3 + plen);
	if (r == NC_OK && rlen)
		r = receive(cmd, reply, rlen);
	return r;
}

/* ---- commands; reply = cmd, lenHi, lenLo, status, data ---- */

int nc_battery(int *ok)
{
	u8 r[5];
	int e = nc_cmd(0x44, NULL, 0, r, sizeof(r));

	if (e == NC_OK)
		*ok = r[4] != 0;
	return e;
}

int nc_count(u8 cmd, int *n)
{
	u8 r[7];
	int e = nc_cmd(cmd, NULL, 0, r, sizeof(r));

	if (e == NC_OK)
		*n = be16(r + 4);
	return e;
}

int nc_get_status(u8 *st20)
{
	u8 r[24];
	int e = nc_cmd(0x83, NULL, 0, r, sizeof(r));

	if (e == NC_OK)
		memcpy(st20, r + 4, 20);
	return e;
}

int nc_get_clock(u8 *t5)
{
	u8 r[9];
	int e = nc_cmd(0x32, NULL, 0, r, sizeof(r));

	if (e == NC_OK)
		memcpy(t5, r + 4, 5);
	return e;
}

int nc_set_clock(const u8 *t5)
{
	u8 r[5];

	if (t5[0] > 99)
		return NC_ERANGE;
	return nc_cmd(0x31, t5, 5, r, sizeof(r));
}

int nc_read_config(unsigned addr, u8 *v)
{
	u8 p[2], r[7];
	int e;

	p[0] = (u8)(addr >> 8);
	p[1] = (u8)addr;
	e = nc_cmd(0x11, p, 2, r, sizeof(r));
	if (e == NC_OK)
		*v = r[6];
	return e;
}

int nc_write_config(unsigned addr, u8 value, u8 mask)
{
	u8 p[4], r[5];

	p[0] = (u8)(addr >> 8);
	p[1] = (u8)addr;
	p[2] = value;
	p[3] = mask;
	return nc_cmd(0x12, p, 4, r, sizeof(r));
}

static void put_ptr_seq(u8 *p, const struct nc_msg *m)
{
	p[0] = (u8)(m->ptr >> 24);
	p[1] = (u8)(m->ptr >> 16);
	p[2] = (u8)(m->ptr >> 8);
	p[3] = (u8)m->ptr;
	p[4] = m->seq;
}

int nc_mark_read(const struct nc_msg *m)
{
	u8 p[5], r[5];

	put_ptr_seq(p, m);
	return nc_cmd(0x01, p, 5, r, sizeof(r));
}

int nc_delete(const struct nc_msg *m)
{
	u8 p[8], r[5];

	put_ptr_seq(p, m);
	p[5] = p[6] = p[7] = 0;
	return nc_cmd(0x02, p, 8, r, sizeof(r));
}

/* alerts: config byte 0xD6, bits 0xC0 */
int nc_alerts(int *on)
{
	u8 v;
	int e = nc_read_config(0xD6, &v);

	if (e == NC_OK)
		*on = (v & 0xC0) != 0;
	return e;
}

int nc_set_alerts(int on)
{
	return nc_write_config(0xD6, on ? 0xC0 : 0x00, 0xC0);
}

int nc_status_all(struct nc_status *s)
{
	int e;

	memset(s, 0, sizeof(*s));
	s->alerts = -1;
	e = nc_battery(&s->battery_ok);
	if (!e)
		e = nc_count(0x82, &s->total);
	if (!e)
		e = nc_count(0x81, &s->unread);
	if (!e)
		e = nc_get_status(s->raw);
	if (!e) {
		s->sw = s->raw[1];
		s->free_blocks = be16(s->raw + 18);
		e = nc_get_clock(s->clock);
	}
	if (!e && nc_alerts(&s->alerts))
		s->alerts = -1;
	return e;
}

/* ---- message store ---- */

static int msg_cmp(const void *a, const void *b)
{
	const struct nc_msg *x = (const struct nc_msg *)a;
	const struct nc_msg *y = (const struct nc_msg *)b;
	int c = nc_year(x->time[0]) - nc_year(y->time[0]);

	if (!c)
		c = memcmp(x->time + 1, y->time + 1, 4);
	if (c)
		return c;
	return (int)x->seq - (int)y->seq;
}

int nc_scan(struct nc_msg *msgs, int max)
{
	u8 head[4], raw[64];
	int k, n = 0, guard = 0, e;
	u32 p;

	for (k = 0; k < NC_DIR_N; k++) {
		e = nc_locked_read(NC_DIR + (u32)k * 0x80, head, 4);
		if (e)
			return e;
		p = be32(head);
		while (p != NC_NONE && valid_ptr(p) && guard++ < 2048) {
			struct nc_msg *m;
			u32 ptr = p & 0x3FFFCUL;

			e = nc_locked_read(ptr, raw, 64);
			if (e)
				return e;
			if (n < max) {
				m = &msgs[n++];
				m->ptr = ptr;
				m->seq = raw[9];
				m->errors = be16(raw + 10);
				m->first = be32(raw + 20);
				m->text_end = be32(raw + 24);
				m->source = raw[36];
				m->flags = raw[38];
				m->blocks = be16(raw + 40);
				memcpy(m->time, raw + 45, 5);
			}
			p = be32(raw);
		}
	}
	qsort(msgs, n, sizeof(*msgs), msg_cmp);
	return n;
}

int nc_text(const struct nc_msg *m, char *buf, unsigned max)
{
	u8 blk[64];
	u32 p = m->first, last = 0;
	unsigned nblk = m->blocks, i, len = 0, j;
	long want;
	int e;

	if (nblk > NC_MAX_BLOCKS)
		nblk = NC_MAX_BLOCKS;
	for (i = 0; i < nblk && p != NC_NONE && valid_ptr(p); i++) {
		e = nc_locked_read(p & 0x3FFFCUL, blk, 64);
		if (e)
			return e;
		if (i == 0)
			last = be32(blk + 4);
		for (j = 0; j < 54 && len + 1 < max; j++)
			buf[len++] = (char)blk[10 + j];
		p = be32(blk);
	}
	/* raw (unmasked) pointers; text_end points at the last text byte */
	want = 54L * nblk - ((long)(last & 0xFFFFFFUL) + 64 - (long)(m->text_end & 0xFFFFFFUL)) + 1;
	if (want >= 0 && (unsigned long)want <= len)
		len = (unsigned)want;
	while (len && buf[len - 1] == 0)
		len--;
	buf[len] = 0;
	return (int)len;
}

int nc_preview(const struct nc_msg *m, char *buf, unsigned max)
{
	u8 blk[64];
	unsigned j, len = 0;
	int e;

	buf[0] = 0;
	if (!m->blocks || m->first == NC_NONE || !valid_ptr(m->first))
		return 0;
	e = nc_locked_read(m->first & 0x3FFFCUL, blk, 64);
	if (e)
		return e;
	for (j = 0; j < 54 && len + 1 < max; j++)
		buf[len++] = (char)blk[10 + j];
	if (m->blocks == 1) {
		/* text_end points at the last text byte of the only block */
		long want = (long)(m->text_end & 0xFFFFFFUL) - (long)((m->first & 0xFFFFFFUL) + 10) + 1;

		if (want >= 0 && (unsigned long)want < len)
			len = (unsigned)want;
	}
	while (len && buf[len - 1] == 0)
		len--;
	buf[len] = 0;
	return (int)len;
}

int nc_year(u8 yy)
{
	return yy >= 93 ? 1900 + yy : 2000 + yy;
}

const char *nc_switch_name(int sw)
{
	switch (sw) {
	case 0x04:
		return "on";
	case 0x02:
		return "silent";
	case 0x01:
		return "off";
	}
	return "unknown";
}

const char *nc_strerror(int err)
{
	static char buf[40];

	switch (err) {
	case NC_OK:
		return "OK";
	case NC_ETIMEOUT:
		return "the card does not answer (timeout)";
	case NC_EIO:
		return "I/O error";
	case NC_ENOTCARD:
		return "the card in the slot is not a NewsCard";
	case NC_EBUS:
		return "cannot map the card memory";
	case NC_ERANGE:
		return "value out of range";
	case NC_EBLANK:
		return "the card memory reads blank (no card, or the window is not mapped)";
	}
	if (err <= NC_NAK_BASE) {
		strcpy(buf, "the card refused the command, code ");
		buf[35] = (char)('0' + (NC_NAK_BASE - err) / 10 % 10);
		buf[36] = (char)('0' + (NC_NAK_BASE - err) % 10);
		buf[37] = 0;
		return buf;
	}
	return "error";
}
