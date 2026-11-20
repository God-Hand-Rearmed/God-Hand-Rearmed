#pragma once

// Where GHR's files live. Two folders, which are the same one in a Windows zip or a Linux tar.gz:
// - the program folder: GHR(.exe), its libraries and resources/. Read only is fine (a Linux AppImage mounts it
//   read-only at $APPDIR/usr/lib/ghr).
// - the play folder (home): everything GHR writes or the player adds. GHR.ini, controls.ini, memcards/, states/,
//   logs/, game_data/, game/, mods/, textures/, cases/, cache/, the update's files, the crash logs.
// KAMITE_HOME names the play folder (the AppImage's AppRun sets it: <the AppImage's folder>/GHR-data when that folder
// exists, else ~/.local/share/GodHandRearmed); unset or empty, the play folder is the program folder, as before.
#include <filesystem>
#include <string>
#include <vector>

namespace kamite::paths
{
    // The folder holding the running executable (/proc/self/exe on Linux, GetModuleFileNameW on Windows).
    const std::filesystem::path &programDir();
    // The play folder: KAMITE_HOME (created when missing) or programDir(). Read once, at the first call.
    const std::filesystem::path &home();
    // programDir() / "resources".
    const std::filesystem::path &resources();
    // Why home() is not KAMITE_HOME although it is set (it could not be created), "" otherwise. For the log.
    const std::string &homeNote();

    // home()'s rule, for the tests: `variable` is KAMITE_HOME's value (nullptr when unset). A relative value is taken
    // against `cwd`. The folder is created; if that fails the program folder is used and `note` says why.
    std::filesystem::path resolveHome(const char *variable, const std::filesystem::path &programDir,
                                      const std::filesystem::path &cwd, std::string &note);

    // Linux's extra places to look for the disc image (main.cpp, after the play folder's game/): ~/Games,
    // ~/Downloads, then every folder one level under <mediaRoot>/<user>/ and under <mediaRoot>/ (removable drives:
    // the Deck's SD card), <mediaRoot>/<user> itself left out. Only folders that exist; `home` / `user` may be null.
    std::vector<std::filesystem::path> discSearchFolders(const char *home, const char *user, const std::filesystem::path &mediaRoot);
}
