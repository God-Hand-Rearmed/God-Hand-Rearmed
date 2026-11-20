set -e
REPO=${REPO:-$(cd "$(dirname "$0")/../../.." && pwd)}
cd $REPO
INC="-I/tmp/stub -Isrc -Isrc/kamite $(for d in runtime/*/include; do echo -n " -I$d"; done) -Iruntime/ps2xRuntime/src -Iruntime/ps2xRuntime/src/lib -Iruntime/ps2xRuntime/src/lib/Kernel -Ithirdparty -Ithirdparty/fmt/include -Ithirdparty/pcsx2-gs/include -Iruntime/ps2xIOP/include -Iruntime/ps2xIOP/src -I/tmp/rl/src -I/tmp/im -I/tmp/rli -I/tmp/dl/a/include"
mkdir -p /tmp/lh/obj
CXX="g++ -std=c++20 -O1 -g -msse4.1 -DKAMITE_STEAM_SHORTCUT=1 -DKAMITE_HAS_LAUNCHER=1 -DKAMITE_HAS_AVIF=1 -DKAMITE_VERSION=\"0.6.9\" -DKAMITE_VERSION_LABEL=\"Beta\" $INC"
for f in src/kamite/launcher.cpp src/kamite/launcher_book.cpp src/kamite/launcher_saloon.cpp src/kamite/launcher_paper.cpp src/kamite/saloon_image.cpp src/kamite/launcher_bark.cpp src/kamite/settings_table.cpp src/kamite/presets.cpp src/kamite/ini_file.cpp src/kamite/mod_list.cpp src/kamite/mod_zip.cpp $REPO/tools/launcher/harness/stubs.cpp $REPO/tools/launcher/harness/main.cpp /tmp/rli/rlImGui.cpp; do
  o=/tmp/lh/obj/$(basename $f .cpp).o
  if [ ! -f $o ] || [ $f -nt $o ]; then $CXX -c $f -o $o; fi
done
for f in imgui imgui_draw imgui_tables imgui_widgets; do o=/tmp/lh/obj/$f.o; [ -f $o ] || g++ -std=c++20 -O1 -I/tmp/im -c /tmp/im/$f.cpp -o $o; done
g++ -o /tmp/lh/launcher /tmp/lh/obj/*.o /tmp/rlb/raylib/libraylib.a /tmp/avt/b/avif/libavif_internal.a /tmp/avt/b/dav1d/libdav1d.a -lGL -lm -lpthread -ldl -lX11
