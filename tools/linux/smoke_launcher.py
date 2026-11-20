"""Starts GHR on a virtual screen and checks that its launcher comes up and draws.

    python -I tools/linux/smoke_launcher.py --out shot.png [--seconds 60] -- <command that starts GHR> [args]

GHR's log goes to its play folder's logs/kamite.log in a release build (GHR.ini LogFile), not to the terminal:
--log names that file, which is followed as well as the program's own output.

The command is the AppImage, the tar.gz folder's GHR, or GHR behind an explicit loader. With no DISPLAY set, an Xvfb
(1280x800, the Steam Deck's screen) is started for the run. GHR must not be given a disc image: without one, a release
build opens the launcher and waits there.

Passes when, within --seconds: GHR logs "launcher: shown", is still running a few seconds later (the saloon loads its
images on threads), and a screenshot of the screen is not one flat colour. The screenshot (PNG) and GHR's output
(<out>.log) are kept for the CI artifacts. Exit code 0 = passed, 1 = failed (the reason is printed).

Needs xwd (x11-apps) and, without a DISPLAY, Xvfb. The image check is done here (an XWD reader and a PNG writer in a
few lines), so ImageMagick is not needed.
"""
import argparse
import os
import shutil
import signal
import struct
import subprocess
import sys
import tempfile
import threading
import time
import zlib
from pathlib import Path

READY = "launcher: shown"
SETTLED = ("launcher: saloon zoom ready", "launcher: no AVIF decoder")


def read_xwd(data: bytes):
    """(width, height, rows of (r, g, b) tuples) from an XWD dump of a 24/32-bit TrueColor screen."""
    header_size, version, _, depth, width, height = struct.unpack_from(">6I", data, 0)
    if version != 7:
        raise ValueError(f"not an XWD v7 file (version {version})")
    (byte_order,) = struct.unpack_from(">I", data, 28)
    (bits_per_pixel,) = struct.unpack_from(">I", data, 44)
    (bytes_per_line,) = struct.unpack_from(">I", data, 48)
    red_mask, green_mask, blue_mask = struct.unpack_from(">3I", data, 56)
    (ncolors,) = struct.unpack_from(">I", data, 76)
    if bits_per_pixel != 32:
        raise ValueError(f"{bits_per_pixel} bits per pixel (expected 32)")
    offset = header_size + 12 * ncolors
    fmt = "<I" if byte_order == 0 else ">I"

    def shift(mask):
        return (mask & -mask).bit_length() - 1

    rs, gs, bs = shift(red_mask), shift(green_mask), shift(blue_mask)
    rows = []
    for y in range(height):
        line = data[offset + y * bytes_per_line: offset + y * bytes_per_line + 4 * width]
        row = []
        for (pixel,) in struct.iter_unpack(fmt, line):
            row.append(((pixel & red_mask) >> rs, (pixel & green_mask) >> gs, (pixel & blue_mask) >> bs))
        rows.append(row)
    return width, height, rows


def write_png(path: Path, width: int, height: int, rows) -> None:
    raw = b"".join(b"\0" + bytes(c for px in row for c in px) for row in rows)

    def chunk(kind, body):
        return struct.pack(">I", len(body)) + kind + body + struct.pack(">I", zlib.crc32(kind + body) & 0xFFFFFFFF)

    png = b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", width, height, 8, 2, 0, 0, 0))
    png += chunk(b"IDAT", zlib.compress(raw, 6)) + chunk(b"IEND", b"")
    path.write_bytes(png)


def spread(rows) -> float:
    """Standard deviation of the luminance over a sparse grid of the picture (0 for one flat colour)."""
    values = [0.299 * r + 0.587 * g + 0.114 * b for row in rows[::8] for (r, g, b) in row[::8]]
    mean = sum(values) / len(values)
    return (sum((v - mean) ** 2 for v in values) / len(values)) ** 0.5


def start_xvfb():
    xvfb = shutil.which("Xvfb")
    if not xvfb:
        raise SystemExit("smoke: no DISPLAY and no Xvfb on PATH")
    for number in range(90, 140):
        if not Path(f"/tmp/.X{number}-lock").exists():
            proc = subprocess.Popen([xvfb, f":{number}", "-screen", "0", "1280x800x24", "-nolisten", "tcp"],
                                    stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
            for _ in range(50):
                if Path(f"/tmp/.X11-unix/X{number}").exists():
                    return proc, f":{number}"
                time.sleep(0.1)
            proc.kill()
    raise SystemExit("smoke: Xvfb did not start")


def main(argv=None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--out", type=Path, required=True, help="the screenshot (PNG); the log goes next to it")
    parser.add_argument("--seconds", type=float, default=60.0)
    parser.add_argument("--min-spread", type=float, default=8.0, help="luminance deviation a drawn launcher exceeds")
    parser.add_argument("--log", type=Path, help="GHR's log file to follow too (a release: <play folder>/logs/kamite.log)")
    parser.add_argument("command", nargs=argparse.REMAINDER)
    args = parser.parse_args(argv)
    command = args.command[1:] if args.command[:1] == ["--"] else args.command
    if not command:
        parser.error("the command that starts GHR is missing")
    xwd = shutil.which("xwd")
    if not xwd:
        raise SystemExit("smoke: xwd not found (x11-apps)")

    env = dict(os.environ)
    xvfb = None
    if not env.get("DISPLAY"):
        xvfb, env["DISPLAY"] = start_xvfb()
    env.pop("WAYLAND_DISPLAY", None)  # the virtual screen is X11
    args.out.parent.mkdir(parents=True, exist_ok=True)
    log_path = args.out.with_suffix(".log")
    lines = []
    seen = threading.Event()
    settled = threading.Event()
    proc = subprocess.Popen(command, env=env, stdin=subprocess.DEVNULL, stdout=subprocess.PIPE,
                            stderr=subprocess.STDOUT, text=True, errors="replace", start_new_session=True)

    def pump():
        with open(log_path, "w", encoding="utf-8") as log:
            for line in proc.stdout:
                log.write(line)
                log.flush()
                note(line)

    def note(line):
        lines.append(line)
        if READY in line:
            seen.set()
        if any(mark in line for mark in SETTLED):
            settled.set()

    stop = threading.Event()

    def follow():
        """GHR's own log file, from its start (it is created or truncated by the run)."""
        handle, position = None, 0
        while not stop.is_set():
            if handle is None and args.log.exists():
                handle = open(args.log, "r", encoding="utf-8", errors="replace")
            if handle is not None:
                handle.seek(position)
                chunk = handle.readline()
                while chunk.endswith("\n"):
                    note(chunk)
                    position = handle.tell()
                    chunk = handle.readline()
            time.sleep(0.2)
        if handle is not None:
            handle.close()

    if args.log and args.log.exists():
        args.log.unlink()  # a previous run's lines must not count
    reader = threading.Thread(target=pump, daemon=True)
    reader.start()
    follower = threading.Thread(target=follow, daemon=True) if args.log else None
    if follower:
        follower.start()
    failure = None
    try:
        deadline = time.monotonic() + args.seconds
        while not seen.is_set() and proc.poll() is None and time.monotonic() < deadline:
            time.sleep(0.2)
        if not seen.is_set():
            failure = (f"GHR exited with code {proc.returncode} before" if proc.poll() is not None
                       else f"no '{READY}' within {args.seconds:.0f} s:") + f" the launcher never showed"
        else:
            settled.wait(timeout=max(1.0, min(20.0, deadline - time.monotonic())))
            time.sleep(3.0)  # a few more frames drawn
            if proc.poll() is not None:
                failure = f"GHR exited with code {proc.returncode} after the launcher showed"
            else:
                dump = subprocess.run([xwd, "-root", "-silent"], env=env, capture_output=True)
                if dump.returncode:
                    failure = f"xwd failed: {dump.stderr.decode(errors='replace').strip()}"
                else:
                    width, height, rows = read_xwd(dump.stdout)
                    write_png(args.out, width, height, rows)
                    value = spread(rows)
                    print(f"smoke: screenshot {args.out} ({width}x{height}), luminance spread {value:.1f}")
                    if value < args.min_spread:
                        failure = f"the screen is one flat colour (spread {value:.1f} < {args.min_spread})"
    finally:
        if proc.poll() is None:
            os.killpg(proc.pid, signal.SIGTERM)
            try:
                proc.wait(timeout=10)
            except subprocess.TimeoutExpired:
                os.killpg(proc.pid, signal.SIGKILL)
                proc.wait()
        reader.join(timeout=5)
        stop.set()
        if follower:
            follower.join(timeout=5)
        if xvfb:
            xvfb.terminate()
            xvfb.wait(timeout=10)
    if failure:
        print(f"smoke: FAILED: {failure}\n--- last lines of {log_path} ---", file=sys.stderr)
        sys.stderr.write("".join(lines[-40:]))
        return 1
    print(f"smoke: passed (log {log_path})")
    return 0


if __name__ == "__main__":
    sys.exit(main())
