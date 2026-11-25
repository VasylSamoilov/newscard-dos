/* SPDX-License-Identifier: GPL-2.0 */
/* Copyright 2026 Vasyl Samoilov */
/* NEWSCARD: command line tool for the Motorola NewsCard under DOS. */
#ifndef __WATCOMC__
#define _POSIX_C_SOURCE 200112L
#include <unistd.h>
#else
#include <io.h>
#endif
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <time.h>
#include "nc.h"
#include "app.h"

#define VERSION "0.1"

/* DOS cannot redirect stderr */
#ifdef __DOS__
#define ERR stdout
#else
#define ERR stderr
#endif

struct bus_opts opts;
static int yes;
long rics[2] = { -1, -1 };
struct nc_msg msgs[NC_MAX_MSGS];
char text[NC_MAX_TEXT + 1];
static const char *keys_script;
static const char *shot_file;

static const char *const usage_text[] = {
	"NEWSCARD " VERSION " - Motorola NewsCard pager card",
	"",
	"NEWSCARD [options]            full-screen program",
	"NEWSCARD [options] command",
	"  check              check the card; writes PROBE.TXT, INFO.TXT, LIST.TXT",
	"  probe              check the card, read-only",
	"  info               battery, switch, clock, counts, alerts",
	"  list               messages, oldest first",
	"  read N             show message N (sequence number or 0x record)",
	"  save N FILE        append message N to FILE",
	"  markread N         mark message N read",
	"  delete N           delete message N",
	"  settime [YYYY-MM-DD hh:mm]   set the card clock (default: now)",
	"  alerts [on|off]    show or set the alert beep",
	"  dump FILE [OFF [LEN]]        save card memory (default RAM, 128 KB)",
	"",
	"options",
	"  /Y                 do not ask before changing the card",
	"  /RICS=a,b          names for address slots 0 and 1 (or NEWSCARD_RICS)",
	"  /SOCKET=n          Socket Services socket (1)",
	"  /WIN=a,b           memory windows (0,1)",
	"  /SEG=aaaa,bbbb     their segments (D000,D400)",
	"  /SPEED=ns          access time (300)",
	"  /ATTR=hh           window attribute byte",
	"  /RAMAT=hhhhhhhh    card offset where the RAM appears (found by itself)",
	"  /SIM=FILE          use a RAM image as a simulated card",
	"  /V                 verbose",
	NULL
};

static void usage(void)
{
	int i;

	for (i = 0; usage_text[i]; i++)
		puts(usage_text[i]);
}

static int opt_match(const char *arg, const char *name, const char **val)
{
	size_t n = strlen(name);
	size_t i;

	if (arg[0] != '/' && arg[0] != '-')
		return 0;
	for (i = 0; i < n; i++)
		if (toupper((unsigned char)arg[1 + i]) != name[i])
			return 0;
	if (arg[1 + n] == '=' || arg[1 + n] == ':') {
		*val = arg + 2 + n;
		return 1;
	}
	if (arg[1 + n] == 0) {
		*val = "";
		return 1;
	}
	return 0;
}

static int parse_pair(const char *s, unsigned *a, unsigned *b, int base)
{
	char *end;

	*a = (unsigned)strtoul(s, &end, base);
	if (*end != ',')
		return -1;
	*b = (unsigned)strtoul(end + 1, &end, base);
	return *end ? -1 : 0;
}

static void parse_rics(const char *s)
{
	char *end;

	if (!s || !*s)
		return;
	rics[0] = strtol(s, &end, 10);
	if (*end == ',')
		rics[1] = strtol(end + 1, &end, 10);
}

const char *who(const struct nc_msg *m)
{
	static char buf[24];
	int slot = NC_MSG_SLOT(m), fn = NC_MSG_FUNC(m);
	char f = fn < 4 ? "ABCD"[fn] : '?';

	if (slot < 2 && rics[slot] >= 0)
		sprintf(buf, "%07ld %c", rics[slot], f);
	else
		sprintf(buf, "slot %d %c", slot, f);
	return buf;
}

const char *when(const u8 *t)
{
	static char buf[32];

	sprintf(buf, "%04d-%02u-%02u %02u:%02u", nc_year(t[0]), t[1], t[2], t[3], t[4]);
	return buf;
}

/* printable form of page text; control bytes as <XX> */
static void put_text(FILE *f, const char *s, int oneline, int max)
{
	int n = 0;

	for (; *s && (max <= 0 || n < max); s++) {
		unsigned char c = (unsigned char)*s;

		if (c == '\n' || c == '\r') {
			if (oneline) {
				fputc(' ', f);
				n++;
			} else if (c == '\n' || s[1] != '\n') {
				fputc('\n', f);
			}
		} else if (c >= 0x20 && c < 0x7F) {
			fputc(c, f);
			n++;
		} else {
			fprintf(f, "<%02X>", c);
			n += 4;
		}
	}
}

/* GET CARD: present (bit 7) but not ready (bit 6) */
void not_ready_hint(void)
{
	int st = bus_card_status();

	if (st >= 0 && (st & 0xC0) == 0x80)
		fputs("NEWSCARD: the card reports not ready; check the card's battery and switch\n", ERR);
}

static int fail(int e)
{
	const char *b = bus_last_error();

	if (e == NC_EBUS && b[0])
		fprintf(ERR, "NEWSCARD: %s (%s)\n", nc_strerror(e), b);
	else
		fprintf(ERR, "NEWSCARD: %s\n", nc_strerror(e));
	return 1;
}

static int confirm(const char *what)
{
	char line[16];

	if (yes)
		return 1;
	printf("%s? (y/n) ", what);
	fflush(stdout);
	if (!fgets(line, sizeof(line), stdin))
		return 0;
	return line[0] == 'y' || line[0] == 'Y';
}

static int scan(void)
{
	int n = nc_scan(msgs, NC_MAX_MSGS);

	if (n < 0)
		fail(n);
	return n;
}

/* N = sequence number, or 0x... record offset */
static struct nc_msg *find(const char *arg, int n)
{
	char *end;
	unsigned long v = strtoul(arg, &end, 0);
	struct nc_msg *hit = NULL;
	int i, count = 0, by_ptr = arg[0] == '0' && (arg[1] == 'x' || arg[1] == 'X');

	if (*end) {
		fprintf(ERR, "NEWSCARD: not a message number: %s\n", arg);
		return NULL;
	}
	for (i = 0; i < n; i++) {
		if (by_ptr ? msgs[i].ptr == v : msgs[i].seq == v) {
			hit = &msgs[i];
			count++;
		}
	}
	if (!count)
		fprintf(ERR, "NEWSCARD: no message %s\n", arg);
	else if (count > 1) {
		fprintf(ERR, "NEWSCARD: %d messages have number %s; use the record (0x...) from LIST\n",
			count, arg);
		hit = NULL;
	}
	return hit;
}

static void hex(const char *label, u32 off, int n)
{
	int i;

	printf("%-11s", label);
	if (bus_begin(off)) {
		puts("(cannot map)");
		return;
	}
	for (i = 0; i < n; i++)
		text[i] = (char)bus_rd(off + (u32)i);
	bus_end();
	for (i = 0; i < n; i++)
		printf(" %02x", (unsigned char)text[i]);
	putchar('\n');
}

static int cmd_probe(void)
{
	char vendor[40], product[40];
	int e;
	u32 i;
	int k, diff = 0, drops = 0, run;
	static u8 rd3[3][256];

	bus_diag(stdout);
	e = nc_identify(vendor, product, sizeof(vendor));
	if (e && e != NC_ENOTCARD)
		return fail(e);
	printf("card        %s / %s: %s\n", vendor, product,
	       e == NC_OK ? "NewsCard" : "NOT a NewsCard");
	if (e) {
		not_ready_hint();
		return 1;
	}
	hex("mailbox", NC_SEM, 16);
	hex("reply", NC_RSP, 16);
	hex("command", NC_CMD, 16);
	hex("directory", NC_DIR, 16);
	/* flash: read the start three times, compare */
	for (run = 0; run < 32; run++) {
		u32 base = NC_FLASH + (u32)run * 256;

		for (k = 0; k < 3; k++) {
			if (bus_begin(base))
				return fail(NC_EBUS);
			for (i = 0; i < 256; i++)
				rd3[k][i] = bus_rd(base + i);
			bus_end();
		}
		for (i = 0; i < 256; i++) {
			u8 o = (u8)(rd3[0][i] | rd3[1][i] | rd3[2][i]);

			if (rd3[0][i] != rd3[1][i] || rd3[1][i] != rd3[2][i])
				diff++;
			for (k = 0; k < 3; k++) {
				u8 m = (u8)(o & ~rd3[k][i]);

				while (m) {
					drops += m & 1;
					m >>= 1;
				}
			}
		}
	}
	printf("flash       8 KB read 3 times: %d bytes differ, %d bits read as 0\n", diff, drops);
	if (diff || drops)
		puts("           try a different /SPEED=");
	return 0;
}

static int cmd_info(void)
{
	struct nc_status s;
	int e = nc_status_all(&s);

	if (e)
		return fail(e);
	printf("battery    %s\n", s.battery_ok ? "OK" : "LOW");
	printf("switch     %s\n", nc_switch_name(s.sw));
	printf("clock      %s\n", when(s.clock));
	printf("messages   %d, %d unread\n", s.total, s.unread);
	printf("free       %d blocks (%ld bytes)\n", s.free_blocks, (long)s.free_blocks * 64);
	printf("alerts     %s\n", s.alerts < 0 ? "?" : s.alerts ? "on" : "off");
	if (nc_event)
		puts("new page   yes");
	return 0;
}

static int cmd_list(void)
{
	int n = scan(), i, e;

	if (n < 0)
		return 1;
	printf("%d message(s)\n", n);
	for (i = 0; i < n; i++) {
		struct nc_msg *m = &msgs[i];

		e = nc_text(m, text, sizeof(text));
		printf("%3u %s %-10s %c ", m->seq, when(m->time), who(m), NC_MSG_READ(m) ? ' ' : '*');
		if (e < 0)
			printf("(%s)", nc_strerror(e));
		else
			put_text(stdout, text, 1, 36);
		putchar('\n');
	}
	return 0;
}

void show(FILE *f, const struct nc_msg *m)
{
	char *a, *b;

	fprintf(f, "Message:  %u (record 0x%05lx)\n", m->seq, (unsigned long)m->ptr);
	fprintf(f, "Received: %s\n", when(m->time));
	fprintf(f, "To:       %s\n", who(m));
	fprintf(f, "State:    %s%s\n", NC_MSG_READ(m) ? "read" : "new",
		m->errors ? ", with errors" : "");
	/* pages of the form from|subject|body */
	a = strchr(text, '|');
	b = a ? strchr(a + 1, '|') : NULL;
	if (a && b) {
		*a = *b = 0;
		fprintf(f, "From:     ");
		put_text(f, text, 1, 0);
		fprintf(f, "\nSubject:  ");
		put_text(f, a + 1, 1, 0);
		fprintf(f, "\n\n");
		put_text(f, b + 1, 0, 0);
		*a = *b = '|';
	} else {
		fputc('\n', f);
		put_text(f, text, 0, 0);
	}
	fputc('\n', f);
}

static int cmd_read(const char *arg, const char *file)
{
	int n = scan(), e;
	struct nc_msg *m;
	FILE *f = stdout;

	if (n < 0 || !(m = find(arg, n)))
		return 1;
	e = nc_text(m, text, sizeof(text));
	if (e < 0)
		return fail(e);
	if (file) {
		f = fopen(file, "a");
		if (!f) {
			perror(file);
			return 1;
		}
	}
	show(f, m);
	if (file) {
		fputc('\n', f);
		fclose(f);
		printf("message %u appended to %s\n", m->seq, file);
	}
	return 0;
}

static int cmd_change(const char *arg, int del)
{
	int n = scan(), e;
	struct nc_msg *m;
	char q[60];

	if (n < 0 || !(m = find(arg, n)))
		return 1;
	sprintf(q, "%s message %u from %s", del ? "Delete" : "Mark read", m->seq, when(m->time));
	if (!confirm(q))
		return 1;
	e = del ? nc_delete(m) : nc_mark_read(m);
	if (e)
		return fail(e);
	printf("message %u %s\n", m->seq, del ? "deleted" : "marked read");
	return 0;
}

static int cmd_settime(const char *d, const char *t)
{
	u8 c[5], now5[5];
	int y, mo, da, h, mi, e;
	char q[60];

	if (d) {
		if (!t || sscanf(d, "%d-%d-%d", &y, &mo, &da) != 3 ||
		    sscanf(t, "%d:%d", &h, &mi) != 2) {
			fputs("NEWSCARD: date and time as YYYY-MM-DD hh:mm\n", ERR);
			return 1;
		}
	} else {
		time_t now = time(NULL);
		struct tm *tm = localtime(&now);

		y = tm->tm_year + 1900;
		mo = tm->tm_mon + 1;
		da = tm->tm_mday;
		h = tm->tm_hour;
		mi = tm->tm_min;
	}
	if (!d && y < 1993) {
		fprintf(ERR, "NEWSCARD: the computer's clock says %04d-%02d-%02d; set it, or give the date:\n"
			"  NEWSCARD settime YYYY-MM-DD hh:mm\n", y, mo, da);
		return 1;
	}
	if (y < 1993 || y > 2092 || mo < 1 || mo > 12 || da < 1 || da > 31 ||
	    h < 0 || h > 23 || mi < 0 || mi > 59) {
		fputs("NEWSCARD: the card keeps dates from 1993 to 2092\n", ERR);
		return 1;
	}
	/* the card keeps a two-digit year */
	c[0] = (u8)(y % 100);
	c[1] = (u8)mo;
	c[2] = (u8)da;
	c[3] = (u8)h;
	c[4] = (u8)mi;
	sprintf(q, "Set the card clock to %s", when(c));
	if (!confirm(q))
		return 1;
	e = nc_set_clock(c);
	if (!e)
		e = nc_get_clock(now5);
	if (e)
		return fail(e);
	printf("card clock %s\n", when(now5));
	return 0;
}

static int cmd_alerts(const char *arg)
{
	int on, e;

	if (arg) {
		int want = !strcmp(arg, "on") || !strcmp(arg, "ON");

		if (!want && strcmp(arg, "off") && strcmp(arg, "OFF")) {
			fputs("NEWSCARD: alerts on or alerts off\n", ERR);
			return 1;
		}
		if (!confirm(want ? "Turn alerts on" : "Turn alerts off"))
			return 1;
		e = nc_set_alerts(want);
		if (e)
			return fail(e);
	}
	e = nc_alerts(&on);
	if (e)
		return fail(e);
	printf("alerts %s\n", on ? "on" : "off");
	return 0;
}

static int cmd_dump(const char *file, const char *off_s, const char *len_s)
{
	u32 off = off_s ? strtoul(off_s, NULL, 0) : 0;
	u32 len = len_s ? strtoul(len_s, NULL, 0) : 0x20000UL;
	u32 done = 0;
	FILE *f = fopen(file, "wb");

	if (!f) {
		perror(file);
		return 1;
	}
	while (done < len) {
		u32 o = off + done;
		unsigned n = (unsigned)(BUS_PAGE - (o & (BUS_PAGE - 1)));
		unsigned i;

		if (n > 512)
			n = 512;
		if (n > len - done)
			n = (unsigned)(len - done);
		if (bus_begin(o)) {
			fclose(f);
			return fail(NC_EBUS);
		}
		for (i = 0; i < n; i++)
			text[i] = (char)bus_rd(o + i);
		bus_end();
		fwrite(text, 1, n, f);
		done += n;
	}
	fclose(f);
	printf("%lu bytes from card offset 0x%lx saved to %s\n", (unsigned long)len,
	       (unsigned long)off, file);
	return 0;
}

/* run one step with stdout going to a file; returns the step's result */
int to_file(const char *name, int (*step)(void))
{
	FILE *f;
	int saved, r;

	fflush(stdout);
	f = fopen(name, "w");
	if (!f) {
		printf("cannot write %s\n", name);
		return 1;
	}
	saved = dup(fileno(stdout));
	dup2(fileno(f), fileno(stdout));
	fclose(f);
	r = step();
	fflush(stdout);
	dup2(saved, fileno(stdout));
	close(saved);
	return r;
}

int step_probe(void)
{
	int e;

	printf("NEWSCARD %s probe\n", VERSION);
	e = bus_open(&opts);
	if (e) {
		printf("NEWSCARD: %s (%s)\n", nc_strerror(e), bus_describe());
		return 1;
	}
	return cmd_probe();
}

static int step_info(void)
{
	return cmd_info();
}

static int step_list(void)
{
	return cmd_list();
}

/* no command: probe, then info and list if the card is a NewsCard */
static int check_card(void)
{
	int r;

	puts("NEWSCARD " VERSION ": checking the card (read-only)");
	r = to_file("PROBE.TXT", step_probe);
	printf("PROBE.TXT  %s\n", r ? "card not usable, see the file" : "NewsCard found");
	if (!r) {
		r = to_file("INFO.TXT", step_info);
		printf("INFO.TXT   %s\n", r ? "failed, see the file" : "written");
		if (!r) {
			r = to_file("LIST.TXT", step_list);
			printf("LIST.TXT   %s\n", r ? "failed, see the file" : "written");
		}
	}
	bus_close();
	return r;
}

/* /KEYS=@FILE: the key script from a file (DOS command lines are short) */
static const char *read_keys(const char *path)
{
	static char buf[2048];
	FILE *f = fopen(path, "r");
	size_t n, i;

	if (!f)
		return "";
	n = fread(buf, 1, sizeof(buf) - 1, f);
	fclose(f);
	buf[n] = 0;
	for (i = 0; i < n; i++)
		if (buf[i] == '\r' || buf[i] == '\n' || buf[i] == '\t')
			buf[i] = ' ';
	return buf;
}

int main(int argc, char **argv)
{
	const char *pos[5], *v;
	int npos = 0, i, e, r;
	unsigned a, b;
	char cmd[16];

	opts.socket = 1;
	opts.win[0] = 0;
	opts.win[1] = 1;
	opts.seg[0] = 0xD000;
	opts.seg[1] = 0xD400;
	opts.speed_ns = 300;
	opts.attr = -1;
	opts.ram_at = -1;
	parse_rics(getenv("NEWSCARD_RICS"));
	for (i = 1; i < argc; i++) {
		const char *s = argv[i];

		if (opt_match(s, "Y", &v) && !*v)
			yes = 1;
		else if (opt_match(s, "V", &v) && !*v)
			opts.verbose = 1;
		else if (opt_match(s, "SIM", &v) && *v)
			opts.sim_file = v;
		else if (opt_match(s, "RICS", &v))
			parse_rics(v);
		else if (opt_match(s, "SOCKET", &v) && *v)
			opts.socket = atoi(v);
		else if (opt_match(s, "SPEED", &v) && *v)
			opts.speed_ns = (unsigned)atoi(v);
		else if (opt_match(s, "KEYS", &v) && *v)
			keys_script = v[0] == '@' ? read_keys(v + 1) : v;
		else if (opt_match(s, "SHOT", &v) && *v)
			shot_file = v;
		else if (opt_match(s, "RAMAT", &v) && *v)
			opts.ram_at = (long)strtoul(v, NULL, 16);
		else if (opt_match(s, "TIMEOUT", &v) && *v)
			nc_timeout_ticks = atoi(v) * 18 + 1;
		else if (opt_match(s, "ATTR", &v) && *v)
			opts.attr = (int)strtoul(v, NULL, 16);
		else if (opt_match(s, "WIN", &v) && !parse_pair(v, &a, &b, 10)) {
			opts.win[0] = (int)a;
			opts.win[1] = (int)b;
		} else if (opt_match(s, "SEG", &v) && !parse_pair(v, &a, &b, 16)) {
			opts.seg[0] = a;
			opts.seg[1] = b;
		} else if (opt_match(s, "?", &v) || opt_match(s, "H", &v)) {
			usage();
			return 0;
		} else if (npos < 5)
			pos[npos++] = s;
	}
	if (!npos)
		return ui_run(keys_script, shot_file);
	for (i = 0; pos[0][i] && i < (int)sizeof(cmd) - 1; i++)
		cmd[i] = (char)tolower((unsigned char)pos[0][i]);
	cmd[i] = 0;
	if (!strcmp(cmd, "check"))
		return check_card();
	e = bus_open(&opts);
	if (opts.verbose)
		fprintf(ERR, "%s\n", bus_describe());
	if (e) {
		fprintf(ERR, "NEWSCARD: %s (%s)\n", nc_strerror(e), bus_describe());
		bus_close();
		return 1;
	}
	if (!strcmp(cmd, "probe")) {
		r = cmd_probe();
		bus_close();
		return r;
	}
	/* nothing is written to a card that is not a NewsCard */
	e = nc_identify(NULL, NULL, 0);
	if (e) {
		fail(e);
		not_ready_hint();
		bus_close();
		return 1;
	}
	if (!strcmp(cmd, "info"))
		r = cmd_info();
	else if (!strcmp(cmd, "list"))
		r = cmd_list();
	else if (!strcmp(cmd, "read") && npos == 2)
		r = cmd_read(pos[1], NULL);
	else if (!strcmp(cmd, "save") && npos == 3)
		r = cmd_read(pos[1], pos[2]);
	else if (!strcmp(cmd, "markread") && npos == 2)
		r = cmd_change(pos[1], 0);
	else if (!strcmp(cmd, "delete") && npos == 2)
		r = cmd_change(pos[1], 1);
	else if (!strcmp(cmd, "settime") && (npos == 1 || npos == 3))
		r = cmd_settime(npos == 3 ? pos[1] : NULL, npos == 3 ? pos[2] : NULL);
	else if (!strcmp(cmd, "alerts") && npos <= 2)
		r = cmd_alerts(npos == 2 ? pos[1] : NULL);
	else if (!strcmp(cmd, "dump") && npos >= 2)
		r = cmd_dump(pos[1], npos > 2 ? pos[2] : NULL, npos > 3 ? pos[3] : NULL);
	else {
		usage();
		r = 1;
	}
	bus_close();
	return r;
}
