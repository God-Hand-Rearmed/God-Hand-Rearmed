"""The Linux display keep-awake (issue #12) against a real session bus: a private dbus-daemon with fake desktop
services (org.freedesktop.ScreenSaver as KDE and most desktops have it, org.gnome.SessionManager as GNOME has it), and
keep_awake_test's "bus" mode, which goes through libdbus-1 as GHR does.

    python3 -I keep_awake_bus.py path/to/keep_awake_test

Checks which service is asked, with which arguments; that a service answering with an error falls through to the next;
that no service, or no bus at all, leaves the game running (no exit), without libdbus starting a bus of its own; that
a call on a bus that went away comes back; and that ps2xHostKeepDisplayAwake, as the game calls it, asks once with
GHR's name and reason. Exit 77 (skipped) without dbus-daemon or jeepney,
1 instead when KAMITE_REQUIRE_ALL_TESTS=1.
"""
import os
import shutil
import subprocess
import sys
import tempfile
import time
import threading

# Skipped (77) without the tools, unless KAMITE_REQUIRE_ALL_TESTS=1 (the Nix check sets it): then it fails instead.
SKIP = 1 if os.environ.get("KAMITE_REQUIRE_ALL_TESTS") == "1" else 77

try:
    from jeepney import HeaderFields, MessageType, new_error, new_method_return
    from jeepney.bus_messages import message_bus
    from jeepney.io.blocking import open_dbus_connection
except ImportError:
    print("skipped: python3's jeepney is missing", file=sys.stderr)
    sys.exit(SKIP)

if not shutil.which("dbus-daemon"):
    print("skipped: dbus-daemon is missing", file=sys.stderr)
    sys.exit(SKIP)

TEST = sys.argv[1]
failures = 0


def check(ok, what):
    global failures
    if not ok:
        failures += 1
        print(f"FAIL: {what}", file=sys.stderr)


class Service:
    """A desktop's inhibit service: owns `name`, answers Inhibit with `cookie` (or with `error`), records the calls."""

    def __init__(self, address, name, cookie=None, error=None, stall=0.0, reply_type="u"):
        self.calls, self.senders, self.cookie, self.error, self.stall = [], [], cookie, error, stall
        self.reply_type = reply_type
        self.conn = open_dbus_connection(bus=address)
        reply = self.conn.send_and_get_reply(message_bus.RequestName(name))
        assert reply.body[0] == 1, f"could not own {name}"
        self.thread = threading.Thread(target=self.serve, daemon=True)
        self.thread.start()

    def serve(self):
        while True:
            try:
                msg = self.conn.receive()
            except Exception:
                return
            if msg.header.message_type != MessageType.method_call:
                continue
            fields = msg.header.fields
            self.calls.append((fields.get(HeaderFields.path), fields.get(HeaderFields.interface),
                               fields.get(HeaderFields.member), msg.body))
            self.senders.append(fields.get(HeaderFields.sender))
            if self.stall:
                time.sleep(self.stall)  # a hung desktop: past the caller's reply timeout
            if self.error:
                self.conn.send(new_error(msg, self.error, "s", ("not here",)))
            else:
                self.conn.send(new_method_return(msg, self.reply_type, (self.cookie,)))


def has_owner(address, name):
    """Whether `name` (a caller's unique name) is still on the bus: what a desktop watches to drop an inhibit."""
    conn = open_dbus_connection(bus=address)
    try:
        return bool(conn.send_and_get_reply(message_bus.NameHasOwner(name)).body[0])
    finally:
        conn.close()


# A session bus configuration of our own: dbus-daemon --session reads /etc/dbus-1/session.conf, which a build sandbox
# (Nix's) or a minimal container lacks.
BUS_DIR = tempfile.mkdtemp(prefix="ghr-keep-awake-bus-")
BUS_CONFIG = os.path.join(BUS_DIR, "session.conf")
BUS_TEMPLATE = f"""<!DOCTYPE busconfig PUBLIC "-//freedesktop//DTD D-Bus Bus Configuration 1.0//EN"
 "http://www.freedesktop.org/standards/dbus/1.0/busconfig.dtd">
<busconfig>
  <type>session</type>
  <listen>{{listen}}</listen>
  <auth>EXTERNAL</auth>
  <policy context="default">
    <allow send_destination="*" eavesdrop="true"/>
    <allow eavesdrop="true"/>
    <allow own="*"/>
  </policy>
</busconfig>
"""


def bus(listen=None):
    """A private session bus: on a socket in BUS_DIR, or at `listen` (a unix:path= address)."""
    with open(BUS_CONFIG, "w") as handle:
        handle.write(BUS_TEMPLATE.replace("{listen}", listen or f"unix:dir={BUS_DIR}"))
    daemon = subprocess.Popen(["dbus-daemon", f"--config-file={BUS_CONFIG}", "--nofork", "--print-address=1"],
                              stdout=subprocess.PIPE, text=True)
    address = daemon.stdout.readline().strip()
    if not address:
        print(f"FAIL: dbus-daemon did not start (exit {daemon.wait()})", file=sys.stderr)
        sys.exit(1)
    return daemon, address


def run_test(address, kill=None, again=False, env_extra=None):
    env = dict(os.environ, DBUS_SESSION_BUS_ADDRESS=address)
    if env_extra is not None:
        env.update(env_extra)
        env = {k: v for k, v in env.items() if v is not None}
    proc = subprocess.Popen([TEST, "bus"], stdin=subprocess.PIPE, stdout=subprocess.PIPE, text=True, env=env)
    first = proc.stdout.readline().strip()
    if kill:
        kill()
    out, _ = proc.communicate("again\n" if again else "go\n", timeout=20)
    lines = out.split()
    return first, (lines[-1] if lines else ""), proc.returncode, out


FREEDESKTOP = ("/org/freedesktop/ScreenSaver", "org.freedesktop.ScreenSaver", "Inhibit")
GNOME = ("/org/gnome/SessionManager", "org.gnome.SessionManager", "Inhibit")

# 1. KDE / most desktops: org.freedesktop.ScreenSaver takes it; GNOME's is never asked. The caller stays on the bus
#    after the call (the desktop keeps the inhibit while it does) and leaves when the process ends.
daemon, address = bus()
fd = Service(address, "org.freedesktop.ScreenSaver", cookie=4242)
gn = Service(address, "org.gnome.SessionManager", cookie=77)
held_while_running = []
first, rest, code, out = run_test(address, kill=lambda: held_while_running.append(
    bool(fd.senders) and has_owner(address, fd.senders[0])))
check(held_while_running == [True], f"the caller left the bus right after the inhibit: {held_while_running}")
check(fd.senders and not has_owner(address, fd.senders[0]), "the caller is still on the bus after it exited")
check(first == "held org.freedesktop.ScreenSaver 4242", f"freedesktop first: {first}")
check(len(fd.calls) == 1 and fd.calls[0][:3] == FREEDESKTOP, f"freedesktop call: {fd.calls}")
check(fd.calls and fd.calls[0][3] == ("God Hand: Rearmed", "Playing"), f"freedesktop args: {fd.calls}")
check(gn.calls == [], f"GNOME asked although freedesktop held: {gn.calls}")
check(rest == "alive" and code == 0, f"freedesktop run: {rest!r} {code}")
daemon.kill(); daemon.wait()

# 2. GNOME: only org.gnome.SessionManager; the idle flag (8) and no toplevel window (0).
daemon, address = bus()
gn = Service(address, "org.gnome.SessionManager", cookie=77)
first, rest, code, out = run_test(address)
check(first == "held org.gnome.SessionManager 77", f"GNOME: {first}")
check(len(gn.calls) == 1 and gn.calls[0][:3] == GNOME, f"GNOME call: {gn.calls}")
check(gn.calls and gn.calls[0][3] == ("God Hand: Rearmed", 0, "Playing", 8), f"GNOME args: {gn.calls}")
check(rest == "alive" and code == 0, f"GNOME run: {rest!r} {code}")
daemon.kill(); daemon.wait()

# 3. A freedesktop service that refuses (GNOME's partial one, say): on to GNOME's.
daemon, address = bus()
fd = Service(address, "org.freedesktop.ScreenSaver", error="org.freedesktop.DBus.Error.NotSupported")
gn = Service(address, "org.gnome.SessionManager", cookie=5)
first, rest, code, out = run_test(address)
check(first == "held org.gnome.SessionManager 5", f"refused, then GNOME: {first}")
check(len(fd.calls) == 1 and len(gn.calls) == 1, f"refused, then GNOME calls: {fd.calls} {gn.calls}")
daemon.kill(); daemon.wait()

# 3b. A freedesktop service whose reply is no cookie (a string): not taken as held, on to GNOME's.
daemon, address = bus()
fd = Service(address, "org.freedesktop.ScreenSaver", cookie="not a cookie", reply_type="s")
gn = Service(address, "org.gnome.SessionManager", cookie=6)
first, rest, code, out = run_test(address)
check(first == "held org.gnome.SessionManager 6", f"a reply that is no cookie: {first}")
daemon.kill(); daemon.wait()

# 4. A bus with neither service: nothing holds, both errors said, the game goes on.
daemon, address = bus()
first, rest, code, out = run_test(address)
check(first.startswith("none ") and "org.freedesktop.ScreenSaver: " in first and "org.gnome.SessionManager: " in first,
      f"no service: {first}")
check(rest == "alive" and code == 0, f"no service run: {rest!r} {code}")
daemon.kill(); daemon.wait()

# 5. No session bus at all.
first, rest, code, out = run_test("unix:path=/nonexistent/ghr-keep-awake-test")
check(first.startswith("nobus "), f"no bus: {first}")
check(rest == "alive" and code == 0, f"no bus run: {rest!r} {code}")

# 5b. No address and no $XDG_RUNTIME_DIR/bus: no bus, and no dbus-launch started by libdbus (X11 autolaunch).
empty = tempfile.mkdtemp()


def autolaunches():
    if not shutil.which("pgrep"):
        return None
    found = subprocess.run(["pgrep", "-f", "dbus-launch --autolaunch"], capture_output=True, text=True).stdout
    return set(found.split())


launches_before = autolaunches()
first, rest, code, out = run_test("", env_extra={"DBUS_SESSION_BUS_ADDRESS": None, "XDG_RUNTIME_DIR": empty,
                                                 "DISPLAY": ":65000"})  # no X server there
check(first.startswith("nobus ") and "XDG_RUNTIME_DIR" in first, f"no address: {first}")
check(rest == "alive" and code == 0, f"no address run: {rest!r} {code}")
launches_after = autolaunches()
check(launches_before is None or launches_after <= launches_before, "a dbus-launch was started")

# 5c. No address, but $XDG_RUNTIME_DIR/bus (systemd's user bus, libdbus's own default): that bus is the one used.
runtime_dir = tempfile.mkdtemp()
daemon, address = bus(listen=f"unix:path={runtime_dir}/bus")
fd = Service(address, "org.freedesktop.ScreenSaver", cookie=55)
first, rest, code, out = run_test("", env_extra={"DBUS_SESSION_BUS_ADDRESS": None, "XDG_RUNTIME_DIR": runtime_dir})
check(first == "held org.freedesktop.ScreenSaver 55", f"the bus at $XDG_RUNTIME_DIR/bus: {first}")
daemon.kill(); daemon.wait()

# 6. The bus goes away after the inhibit: a later call on the dead connection comes back, the process goes on.
daemon, address = bus()
fd = Service(address, "org.freedesktop.ScreenSaver", cookie=9)
first, rest, code, out = run_test(address, kill=lambda: (daemon.kill(), daemon.wait()), again=True)
check(first == "held org.freedesktop.ScreenSaver 9", f"before the bus died: {first}")
check(rest == "alive" and code == 0, f"after the bus died: {out!r} {code}")
check("again none" in out, f"the second ask on the dead bus: {out!r}")

# 7. The game's own call: ps2xHostKeepDisplayAwake() twice -> one Inhibit, GHR's name and reason, the log line.
daemon, address = bus()
fd = Service(address, "org.freedesktop.ScreenSaver", cookie=31)
no_display = {k: v for k, v in os.environ.items() if k != "DISPLAY"}
entry = subprocess.run([TEST, "entry"], capture_output=True, text=True, timeout=30,
                       env=dict(no_display, DBUS_SESSION_BUS_ADDRESS=address))
check(entry.returncode == 0, f"entry exit {entry.returncode}: {entry.stderr}")
check(len(fd.calls) == 1, f"entry asked {len(fd.calls)} times: {fd.calls}")
check(fd.calls and fd.calls[0][3] == ("God Hand: Rearmed", "Playing (a controller doesn't count as activity)"),
      f"entry args: {fd.calls}")
check("[window] keeping the display awake: org.freedesktop.ScreenSaver (cookie 31)" in entry.stderr,
      f"entry log: {entry.stderr!r}")
daemon.kill(); daemon.wait()

# 8. A hung desktop service (the bus answers, freedesktop's service never does): the game's call still returns at once,
#    the 3 s reply timeout gives up on that service, and GNOME's takes the inhibit. (A bus that never answers at all is
#    keep_awake_test entry-x11's: there the X11 reset must still run.)
daemon, address = bus()
fd = Service(address, "org.freedesktop.ScreenSaver", cookie=1, stall=8.0)
gn = Service(address, "org.gnome.SessionManager", cookie=12)
started = time.monotonic()
entry = subprocess.run([TEST, "entry", "6"], capture_output=True, text=True, timeout=30,
                       env=dict(no_display, DBUS_SESSION_BUS_ADDRESS=address))
returned = [int(line.split()[1]) for line in entry.stdout.splitlines() if line.startswith("entry-returned-ms ")]
check(returned and returned[0] < 200, f"the game's call waited on the bus: {entry.stdout!r}")
check("[window] keeping the display awake: org.gnome.SessionManager (cookie 12)" in entry.stderr,
      f"no fallback to GNOME within the timeout: {entry.stderr!r}")
check(len(gn.calls) == 1, f"GNOME asked {len(gn.calls)} times")
daemon.kill(); daemon.wait()

print(f"keep_awake_bus: {'OK' if failures == 0 else f'{failures} failed'}", file=sys.stderr)
sys.exit(1 if failures else 0)
