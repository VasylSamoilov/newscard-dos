/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Copyright 2026 Vasyl Samoilov */
/*
 * Card memory through PCMCIA Socket Services 1.x (INT 1Ah AH=80h-90h):
 * two 16 KB memory windows, [0] on card page 0 and [1] moved as needed.
 *
 * Each bracket turns interrupts off, saves the host's window mapping,
 * maps the card, and at the end restores the mapping before interrupts
 * are turned on again. On the HP 100LX/200LX the whole map is saved and
 * restored with INT 63h AH=1; elsewhere the two windows are read back with
 * GET WINDOW and set again afterwards. Nothing is printed inside a
 * bracket: errors are kept and reported afterwards.
 *
 * Where the card's RAM appears depends on the host: on some the high card
 * address bits come out the other way round, and offset 0 shows the
 * flash. At open, card offsets 0, 0x02000000, 0x01000000 and 0x03000000
 * are read (read-only); the RAM is where the CIS strings are. Offsets
 * used above this layer are then XORed with that base.
 */
#include <i86.h>
#include <dos.h>
#include <conio.h>
#include <string.h>
#include <stdio.h>
#include "nc.h"
#include "ss.h"

#define SS_SIG 0x5353 /* 'SS' */
#define HP_MAP_SIZE 19

static int hp;
static int adapter;
static int socket_no = 1;
static int win[2];
static unsigned seg[2];
static u8 speed_bit = 0x20;
static u8 attr;

static u8 hp_map[HP_MAP_SIZE];
struct win_state {
	u8 socket, dh, dl;
	unsigned cx, si, di;
};
static struct win_state saved[2];
static int have_saved[2];

static u32 page_b = NC_NONE;
static volatile u8 __far *wa;
static volatile u8 __far *wb;
static u32 card_xor;
static const u32 bases[4] = { 0, 0x02000000UL, 0x01000000UL, 0x03000000UL };
static char desc[160];
static char last_err[60];

static void note(const char *what, int code)
{
	sprintf(last_err, "%s: error %02X", what, code);
}

static int ss_present(int *adapters)
{
	union REGS r;

	memset(&r, 0, sizeof(r));
	r.h.ah = 0x80;
	r.w.cx = 0;
	int86(0x1A, &r, &r);
	*adapters = r.h.al;
	return !r.w.cflag && r.w.cx == SS_SIG;
}

static int is_hp_palmtop(void)
{
	union REGS r;

	memset(&r, 0, sizeof(r));
	r.w.ax = 0x4DD4;
	int86(0x15, &r, &r);
	return r.w.bx == 0x4850; /* 'HP' */
}

/* INT 63h AH=1 AL=3 returns the size of the map save area */
static int hp_map_size(void)
{
	union REGS r;

	memset(&r, 0, sizeof(r));
	r.h.ah = 0x01;
	r.h.al = 0x03;
	int86(0x63, &r, &r);
	return r.h.ah == 0 ? r.h.al : -1;
}

static int set_window(int w, unsigned s, u32 card_off, u8 dh)
{
	union REGS r;

	memset(&r, 0, sizeof(r));
	r.h.ah = 0x89;
	r.h.al = (u8)adapter;
	r.h.bh = (u8)w;
	r.h.bl = (u8)socket_no;
	r.w.cx = 4; /* 16 KB in 4 KB units */
	r.h.dh = dh;
	r.h.dl = speed_bit;
	r.w.si = s >> 8;
	r.w.di = (unsigned)(card_off >> 12);
	int86(0x1A, &r, &r);
	if (r.w.cflag) {
		note(w == win[0] ? "SET WINDOW (page 0)" : "SET WINDOW (page)", r.h.ah);
		return -1;
	}
	return 0;
}

static int get_window(int w, struct win_state *st)
{
	union REGS r;

	memset(&r, 0, sizeof(r));
	r.h.ah = 0x88;
	r.h.al = (u8)adapter;
	r.h.bh = (u8)w;
	int86(0x1A, &r, &r);
	if (r.w.cflag)
		return -1;
	st->socket = r.h.bl;
	st->cx = r.w.cx;
	st->dh = r.h.dh;
	st->dl = r.h.dl;
	st->si = r.w.si;
	st->di = r.w.di;
	return 0;
}

/* 0 if the current mapping was saved; nothing may be changed otherwise */
static int save_map(void)
{
	int i;

	if (hp) {
		union REGS r;
		struct SREGS sr;

		segread(&sr);
		memset(&r, 0, sizeof(r));
		r.h.ah = 0x01;
		r.h.al = 0x00;
		sr.es = FP_SEG((void __far *)hp_map);
		r.w.di = FP_OFF((void __far *)hp_map);
		int86x(0x63, &r, &r, &sr);
		if (r.h.ah) {
			note("INT 63h save", r.h.ah);
			return -1;
		}
		return 0;
	}
	for (i = 0; i < 2; i++)
		have_saved[i] = get_window(win[i], &saved[i]) == 0;
	return 0;
}

static void restore_map(void)
{
	int i;

	if (hp) {
		union REGS r;
		struct SREGS sr;

		segread(&sr);
		memset(&r, 0, sizeof(r));
		r.h.ah = 0x01;
		r.h.al = 0x01;
		sr.ds = FP_SEG((void __far *)hp_map);
		r.w.si = FP_OFF((void __far *)hp_map);
		int86x(0x63, &r, &r, &sr);
		return;
	}
	for (i = 0; i < 2; i++) {
		union REGS r;
		struct win_state *st = &saved[i];

		memset(&r, 0, sizeof(r));
		r.h.ah = 0x89;
		r.h.al = (u8)adapter;
		r.h.bh = (u8)win[i];
		if (have_saved[i] && st->socket) {
			r.h.bl = st->socket;
			r.w.cx = st->cx;
			r.h.dh = st->dh;
			r.h.dl = st->dl;
			r.w.si = st->si;
			r.w.di = st->di;
		} else {
			/* was not in use: leave it disabled */
			r.h.bl = (u8)socket_no;
			r.w.cx = 4;
			r.h.dh = (u8)(attr & ~0x04);
			r.h.dl = speed_bit;
			r.w.si = seg[i] >> 8;
			r.w.di = 0;
		}
		int86(0x1A, &r, &r);
	}
}

static int map(u32 off)
{
	if (set_window(win[0], seg[0], card_xor, attr))
		return -1;
	page_b = NC_NONE;
	if (off >= BUS_PAGE) {
		u32 pg = off & ~(BUS_PAGE - 1);

		if (set_window(win[1], seg[1], pg ^ card_xor, attr))
			return -1;
		page_b = pg;
	}
	return 0;
}

/* HP palmtops: chip register through index port 22h, data port 23h */
static u8 chip_reg(u8 idx)
{
	u8 old = (u8)inp(0x22), v;

	outp(0x22, idx);
	v = (u8)inp(0x23);
	outp(0x22, old);
	return v;
}

/* map registers of window w as read back while it is mapped */
static u16 map_reg_seen;

/*
 * Read n bytes at a physical card offset (inside one 16 KB page) with
 * window attribute a; read-only.
 */
static int peek_attr(u32 phys, u8 *buf, int n, u8 a)
{
	int i;

	_disable();
	if (save_map()) {
		_enable();
		return -1;
	}
	if (set_window(win[0], seg[0], phys & ~(BUS_PAGE - 1), a)) {
		restore_map();
		_enable();
		return -1;
	}
	for (i = 0; i < n; i++)
		buf[i] = wa[(unsigned)((phys & (BUS_PAGE - 1)) + i)];
	if (hp)
		map_reg_seen = (u16)((chip_reg((u8)(0x88 + 2 * win[0])) << 8) |
				     chip_reg((u8)(0x89 + 2 * win[0])));
	restore_map();
	_enable();
	return 0;
}

static int peek(u32 phys, u8 *buf, int n)
{
	return peek_attr(phys, buf, n, attr);
}

static int is_ram_cis(const u8 *b)
{
	static const char want[] = "NewsCard Data Receiver";
	int i;

	if (b[0] != 0x01)
		return 0;
	for (i = 0; i + (int)sizeof(want) <= 0x80; i++)
		if (!memcmp(b + i, want, sizeof(want) - 1))
			return 1;
	return 0;
}

static int find_ram(void)
{
	u8 b[0x80];
	int k;

	for (k = 0; k < 4; k++)
		if (peek(bases[k], b, 0x80) == 0 && is_ram_cis(b)) {
			card_xor = bases[k];
			return 0;
		}
	card_xor = 0;
	return -1;
}

static u8 speed_to_bit(unsigned ns)
{
	if (ns <= 100)
		return 0x02;
	if (ns <= 150)
		return 0x04;
	if (ns <= 200)
		return 0x08;
	if (ns <= 250)
		return 0x10;
	if (ns <= 300)
		return 0x20;
	return 0x40;
}

int ss_open(const struct bus_opts *o)
{
	int adapters;

	last_err[0] = 0;
	if (!ss_present(&adapters)) {
		strcpy(desc, "no PCMCIA Socket Services found");
		return NC_EBUS;
	}
	hp = is_hp_palmtop();
	if (hp && hp_map_size() != HP_MAP_SIZE) {
		strcpy(desc, "HP palmtop, but INT 63h does not offer the 19-byte map save");
		return NC_EBUS;
	}
	adapter = 0;
	socket_no = o->socket > 0 ? o->socket : 1;
	win[0] = o->win[0];
	win[1] = o->win[1];
	seg[0] = o->seg[0];
	seg[1] = o->seg[1];
	speed_bit = speed_to_bit(o->speed_ns);
	/*
	 * Memory window, common memory, enabled: 04h on the HP palmtops
	 * (bit 0 set selects an I/O window there), 05h in Socket Services 1.0
	 * (bit 0 set selects a memory window). No automatic second try: the
	 * wrong value would set up an I/O window.
	 */
	attr = o->attr >= 0 ? (u8)o->attr : (u8)(hp ? 0x04 : 0x05);
	wa = (volatile u8 __far *)MK_FP(seg[0], 0);
	wb = (volatile u8 __far *)MK_FP(seg[1], 0);
	sprintf(desc, "%sSocket Services, %d adapter(s), socket %d, windows %d/%d at %04X/%04X, attribute %02X",
		hp ? "HP palmtop, " : "", adapters, socket_no, win[0], win[1], seg[0], seg[1], attr);
	if (o->ram_at >= 0)
		card_xor = (u32)o->ram_at;
	else if (find_ram())
		strcat(desc, ", RAM not found");
	if (o->ram_at >= 0 || card_xor)
		sprintf(desc + strlen(desc), ", RAM at card offset %08lX", (unsigned long)card_xor);
	return NC_OK;
}

void ss_close(void)
{
}

int ss_begin(u32 off)
{
	_disable();
	if (save_map()) {
		_enable();
		return NC_EBUS;
	}
	if (map(off)) {
		restore_map();
		_enable();
		return NC_EBUS;
	}
	return NC_OK;
}

void ss_end(void)
{
	restore_map();
	_enable();
}

u8 ss_rd(u32 off)
{
	if (off < BUS_PAGE)
		return wa[(unsigned)off];
	if ((off & ~(BUS_PAGE - 1)) == page_b)
		return wb[(unsigned)(off & (BUS_PAGE - 1))];
	return 0xFF;
}

void ss_wr(u32 off, u8 v)
{
	if (off < BUS_PAGE)
		wa[(unsigned)off] = v;
	else if ((off & ~(BUS_PAGE - 1)) == page_b)
		wb[(unsigned)(off & (BUS_PAGE - 1))] = v;
}

int ss_card_status(void)
{
	union REGS r;

	memset(&r, 0, sizeof(r));
	r.h.ah = 0x8F;
	r.h.al = (u8)adapter;
	r.h.bl = (u8)socket_no;
	int86(0x1A, &r, &r);
	if (r.w.cflag)
		return -1;
	return r.h.dl;
}

static void print_regs(FILE *f, const char *what, union REGS *r)
{
	fprintf(f, "%-11s %s AX=%04X BX=%04X CX=%04X DX=%04X SI=%04X DI=%04X\n", what,
		r->w.cflag ? "error" : "ok   ", r->w.ax, r->w.bx, r->w.cx, r->w.dx, r->w.si, r->w.di);
}

/* Socket Services readings and the first bytes seen through window 0; read-only */
void ss_diag(FILE *f)
{
	union REGS r;
	struct win_state ws;
	u8 first[32];
	int i, ok = 0, got_ws = 0;

	fprintf(f, "backend     %s\n", desc);
	memset(&r, 0, sizeof(r));
	r.h.ah = 0x83;
	r.h.al = (u8)adapter;
	int86(0x1A, &r, &r);
	print_regs(f, "SS version", &r);
	memset(&r, 0, sizeof(r));
	r.h.ah = 0x8D;
	r.h.al = (u8)adapter;
	r.h.bl = (u8)socket_no;
	int86(0x1A, &r, &r);
	print_regs(f, "GET SOCKET", &r);
	memset(&r, 0, sizeof(r));
	r.h.ah = 0x8F;
	r.h.al = (u8)adapter;
	r.h.bl = (u8)socket_no;
	int86(0x1A, &r, &r);
	print_regs(f, "GET CARD", &r);
	last_err[0] = 0;
	if (ss_begin(0) == NC_OK) {
		got_ws = get_window(win[0], &ws) == 0;
		for (i = 0; i < 32; i++)
			first[i] = wa[i];
		ss_end();
		ok = 1;
	}
	if (!ok) {
		fprintf(f, "map         failed (%s)\n", last_err[0] ? last_err : "?");
		return;
	}
	if (got_ws)
		fprintf(f, "GET WINDOW  socket %u size %u attr %02X speed %02X base %04X offset %04X\n",
			ws.socket, ws.cx, ws.dh, ws.dl, ws.si, ws.di);
	fprintf(f, "card 0x00  ");
	for (i = 0; i < 32; i++)
		fprintf(f, " %02x", first[i]);
	fputc('\n', f);
	{
		static const u32 offs[] = { 0, 0x4000UL, 0x8000UL, 0x10000UL, 0x20000UL,
					    0x01000000UL, 0x02000000UL, 0x03000000UL };
		static const u8 regs[] = { 0x46, 0x49, 0x4D, 0x4E, 0x4F, 0x51, 0x52, 0x53,
					   0x55, 0x58, 0x59, 0x5A, 0x5B, 0x82, 0x87, 0x88,
					   0x89, 0x8A, 0x8B, 0x9E };
		int j, k;

		for (k = 0; k < 2; k++) {
			u8 a = k ? (u8)(attr | 0x02) : attr;

			for (i = 0; i < (int)(sizeof(offs) / sizeof(offs[0])); i++) {
				fprintf(f, "%s %08lX", k ? "attr" : "at  ", (unsigned long)offs[i]);
				map_reg_seen = 0;
				if (peek_attr(offs[i], first, 16, a)) {
					fprintf(f, " cannot map (%s)\n", last_err);
					continue;
				}
				for (j = 0; j < 16; j++)
					fprintf(f, " %02x", first[j]);
				if (hp)
					fprintf(f, "  map %04X", map_reg_seen);
				fputc('\n', f);
			}
		}
		if (hp) {
			fprintf(f, "chip regs  ");
			_disable();
			for (j = 0; j < (int)sizeof(regs); j++)
				first[j] = chip_reg(regs[j]);
			_enable();
			for (j = 0; j < (int)sizeof(regs); j++)
				fprintf(f, " %02X=%02X", regs[j], first[j]);
			fputc('\n', f);
		}
	}
}

const char *ss_describe(void)
{
	return desc;
}

const char *ss_last_error(void)
{
	return last_err;
}
