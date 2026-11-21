#pragma once

#include <cstddef>
#include <filesystem>
#include <string>
#include <string_view>

// The mods at boot (the folders, mod.ini and the load order: mod_list.h; claude/mod-support.md). GHR.ini [Mods]
// LoadOrder= / Disabled= reach here as KAMITE_MOD_ORDER / KAMITE_MODS_DISABLED. No mods at all in test-case replays and
// --test runs (they run as the recorder's vanilla game), or with KAMITE_MODS=0.
namespace kamite::mods
{
    // Before the pnach patches are applied (main.cpp: right after ps2x::pnach::loadFromEnvironment): lists the mods in
    // the log, puts the enabled mods' textures/ ahead of the player's texture pack (PS2X_PCSX2_TEXTURE_FIRST, the last
    // mod in the order first: PCSX2 keeps the first image of a name), loads their patches/*.pnach after PS2X_PNACH's (in
    // load order, so a lower mod's lines are written last) and registers the save state's record of them. Returns the
    // patches loaded.
    size_t prepare(const std::filesystem::path &exeDir);

    // After PS2Runtime::setIoPaths, before characters::install (the Character row's choice wins its files over a mod's):
    // every game file an enabled mod has, served by the file layer from the lowest mod in the order that has it; an
    // archive (.afs) some mods replace entries of is served with those entries swapped in (afs_archive.h), their audio
    // converted to ADX first (mod_audio.h, cached under <exe>/cache/mod-audio).
    void installFiles();

    // The enabled mod (its name) whose file replaces a game file ("em/em06.dat", lower case), or nullptr. For the
    // port's own features on the game's files: they step aside for a modded file (decision 3, until step 3).
    const std::string *replacing(std::string_view path);
}
