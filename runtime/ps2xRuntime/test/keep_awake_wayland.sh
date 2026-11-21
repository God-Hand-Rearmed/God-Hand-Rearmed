#!/usr/bin/env bash
# The Wayland idle inhibit (runtime/keep_awake.h, startWaylandInhibit) against a real sway (headless, pixman, no
# input devices, its own $XDG_RUNTIME_DIR), with swayidle (timeout 1 s) as the judge of "the session went idle":
#   1. control: with nothing held, swayidle fires (the setup can see idle at all);
#   2. keep_awake_test "wayland": while it holds, swayidle doesn't fire;
#   3. its output disabled (a second output made first): sway closes the layer surface, a new one is held on the other
#      output, and swayidle still doesn't fire;
#   4. the process gone: swayidle fires again (the compositor dropped the inhibit with the connection);
#   5. the game's own call (keep_awake_test "entry": ps2xHostKeepDisplayAwake, no X display, no session bus): its log
#      line, and swayidle doesn't fire;
#   6. with keep_awake_test_pause: the output going while the inhibitor is being made still ends in a held surface.
#   keep_awake_wayland.sh <keep_awake_test> [<keep_awake_test_pause>]
# Exit 77 (skipped) without sway, swaymsg or swayidle; 1 instead of 77 when KAMITE_REQUIRE_ALL_TESTS=1.
set -u
test_bin="$1"
skip=77
[[ "${KAMITE_REQUIRE_ALL_TESTS:-0}" == 1 ]] && skip=1
for tool in sway swaymsg swayidle; do
    if ! command -v "$tool" >/dev/null; then
        echo "skipped: $tool is missing" >&2
        exit "$skip"
    fi
done

rt=$(mktemp -d)
chmod 700 "$rt"
pids=()
cleanup() {
    for p in "${pids[@]}"; do kill "$p" 2>/dev/null; done
    wait 2>/dev/null
    rm -rf "$rt"
}
trap cleanup EXIT
fail() {
    echo "FAIL: $*" >&2
    for f in "$rt"/k*.err; do [[ -f $f ]] && { echo "--- $f" >&2; cat "$f" >&2; }; done
    exit 1
}

printf 'xwayland disable\n' >"$rt/config"
export XDG_RUNTIME_DIR="$rt"
unset WAYLAND_DISPLAY WAYLAND_SOCKET DISPLAY DBUS_SESSION_BUS_ADDRESS SWAYSOCK
WLR_BACKENDS=headless WLR_RENDERER=pixman WLR_LIBINPUT_NO_DEVICES=1 sway -c "$rt/config" >"$rt/sway.log" 2>&1 &
pids+=($!)
# first <glob>: the first existing match, if any (plain globs: the Nix sandbox's bash has no compgen).
first() {
    local f
    for f in $1; do
        [[ -S "$f" ]] && { echo "$f"; return 0; }
    done
    return 1
}
socket="" SWAYSOCK=""
for _ in $(seq 1 100); do
    socket=$(first "$rt/wayland-[0-9]") || socket=$(first "$rt/wayland-[0-9][0-9]")
    SWAYSOCK=$(first "$rt/sway-ipc.*.sock")
    [[ -n "$socket" && -n "$SWAYSOCK" ]] && break
    sleep 0.1
done
[[ -n "$socket" && -n "$SWAYSOCK" ]] || { cat "$rt/sway.log" >&2; fail "sway did not start"; }
export WAYLAND_DISPLAY="${socket##*/}"
export SWAYSOCK

# went_idle <seconds>: does swayidle (timeout 1 s) fire within <seconds>? Prints "idle" or "awake".
went_idle() {
    rm -f "$rt/idle"
    swayidle -w timeout 1 "echo idle >>'$rt/idle'" >/dev/null 2>&1 &
    local p=$!
    sleep "$1"
    kill "$p" 2>/dev/null
    wait "$p" 2>/dev/null
    [[ -s "$rt/idle" ]] && echo idle || echo awake
}

# wait_for <file> <pattern>: up to 10 s.
wait_for() {
    for _ in $(seq 1 100); do
        grep -q "$2" "$1" 2>/dev/null && return 0
        sleep 0.1
    done
    return 1
}

# 1.
[[ $(went_idle 3) == idle ]] || fail "control: swayidle never fired with nothing held (the setup sees no idle)"

# 2. WAYLAND_SOCKET set to an fd that isn't open: libwayland's wl_display_connect would take it (and fail); ours must
# leave it alone and use $WAYLAND_DISPLAY. WAYLAND_DEBUG=client: libwayland's trace of every request, for the shape
# of the surface (what sway can't be asked).
WAYLAND_SOCKET=999 WAYLAND_DEBUG=client "$test_bin" wayland 30 >"$rt/k.out" 2>"$rt/k.err" &
held=$!
pids+=($held)
wait_for "$rt/k.out" "^held\|^failed" || fail "wayland: no answer in 10 s"
grep -q "^held" "$rt/k.out" || fail "wayland: $(cat "$rt/k.out")"
[[ $(went_idle 3) == awake ]] || fail "held: swayidle fired anyway"
# The background layer (0), no output of its own asked for (nil), the namespace; 1x1; the pixel is ARGB8888 (format 0,
# a new memfd's zeros: transparent); its input region is set and empty (no wl_region.add: clicks go through it).
trace="$rt/k.err"
grep -q 'get_layer_surface(new id zwlr_layer_surface_v1[@#][0-9]*, wl_surface[@#][0-9]*, nil, 0, "ghr-keep-awake")' "$trace" \
    || fail "not a background layer surface on no named output"
grep -q 'zwlr_layer_surface_v1[@#][0-9]*\.set_size(1, 1)' "$trace" || fail "the surface isn't 1x1"
grep -q 'create_buffer(new id wl_buffer[@#][0-9]*, 0, 1, 1, 4, 0)' "$trace" || fail "the pixel isn't 1x1 ARGB8888"
grep -q 'wl_surface[@#][0-9]*\.set_input_region(wl_region[@#][0-9]*)' "$trace" || fail "no input region set (it would take clicks)"
! grep -q 'wl_region[@#][0-9]*\.add(' "$trace" || fail "the input region isn't empty"
grep -q 'zwp_idle_inhibit_manager_v1[@#][0-9]*\.create_inhibitor' "$trace" || fail "no inhibitor asked for"

# 3.
swaymsg -q create_output || fail "swaymsg create_output"
first=$(swaymsg -t get_outputs | grep -o '"name": "[^"]*"' | head -1 | cut -d'"' -f4)
swaymsg -q output "$first" disable || fail "swaymsg output $first disable"
wait_for "$rt/k.err" "held again" || fail "output $first disabled: no new layer surface"
[[ $(went_idle 3) == awake ]] || fail "after the output change: swayidle fired"

# 4.
kill "$held"
wait "$held" 2>/dev/null
[[ $(went_idle 3) == idle ]] || fail "the process gone: swayidle still held off (the inhibit outlived it)"

# 5.
"$test_bin" entry 8 >"$rt/k2.out" 2>"$rt/k2.err" &
pids+=($!)
wait_for "$rt/k2.err" "Wayland idle inhibit" || fail "entry: no Wayland log line"
grep -q "keeping the display awake: Wayland idle inhibit" "$rt/k2.err" || fail "entry: $(cat "$rt/k2.err")"
[[ $(went_idle 3) == awake ]] || fail "entry: swayidle fired while the game's call held"

# 6. (with <keep_awake_test_pause>, built with PS2X_KEEP_AWAKE_TEST_PAUSE_AFTER_MAP: 2 s between the surface's map and
# the inhibitor's roundtrip) the output goes in that window: the close is read by that roundtrip, not by the dispatch
# loop, and a new surface must still be made (it once wasn't: "held", nothing held, no log line).
if [[ -n "${2:-}" ]]; then
    names() { swaymsg -t get_outputs | grep -o '"name": "[^"]*"' | cut -d'"' -f4; }
    before=$(names)
    swaymsg -q create_output || fail "swaymsg create_output"
    newest=$(names | grep -vxF "$before") # get_outputs isn't in creation order
    [[ -n "$newest" ]] || fail "pause: no new output"
    "$2" wayland 30 >"$rt/k3.out" 2>"$rt/k3.err" &
    pids+=($!)
    wait_for "$rt/k3.err" "paused after the map" || fail "pause: never paused"
    for o in $before; do # every output but the new one: wherever sway put the surface, it goes
        swaymsg -q output "$o" disable
    done
    wait_for "$rt/k3.out" "^held\|^failed" || fail "pause: no answer"
    grep -q "^held" "$rt/k3.out" || fail "pause: $(cat "$rt/k3.out")"
    # Said so at once (the first surface isn't counted held), then retried.
    grep -q "no keep-awake surface yet" "$rt/k3.err" || fail "pause: the closed first surface was taken as held"
    wait_for "$rt/k3.err" "held again" || fail "pause: the output went during the inhibitor's roundtrip, no new surface"
    [[ $(went_idle 3) == awake ]] || fail "pause: swayidle fired after the new surface"
fi

echo "keep_awake_wayland: OK" >&2
