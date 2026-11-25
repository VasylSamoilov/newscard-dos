/* SPDX-License-Identifier: GPL-2.0 */
/* Copyright 2026 Vasyl Samoilov */
/*
 * Full-screen program, two panels: messages on the left, the selected
 * message on the right, function keys at the bottom. Every action also
 * has a letter key.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <signal.h>
#ifdef __DOS__
#include <dos.h>
#endif
#include "nc.h"
#include "app.h"
#include "scr.h"
#include "pcclock.h"

#define DOC_MAX (NC_MAX_TEXT + 400)
#ifdef __DOS__
#include <malloc.h>
#define PV_FAR __far
#define pv_alloc(n) _fmalloc(n)
#define pv_free(p) _ffree(p)
#else
#define PV_FAR
#define pv_alloc(n) malloc(n)
#define pv_free(p) free(p)
#endif
#define PV_LEN 40

static int n_msgs, cur, top;
static u32 cur_ptr = NC_NONE;
static struct nc_status st;
static int st_ok, st_dirty = 1;
static u32 scan_ticks;
static char msgline[100];
static u32 text_ptr = NC_NONE;
static char doc[DOC_MAX];
static u32 doc_ptr = NC_NONE;

/* list previews, one per message, read once per scan */
static char PV_FAR *pv;
static unsigned char pv_ok[NC_MAX_MSGS];

static void say(const char *s)
{
	strncpy(msgline, s, sizeof(msgline) - 1);
	msgline[sizeof(msgline) - 1] = 0;
}

static void say_err(const char *what, int e)
{
	sprintf(msgline, "%s: %.60s", what, nc_strerror(e));
}

static void forget_texts(void)
{
	memset(pv_ok, 0, sizeof(pv_ok));
	text_ptr = doc_ptr = NC_NONE;
}

static const char *preview(int i)
{
	static char t[60];
	int j;

	if (!pv) {
		pv = (char PV_FAR *)pv_alloc((size_t)NC_MAX_MSGS * PV_LEN);
		if (!pv) {
			strcpy(t, "");
			return t;
		}
	}
	if (!pv_ok[i]) {
		int e = nc_preview(&msgs[i], t, sizeof(t));

		if (e < 0)
			return "(cannot read)";
		for (j = 0; j < PV_LEN; j++)
			pv[(unsigned)i * PV_LEN + j] = t[j < (int)sizeof(t) ? j : 0];
		pv[(unsigned)i * PV_LEN + PV_LEN - 1] = 0;
		pv_ok[i] = 1;
	}
	for (j = 0; j < PV_LEN; j++)
		t[j] = pv[(unsigned)i * PV_LEN + j];
	return t;
}

static int load_text(int i)
{
	int e;

	if (text_ptr == msgs[i].ptr)
		return 0;
	e = nc_text(&msgs[i], text, sizeof(text));
	if (e < 0) {
		text_ptr = NC_NONE;
		text[0] = 0;
		return e;
	}
	text_ptr = msgs[i].ptr;
	return 0;
}

static void add(char **p, const char *s)
{
	size_t room = (size_t)(doc + DOC_MAX - 1 - *p), n = strlen(s);

	if (n > room)
		n = room;
	memcpy(*p, s, n);
	*p += n;
	**p = 0;
}

/* header lines and text of message i, as shown in the panel and the viewer */
static void compose(int i)
{
	const struct nc_msg *m = &msgs[i];
	char line[80], *p = doc, *a, *b;
	int e;

	if (doc_ptr == m->ptr)
		return;
	doc[0] = 0;
	sprintf(line, "Received: %s\n", when(m->time));
	add(&p, line);
	sprintf(line, "To:       %s\n", who(m));
	add(&p, line);
	sprintf(line, "State:    %s%s\n", NC_MSG_READ(m) ? "read" : "new",
		m->errors ? ", with errors" : "");
	add(&p, line);
	e = load_text(i);
	if (e) {
		sprintf(line, "\n(%.60s)\n", nc_strerror(e));
		add(&p, line);
		doc_ptr = NC_NONE;
		return;
	}
	a = strchr(text, '|');
	b = a ? strchr(a + 1, '|') : NULL;
	if (a && b) {
		*a = *b = 0;
		add(&p, "From:     ");
		add(&p, text);
		add(&p, "\nSubject:  ");
		add(&p, a + 1);
		add(&p, "\n\n");
		add(&p, b + 1);
		*a = *b = '|';
	} else {
		add(&p, "\n");
		add(&p, text);
	}
	doc_ptr = m->ptr;
}

/*
 * One screen line of s starting at *pos, at most w characters, broken at
 * a blank where possible. CR, LF and CR LF end a line; other control bytes
 * show as <XX>. Returns 0 at the end of s.
 */
static int wrap(const char *s, int *pos, int w, char *out)
{
	int i = *pos, n = 0, brk_i = -1, brk_n = 0;

	if (!s[i]) {
		out[0] = 0;
		return 0;
	}
	while (s[i] && n < w) {
		unsigned char c = (unsigned char)s[i];

		if (c == '\n' || c == '\r') {
			i += (c == '\r' && s[i + 1] == '\n') ? 2 : 1;
			out[n] = 0;
			*pos = i;
			return 1;
		}
		if (c < 0x20 || c > 0x7E) {
			if (n + 4 > w)
				break;
			sprintf(out + n, "<%02X>", c);
			n += 4;
		} else {
			if (c == ' ') {
				brk_i = i;
				brk_n = n;
			}
			out[n++] = (char)c;
		}
		i++;
	}
	if (s[i] && s[i] != '\n' && s[i] != '\r' && brk_i > *pos) {
		n = brk_n;
		i = brk_i + 1;
	}
	out[n] = 0;
	*pos = i;
	return 1;
}

static void rescan(void)
{
	u32 t0 = bus_ticks();
	int n = nc_scan(msgs, NC_MAX_MSGS), i;

	scan_ticks = bus_ticks() - t0;
	forget_texts();
	st_dirty = 1;
	if (n < 0) {
		say_err("reading the messages", n);
		n_msgs = 0;
		return;
	}
	n_msgs = n;
	for (i = 0; i < n; i++)
		if (msgs[i].ptr == cur_ptr) {
			cur = i;
			break;
		}
	if (i == n && cur >= n)
		cur = n ? n - 1 : 0;
	cur_ptr = n ? msgs[cur].ptr : NC_NONE;
}

static void load_status(void)
{
	int e;

	if (!st_dirty)
		return;
	e = nc_status_all(&st);
	st_ok = e == 0;
	st_dirty = 0;
	if (e)
		say_err("reading the status", e);
}

/* after any command: a page that arrived meanwhile */
static void check_event(void)
{
	if (nc_event) {
		nc_event = 0;
		rescan();
		scr_beep();
		say("New page received.");
	}
}

/* ---- drawing ---- */

static int list_rows(void)
{
	return scr_h - 5;
}

static int panel_w(void)
{
	return scr_w >= 64 ? scr_w / 2 : scr_w;
}

static void draw_title(void)
{
	char t[100], r[100];
	int unread = 0, i, n;

	for (i = 0; i < n_msgs; i++)
		if (!NC_MSG_READ(&msgs[i]))
			unread++;
	sprintf(t, " NewsCard  %d message%s, %d new", n_msgs, n_msgs == 1 ? "" : "s", unread);
	scr_put(0, 0, A_INV, t, scr_w);
	if (!st_ok)
		return;
	sprintf(r, "battery %s  switch %s  alerts %s  %s ", st.battery_ok ? "OK" : "LOW",
		nc_switch_name(st.sw), st.alerts < 0 ? "?" : st.alerts ? "on" : "off", when(st.clock));
	n = (int)strlen(r);
	if ((int)strlen(t) + n + 2 > scr_w) {
		sprintf(r, "bat %s  sw %s  beep %s  %s ", st.battery_ok ? "OK" : "LOW",
			nc_switch_name(st.sw), st.alerts < 0 ? "?" : st.alerts ? "on" : "off",
			when(st.clock));
		n = (int)strlen(r);
	}
	if ((int)strlen(t) + n + 2 <= scr_w)
		scr_put(scr_w - n, 0, A_INV, r, n);
}

static void draw_list(void)
{
	int w = panel_w(), rows = list_rows(), r;
	char line[140];

	scr_box(0, 1, w, rows + 2, A_NORM, "Messages");
	if (cur < top)
		top = cur;
	if (cur >= top + rows)
		top = cur - rows + 1;
	if (!n_msgs)
		scr_put(2, 3, A_NORM, "No messages on the card.", w - 4);
	for (r = 0; r < rows && top + r < n_msgs; r++) {
		const struct nc_msg *m = &msgs[top + r];
		int fn = NC_MSG_FUNC(m), n, pos = 0;
		char pv[60];

		n = sprintf(line, "%c%02u-%02u %02u:%02u %d%c ", NC_MSG_READ(m) ? ' ' : '*',
			    m->time[1], m->time[2], m->time[3], m->time[4], NC_MSG_SLOT(m),
			    fn < 4 ? "ABCD"[fn] : '?');
		wrap(preview(top + r), &pos, (int)sizeof(pv) - 1, pv);
		strncat(line, pv, sizeof(line) - (size_t)n - 1);
		scr_put(1, 2 + r, top + r == cur ? A_INV : (NC_MSG_READ(m) ? A_NORM : A_BRIGHT), line,
			w - 2);
	}
}

static void draw_message(void)
{
	int x = panel_w(), w = scr_w - x, rows = list_rows(), r, pos = 0;
	char title[30], line[140];

	if (scr_w < 64)
		return;
	if (!n_msgs) {
		scr_box(x, 1, w, rows + 2, A_NORM, "Message");
		return;
	}
	sprintf(title, "Message %u", msgs[cur].seq);
	scr_box(x, 1, w, rows + 2, A_NORM, title);
	compose(cur);
	for (r = 0; r < rows && wrap(doc, &pos, w - 2, line); r++)
		scr_put(x + 1, 2 + r, A_NORM, line, w - 2);
}

static const char *const fkeys[10] = {
	"Help", "Info", "View", "Read", "Save", "Alert", "Clock", "Delete", "Reload", "Quit"
};

static void draw_keys(void)
{
	int i, x = 0, cw = scr_w / 10;

	scr_fill(0, scr_h - 1, scr_w, 1, A_NORM, ' ');
	for (i = 0; i < 10; i++) {
		char num[3];

		sprintf(num, "%d", i + 1);
		scr_put(x, scr_h - 1, A_NORM, num, (int)strlen(num));
		scr_put(x + (int)strlen(num), scr_h - 1, A_INV, fkeys[i], cw - (int)strlen(num) - 1);
		x += cw;
	}
}

static void draw(void)
{
	load_status();
	draw_title();
	draw_list();
	draw_message();
	scr_put(0, scr_h - 2, A_NORM, msgline, scr_w);
	draw_keys();
}

/* ---- dialogs ---- */

static void dialog_box(int w, int h, const char *title, int *x, int *y)
{
	if (w > scr_w - 2)
		w = scr_w - 2;
	*x = (scr_w - w) / 2;
	*y = (scr_h - h) / 2;
	scr_box(*x, *y, w, h, A_INV, title);
}

static int ask_yes(const char *q)
{
	int w = (int)strlen(q) + 6, x, y;
	unsigned k;

	if (w < 32)
		w = 32;
	dialog_box(w, 5, "NewsCard", &x, &y);
	scr_put(x + 3, y + 1, A_INV, q, w - 6);
	scr_put(x + 3, y + 3, A_INV, "Y = yes    N = no", w - 6);
	for (;;) {
		k = scr_key(-1);
		if (k == 'y' || k == 'Y' || k == K_ENTER)
			return 1;
		if (k == 'n' || k == 'N' || k == K_ESC || k == K_SCRIPT_END)
			return 0;
	}
}

/* edit buf in a one-line field; 1 = Enter, 0 = Esc */
static int ask_text(const char *title, const char *label, char *buf, int max)
{
	int w = 50, x, y, n = (int)strlen(buf), fw;
	unsigned k;

	dialog_box(w, 6, title, &x, &y);
	if (w > scr_w - 2)
		w = scr_w - 2;
	fw = w - 6;
	scr_put(x + 3, y + 1, A_INV, label, fw);
	scr_put(x + 3, y + 4, A_INV, "Enter = OK    Esc = cancel", fw);
	for (;;) {
		int off = n >= fw ? n - fw + 1 : 0;

		scr_put(x + 3, y + 2, A_NORM, buf + off, fw);
		scr_put(x + 3 + n - off, y + 2, A_INV, " ", 1);
		k = scr_key(-1);
		if (k == K_ENTER)
			return 1;
		if (k == K_ESC || k == K_SCRIPT_END)
			return 0;
		if (k == K_BKSP && n > 0)
			buf[--n] = 0;
		else if (k >= 0x20 && k < 0x7F && n < max - 1) {
			buf[n++] = (char)k;
			buf[n] = 0;
		}
	}
}

static void show_lines(const char *title, const char *const *lines, int n)
{
	int w = 20, i, x, y;

	for (i = 0; i < n; i++)
		if ((int)strlen(lines[i]) + 6 > w)
			w = (int)strlen(lines[i]) + 6;
	dialog_box(w, n + 4, title, &x, &y);
	for (i = 0; i < n; i++)
		scr_put(x + 3, y + 1 + i, A_INV, lines[i], w - 6);
	scr_put(x + 3, y + n + 2, A_INV, "Press any key", w - 6);
	scr_key(-1);
}

/* ---- actions ---- */

/* returns an action key pressed in the viewer, for the main loop, or 0 */
static unsigned act_view(void)
{
	int scroll = 0, total, w = scr_w - 2, rows = scr_h - 3, r, pos;
	char line[140], title[60];
	unsigned k;

	if (!n_msgs)
		return 0;
	compose(cur);
	for (;;) {
		pos = 0;
		total = 0;
		while (wrap(doc, &pos, w, line))
			total++;
		if (scroll > total - rows)
			scroll = total - rows;
		if (scroll < 0)
			scroll = 0;
		sprintf(title, "Message %u", msgs[cur].seq);
		scr_box(0, 0, scr_w, scr_h - 1, A_NORM, title);
		pos = 0;
		for (r = 0; r < scroll && wrap(doc, &pos, w, line); r++)
			;
		for (r = 0; r < rows && wrap(doc, &pos, w, line); r++)
			scr_put(1, 1 + r, A_NORM, line, w);
		scr_put(0, scr_h - 1, A_INV,
			" Up/Down/PgUp/PgDn scroll   F4/R read   F8/D delete   F5/S save   Esc back",
			scr_w);
		k = scr_key(-1);
		if (k == K_UP)
			scroll--;
		else if (k == K_DOWN)
			scroll++;
		else if (k == K_PGUP)
			scroll -= rows - 1;
		else if (k == K_PGDN || k == ' ')
			scroll += rows - 1;
		else if (k == K_HOME)
			scroll = 0;
		else if (k == K_END)
			scroll = total;
		else
			break;
	}
	if (k == K_F(4) || k == 'r' || k == 'R' || k == K_F(8) || k == 'd' || k == 'D' ||
	    k == K_DEL || k == K_F(5) || k == 's' || k == 'S' || k == K_SCRIPT_END)
		return k;
	return 0;
}

static void act_mark(void)
{
	struct nc_msg *m;
	int e;

	if (!n_msgs)
		return;
	m = &msgs[cur];
	if (NC_MSG_READ(m)) {
		say("This message is already read.");
		return;
	}
	e = nc_mark_read(m);
	if (e) {
		say_err("mark read", e);
		return;
	}
	m->flags |= 4;
	doc_ptr = NC_NONE;
	st_dirty = 1;
	sprintf(msgline, "Message %u marked read.", m->seq);
	if (cur + 1 < n_msgs)
		cur++;
	cur_ptr = msgs[cur].ptr;
}

static void act_delete(void)
{
	char q[80];
	int e, seq;

	if (!n_msgs)
		return;
	seq = msgs[cur].seq;
	sprintf(q, "Delete message %d from %s?", seq, when(msgs[cur].time));
	if (!ask_yes(q))
		return;
	e = nc_delete(&msgs[cur]);
	if (e) {
		say_err("delete", e);
		return;
	}
	cur_ptr = cur + 1 < n_msgs ? msgs[cur + 1].ptr : (cur > 0 ? msgs[cur - 1].ptr : NC_NONE);
	rescan();
	sprintf(msgline, "Message %d deleted.", seq);
}

static void act_save(void)
{
	static char name[64] = "NEWSCARD.TXT";
	FILE *f;
	int e;

	if (!n_msgs || !ask_text("Save", "Append the message to file:", name, sizeof(name)))
		return;
	e = load_text(cur);
	if (e) {
		say_err("reading the message", e);
		return;
	}
	f = fopen(name, "a");
	if (!f) {
		sprintf(msgline, "Cannot write %.60s.", name);
		return;
	}
	show(f, &msgs[cur]);
	fputc('\n', f);
	if (fclose(f))
		sprintf(msgline, "Error writing %.60s.", name);
	else
		sprintf(msgline, "Message %u appended to %.60s.", msgs[cur].seq, name);
}

static void act_info(void)
{
	static char l[9][80];
	const char *lines[9];
	int i;

	st_dirty = 1;
	load_status();
	if (!st_ok)
		return;
	sprintf(l[0], "Battery:   %s", st.battery_ok ? "OK" : "LOW");
	sprintf(l[1], "Switch:    %s", nc_switch_name(st.sw));
	sprintf(l[2], "Clock:     %s", when(st.clock));
	sprintf(l[3], "Messages:  %d, %d new", st.total, st.unread);
	sprintf(l[4], "Free:      %d blocks (%ld bytes)", st.free_blocks, (long)st.free_blocks * 64);
	sprintf(l[5], "Alerts:    %s", st.alerts < 0 ? "?" : st.alerts ? "on" : "off");
	sprintf(l[6], "Read in:   %lu.%lu s", (unsigned long)(scan_ticks * 10 / 182),
		(unsigned long)(scan_ticks * 100 / 182 % 10));
	sprintf(l[7], "%.70s", bus_describe());
	sprintf(l[8], "%s", "");
	for (i = 0; i < 9; i++)
		lines[i] = l[i];
	show_lines("Card", lines, 8);
}

static void act_alerts(void)
{
	int e, on;

	st_dirty = 1;
	load_status();
	if (!st_ok || st.alerts < 0)
		return;
	on = !st.alerts;
	if (!ask_yes(on ? "Turn the alert beep on?" : "Turn the alert beep off?"))
		return;
	e = nc_set_alerts(on);
	st_dirty = 1;
	if (e)
		say_err("alerts", e);
	else
		say(on ? "Alerts on." : "Alerts off.");
}

/* "YYYY-MM-DD hh:mm" -> t[5]; 0 if valid for both clocks */
static int parse_when(const char *buf, int *t)
{
	if (sscanf(buf, "%d-%d-%d %d:%d", &t[0], &t[1], &t[2], &t[3], &t[4]) != 5 ||
	    t[0] < 1993 || t[0] > 2092 || t[1] < 1 || t[1] > 12 || t[2] < 1 || t[2] > 31 ||
	    t[3] < 0 || t[3] > 23 || t[4] < 0 || t[4] > 59)
		return -1;
	return 0;
}

static int set_card_clock(const int *t)
{
	u8 c[5];
	int e;

	c[0] = (u8)(t[0] % 100); /* the card keeps a two-digit year */
	c[1] = (u8)t[1];
	c[2] = (u8)t[2];
	c[3] = (u8)t[3];
	c[4] = (u8)t[4];
	e = nc_set_clock(c);
	st_dirty = 1;
	if (e)
		say_err("set the card clock", e);
	return e;
}

static void fmt_t(char *buf, const int *t)
{
	sprintf(buf, "%04d-%02d-%02d %02d:%02d", t[0], t[1], t[2], t[3], t[4]);
}

/* F7: palmtop clock and card clock */
static void act_clock(void)
{
	char l_pc[100], l_card[100], buf[64];
	int pc[5], cd[5], t[5], x, y, w = 50, i;
	unsigned k;

	st_dirty = 1;
	load_status();
	pc_get_clock(pc);
	fmt_t(buf, pc);
	sprintf(l_pc, "Computer clock:  %s", buf);
	cd[0] = st_ok ? nc_year(st.clock[0]) : 0;
	for (i = 1; i < 5; i++)
		cd[i] = st_ok ? st.clock[i] : 0;
	sprintf(l_card, "Card clock:      %s", st_ok ? when(st.clock) : "?");
	dialog_box(w, 10, "Clock", &x, &y);
	scr_put(x + 3, y + 1, A_INV, l_pc, w - 6);
	scr_put(x + 3, y + 2, A_INV, l_card, w - 6);
	scr_put(x + 3, y + 4, A_INV, "1  set the card clock from the computer", w - 6);
	scr_put(x + 3, y + 5, A_INV, "2  set the computer clock from the card", w - 6);
	scr_put(x + 3, y + 6, A_INV, "3  type a date and time for both", w - 6);
	scr_put(x + 3, y + 8, A_INV, "Esc  cancel", w - 6);
	do
		k = scr_key(-1);
	while (k != '1' && k != '2' && k != '3' && k != K_ESC && k != K_SCRIPT_END);
	if (k == '1') {
		if (pc[0] < 1993 || pc[0] > 2092) {
			say("The computer clock is not set: use 2 or 3.");
			return;
		}
		if (!set_card_clock(pc)) {
			fmt_t(buf, pc);
			sprintf(msgline, "Card clock set to %s.", buf);
		}
	} else if (k == '2') {
		if (!st_ok) {
			say("Cannot read the card clock.");
			return;
		}
		if (pc_set_clock(cd))
			say("Cannot set the computer clock.");
		else {
			fmt_t(buf, cd);
			sprintf(msgline, "Computer clock set to %s.", buf);
		}
	} else if (k == '3') {
		if (pc[0] >= 1993 && pc[0] <= 2092)
			fmt_t(buf, pc);
		else if (st_ok)
			strcpy(buf, when(st.clock));
		else
			strcpy(buf, "2026-01-01 00:00");
		if (!ask_text("Clock", "Date and time (YYYY-MM-DD hh:mm):", buf, sizeof(buf)))
			return;
		if (parse_when(buf, t)) {
			say("Give the date as YYYY-MM-DD hh:mm, 1993 to 2092.");
			return;
		}
		if (pc_set_clock(t)) {
			say("Cannot set the computer clock.");
			return;
		}
		if (!set_card_clock(t)) {
			fmt_t(buf, t);
			sprintf(msgline, "Both clocks set to %s.", buf);
		}
	}
}

static void act_help(void)
{
	static const char *const lines[] = {
		"Up/Down, PgUp/PgDn, Home/End   choose a message",
		"F3, V or Enter   view the whole message",
		"F4 or R          mark it read",
		"F5 or S          append it to a file",
		"F8, D or Del     delete it",
		"F2 or I          card status",
		"F6 or A          alert beep on/off",
		"F7 or C          clocks: card and computer",
		"F9 or L          read the card again",
		"F10 or Q         quit",
	};

	show_lines("Help", lines, (int)(sizeof(lines) / sizeof(lines[0])));
}

/* ---- start ---- */

#ifdef __DOS__
/* DOS critical error (disk not ready, write protected...): fail the call */
static int __far crit_error(unsigned deverr, unsigned errcode, unsigned __far *devhdr)
{
	(void)deverr;
	(void)errcode;
	(void)devhdr;
	return _HARDERR_FAIL;
}
#endif

static int start_failed(int e)
{
	static char l[3][100];
	const char *lines[3];
	int st_card = bus_card_status();

	sprintf(l[0], "The card cannot be used: %.60s.", nc_strerror(e));
	if (st_card >= 0 && (st_card & 0xC0) == 0x80)
		strcpy(l[1], "The card reports not ready: check its battery and switch.");
	else
		strcpy(l[1], "");
	bus_close();
	to_file("PROBE.TXT", step_probe);
	bus_close();
	strcpy(l[2], "Details are in PROBE.TXT.");
	lines[0] = l[0];
	lines[1] = l[1];
	lines[2] = l[2];
	show_lines("NewsCard", lines, 3);
	return 1;
}

int ui_run(const char *keys, const char *shot)
{
	unsigned k, pending = 0;
	int e, rows;

	/* Ctrl-C and Ctrl-Break must not leave the screen and the card mapping behind */
	signal(SIGINT, SIG_IGN);
#ifdef __DOS__
	signal(SIGBREAK, SIG_IGN);
	_harderr(crit_error);
#endif
	scr_init(keys, shot);
	scr_put(0, 0, A_INV, " NewsCard   reading the card...", scr_w);
	e = bus_open(&opts);
	if (!e)
		e = nc_identify(NULL, NULL, 0);
	if (e) {
		start_failed(e);
		scr_done();
		return 1;
	}
	rescan();
	if (n_msgs)
		cur = n_msgs - 1; /* newest */
	cur_ptr = n_msgs ? msgs[cur].ptr : NC_NONE;
	for (;;) {
		check_event();
		if (!scr_key_waiting())
			draw();
		k = pending ? pending : scr_key(-1);
		pending = 0;
		msgline[0] = 0;
		rows = list_rows();
		if (k == K_SCRIPT_END || k == K_F(10) || k == 'q' || k == 'Q')
			break;
		switch (k) {
		case K_UP:
			if (cur > 0)
				cur--;
			break;
		case K_DOWN:
			if (cur + 1 < n_msgs)
				cur++;
			break;
		case K_PGUP:
			cur = cur > rows ? cur - rows : 0;
			break;
		case K_PGDN:
			cur = cur + rows < n_msgs ? cur + rows : (n_msgs ? n_msgs - 1 : 0);
			break;
		case K_HOME:
			cur = 0;
			break;
		case K_END:
			cur = n_msgs ? n_msgs - 1 : 0;
			break;
		case K_F(1):
		case 'h':
		case 'H':
			act_help();
			break;
		case K_F(2):
		case 'i':
		case 'I':
			act_info();
			break;
		case K_F(3):
		case K_ENTER:
		case 'v':
		case 'V':
			pending = act_view();
			break;
		case K_F(4):
		case 'r':
		case 'R':
			act_mark();
			break;
		case K_F(5):
		case 's':
		case 'S':
			act_save();
			break;
		case K_F(6):
		case 'a':
		case 'A':
			act_alerts();
			break;
		case K_F(7):
		case 'c':
		case 'C':
			act_clock();
			break;
		case K_F(8):
		case K_DEL:
		case 'd':
		case 'D':
			act_delete();
			break;
		case K_F(9):
		case 'l':
		case 'L':
			rescan();
			sprintf(msgline, "%d messages read.", n_msgs);
			break;
		}
		if (n_msgs)
			cur_ptr = msgs[cur].ptr;
	}
	bus_close();
	scr_done();
	if (pv)
		pv_free(pv);
	return 0;
}
