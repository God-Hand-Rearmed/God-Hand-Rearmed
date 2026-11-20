#pragma once

// Steam's files for "Add to Steam" (launcher_steam.cpp), without any operating-system call, so the unit tests can
// reach them: the signed-in accounts (config/loginusers.vdf, text) and the non-Steam shortcut list
// (userdata/<account>/config/shortcuts.vdf, binary KeyValues).
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace kamite::launcher::steam::vdf
{
    inline constexpr const char *kAppName = "God Hand: Rearmed";

    // Binary KeyValues: a type byte, a key (C string), the value; 0x08 ends a map. Every node is kept as read, so
    // whatever Steam or other tools stored round-trips; an unknown type stops the read and nothing is written.
    struct Node
    {
        uint8_t type = 0;
        std::string key;
        std::string text;         // 1: string
        std::vector<uint8_t> raw; // 2 / 3 / 4 / 6: four bytes; 7 / 10: eight (little-endian)
        std::vector<Node> kids;   // 0: map
    };

    bool readMap(const std::vector<uint8_t> &d, size_t &p, std::vector<Node> &out, int depth);
    void writeMap(std::vector<uint8_t> &out, const std::vector<Node> &nodes);
    Node *find(std::vector<Node> &nodes, const char *key);
    uint32_t numberOf(const Node &node);

    // loginusers.vdf's text: the account marked MostRecent (the newest Timestamp otherwise), as its account id (the
    // userdata folder's name); "" when there is none.
    std::string recentAccount(const std::vector<uint8_t> &loginUsers);

    // The same file on this system: Windows paths compare without case and with either slash; elsewhere exactly.
    bool samePath(const std::string &a, const std::string &b);

    // Our entry in the shortcut list: the one starting `exe`, else one named God Hand: Rearmed (the program was
    // moved: Update re-points it).
    Node *ourEntry(std::vector<Node> &shortcuts, const std::string &exe);

    // shortcuts.vdf's bytes read into `root` (its "shortcuts" map made when missing). Empty data is an empty list;
    // data in a format we don't know is false.
    bool readShortcuts(const std::vector<uint8_t> &data, std::vector<Node> &root);

    // Adds our entry to the list readShortcuts made, or re-points the one there (true): Exe (quoted), StartDir (quoted
    // `startDir`), the icon in `grid`; Steam's id for it in appid.
    bool addEntry(std::vector<Node> &root, const std::filesystem::path &exePath, const std::filesystem::path &startDir,
                  const std::filesystem::path &grid, uint32_t &appid);
}
