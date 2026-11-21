#!/usr/bin/env bash
# The X11 screensaver reset when its X server goes away (runtime/keep_awake.h, startX11Reset). keep_awake_test's
# "x11-lost" mode starts the reset every 200 ms on a private Xvfb; this script kills the Xvfb.
#   1. alone: the process must go on ("alive", exit 0). libX11's default for a lost connection is exit(), from the
#      reset thread, which would end the game;
#   2. "x11-lost other", with a second connection of the test's own (standing for GLFW's): that one's loss must still
#      end the process the way libX11 always has (exit 1, libX11's own message): only the reset's connection is spared.
#   keep_awake_x11_lost.sh <keep_awake_test>
# Exit 77 (skipped) without Xvfb, or a libX11 older than 1.7 (no XSetIOErrorExitHandler: there the exit() stays);
# 1 instead of 77 when KAMITE_REQUIRE_ALL_TESTS=1.
set -u
test_bin="$1"
skip=77
[[ "${KAMITE_REQUIRE_ALL_TESTS:-0}" == 1 ]] && skip=1
if ! command -v Xvfb >/dev/null; then
    echo "skipped: Xvfb is missing" >&2
    exit "$skip"
fi

# run <mode args...>: prints the test's output to stderr, sets $code and $out_text.
run() {
    # A free display number from Xvfb itself (-displayfd), never one a live server or a stale lock holds.
    local fifo
    fifo=$(mktemp -u)
    mkfifo "$fifo"
    Xvfb -displayfd 3 -nolisten tcp 3>"$fifo" >/dev/null 2>&1 &
    local xvfb=$!
    local number=""
    read -r -t 10 number <"$fifo" || true
    rm -f "$fifo"
    if [[ -z "$number" ]]; then
        kill "$xvfb" 2>/dev/null
        echo "FAIL: Xvfb did not start" >&2
        exit 1
    fi
    local display=":$number"
    local out
    out=$(mktemp)
    DISPLAY="$display" "$test_bin" "$@" >"$out" 2>&1 &
    local pid=$!
    for _ in $(seq 1 50); do
        grep -q "^ready\|^no-exit-handler\|^failed" "$out" && break
        sleep 0.1
    done
    kill -TERM "$xvfb" 2>/dev/null # the server ends: every client's connection drops (and its socket and lock go)
    wait "$xvfb" 2>/dev/null
    wait "$pid"
    code=$?
    out_text=$(cat "$out")
    rm -f "$out"
    printf '%s\n' "$out_text" >&2
}

run x11-lost
if grep -q "^no-exit-handler" <<<"$out_text"; then
    echo "skipped: this libX11 has no XSetIOErrorExitHandler" >&2
    exit "$skip"
fi
if ! grep -q "^ready" <<<"$out_text"; then
    echo "FAIL: x11-lost never got ready" >&2
    exit 1
fi
if [[ $code -ne 0 ]] || ! grep -q "^alive" <<<"$out_text"; then
    echo "FAIL: the process ended (exit $code) when the reset's X server went away, instead of going on" >&2
    exit 1
fi
if ! grep -q "X11 screensaver reset stopped" <<<"$out_text"; then
    echo "FAIL: the reset's thread didn't stop when its X connection was lost" >&2
    exit 1
fi

run x11-lost other
# libX11's message comes from the handler that was there before ours (chained): without the chain the process would
# still end (libX11's default exit handler) but silently.
if [[ $code -ne 1 ]] || grep -q "^other-survived" <<<"$out_text" || ! grep -qE "X connection to .* broken|XIO:" <<<"$out_text"; then
    echo "FAIL: another connection's loss no longer ends the process as libX11 does (exit $code)" >&2
    exit 1
fi
echo "keep_awake_x11_lost: OK" >&2
