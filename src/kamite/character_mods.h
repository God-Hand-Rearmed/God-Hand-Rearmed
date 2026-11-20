#pragma once

#include <cstdint>
#include <filesystem>
#include <map>
#include <string>
#include <vector>

namespace kamite::characters
{
    // A playable character model in place of Gene: <exe>/mods/characters/<id>/ holds replacement game files under
    // their disc paths (pl/pl00.dat ...), built by 'build.py models' from your disc and the mod's own data, plus
    // character.ini ([Character] Name=, Credit=).
    struct Costume
    {
        std::string id;   // [Mods] Costume
        std::string name; // shown in the launcher
    };
    struct Character
    {
        std::string id;     // folder name; "gene" = the game's own, "olivia" = built at boot (olivia_player.h)
        std::string name;   // shown in the launcher
        std::string credit; // shown under it
        std::vector<Costume> costumes; // the launcher's Costume row (none: no row); the first is the default
    };

    // Gene first, Olivia (not in releases), the built-in characters (builtin_characters.h: resources/characters), then
    // every other folder under mods/characters (a folder named like a built-in character is left out: the built-in
    // one wins).
    std::vector<Character> list(const std::filesystem::path &exeDir);
    // A character folder's id and its character.ini [Character] Name= / Credit=.
    Character describe(const std::filesystem::path &folder);

    // After PS2Runtime::setIoPaths, before the game runs: [Mods] Character (KAMITE_CHARACTER). Every file in the
    // character's folder under a game file's disc path replaces that file through the file layer (file_layer.h: any
    // size, served at the read). A textures/ folder (PCSX2 replacement images named after the character's in-game
    // textures; every character's, the built-in ones' too) becomes an extra replacement folder of the hardware renderer
    // (PS2X_PCSX2_TEXTURE_EXTRA), so the character draws at full resolution. Olivia and the built-in characters are
    // served by their own modules (olivia_player.h, builtin_characters.h).
    void install(const std::filesystem::path &exeDir);

    // character.ini [Hair]: strands on spare head bones that the port swings (bone_tweaks). bone < 0: none.
    struct HairStrand
    {
        int bone = -1;
        float pivot[3] = {}, dir[3] = {}; // relative to the head bone (4), game bind space; dir = root to tip at rest
        float length = 0.2f;
        bool hinge = false;               // true: swings only about the head's side axis (two strands, one bone)
    };
    struct Hair
    {
        HairStrand back, side;
        float stiffness = 40.0f, damping = 5.0f, gravity = 0.6f, wind = 1.2f;
        float backForward = 8.0f, backBack = 40.0f, backSide = 25.0f, sideSwing = 20.0f; // degrees
    };
    const Hair &hair();
    // The hair of a character served by another module (a built-in character, once its files are in place).
    void useHair(const std::filesystem::path &folder);
}
