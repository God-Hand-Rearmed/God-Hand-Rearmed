#!/usr/bin/env python3
"""One verdict line per case for a replay batch: the file a chat reads instead of fingerprints / snapdiffs / chains.

    python tools/fps60/replay_verdict.py              (in the play folder; Replay-Cases.cmd writes cases/verdict.txt)
    python tools/fps60/replay_verdict.py --promote    (make this build the reference: cases/ref/)

Against the reference (cases/ref/: a build's chain digests, both runs, and its fingerprint lines; set by --promote once
a build is accepted; without it the previous build's .prev files and fingerprint lines stand in):
  ram / csv   this build's last run = the reference's (fingerprints.txt hashes); csv "~": differs, but so do this
              build's own two runs (the host-timing columns), so it says nothing
  chain       the DMA chain digest (tools/dma/chain_compare.py): "=" or how many chains differ from which test frame on.
              A chain counts only where run 1 = run 2 in both builds (single chains differ between two runs of one
              build: host-timing noise)
  gs          the GS-input digest (gs.bin, rung 3 step b: what the GS received, per VU job), by the same rule: "=" or
              how many jobs differ from which test frame on; "-" while the reference has none (promote a batch that
              wrote gs.bin first). Live, chain.bin hashes the chain with the listed packets' bytes put back, so both
              chain and gs must stay "=".
  runs        this build's run 1 = run 2 (ram, csv, chain, gs); only with RUNS=2
  settings    (one block under the header) every GHR.ini setting whose value differs from the reference batch's
              (<case>.<rate>.settings.txt, cases/ref/ref-settings.txt): render settings are no case setting (Nov 28)
  snap        snap_compare.py's verdict against the previous build (fingerprints.txt), when it ran
Then the draw list's totals (this batch's *.drawlist.txt, live or shadow: src/kamite/native/draw_list.h), the natives' shadow
totals (this batch's *.native.txt: a build that shadows nothing called writes none, and the
older ones left in cases/ say nothing about it) and the commit (this batch's *.stacks.txt headers, written by every dev
replay). Anything that moves: list every commit in the
range (render, HUD and tools commits too, not only natives) before blaming one (Nov 23: a 16:9 HUD commit moved 8 cases'
chains and was blamed on func_00154240 for two builds).
"""
import glob
import os
import shutil
import struct
import sys

CASES = "cases"
REF = os.path.join(CASES, "ref")


MAGIC = {"chain": b"GHRCHN1\0", "gs": b"GHRGSD1\0"}
UNIT = {"chain": "chains", "gs": "jobs"}


def chain_records(path, kind="chain"):
    """16-byte records after the 8-byte magic: chain.bin (frame, channel | bytes, hash) or gs.bin (frame, packets,
    hash)."""
    try:
        data = open(path, "rb").read()
    except OSError:
        return None
    if data[:8] != MAGIC[kind]:
        return None
    return [struct.unpack_from("<4I", data, off) for off in range(8, len(data) - 15, 16)]


def chain_verdict(cur, cur1, ref, ref1, kind="chain"):
    a, b = chain_records(ref, kind), chain_records(cur, kind)
    if a is None or b is None:
        return f"{kind} -"
    a1, b1 = chain_records(ref1, kind) or a, chain_records(cur1, kind) or b
    if len(a) != len(b):
        return f"{kind} !={len(a)}->{len(b)} {UNIT[kind]}"
    diff = [b[i][0] for i in range(len(a)) if a[i] != b[i]
            and (i >= len(a1) or a1[i] == a[i]) and (i >= len(b1) or b1[i] == b[i])]
    if not diff:
        return f"{kind} ="
    return f"{kind} != {len(diff)} from frame {diff[0]} to {diff[-1]}"


def drawlist_totals(paths):
    """Sums the draw list shadow's reports (draw_list.cpp end()) into {key: number}; keys: registered, found, ...,
    parse-ok, live-start-same, ops-mscal, "microcode 0x..."; plus the first diff / parsefail lines."""
    tot, notes = {}, []

    def add(key, value):
        try:
            tot[key] = tot.get(key, 0) + int(value)
        except ValueError:
            pass

    for p in paths:
        for line in open(p, encoding="utf-8", errors="replace"):
            f = line.split()
            if not f or line.startswith("#"):
                continue
            if f[0] in ("diff", "parsefail"):
                if len(notes) < 8:
                    notes.append(os.path.basename(p).split(".case")[0][:32] + ": " + line.strip())
            elif f[0] == "microcode" and len(f) == 3:
                add("microcode " + f[1], f[2])
            elif f[0] in ("registered", "segments"):
                for key, value in zip(f[0::2], f[1::2]):
                    add(key, value)
            elif f[0] == "copiedby" and len(f) == 5:  # copiedby WHY CALLER PACKETS BYTES
                add(f"copiedby {f[1]} {f[2]} n", f[3])
                add(f"copiedby {f[1]} {f[2]} b", f[4])
            elif f[0] == "copiedafter" and len(f) == 5:  # copiedafter HEAD BYTES caller CALLER
                add(f"copiedafter {f[1]} {f[4]}", f[2])
            elif f[0] in ("parse", "ops", "cache", "kinds", "inplace", "rewrites", "executor", "uploads", "copiedwhy", "copiedgap", "copiedfirstage",
                          "copiedops", "copiedunlisted"):
                for key, value in zip(f[1::2], f[2::2]):
                    add(f"{f[0]}-{key}", value)
            elif f[0] == "live":  # live start same N differ N end same N differ N
                for k in range(1, len(f) - 4, 5):
                    add(f"live-{f[k]}-{f[k + 1]}", f[k + 2])
                    add(f"live-{f[k]}-{f[k + 3]}", f[k + 4])
                if "marked" in f[:-1]:
                    add("live-marked", f[f.index("marked") + 1])
    return tot, notes


def fingerprints(path):
    """{build: {(case, run): {field: hash}}, ...} in file order, plus snap verdicts per build."""
    builds, verdicts, order, cur = {}, {}, [], None
    try:
        lines = open(path, encoding="utf-8", errors="replace").read().splitlines()
    except OSError:
        return builds, verdicts, order
    for line in lines:
        parts = line.split(";")
        if len(parts) > 3 and parts[1].startswith("run "):
            cur = parts[0]
            if cur not in builds:
                builds[cur], verdicts[cur] = {}, {}
                order.append(cur)
            fields = dict(p.split("=", 1) for p in parts[3:] if "=" in p)
            builds[cur][(parts[2], parts[1][4:])] = fields
        elif cur and ".case " in line and ": " in line:
            name, text = line.split(": ", 1)
            verdicts[cur][name.strip()] = text
    return builds, verdicts, order


def last_run(build, case):
    runs = sorted(r for (c, r) in build if c == case)
    return build.get((case, runs[-1])) if runs else None


def this_batch(path, case, rate):
    """A per-replay file the last replay of <case> at <rate> wrote, not one an older build left behind (Nov 23: 2b12c70
    shadowed nothing called, wrote no .native.txt, and the 13:33 build's showed up as its totals and commit). The replay
    writes its .csv at the same end, so anything a batch older than that is stale."""
    try:
        return os.path.getmtime(path) >= os.path.getmtime(os.path.join(CASES, f"{case}.{rate}.csv")) - 120
    except OSError:
        return False


def batch_files(cases, suffix):
    out, stale = [], 0
    for case in cases:
        for rate in ("30", "60"):
            p = os.path.join(CASES, f"{case}.{rate}.{suffix}")
            if os.path.exists(p):
                if this_batch(p, case, rate):
                    out.append(p)
                else:
                    stale += 1
    return out, stale


def head_commit(path):
    try:
        with open(path, encoding="utf-8", errors="replace") as f:
            head = f.readline()
    except OSError:
        return None
    return head.split("commit=")[1].split()[0] if "commit=" in head else None


def batch_settings(paths):
    """The GHR.ini settings a batch's replays booted with (<case>.<rate>.settings.txt, test_mode.cpp keepBootSettings):
    {name: {values}} from the "ini: NAME=VALUE" lines, and the names every replay sets itself ("case:" / "replay:"
    lines: the recorder's or a pin, so GHR.ini's value has no effect there)."""
    ini, overridden = {}, None
    for p in paths:
        mine = set()
        with open(p, encoding="utf-8", errors="replace") as f:
            for line in f:
                line = line.rstrip("\n")
                kind, _, rest = line.partition(": ")
                name, eq, value = rest.partition("=")
                if not eq:
                    continue
                if kind == "ini":
                    ini.setdefault(name, set()).add(value)
                elif kind in ("case", "replay"):
                    mine.add(name)
        overridden = mine if overridden is None else overridden & mine
    return ini, overridden or set()


def settings_lines(cur_paths):
    """Every GHR.ini setting whose value differs from the reference batch's (cases/ref/ref-settings.txt, written by
    --promote): K, Nov 28, Cash 115 -> 130 moved 7 cases' chains and was taken for C3's natives for 3 batches."""
    ref_path = os.path.join(REF, "ref-settings.txt")
    if not cur_paths:
        return ["settings: this batch wrote no .settings.txt (a build older than the settings check)"]
    if not os.path.exists(ref_path):
        return ["settings: the reference has none (promote a batch that wrote .settings.txt)"]
    cur, overridden = batch_settings(cur_paths)
    ref, _ = batch_settings([ref_path])
    show = lambda v: "|".join(sorted(v)) if v else "(not set)"
    out = []
    for name in sorted(set(cur) | set(ref)):
        a, b = ref.get(name, set()), cur.get(name, set())
        if a != b:
            note = "  (no effect: every replay sets it)" if name in overridden else ""
            out.append(f"  {name}: {show(a)} -> {show(b)}{note}")
    if not out:
        return ["settings (GHR.ini) = the reference's"]
    return ["settings (GHR.ini) that differ from the reference's: read these before blaming a commit"] + out


def main():
    promote = "--promote" in sys.argv
    cases = sorted(os.path.basename(p) for p in glob.glob(os.path.join(CASES, "*.case")))
    builds, verdicts, order = fingerprints(os.path.join(CASES, "fingerprints.txt"))
    if not order:
        sys.exit("no fingerprints yet")
    now = order[-1]
    if promote:
        # Copy over the old reference first, then drop what no case of the batch wrote: a folder where deleting isn't
        # allowed (Claude's shell on K's PC, Nov 23) still gets a whole new reference, and leftovers of cases no longer
        # in cases/ are never read (the verdict walks cases/*.case).
        os.makedirs(REF, exist_ok=True)
        kept = {"ref-fingerprints.txt"}
        for case in cases:
            for rate in ("30", "60"):
                for suffix in ("chain.bin", "chain.run1.bin", "gs.bin", "gs.run1.bin"):
                    p = os.path.join(CASES, f"{case}.{rate}.{suffix}")
                    if os.path.exists(p):
                        shutil.copy2(p, REF)
                        kept.add(os.path.basename(p))
        with open(os.path.join(CASES, "fingerprints.txt"), encoding="utf-8", errors="replace") as f, \
                open(os.path.join(REF, "ref-fingerprints.txt"), "w", encoding="utf-8") as out:
            out.writelines(line for line in f if line.startswith(now + ";"))
        # The batch's GHR.ini settings, one "ini: NAME=VALUE" line each (settings_lines compares the next batches').
        settings, _ = batch_files(cases, "settings.txt")
        if settings:
            ini, _ = batch_settings(settings)
            with open(os.path.join(REF, "ref-settings.txt"), "w", encoding="utf-8") as out:
                for name in sorted(ini):
                    for value in sorted(ini[name]):
                        out.write(f"ini: {name}={value}\n")
            kept.add("ref-settings.txt")
        left = []
        for p in glob.glob(os.path.join(REF, "*")):
            if os.path.basename(p) not in kept:
                try:
                    os.remove(p)
                except OSError:
                    left.append(os.path.basename(p))
        print(f"reference = {now} ({len(kept - {'ref-fingerprints.txt', 'ref-settings.txt'})} chain / gs files"
              f"{', its settings' if 'ref-settings.txt' in kept else ''})")
        if left:
            print(f"left in cases/ref (no case of this batch; never read): {len(left)} files")
        return
    ref_fp = os.path.join(REF, "ref-fingerprints.txt")
    if os.path.exists(ref_fp):
        rb, _, ro = fingerprints(ref_fp)
        ref_name, ref_build = ro[0], rb[ro[0]]
    else:
        ref_name = order[-2] if len(order) > 1 else None
        ref_build = builds.get(ref_name, {})
    # this batch's own files: a case no longer in cases/ leaves its last ones behind (2026-11-03_14-53-38's), and a build
    # that shadows nothing called writes no .native.txt, so the older build's stay (Nov 23, 2b12c70)
    natives, stale_natives = batch_files(cases, "native.txt")
    stacks, _ = batch_files(cases, "stacks.txt")
    commits = {c for c in map(head_commit, stacks or natives) if c}
    print(f"build {now}  commit {', '.join(sorted(commits)) or '?'}")
    print(f"reference {ref_name or '-'} ({'cases/ref' if os.path.exists(ref_fp) else 'the previous build'})")
    print("list every commit between the reference's and this one before attributing anything that moved")
    settings, _ = batch_files(cases, "settings.txt")
    for line in settings_lines(settings):
        print(line)
    print()
    moved = 0
    for case in cases:
        cur = last_run(builds[now], case)
        ref = last_run(ref_build, case) if ref_build else None
        run1 = builds[now].get((case, "1"))
        for rate in ("30", "60"):
            out = []
            for field in ("ram.bin", "csv"):
                k = f"{rate}.{field}"
                if cur and ref and k in cur and k in ref:
                    noisy = field == "csv" and run1 is not None and run1 is not cur and run1.get(k) != cur.get(k)
                    out.append(f"{field.split('.')[0]} " + ("~" if noisy and cur[k] != ref[k] else
                                                           "=" if cur[k] == ref[k] else "!="))
            base = os.path.join(CASES, f"{case}.{rate}")
            if os.path.exists(os.path.join(REF, f"{case}.{rate}.chain.bin")):
                rbase = os.path.join(REF, f"{case}.{rate}")
                cv = chain_verdict(base + ".chain.bin", base + ".chain.run1.bin", rbase + ".chain.bin",
                                   rbase + ".chain.run1.bin")
            else:
                cv = chain_verdict(base + ".chain.bin", base + ".chain.run1.bin", base + ".chain.prev.bin", "")
            out.append(cv)
            if os.path.exists(os.path.join(REF, f"{case}.{rate}.gs.bin")):
                rbase = os.path.join(REF, f"{case}.{rate}")
                out.append(chain_verdict(base + ".gs.bin", base + ".gs.run1.bin", rbase + ".gs.bin", rbase + ".gs.run1.bin",
                                         "gs"))
            else:
                out.append(chain_verdict(base + ".gs.bin", base + ".gs.run1.bin", base + ".gs.prev.bin", "", "gs"))
            if run1 and cur and run1 is not cur:
                fields = [f for f in ("ram.bin", "csv", "chain.bin", "gs.bin") if f"{rate}.{f}" in run1]
                same = [f for f in fields if run1.get(f"{rate}.{f}") == cur.get(f"{rate}.{f}")]
                out.append("runs " + ("=" if len(same) == len(fields) else "differ: " + ",".join(
                    f for f in fields if f not in same)))
            v = verdicts[now].get(f"{case} {rate}")
            if v:
                out.append("snap " + ("=" if v.startswith("same") else v.split("(")[0].replace("DIFFERS from the previous build in ", "!= ").strip()))
            line = "  ".join(out)
            if any(o.startswith(("ram !=", "csv !=", "chain !=", "gs !=")) for o in out):
                moved += 1
            print(f"{case[:48]:48} {rate}  {line}")
    print()
    drawlists, stale_drawlists = batch_files(cases, "drawlist.txt")
    if drawlists:
        t, notes = drawlist_totals(drawlists)
        g = lambda k: t.get(k, 0)
        whyb = sum(g(f'copiedwhy-{k}b') for k in ("first", "reused", "marked", "shadow"))
        gaps = sum(g(f'copiedgap-b{k}') for k in ("16", "64", "1k", "16k", "big"))
        # packets' bytes = in place + the registered ones still copied (chainbytes = copied + in place)
        share = 100.0 * (g("bytes") - whyb) / g("chainbytes") if g("chainbytes") else 0.0
        modes = sorted({open(p, encoding="utf-8", errors="replace").readline().split()[4] for p in drawlists})
        print(f"draw list {'/'.join(modes)} ({len(drawlists)} replays): registered {g('registered')}, walked "
              f"{g('found')}, never walked {g('unseen')}, head / tail changed {g('tagchanged')}, table full {g('full')}")
        print(f"  packets {g('segments')} (unclosed {g('unclosed')}), {share:.1f}% of the VIF1 chain bytes"
              f"{' read in place, not copied' if modes == ['live'] else ' in packets'}; VIF commands on the edges: ok "
              f"{g('parse-ok')}, fail {g('parse-fail')}{'  <-- PARSE FAILURES' if g('parse-fail') or g('unclosed') else ''}")
        if g("cache-hits") or g("cache-misses"):
            hm = g("cache-hits") + g("cache-misses")
            print(f"  cached decode: {g('cache-hits')} packets jumped head to tail, {g('cache-misses')} walked "
                  f"({100.0 * g('cache-hits') / hm:.1f}% jumped)")
        if g('kinds-shadow'):
            print(f"  shadow packets (copied, checked): {g('kinds-shadow')} walked, {g('kinds-registeredshadow')} "
                  f"registered; live {g('kinds-live')}")
        if g('kinds-queuelive') or g('kinds-queuereused') or g('kinds-queuenew'):
            print(f"  queue natives' packets: {g('kinds-queuelive')} in place (double-buffered), {g('kinds-queuereused')} "
                  f"copied (one buffer for both halves), {g('kinds-queuenew')} copied (first frame); a diff on a head "
                  f"with halves != 3 would mean the rule missed one")
        if g('inplace-same') or g('inplace-differ'):
            print(f"  read in place, bytes at job start = job end: {g('inplace-same')} same / {g('inplace-differ')} differ"
                  f"{'  <-- DIFFER (rewritten while the VU read it: see the diff inplace lines)' if g('inplace-differ') else ''}")
        if g('rewrites-marks') or g('rewrites-waits'):
            print(f"  both-halves writers (0x155D60 changing the half not being built): copied for good {g('rewrites-marks')}, "
                  f"waits for the VU {g('rewrites-waits')}, timeouts {g('rewrites-timeouts')}"
                  f"{'  <-- TIMEOUTS' if g('rewrites-timeouts') else ''}")
        print(f"  VIF executor: packets whose rest was gathered {g('executor-gathers')} (0 expected: every split is VIFcode | payload)")
        print(f"  texture uploads queued with nothing listing them: {g('uploads-unlisted')} (0 expected since the Txn lists its own)"
              f"{'  <-- UNLISTED' if g('uploads-unlisted') else ''}")
        if "shadow" in modes or g('kinds-shadow'):
            print(f"  bytes in RAM = the walker's copy: job start {g('live-start-same')} same / {g('live-start-differ')} "
                  f"differ, job end {g('live-end-same')} same / {g('live-end-differ')} differ (marked, not compared: "
                  f"{g('live-marked')})"
                  f"{'  <-- DIFFER' if g('live-start-differ') or g('live-end-differ') else ''}")
        print(f"  ops: MSCAL {g('ops-mscal')}, MSCNT {g('ops-mscnt')}, MPG {g('ops-mpg')} (packets without one "
              f"{g('ops-nompg')}), DIRECT {g('ops-direct')}, UNPACK {g('ops-unpack')}; unpack before STCYCL "
              f"{g('parse-unpackbeforecycle')}, masked {g('parse-maskedunpacks')}")
        copied = whyb + gaps + g('copiedunlisted-bytes')
        if copied:
            mb = lambda b: f"{b / 1048576.0:.2f} MB"
            pc = lambda b: f"{100.0 * b / copied:.1f}%"
            total = g('chainbytes') + g('copiedunlisted-bytes')
            print(f"  copied on the game thread: {mb(copied)} = {100.0 * copied / total:.1f}% of {mb(total)} chain bytes "
                  f"(% below: of the copied bytes; chains without packets {mb(g('copiedunlisted-bytes'))} in "
                  f"{g('copiedunlisted-items')} chains)")
            for label, k in (("first frames (queue rule)", "first"), ("one buffer for both halves / twice", "reused"),
                             ("both-halves writers' marks", "marked"), ("shadowed sources", "shadow")):
                print(f"    {label:36} {g(f'copiedwhy-{k}n'):>7} packets  {mb(g(f'copiedwhy-{k}b')):>9}  "
                      f"{pc(g(f'copiedwhy-{k}b'))}")
            print(f"    {'between packets (not registered)':36} {sum(g(f'copiedgap-n{k}') for k in ('16', '64', '1k', '16k', 'big')):>7}"
                  f" runs     {mb(gaps):>9}  {pc(gaps)}: <=16 B {g('copiedgap-n16')}, <=64 B {g('copiedgap-n64')}, "
                  f"<=1 KB {g('copiedgap-n1k')}, <=16 KB {g('copiedgap-n16k')} ({mb(g('copiedgap-b16k'))}), "
                  f"bigger {g('copiedgap-nbig')} ({mb(g('copiedgap-bbig'))})")
            if g('copiedfirstage-n30') or g('copiedfirstage-n300') or g('copiedfirstage-nlater'):
                print(f"    first frames by when (their half's generation): < 30 {g('copiedfirstage-n30')} packets "
                      f"{mb(g('copiedfirstage-b30'))}, < 300 {g('copiedfirstage-n300')} {mb(g('copiedfirstage-b300'))}, "
                      f"later {g('copiedfirstage-nlater')} {mb(g('copiedfirstage-blater'))}; reuse record resets "
                      f"{g('copiedfirstage-reuseclears')}")
            whyname = {"1": "first", "2": "both halves", "3": "marked", "4": "shadow"}
            by = sorted(((t[k], k.split()[1], k.split()[2]) for k in t if k.startswith("copiedby ") and k.endswith(" b")),
                        reverse=True)[:10]
            if by:
                print("    copied packets by why and queue caller (bytes, packets): " +
                      ", ".join(f"{whyname.get(w, w)} {c} {mb(b)} {g(f'copiedby {w} {c} n')}" for b, w, c in by))
            ops = lambda pre: ", ".join(f"{n} {mb(g(f'{pre}-{k}'))}" for n, k in (("DIRECT", "direct"), ("UNPACK", "unpack"),
                                        ("MPG", "mpg"), ("other", "other"), ("unparsed", "unparsed")))
            print(f"    between packets by command: {ops('copiedops')}")
            if g('copiedunlisted-bytes'):
                print(f"    chains without packets by command: {ops('copiedunlisted')}")
            after = sorted(((v, k.split()[1], k.split()[2]) for k, v in t.items() if k.startswith("copiedafter ")),
                           reverse=True)[:8]
            if after:
                print("    biggest runs (> 1 KB) after packet (head, its queue caller): " +
                      ", ".join(f"{h} {c} {mb(v)}" for v, h, c in after))
        micro = sorted((k.split()[1], v) for k, v in t.items() if k.startswith("microcode "))
        if micro:
            print("  microcode uploads (source: count): " + ", ".join(f"{k}: {v}" for k, v in micro))
        for n in notes:
            print("  " + n)
        if stale_drawlists:
            print(f"  ({stale_drawlists} older .drawlist.txt left out)")
        print()
    totals = {}
    for p in natives:
        for line in open(p, encoding="utf-8", errors="replace"):
            if line.startswith("#"):
                continue
            f = line.split()
            if len(f) < 9:
                continue
            t = totals.setdefault(f[0], [0] * 6)
            for i, col in enumerate((2, 3, 4, 5, 6, 8)):
                t[i] += int(f[col])
    print("shadowed natives: calls checked matched declined skipped mismatches")
    for name, t in sorted(totals.items()):
        print(f"  {name:28} {t[0]:>9} {t[1]:>9} {t[2]:>9} {t[3]:>6} {t[4]:>6} {t[5]:>5}{'  <-- MISMATCHES' if t[5] else ''}")
    if not totals:
        print("  none: this build shadowed nothing that was called")
    if stale_natives:
        print(f"  ({stale_natives} older .native.txt left out: an earlier batch's)")
    print()
    print(f"{moved} case/rate lines with ram, csv, chain or gs != the reference")
    print("(csv ~ = differs but this build's own two runs differ too: host-timing columns; a few snap words per case")
    print(" and single noisy chains are usual; a block of ~1000 words or chains from a frame on is not)")


if __name__ == "__main__":
    main()
