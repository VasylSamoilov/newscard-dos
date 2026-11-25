# NEWSCARD: Motorola NewsCard for DOS and the HP 200LX

A DOS program for the Motorola NewsCard Data Receiver, a POCSAG pager on
a PC Card. Apple sold it as the Newton Messaging Card. It reads, saves,
marks read and deletes pages, sets the card's clock, and switches the
alert beep on and off. It reaches the card through PCMCIA Socket Services,
as built into the HP 100LX/200LX.

Status: works on an HP 200LX: the full-screen program and the command
line read the card, mark messages read, delete them and switch alerts.
Setting the computer clock is tested in DOSBox-X only. Other DOS laptops
with Socket Services 1.x should work the same way but are untested (try
`/ATTR=` and `/SEG=` if `probe` fails).

## The program

`NEWSCARD` without a command opens the full-screen program: the messages
on the left, the selected message on the right, the function keys at the
bottom.

| key | letter | action |
|---|---|---|
| Up, Down, PgUp, PgDn, Home, End | | choose a message |
| F1 | H | help |
| F2 | I | card status |
| F3, Enter | V | view the whole message |
| F4 | R | mark read |
| F5 | S | append to a file |
| F6 | A | alert beep on/off |
| F7 | C | set the card clock |
| F8, Del | D | delete |
| F9 | L | read the card again |
| F10 | Q | quit |

If the card cannot be used, the program says why and writes `PROBE.TXT`.

## Commands

```text
NEWSCARD check              check the card: PROBE.TXT, INFO.TXT, LIST.TXT
NEWSCARD probe              check the card, read-only
NEWSCARD info               battery, switch, clock, counts, alerts
NEWSCARD list               messages, oldest first (* = new)
NEWSCARD read N             show message N
NEWSCARD save N FILE        append message N to FILE
NEWSCARD markread N         mark message N read
NEWSCARD delete N           delete message N
NEWSCARD settime [D T]      set the card clock (default: the DOS clock)
NEWSCARD alerts on|off      alert beep on or off
NEWSCARD dump FILE          save the card's RAM (128 KB)
```

`N` is the message number from `list`, or the record offset (`0x...`) if
two messages have the same number. Commands that change the card ask
first; `/Y` skips the question. `/RICS=1234567,7654321` (or the
environment variable `NEWSCARD_RICS`) names the card's two address slots.
Pages of the form `from|subject|body` are shown with From and Subject.

Before writing anything to the card, NEWSCARD checks that the card's CIS
names the NewsCard. Other cards are left alone. Where the card's RAM
appears differs between hosts (card offset 0 on some, 0x02000000 on
others); NEWSCARD finds it by reading, and `/RAMAT=` sets it.

## Options

| option | default | meaning |
|---|---|---|
| `/SOCKET=n` | 1 | Socket Services socket |
| `/WIN=a,b` | 0,1 | two 16 KB memory windows |
| `/SEG=aaaa,bbbb` | D000,D400 | their host segments |
| `/SPEED=ns` | 300 | access time: 100, 150, 200, 250, 300 or 600 |
| `/ATTR=hh` | 04 (HP), 05 | window attribute byte |
| `/RAMAT=hhhhhhhh` | found | card offset where the card's RAM appears |
| `/SIM=FILE` | | use a RAM image as a simulated card |
| `/KEYS="..."` | | key script for tests (`DOWN F4 y SHOT`), `@FILE` |
| `/SHOT=FILE` | | where `SHOT` in the key script writes the screen |
| `/V` | | verbose |

## First test on an HP 200LX

1. Back up drive C:.
2. Copy `NEWSCARD.EXE` to C:. The NewsCard uses the only slot, so copy it
   over the serial cable, or from a memory card before inserting the
   NewsCard.
3. Leave the System Manager for the DOS prompt (More, Application,
   Terminate All). If that leaves you inside the System Manager, use its
   DOS application instead.
4. Insert the NewsCard and type `NEWSCARD check`. It writes `PROBE.TXT`,
   `INFO.TXT` and `LIST.TXT` in the current directory and changes nothing
   on the card.
5. The card needs its own battery: without it the card reports not
   ready and only its flash answers, so NEWSCARD does not find it.
6. If `probe` reports bytes that differ or bits read as 0, try
   `/SPEED=600`. If it says the memory reads blank or the card is not a
   NewsCard, keep `PROBE.TXT`: it shows what Socket Services reported and
   the first bytes seen through the window.

## Building

`NEWSCARD.EXE` in the repository is built from these sources; rebuild it
with `make` before committing a change. Needs Open Watcom v2 (`$WATCOM`,
default `~/opt/watcom`).

```text
make            NEWSCARD.EXE (DOS, 8086, small model)
make host       Linux build with the simulated card only
make test       tests against the simulated card
```

`tools/mkimage.py OUT` builds a RAM image with test pages for `/SIM=`.
The simulated card answers the mailbox commands like the card does, and
writes changes back to the image. `NEWSCARD_SIM_FAULT` makes it slow,
busy or unresponsive (see `bus_sim.c`).

## License

GNU General Public License, version 2 or (at your option) any later
version (GPL-2.0-or-later, `LICENSE`). Copyright 2026 Vasyl Samoilov.
