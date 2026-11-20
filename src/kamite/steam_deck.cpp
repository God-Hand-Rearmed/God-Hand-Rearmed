// The Steam Deck's detection and starting settings (steam_deck.h).
#include "kamite/steam_deck.h"

#include <cctype>
#include <cstdlib>
#include <fstream>
#include <sstream>

namespace kamite::steam_deck
{
    namespace fs = std::filesystem;

    namespace
    {
        // A one-line sysfs file without its newline and spaces ("" when it cannot be read).
        std::string firstLine(const fs::path &path)
        {
            std::ifstream file(path);
            std::string line;
            std::getline(file, line);
            const size_t begin = line.find_first_not_of(" \t\r\n");
            const size_t end = line.find_last_not_of(" \t\r\n");
            return begin == std::string::npos ? std::string() : line.substr(begin, end - begin + 1);
        }

        std::string lower(std::string text)
        {
            for (char &c : text)
                c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            return text;
        }

        std::string trim(const std::string &text)
        {
            const size_t begin = text.find_first_not_of(" \t\r");
            const size_t end = text.find_last_not_of(" \t\r");
            return begin == std::string::npos ? std::string() : text.substr(begin, end - begin + 1);
        }
    }

    bool detect(const char *steamDeckVariable, const fs::path &sysRoot)
    {
        if (steamDeckVariable != nullptr && std::string(steamDeckVariable) == "1")
            return true;
        const fs::path dmi = sysRoot / "sys" / "devices" / "virtual" / "dmi" / "id";
        if (firstLine(dmi / "board_vendor") != "Valve")
            return false;
        const std::string product = firstLine(dmi / "product_name");
        return product == "Jupiter" || product == "Galileo";
    }

    bool detected()
    {
#if defined(_WIN32)
        return false;
#else
        static const bool deck = detect(std::getenv("SteamDeck"), "/");
        return deck;
#endif
    }

    std::string withDeckDefaults(const std::string &ini)
    {
        struct Value
        {
            const char *key; // lower case
            const char *value;
        };
        static constexpr Value kVideo[] = {{"startfullscreen", "1"}, {"startmaximized", "0"}, {"aspectratio", "auto"}};
        std::istringstream in(ini);
        std::string out, line, section;
        while (std::getline(in, line))
        {
            const bool cr = !line.empty() && line.back() == '\r';
            const std::string body = cr ? line.substr(0, line.size() - 1) : line;
            const std::string t = trim(body);
            if (t.size() >= 2 && t.front() == '[' && t.back() == ']')
                section = lower(trim(t.substr(1, t.size() - 2)));
            else if (section == "video" && !t.empty() && t[0] != '#' && t[0] != ';')
            {
                const size_t equals = body.find('=');
                if (equals != std::string::npos)
                {
                    const std::string key = lower(trim(body.substr(0, equals)));
                    for (const Value &v : kVideo)
                        if (key == v.key)
                            line = body.substr(0, equals + 1) + v.value + (cr ? "\r" : "");
                }
            }
            out += line;
            out += '\n';
        }
        // getline drops the last newline's absence: keep the text's own ending.
        if (!ini.empty() && ini.back() != '\n' && !out.empty())
            out.pop_back();
        return out;
    }
}
