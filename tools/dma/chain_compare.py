#!/usr/bin/env python3
"""Compare two DMA chain digests (<case>.<30|60>.chain.bin, written by dev builds' replays: src/kamite/native/
chain_digest.h; claude/native-next.md rung 3). Prints the chains per channel in each and the first records that differ
(test frame, channel, bytes, hash).

    python3 tools/dma/chain_compare.py <a.chain.bin> <b.chain.bin> [--max 10]
    python3 tools/dma/chain_compare.py <a.run1> <a.run2> <b.run1> <b.run2>    (two builds, both runs each)

Two builds: a chain counts as different only where both builds' run 1 = run 2 there (a few single chains differ
between two runs of one build: host-timing noise, 4 of ~45,000 in K's 24ba1d9 replays; claude/native-next.md).
"""
import struct
import sys

CHANNELS = {0x8: "VIF0", 0x9: "VIF1", 0xA: "GIF"}


def read(path):
    data = open(path, "rb").read()
    if data[:8] != b"GHRCHN1\0":
        sys.exit(f"{path}: not a chain digest")
    out = []
    for off in range(8, len(data) - 15, 16):
        frame, word, lo, hi = struct.unpack_from("<4I", data, off)
        out.append((frame, word >> 28, word & 0x0FFFFFFF, lo | hi << 32))
    return out


def summary(recs):
    per = {}
    for _, ch, size, _ in recs:
        n, b = per.get(ch, (0, 0))
        per[ch] = (n + 1, b + size)
    frames = recs[-1][0] + 1 if recs else 0
    return f"{len(recs)} chains over {frames} frames: " + ", ".join(
        f"{CHANNELS.get(ch, hex(ch))} {n} ({b / max(frames, 1) / 1024:.0f} KB a frame)" for ch, (n, b) in sorted(per.items()))


def main():
    args = [a for a in sys.argv[1:] if not a.startswith("--")]
    limit = 10
    if "--max" in sys.argv:
        limit = int(sys.argv[sys.argv.index("--max") + 1])
        args = [a for a in args if a != str(limit)]
    if len(args) == 4:
        a1, a2, b1, b2 = (read(x) for x in args)
        if not (len(a1) == len(a2) == len(b1) == len(b2)):
            print(f"chain counts differ: {len(a1)} {len(a2)} {len(b1)} {len(b2)}")
        noise = diff = 0
        for i, recs in enumerate(zip(a1, a2, b1, b2)):
            x1, x2, y1, y2 = recs
            if x1 != x2 or y1 != y2:
                noise += 1
                continue
            if x1 != y1:
                diff += 1
                if diff <= limit:
                    print(f"#{i}: frame {x1[0]} {CHANNELS.get(x1[1], x1[1])}: A {x1[2]} bytes {x1[3]:016x} | B {y1[2]} bytes {y1[3]:016x}")
        print(f"{diff} chains differ between the builds ({noise} skipped: run 1 != run 2 in a build)")
        return
    a, b = read(args[0]), read(args[1])
    print("A:", summary(a))
    print("B:", summary(b))
    shown = 0
    for i, (x, y) in enumerate(zip(a, b)):
        if x != y:
            print(f"#{i}: A frame {x[0]} {CHANNELS.get(x[1], x[1])} {x[2]} bytes {x[3]:016x} | "
                  f"B frame {y[0]} {CHANNELS.get(y[1], y[1])} {y[2]} bytes {y[3]:016x}")
            shown += 1
            if shown >= limit:
                break
    if len(a) != len(b):
        print(f"lengths differ: {len(a)} vs {len(b)} chains")
    if shown == 0 and len(a) == len(b):
        print("identical")


if __name__ == "__main__":
    main()
