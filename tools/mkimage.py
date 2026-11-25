#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0
# Copyright 2026 Vasyl Samoilov
"""Build a 128 KB NewsCard RAM image with test pages (store layout as in
PROTOCOL.md), for the simulated card: NEWSCARD /SIM=FILE ...

mkimage.py OUT [--page 'slot,func,YYYY-MM-DD hh:mm,text'] ... [--read N]
Without --page, a default set of test pages is used.
"""

import argparse
import struct
import sys

RAM = 0x20000
DIR, HEAP = 0x1400, 0x1C00
RAW = 0x03000000          # card pointers carry high bits; the host masks them
CIS = bytes.fromhex(
    "01000b00d4001b000000db000000fb00000076005a000400ff00120004003000"
    "000000000000fffffcffffffef7dffef1303434953180a000000000000000001"
    "a7153404014d6f746f726f6c612c20496e632e004e6577734361726420446174"
    "612052656365697665720056657220312e302e300000ff120400000002")

DEFAULT = [
    (0, 0, "2026-11-20 09:15", "Hello from the pager network"),
    (0, 1, "2026-11-21 12:30", "Alice|Lunch|Meet at noon at the usual place?"),
    (1, 0, "2026-11-22 18:05", "Server alert: disk 90% full on host build-2\nCheck /var"),
    (0, 3, "2026-11-23 07:45", "X" * 300),
    (1, 2, "2026-11-24 23:59", "Bob|Re: plans|Sounds good.\rSee you tomorrow."),
    (0, 0, "1993-01-01 00:08", "Received before the clock was set"),
]


def build(pages, read):
    img = bytearray(b"\xff" * RAM)
    for a in range(0, 0x1400):
        img[a] = 0
    img[0:0x80] = b"\xff" * 0x80
    img[0:len(CIS)] = CIS
    img[0xFFE] = 0x55
    heads = {}
    nxt = [HEAP]

    def alloc():
        a = nxt[0]
        nxt[0] += 64
        if nxt[0] > RAM:
            sys.exit("image full")
        img[a:a + 64] = b"\x00" * 64
        return a

    for seq, (slot, func, when, txt) in enumerate(pages, 1):
        data = txt.encode("latin1")
        src = slot * 8 + func
        nblk = max(1, (len(data) + 53) // 54)
        rec = alloc()
        blocks = [alloc() for _ in range(nblk)]
        for i, b in enumerate(blocks):
            struct.pack_into(">II", img, b, RAW | blocks[(i + 1) % nblk],
                             RAW | blocks[i - 1])
            chunk = data[i * 54:(i + 1) * 54]
            img[b + 10:b + 10 + len(chunk)] = chunk
        last = blocks[-1]
        k = len(data) - 54 * (nblk - 1)
        text_end = (RAW | last) + 10 + k - 1
        y, mo, d = (int(x) for x in when[:10].split("-"))
        h, mi = (int(x) for x in when[11:].split(":"))
        head = heads.get(src, 0xFFFFFFFF)
        struct.pack_into(">II", img, rec, head, RAW | (DIR + src * 0x80))
        if head != 0xFFFFFFFF:
            struct.pack_into(">I", img, (head & 0x3FFFC) + 4, RAW | rec)
        img[rec + 9] = seq & 0xFF
        struct.pack_into(">H", img, rec + 10, 0)
        struct.pack_into(">II", img, rec + 20, RAW | blocks[0], text_end)
        img[rec + 36] = src
        img[rec + 38] = 4 if seq in read else 0
        struct.pack_into(">H", img, rec + 40, nblk)
        img[rec + 45:rec + 50] = bytes([y % 100, mo, d, h, mi])
        heads[src] = RAW | rec
    for k in range(16):
        struct.pack_into(">I", img, DIR + k * 0x80, heads.get(k, 0xFFFFFFFF))
    return img


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("out")
    ap.add_argument("--page", action="append", default=[])
    ap.add_argument("--read", type=int, action="append", default=[])
    ap.add_argument("--empty", action="store_true")
    a = ap.parse_args()
    pages = []
    for p in a.page:
        slot, func, when, txt = p.split(",", 3)
        pages.append((int(slot), "ABCD".index(func.upper()), when, txt))
    if not pages and not a.empty:
        pages = DEFAULT
    with open(a.out, "wb") as f:
        f.write(build(pages, set(a.read)))


if __name__ == "__main__":
    main()
