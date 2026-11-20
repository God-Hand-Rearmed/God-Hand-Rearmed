#!/bin/sh
# Runs the packaged Linux build on a stock distribution (.github/workflows/ci.yml runs it in Debian 11, Ubuntu 20.04
# and 24.04, Fedora, Arch and the Steam Runtime's sniper containers): the proof that the AppImage and the tar.gz start
# on systems that were not the build machine.
#
#   tools/linux/ci_distro_smoke.sh <folder holding GHR-v*-Linux-x64.tar.gz and GHR-v*-Linux-x64.AppImage> <results dir>
#
# For each form: "GHR --help" must exit 0, then (when a virtual screen can be installed) the launcher must come up and
# draw (tools/linux/smoke_launcher.py). The AppImage runs with APPIMAGE_EXTRACT_AND_RUN=1 (containers have no FUSE)
# and must create its play folder under XDG_DATA_HOME. Installs only what a desktop has anyway: X11 client libraries,
# Mesa, plus Xvfb and xwd for the screenshot. Run as root in a throwaway container.
set -eu
DIST=$(cd "$1" && pwd)
OUT=$(mkdir -p "$2" && cd "$2" && pwd)
HERE=$(cd "$(dirname "$0")" && pwd)
# shellcheck source=/dev/null
. /etc/os-release
echo "== $PRETTY_NAME (glibc $(ldd --version 2>/dev/null | head -1 | grep -o '[0-9.]*$'))"

screen=1
case "$ID" in
    debian|ubuntu|steamrt)
        export DEBIAN_FRONTEND=noninteractive
        # Debian 11 left the main mirrors with its end of life: its packages (security updates included) are on
        # archive.debian.org now, whose release files no longer get fresh dates.
        if [ "$ID" = debian ] && [ "${VERSION_ID:-}" = 11 ]; then
            sed -i 's|deb.debian.org|archive.debian.org|g; s|security.debian.org|archive.debian.org|g' \
                /etc/apt/sources.list /etc/apt/sources.list.d/* 2>/dev/null || true
            set -- -o Acquire::Check-Valid-Until=false
        else
            set --
        fi
        if command -v apt-get >/dev/null && apt-get "$@" update -qq >/dev/null; then
            apt-get "$@" install -y -qq python3 xvfb x11-apps libgl1 libgl1-mesa-dri libx11-6 libxrandr2 libxinerama1 \
                libxcursor1 libxi6 libxext6 >/dev/null || screen=0
        else
            screen=0
        fi ;;
    fedora)
        dnf install -y -q python3 xorg-x11-server-Xvfb xwd mesa-dri-drivers mesa-libGL libX11 libXrandr libXinerama \
            libXcursor libXi libXext >/dev/null || screen=0 ;;
    arch)
        # A full upgrade first: Arch supports no partial ones, and an older image's packages conflict with the
        # current repositories' (openssl / openssl-3 owning the same files).
        pacman -Syu --noconfirm --needed python xorg-server-xvfb xorg-xwd mesa libx11 libxrandr libxinerama libxcursor \
            libxi libxext >/dev/null || screen=0 ;;
    *)
        screen=0 ;;
esac
command -v python3 >/dev/null && command -v Xvfb >/dev/null && command -v xwd >/dev/null || screen=0
# Every distribution in CI must get the launcher test: a failed install is a failure, not a quiet "--help only".
# KAMITE_SMOKE_HELP_ONLY=1 allows the fallback for a system where no virtual screen can be installed.
if [ "$screen" != 1 ]; then
    if [ "${KAMITE_SMOKE_HELP_ONLY:-0}" = 1 ]; then
        echo "   (no virtual screen here: --help only)"
    else
        echo "FAILED: the virtual screen (Xvfb, xwd, Mesa, python3) could not be installed on $PRETTY_NAME"
        exit 1
    fi
fi

TAR=""; APPIMAGE=""
for f in "$DIST"/GHR-v*-Linux-x64.tar.gz; do [ -f "$f" ] && TAR=$f && break; done
for f in "$DIST"/GHR-v*-Linux-x64.AppImage; do [ -f "$f" ] && APPIMAGE=$f && break; done
[ -n "$TAR" ] && [ -n "$APPIMAGE" ] || { echo "FAILED: no tar.gz / AppImage in $DIST"; exit 1; }
WORK=$(mktemp -d)

echo "-- tar.gz: $(basename "$TAR")"
tar xzf "$TAR" -C "$WORK"
FOLDER="$WORK/God Hand Rearmed"
"$FOLDER/GHR" --help > "$OUT/tar-help.txt" 2>&1 || { cat "$OUT/tar-help.txt"; echo "FAILED: GHR --help (tar.gz)"; exit 1; }
grep -q "usage:" "$OUT/tar-help.txt"
echo "   --help: ok"
if [ "$screen" = 1 ]; then
    (cd "$FOLDER" && python3 -I "$HERE/smoke_launcher.py" --out "$OUT/tar-launcher.png" --seconds 120 --log logs/kamite.log -- ./GHR)
    [ -f "$FOLDER/GHR.ini" ] && [ -d "$FOLDER/logs" ] || { echo "FAILED: the tar.gz folder is not the play folder"; exit 1; }
fi

echo "-- AppImage: $(basename "$APPIMAGE")"
chmod +x "$APPIMAGE"
export APPIMAGE_EXTRACT_AND_RUN=1 HOME="$WORK/home" XDG_DATA_HOME="$WORK/home/.local/share"
mkdir -p "$HOME"
cd "$WORK"
"$APPIMAGE" --help > "$OUT/appimage-help.txt" 2>&1 || { cat "$OUT/appimage-help.txt"; echo "FAILED: --help (AppImage)"; exit 1; }
grep -q "usage:" "$OUT/appimage-help.txt"
[ -d "$XDG_DATA_HOME/GodHandRearmed" ] || { echo "FAILED: the AppImage made no play folder in XDG_DATA_HOME"; exit 1; }
echo "   --help: ok, play folder $XDG_DATA_HOME/GodHandRearmed"
if [ "$screen" = 1 ]; then
    python3 -I "$HERE/smoke_launcher.py" --out "$OUT/appimage-launcher.png" --seconds 120 \
        --log "$XDG_DATA_HOME/GodHandRearmed/logs/kamite.log" -- "$APPIMAGE"
    [ -f "$XDG_DATA_HOME/GodHandRearmed/GHR.ini" ] || { echo "FAILED: GHR.ini not written to the AppImage's play folder"; exit 1; }
fi
echo "== passed on $PRETTY_NAME"
