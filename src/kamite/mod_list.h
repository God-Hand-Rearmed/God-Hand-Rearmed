#pragma once

#include <cstddef>
#include <filesystem>
#include <string>
#include <vector>

// Mods: the folders under <exe>/mods, one mod each (claude/mod-support.md):
//   mods/<name>/mod.ini          [Mod] Name=, Author=, Version=, Description= (all optional; "\n" in Description = a
//                                new line). A folder without one is still a mod (named after the folder).
//   mods/<name>/<disc path>      game files replaced whole (pl/pl00.dat, em/em06.dat ...): every file in a subfolder
//                                except the ones below; files at the top (mod.ini, a readme, pictures) are not.
//   mods/<name>/<x.afs>/<entry>  one entry of an archive (bgm/bgm00.afs/12.ogg, se/ev/ev01.afs/ev01_cut00.wav): by
//                                its stored name or number; audio converted to the game's ADX (step 2)
//   mods/<name>/textures/        PCSX2 replacement images (before the player's texture pack)
//   mods/<name>/patches/*.pnach  PCSX2 patches
//   mods/<name>/characters/      characters (step 4; not game files)
// mods/characters (the character folders of before) and folders starting with '.' are not mods.
// The load order and which mods are off are kept in GHR.ini [Mods] LoadOrder= / Disabled= (folder names, '|' between):
// the lower of two mods that replace one file wins. A folder in neither list is new: on, at the bottom (A-Z).
// Shared by the launcher's Mods tab and the boot (mods.h); plain std, no runtime.
namespace kamite::mod_list
{
    struct Mod
    {
        std::string folder;   // its folder's name (GHR.ini's lists name it)
        std::filesystem::path path;
        std::string name, author, version, description; // mod.ini [Mod]; name = folder when empty
        bool hasIni = false;
        bool enabled = true;
        bool isNew = false;   // in neither list
        std::vector<std::string> files;    // game files: paths relative to the folder, lower case, '/', sorted
        std::vector<std::string> textures; // textures/ images (.png / .dds): file names without extension, lower case,
                                           // sorted (only with details)
        size_t textureCount = 0;           // textures/ images (only with details)
        bool hasTextures = false;          // a textures/ folder
        std::vector<std::filesystem::path> patches; // patches/*.pnach, by name
        bool hasCharacters = false;        // a characters/ folder
    };

    // Every mod under modsDir in load order. details: also list textures/ (the launcher; the boot only needs the folder).
    std::vector<Mod> scan(const std::filesystem::path &modsDir, const std::string &loadOrder, const std::string &disabled,
                          bool details);
    // GHR.ini's two lists for these mods (every mod in order / the ones off).
    std::string orderText(const std::vector<Mod> &mods);
    std::string disabledText(const std::vector<Mod> &mods);
    // The mods' folder names as they are now (one directory listing: to notice a folder added or removed).
    std::vector<std::string> folderNames(const std::filesystem::path &modsDir);
    // A name usable as a mod's folder (characters Windows refuses dropped; "characters" and empty names changed).
    std::string folderName(const std::string &wanted);

    // What the enabled mods take from each other: for each mod (by index), the mods above it whose files / textures it
    // wins (wins) and the mods below it that win its own (loses). Off mods take part in nothing.
    struct Conflict
    {
        size_t other = 0;
        std::vector<std::string> files;
        size_t textures = 0;
    };
    struct Overrides
    {
        std::vector<Conflict> wins, loses;
    };
    std::vector<Overrides> overrides(const std::vector<Mod> &mods);

    // The mods' identity as a save state keeps it: the enabled ones in order, folder and version.
    struct Entry
    {
        std::string folder, version;
        bool operator==(const Entry &o) const = default;
    };
    std::vector<Entry> enabledEntries(const std::vector<Mod> &mods);
    // What differs between the mods a state was made with and the ones on now, in a few words ("" when nothing does).
    std::string describeDifference(const std::vector<Entry> &state, const std::vector<Entry> &now);

    // A mod's file inside one of the game's archives (step 2): "bgm/bgm00.afs/<entry>.ogg" -> "bgm/bgm00.afs" ("" for
    // any other path). The entry: its stored name or its number, any extension (afs_archive.h, mod_audio.h).
    std::string archiveOf(const std::string &file);
    // What two mods' files clash on: the path, or for an archive entry the archive + the entry without its extension.
    std::string fileKey(const std::string &file);

    std::string lower(std::string s);
    std::string utf8(const std::filesystem::path &p);
    std::filesystem::path fromUtf8(const std::string &s);
}
