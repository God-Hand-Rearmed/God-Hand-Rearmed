#!/usr/bin/env bash
# Runs a renderer test program (pcsx2_gs_smoke: test/SmokeTest.cpp; gs_pcsx2_backend_test:
# runtime/ps2xRuntime/test/gs_pcsx2_backend_test.cpp) inside a fresh Xvfb (xvfb-run) when there is no X display or
# KAMITE_SMOKE_XVFB=1, else on $DISPLAY. The Vulkan driver is the environment's: VK_DRIVER_FILES (VK_ICD_FILENAMES for
# older loaders) set to Mesa's share/vulkan/icd.d/lvp_icd.x86_64.json runs it on lavapipe, which needs no GPU
# (thirdparty/README.md, "Linux", has the full command).
#   run_smoke.sh <test program> [arguments]
set -euo pipefail
smoke="$1"
shift
if [[ -z "${DISPLAY:-}" || "${KAMITE_SMOKE_XVFB:-0}" == 1 ]]; then
    if ! command -v xvfb-run >/dev/null; then
        echo "run_smoke.sh: no X display and no xvfb-run" >&2
        exit 1
    fi
    exec xvfb-run -a -s "-screen 0 1280x1024x24" "$smoke" "$@"
fi
exec "$smoke" "$@"
