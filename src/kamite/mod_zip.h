#pragma once

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

// Install from zip (the launcher's Mods tab; claude/mod-support.md decision 5): a zip holding one or more mods, each a
// folder with a mod.ini, unpacked into mods/. Read from the file (not loaded whole); stored and deflated entries,
// ZIP64 for big archives; an entry up to 2 GB.
namespace kamite::mod_zip
{
    struct Found
    {
        std::string folder;         // where it goes: mods/<folder> (an existing folder's own spelling when it is there)
        std::string prefix;         // its folder in the zip ("" for the zip's root, else "Some Mod/")
        std::string name, version;  // its mod.ini [Mod] Name= / Version= (name: the folder's when empty)
        bool exists = false;        // mods/<folder> is there already: installing replaces it (its place in the order and
                                    // its tick stay, the list names the folder)
        std::string oldVersion;     // the installed one's mod.ini Version=
        size_t files = 0;
        uint64_t bytes = 0;
    };
    struct Plan
    {
        std::filesystem::path zip;
        std::vector<Found> mods;
        std::string error; // why it can't be installed ("" when it can)
    };

    // Reads the zip's table. A mod is every folder holding a mod.ini (the zip's root, or any folder below; a mod.ini in
    // a folder of another mod's is that mod's file); files outside the mods (a readme next to them) are left out. error:
    // not a zip, no mod.ini, encrypted, packed some way other than stored / deflate, or a name leaving its folder.
    Plan inspect(const std::filesystem::path &zip, const std::filesystem::path &modsDir);

    struct Progress
    {
        std::atomic<uint64_t> done{0}, total{0}; // bytes unpacked / to unpack
    };
    // Unpacks each mod into mods/.installing-<folder>, checks every file's CRC, then puts it in place (a folder of that
    // name moved aside and removed). False + error at the first failure: nothing half-unpacked is left as a mod.
    bool install(const Plan &plan, const std::filesystem::path &modsDir, std::string &error, Progress *progress = nullptr);
}
