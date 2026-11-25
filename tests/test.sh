#!/bin/sh
# SPDX-License-Identifier: GPL-2.0
# Copyright 2026 Vasyl Samoilov
# Tests of the host build against the simulated card.
set -u
cd "$(dirname "$0")/.."
D=$(pwd)
T=$(mktemp -d)
trap 'rm -rf "$T"' EXIT
NC="./newscard-host /SIM=$T/card.bin"
fails=0

check() { # description, command, expected substring
	out=$(eval "$2" 2>&1)
	case "$out" in
	*"$3"*) echo "ok   $1" ;;
	*) echo "FAIL $1"; echo "  expected: $3"; echo "$out" | sed 's/^/  | /'; fails=$((fails + 1)) ;;
	esac
}

python3 tools/mkimage.py "$T/card.bin"
check "info counts" "$NC info" "messages   6, 6 unread"
check "list" "$NC list" "6 message(s)"
check "list marks new" "$NC list" "  1 2026-11-20 09:15 slot 0 A   * Hello"
check "1993 sorts first" "$NC list | sed -n 2p" "  6 1993-01-01 00:08"
check "slot names" "$NC /RICS=1101,1407 list" "0001407 A"
check "from|subject|body" "$NC read 2" "Subject:  Lunch"
check "long page" "$NC read 4 | tail -1 | wc -c" "301"
check "CR as line break" "$NC read 5 | tail -1" "See you tomorrow."
check "unknown message" "$NC read 9" "no message 9"
check "prompt, answer no" "echo n | $NC markread 1; $NC info" "6, 6 unread"
check "mark read" "$NC /Y markread 1 && $NC info" "6, 5 unread"
check "mark read shows" "$NC list" "  1 2026-11-20 09:15 slot 0 A     Hello"
check "delete by record" "$NC /Y delete 0x01d00 && $NC info" "messages   5, 4 unread"
check "deleted gone" "$NC list" "5 message(s)"
check "free grows" "$NC info" "free       1921 blocks"
check "set time" "$NC /Y settime 2027-01-02 03:04" "card clock 2027-01-02 03:04"
check "year range" "$NC /Y settime 2093-01-01 00:00" "1993 to 2092"
check "alerts off" "$NC /Y alerts off" "alerts off"
check "alerts on" "$NC /Y alerts on" "alerts on"
check "save" "$NC save 2 $T/out.txt && cat $T/out.txt" "From:     Alice"
# the mailbox (below 0x1000) changes with every command
check "dump" "$NC dump $T/d.bin && cmp -i 4096 $T/d.bin $T/card.bin && echo same" "same"
check "probe" "$NC probe" "NewsCard Data Receiver: NewsCard"
if [ -f ../newscard-linux/newscard.py ]; then
	check "Linux tool agrees after delete" \
		"python3 ../newscard-linux/newscard.py list --image $T/card.bin" "5 message(s)"
fi
# a card that is not a NewsCard: nothing is written
python3 tools/mkimage.py "$T/card.bin"
printf 'NotACard' | dd of="$T/card.bin" bs=1 seek=$((0x53)) conv=notrunc 2>/dev/null
cp "$T/card.bin" "$T/before.bin"
check "other card refused" "$NC /Y delete 1" "not a NewsCard"
check "other card untouched" "cmp $T/card.bin $T/before.bin && echo same" "same"
# no command: the three files
python3 tools/mkimage.py "$T/card.bin"
check "check, screen" "(cd $T && $D/newscard-host /SIM=card.bin check)" "LIST.TXT   written"
check "check, PROBE.TXT" "cat $T/PROBE.TXT" "NewsCard Data Receiver: NewsCard"
check "check, INFO.TXT" "cat $T/INFO.TXT" "messages   6, 6 unread"
check "check, LIST.TXT" "cat $T/LIST.TXT" "6 message(s)"
check "check, card unchanged" "(cd $T && $D/newscard-host /SIM=card.bin check >/dev/null); $NC info" "6, 6 unread"
# full-screen program, driven by scripted keys
python3 tools/mkimage.py "$T/card.bin"
UI="$NC /SHOT=-"
check "ui: newest selected" "$UI '/KEYS=SHOT'" "Message 5"
check "ui: title" "$UI '/KEYS=SHOT'" "6 messages, 6 new"
check "ui: preview" "$UI '/KEYS=SHOT'" "From:     Bob"
check "ui: 1993 page first" "$UI '/KEYS=SHOT' | sed -n 3p" "*01-01 00:08 0A Received"
check "ui: view" "$UI '/KEYS=UP UP ENTER SHOT'" "Server alert: disk 90% full on host build-2"
check "ui: mark read" "$UI '/KEYS=HOME DOWN F4 SHOT' && $NC info" "6, 5 unread"
check "ui: mark read from viewer" "$UI '/KEYS=HOME DOWN DOWN ENTER r' && $NC info" "6, 4 unread"
check "ui: delete, no" "$UI '/KEYS=HOME F8 n' && $NC info" "messages   6"
check "ui: delete, yes" "$UI '/KEYS=HOME F8 y SHOT' && $NC info" "messages   5"
check "ui: delete keeps the cursor" "$UI '/KEYS=HOME F8 y SHOT'" "Message 2"
python3 tools/mkimage.py "$T/card.bin"
check "ui: save" "(cd $T && $D/newscard-host /SIM=card.bin '/KEYS=HOME DOWN F5 ENTER') && cat $T/NEWSCARD.TXT" "Hello from the pager network"
check "ui: clock menu" "$UI '/KEYS=F7 SHOT ESC'" "2  set the computer clock from the card"
check "ui: card clock from computer" "$UI '/KEYS=F7 1 SHOT'" "Card clock set to 20"
check "ui: both clocks typed" "$UI '/KEYS=F7 3 BKSP BKSP BKSP BKSP BKSP BKSP BKSP BKSP BKSP BKSP BKSP BKSP BKSP BKSP BKSP BKSP 2027-03-04 SPACE 05:06 ENTER SHOT'" "beep on  2027-03-04 05:06"
check "ui: computer clock from card" "$UI '/KEYS=F7 3 BKSP BKSP BKSP BKSP BKSP BKSP BKSP BKSP BKSP BKSP BKSP BKSP BKSP BKSP BKSP BKSP 2027-03-04 SPACE 05:06 ENTER F7 1 F7 SHOT ESC'" "Computer clock:  2027-03-04 05:06"
check "ui: set computer from card" "$UI '/KEYS=F7 2 SHOT'" "Computer clock set to 20"
check "ui: bad date" "$UI '/KEYS=F7 3 BKSP BKSP BKSP BKSP BKSP BKSP BKSP BKSP BKSP BKSP BKSP BKSP BKSP BKSP BKSP BKSP 1980-01-01 SPACE 00:00 ENTER SHOT'" "1993 to 2092"
check "ui: alerts" "$UI '/KEYS=F6 y SHOT'" "Alerts off."
check "ui: help" "$UI '/KEYS=F1 SHOT'" "F8, D or Del     delete it"
check "ui: not a card" "dd if=/dev/zero of=$T/card.bin bs=1 count=128 conv=notrunc 2>/dev/null; (cd $T && $D/newscard-host /SIM=card.bin /SHOT=- '/KEYS=SHOT ENTER'); cat $T/PROBE.TXT" "reads blank"
# a card that is slow or busy
python3 tools/mkimage.py "$T/card.bin"
check "card holds the mailbox" "NEWSCARD_SIM_FAULT=acq=1800 $NC info" "messages   6, 6 unread"
check "card takes the mailbox mid-read" "NEWSCARD_SIM_FAULT=own=40 $NC list" "6 message(s)"
check "late replies" "NEWSCARD_SIM_FAULT=late=300 $NC /Y markread 1 && $NC info" "6, 5 unread"
check "refused commands" "NEWSCARD_SIM_FAULT=nak $NC info" "refused the command, code 05"
check "card never answers" "NEWSCARD_SIM_FAULT=dead $NC /TIMEOUT=1 info" "does not answer"
# blank memory (no card, or window not mapped)
python3 tools/mkimage.py "$T/card.bin"
dd if=/dev/zero of="$T/card.bin" bs=1 count=128 conv=notrunc 2>/dev/null
check "blank memory" "$NC list" "reads blank"
check "blank memory, probe" "$NC probe" "reads blank"
# delete the head and the tail of a bucket
python3 tools/mkimage.py "$T/card.bin" --page '0,A,2026-01-01 00:00,one' \
	--page '0,A,2026-01-02 00:00,two' --page '0,A,2026-01-03 00:00,three'
check "delete newest" "$NC /Y delete 3 && $NC list" "2 message(s)"
check "delete oldest" "$NC /Y delete 1 && $NC list" "  2 2026-01-02 00:00 slot 0 A   * two"
check "Linux tool agrees" "python3 ../newscard-linux/newscard.py list --image $T/card.bin" "1 message(s)"
echo "$fails failure(s)"
[ "$fails" -eq 0 ]
