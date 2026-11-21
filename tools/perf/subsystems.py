"""Where the game thread's time goes, by subsystem (rung 1 of claude/native-next.md).

    python3 tools/perf/subsystems.py <cases folder or *.stacks.txt ...> [--top N] [--flame out.svg] [--rate 30|60]

Reads the folded stacks the dev build's PC sampler writes beside each replayed case (<case>.<30|60>.stacks.txt,
pc_sampler.cpp: guest functions outermost first as name@ADDRESS, the innermost host frame as ~module!function, a
first token [idle] for samples stopped in a kernel wait). Idle samples are left out: shares are of the busy samples.

Per rate it prints:
- subsystems: "own" = samples whose nearest classified frame (from the innermost guest function outward, helpers
  skipped) is in the subsystem; "under" = samples with any of its functions on the stack. Helper lines (libvu0, libc /
  maths) show their own time; that time also counts as the caller's "own".
- the hottest guest functions by own time (innermost guest frame) and by time under them, with their subsystem.
- where the innermost frame was: game code, the runtime's dispatch, guest memory access, the float slow path, libc...
--flame writes a flame graph (SVG, no scripts needed) of the busy samples, both rates merged unless --rate.

Subsystems come from tools/perf/subsystems.csv (rules) + idantony_modules.csv (import_idantony.py). Standard library
only.
"""
import argparse
import bisect
import csv
import glob
import html
import os
import re
import sys
from collections import Counter, defaultdict

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))
NAMES_INC = os.path.join(ROOT, "src", "kamite", "kamite_function_names.inc")


def load_names():
    starts, ends, names = [], [], []
    rx = re.compile(r'X\(0x([0-9A-Fa-f]+)u, 0x([0-9A-Fa-f]+)u, "([^"]+)"\)')
    with open(NAMES_INC, encoding="utf-8") as f:
        for line in f:
            m = rx.search(line)
            if m:
                starts.append(int(m.group(1), 16))
                ends.append(int(m.group(2), 16))
                names.append(m.group(3))
    return starts, ends, names


class Classifier:
    def __init__(self):
        self.address, self.ranges, self.modules, self.names, self.forced = {}, [], [], [], []
        with open(os.path.join(HERE, "subsystems.csv"), encoding="utf-8") as f:
            rows = [r for r in csv.reader(line for line in f if not line.startswith("#"))][1:]
        for kind, pattern, subsystem in rows:
            if kind == "address":
                self.address[int(pattern, 16)] = subsystem
            elif kind == "range":
                a, b = pattern.split("-")
                self.ranges.append((int(a, 16), int(b, 16), subsystem))
            elif kind == "module":
                self.modules.append((re.compile(pattern), subsystem))
            elif kind == "name":
                self.names.append((re.compile(pattern), subsystem))
            elif kind == "force":
                self.forced.append((re.compile(pattern), subsystem))
        self.module_of = {}
        path = os.path.join(HERE, "idantony_modules.csv")
        if os.path.exists(path):
            with open(path, encoding="utf-8") as f:
                for row in csv.reader(line for line in f if not line.startswith("#")):
                    if row and row[0].startswith("0x"):
                        self.module_of[int(row[0], 16)] = row[1].split("|")
        self.starts, self.ends, self.fnames = load_names()
        self.cache = {}

    def name_at(self, address):
        i = bisect.bisect_right(self.starts, address) - 1
        if i >= 0 and address < self.ends[i]:
            return self.fnames[i]
        return None

    def __call__(self, address, name):
        hit = self.cache.get(address)
        if hit is not None:
            return hit
        result = self.address.get(address)
        n = name if name and name != "func" and not name.startswith("func_") else self.name_at(address)
        if result is None and n:
            for rx, s in self.forced:
                if rx.search(n):
                    result = s
                    break
        if result is None:
            for a, b, s in self.ranges:
                if a <= address < b:
                    result = s
                    break
        if result is None:
            for module in self.module_of.get(address, ()):
                for rx, s in self.modules:
                    if rx.search(module):
                        result = s
                        break
                if result:
                    break
        if result is None:
            if n:
                for rx, s in self.names:
                    if rx.search(n):
                        result = s
                        break
        result = result or "unclassified"
        self.cache[address] = result
        return result


HOST_KINDS = [
    ("float slow path (ps2ee)", re.compile(r"ps2ee|ee_float", re.I)),
    ("runtime dispatch", re.compile(r"dispatch|callGuest|anyFunction|branchTarget|lookupFunction|Checkpoint|accountCycles|pushDispatch", re.I)),
    ("guest memory / IO", re.compile(r"PS2Memory|translateAddress|IORegister|Load128|Store128|readMem|writeMem|ps2_memory", re.I)),
    ("libc copy / fill", re.compile(r"mem(cpy|move|set)|memcmp", re.I)),
    ("kernel / sync", re.compile(r"ntdll|kernel|Semaphore|Mutex|Event|Sleep|Wait", re.I)),
]


def host_kind(token):
    for kind, rx in HOST_KINDS:
        if rx.search(token):
            return kind
    return "other host code"


GENERATED = re.compile(r"^~[^!]*!(.+)_0x([0-9a-fA-F]+)$")


def parse_frame(token):
    """(name, address) for a guest frame, None for host / idle. Recompiled code the sampler couldn't map through the
    table (a slot holding a hook's trampoline) arrives as a host frame named <name>_0x<address>: a guest frame too."""
    if token.startswith("~"):
        m = GENERATED.match(token)
        return (m.group(1), int(m.group(2), 16)) if m else None
    if token.startswith("["):
        return None
    name, _, addr = token.rpartition("@")
    try:
        return name, int(addr, 16)
    except ValueError:
        return None


def read_stacks(paths):
    """{rate: Counter(tuple(tokens) -> count)}, {rate: (files, samples, idle)}"""
    out = defaultdict(Counter)
    meta = defaultdict(lambda: [0, 0, 0])
    for path in paths:
        rate = 60 if ".60.stacks" in path else 30 if ".30.stacks" in path else 0
        with open(path, encoding="utf-8", errors="replace") as f:
            for line in f:
                if line.startswith("#"):
                    m = re.search(r"rate=(\d+)", line)
                    if m and rate == 0:
                        rate = int(m.group(1))
                    meta[rate][0] += 1
                    continue
                stack, _, count = line.rstrip("\n").rpartition(" ")
                if not stack:
                    continue
                n = int(count)
                tokens = tuple(stack.split(";"))
                meta[rate][1] += n
                if tokens[0] == "[idle]":
                    meta[rate][2] += n
                    continue
                out[rate][tokens] += n
    return out, meta


def analyse(stacks, classify):
    busy = sum(stacks.values())
    own, under = Counter(), Counter()
    fself, funder, fsub = Counter(), Counter(), {}
    kinds = Counter()
    for tokens, n in stacks.items():
        frames = [f for f in (parse_frame(t) for t in tokens) if f]
        innermost_host = tokens[-1] if tokens[-1].startswith("~") and not GENERATED.match(tokens[-1]) else None
        kinds[host_kind(innermost_host) if innermost_host else "game code"] += n
        if not frames:
            own["(no guest frame)"] += n
            continue
        subs = [classify(a, name) for name, a in frames]
        owner = next((s for s in reversed(subs) if not s.startswith("helper:")), None)
        if owner is None:  # only dispatchers / helpers on the stack: the host code they called owns it
            if any("test_mode" in t for t in tokens) or (innermost_host and re.search(r"fputs|free_base", innermost_host)):
                owner = "(replay tooling)"
            else:
                owner = "(host: " + (host_kind(innermost_host) if innermost_host else "game code") + ")"
        own[owner] += n
        innermost_sub = subs[-1]
        if innermost_sub.startswith("helper:"):
            own[innermost_sub] += n  # shown on its own line too (not part of the total of the others)
        for s in set(subs):
            under[s] += n
        name, a = frames[-1]
        fself[a] += n
        for (name2, a2), s in zip(frames, subs):
            fsub[a2] = (name2, s)
        for a2 in {a2 for _, a2 in frames}:
            funder[a2] += n
    return busy, own, under, fself, funder, fsub, kinds


def pct(n, total):
    return f"{100.0 * n / max(1, total):5.1f}%"


def report(rate, stacks, meta, classify, top):
    busy, own, under, fself, funder, fsub, kinds = analyse(stacks, classify)
    files, samples, idle = meta
    print(f"\n=== {rate} FPS: {files} files, {samples} samples, {idle} idle ({pct(idle, samples).strip()}), {busy} busy ===")
    print("\nsubsystem               own   under")
    rows = sorted(set(own) | set(under), key=lambda s: (s.startswith("helper:"), -own[s], -under[s]))
    for s in rows:
        print(f"  {s:20s} {pct(own[s], busy)} {pct(under[s], busy)}")
    print(f"\nhottest guest functions, own time (innermost guest frame):")
    for a, n in fself.most_common(top):
        name, s = fsub.get(a, ("?", "?"))
        print(f"  {pct(n, busy)}  under {pct(funder[a], busy)}  {name}@{a:08X}  [{s}]")
    print(f"\nguest functions by time under them (callers included):")
    for a, n in funder.most_common(top):
        name, s = fsub.get(a, ("?", "?"))
        print(f"  {pct(n, busy)}  own {pct(fself[a], busy)}  {name}@{a:08X}  [{s}]")
    print("\ninnermost frame:")
    for k, n in kinds.most_common():
        print(f"  {pct(n, busy)}  {k}")


PALETTE = ["#e69138", "#cc4125", "#6aa84f", "#3d85c6", "#a64d79", "#f1c232", "#45818e", "#8e7cc3", "#38761d",
           "#e06666", "#0b5394", "#b4a7d6", "#bf9000", "#76a5af", "#f6b26b", "#741b47"]


def flame(stacks, path, title, classify):
    """A plain SVG flame graph (outermost at the bottom), widths by samples, guest frames coloured by subsystem (the
    legend on top), host code grey; frames under 0.1% are merged away. Hover a frame for its name and share."""
    tree = {}
    total = 0
    colours = {}
    for tokens, n in stacks.items():
        total += n
        node = tree
        for raw in tokens:
            frame = parse_frame(raw)
            if frame:
                sub = classify(frame[1], frame[0])
                t = f"{frame[0]}@{frame[1]:08X}|{sub}"
            else:
                t = raw + "|"
            entry = node.setdefault(t, [0, {}])
            entry[0] += n
            node = entry[1]
    width, row = 1600, 16
    rects = []
    depth_max = [0]

    def walk(node, x, depth):
        for name, (n, children) in sorted(node.items(), key=lambda kv: -kv[1][0]):
            w = width * n / max(1, total)
            if w < width * 0.001:
                x += w
                continue
            rects.append((x, depth, w, name, n))
            depth_max[0] = max(depth_max[0], depth)
            walk(children, x, depth + 1)
            x += w

    walk(tree, 0.0, 0)
    subs = sorted({key.split("|")[1] for _, _, _, key, _ in rects} - {""})
    for i, sub in enumerate(subs):
        colours[sub] = ("#d0d0d0" if sub.startswith("helper:") else "#b7b7a4" if sub == "unclassified"
                        else PALETTE[i % len(PALETTE)])
    legend_rows = (len(subs) + 7) // 8
    top = 22 + legend_rows * 16
    height = top + (depth_max[0] + 2) * row
    out = [f'<svg xmlns="http://www.w3.org/2000/svg" width="{width}" height="{height}" font-family="monospace" font-size="11">',
           f'<rect width="100%" height="100%" fill="#fffdf6"/><text x="6" y="14">{html.escape(title)}</text>']
    for i, sub in enumerate(subs):
        lx, ly = 6 + (i % 8) * 198, 22 + (i // 8) * 16
        out.append(f'<rect x="{lx}" y="{ly}" width="12" height="11" fill="{colours[sub]}"/>'
                   f'<text x="{lx + 16}" y="{ly + 10}">{html.escape(sub)}</text>')
    for x, depth, w, key, n in rects:
        name, _, sub = key.partition("|")
        y = height - (depth + 1) * row
        colour = colours.get(sub, "#c9d8ef")
        label = name if len(name) * 6.6 < w else name[: max(0, int(w / 6.6) - 2)] + ".." if w > 30 else ""
        out.append(f'<g><title>{html.escape(name)} [{html.escape(sub or "host")}] {100.0 * n / total:.2f}%</title>'
                   f'<rect x="{x:.1f}" y="{y}" width="{max(w - 0.5, 0.5):.1f}" height="{row - 1}" fill="{colour}"/>'
                   f'<text x="{x + 3:.1f}" y="{y + 11}">{html.escape(label)}</text></g>')
    out.append("</svg>")
    with open(path, "w", encoding="utf-8") as f:
        f.write("\n".join(out))


def main(argv):
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("inputs", nargs="+")
    ap.add_argument("--top", type=int, default=30)
    ap.add_argument("--flame")
    ap.add_argument("--rate", type=int, choices=(30, 60))
    args = ap.parse_args(argv[1:])
    paths = []
    for item in args.inputs:
        paths += sorted(glob.glob(os.path.join(item, "**", "*.stacks.txt"), recursive=True)) if os.path.isdir(item) else [item]
    if not paths:
        print("no .stacks.txt files")
        return 1
    stacks, meta = read_stacks(paths)
    classify = Classifier()
    for rate in sorted(stacks):
        if args.rate and rate != args.rate:
            continue
        report(rate, stacks[rate], meta[rate], classify, args.top)
    if args.flame:
        merged = Counter()
        for rate, c in stacks.items():
            if not args.rate or rate == args.rate:
                merged.update(c)
        flame(merged, args.flame, f"GHR game thread, busy samples ({len(paths)} files"
              + (f", {args.rate} FPS)" if args.rate else ")"), classify)
        print(f"\nflame graph: {args.flame}")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
