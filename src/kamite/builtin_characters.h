#pragma once

#include <filesystem>
#include <string>
#include <vector>

// The playable characters that ship with the port (Xavier): <exe>/resources/characters/<id>/ holds the character's own
// work (character.json, its nodes and textures, its hand-painted layers, character.ini, full-resolution textures/),
// copied from assets/characters/<id>/ by the build. No game data ships: the player files are made from the player's own
// disc (character_build.h) the first time GHR starts and again whenever an input changes, and kept in
// <exe>/cache/characters/<id>/ (pl/pl00.dat ..., textures/ for the painted textures' full-resolution images,
// build.key: the hash of every input). Whatever character is chosen, each built-in one's cache is made when missing
// or stale, and its textures/ folder is added to the replacement folders (a save state made as Xavier and loaded with
// Gene chosen brings Xavier's textures back, and finds their images).
//
// The chosen one is served through the file layer: its pl/pl00.dat for all of Gene's body files (every costume's) and
// its pl/pl01.dat for all of his coat files, so Gene's costume choice never reaches the character.
namespace kamite::builtin_characters
{
    // The built-in characters' folders (those with a character.json), sorted.
    std::vector<std::filesystem::path> folders(const std::filesystem::path &exeDir);
    bool isBuiltIn(const std::filesystem::path &exeDir, const std::string &id);

    // After characters::install and hd_textures::install (the pack's names), before the game runs.
    void install(const std::filesystem::path &exeDir);
}
