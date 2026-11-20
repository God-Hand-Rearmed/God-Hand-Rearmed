#pragma once

// The Steam Deck: told apart from other PCs so a fresh GHR.ini starts with the settings its screen wants.
#include <filesystem>
#include <string>

namespace kamite::steam_deck
{
    // A Steam Deck: SteamDeck=1 in the environment (Steam sets it in Game Mode and Desktop Mode), or the board Valve
    // made: <sysRoot>/sys/devices/virtual/dmi/id/board_vendor "Valve" and product_name "Jupiter" (LCD) or "Galileo"
    // (OLED). `steamDeckVariable` is SteamDeck's value (nullptr when unset); `sysRoot` is "/" outside the tests.
    bool detect(const char *steamDeckVariable, const std::filesystem::path &sysRoot);
    // detect() with the real environment and "/" (always false on Windows: no sysfs, and SteamDeck there is ignored).
    bool detected();

    // GHR.ini's text with the Deck's starting values in [Video]: borderless full screen (StartFullscreen=1,
    // StartMaximized=0) and the picture's shape matching the 16:10 screen (AspectRatio=auto, the launcher's "Match the
    // screen"). Only the values of those keys change; comments, order and the other sections stay as they are.
    std::string withDeckDefaults(const std::string &ini);
}
