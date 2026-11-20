// Steam's account list and shortcut list (steam_shortcuts.h). Moved out of launcher_steam.cpp unchanged, apart from
// taking the files' bytes instead of their paths and StartDir as a parameter, so the unit tests can run them.
#include "kamite/steam_shortcuts.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <cstring>

namespace kamite::launcher::steam::vdf
{
    namespace fs = std::filesystem;

    namespace
    {
        std::string utf8(const fs::path &path)
        {
            const std::u8string text = path.u8string();
            return std::string(text.begin(), text.end());
        }
    }

    bool samePath(const std::string &a, const std::string &b)
    {
#if defined(_WIN32)
        std::string x = a, y = b;
        for (std::string *s : {&x, &y})
            for (char &c : *s)
                c = c == '/' ? '\\' : static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        return x == y;
#else
        return a == b;
#endif
    }

    // config/loginusers.vdf (text KeyValues): the account marked MostRecent (the newest Timestamp otherwise),
    // as its account id (the userdata folder's name).
    std::string recentAccount(const std::vector<uint8_t> &data)
    {
        std::vector<std::string> tokens; // quoted strings; "{" and "}" as themselves
        for (size_t i = 0; i < data.size(); ++i)
        {
            const char c = static_cast<char>(data[i]);
            if (c == '{' || c == '}')
                tokens.emplace_back(1, c);
            else if (c == '"')
            {
                std::string text;
                for (++i; i < data.size() && data[i] != '"'; ++i)
                {
                    if (data[i] == '\\' && i + 1 < data.size())
                        ++i;
                    text += static_cast<char>(data[i]);
                }
                tokens.push_back(text);
            }
        }
        std::string best;
        bool bestRecent = false;
        unsigned long long bestTime = 0;
        int depth = 0;
        std::string user;
        bool recent = false;
        unsigned long long time = 0;
        for (size_t i = 0; i < tokens.size(); ++i)
        {
            if (tokens[i] == "{")
                ++depth;
            else if (tokens[i] == "}")
            {
                if (depth == 2 && !user.empty() && (best.empty() || (recent && !bestRecent) || (recent == bestRecent && time > bestTime)))
                {
                    best = user;
                    bestRecent = recent;
                    bestTime = time;
                }
                --depth;
            }
            else if (depth == 1 && i + 1 < tokens.size() && tokens[i + 1] == "{")
            {
                user = tokens[i];
                recent = false;
                time = 0;
            }
            else if (depth == 2 && i + 1 < tokens.size() && tokens[i + 1] != "{" && tokens[i + 1] != "}")
            {
                std::string key = tokens[i];
                std::transform(key.begin(), key.end(), key.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
                if (key == "mostrecent")
                    recent = tokens[i + 1] == "1";
                else if (key == "timestamp")
                    time = std::strtoull(tokens[i + 1].c_str(), nullptr, 10);
                ++i;
            }
        }
        const unsigned long long id64 = std::strtoull(best.c_str(), nullptr, 10);
        constexpr unsigned long long kIndividualBase = 76561197960265728ull;
        if (id64 <= kIndividualBase)
            return {};
        return std::to_string(id64 - kIndividualBase);
    }

    static bool readText(const std::vector<uint8_t> &d, size_t &p, std::string &out)
    {
        const size_t start = p;
        while (p < d.size() && d[p] != 0)
            ++p;
        if (p >= d.size())
            return false;
        out.assign(reinterpret_cast<const char *>(d.data()) + start, p - start);
        ++p;
        return true;
    }

    bool readMap(const std::vector<uint8_t> &d, size_t &p, std::vector<Node> &out, int depth)
    {
        if (depth > 32)
            return false;
        while (p < d.size())
        {
            Node node;
            node.type = d[p++];
            if (node.type == 0x08)
                return true;
            if (!readText(d, p, node.key))
                return false;
            size_t bytes = 0;
            switch (node.type)
            {
            case 0:
                if (!readMap(d, p, node.kids, depth + 1))
                    return false;
                break;
            case 1:
                if (!readText(d, p, node.text))
                    return false;
                break;
            case 2:
            case 3:
            case 4:
            case 6:
                bytes = 4;
                break;
            case 7:
            case 10:
                bytes = 8;
                break;
            default:
                return false;
            }
            if (bytes != 0)
            {
                if (p + bytes > d.size())
                    return false;
                node.raw.assign(d.begin() + static_cast<std::ptrdiff_t>(p), d.begin() + static_cast<std::ptrdiff_t>(p + bytes));
                p += bytes;
            }
            out.push_back(std::move(node));
        }
        return false; // ran out before the map's end
    }

    void writeMap(std::vector<uint8_t> &out, const std::vector<Node> &nodes)
    {
        for (const Node &node : nodes)
        {
            out.push_back(node.type);
            out.insert(out.end(), node.key.begin(), node.key.end());
            out.push_back(0);
            if (node.type == 0)
                writeMap(out, node.kids);
            else if (node.type == 1)
            {
                out.insert(out.end(), node.text.begin(), node.text.end());
                out.push_back(0);
            }
            else
                out.insert(out.end(), node.raw.begin(), node.raw.end());
        }
        out.push_back(0x08);
    }

    static bool sameKey(const std::string &a, const char *b)
    {
        const size_t n = std::strlen(b);
        if (a.size() != n)
            return false;
        for (size_t i = 0; i < n; ++i)
            if (std::tolower(static_cast<unsigned char>(a[i])) != std::tolower(static_cast<unsigned char>(b[i])))
                return false;
        return true;
    }

    Node *find(std::vector<Node> &nodes, const char *key)
    {
        for (Node &node : nodes)
            if (sameKey(node.key, key))
                return &node;
        return nullptr;
    }

    static std::string unquoted(std::string text)
    {
        if (text.size() >= 2 && text.front() == '"' && text.back() == '"')
            text = text.substr(1, text.size() - 2);
        return text;
    }

    static void setText(std::vector<Node> &entry, const char *key, const std::string &value)
    {
        if (Node *node = find(entry, key); node != nullptr && node->type == 1)
        {
            node->text = value;
            return;
        }
        Node node;
        node.type = 1;
        node.key = key;
        node.text = value;
        entry.push_back(std::move(node));
    }

    static void setNumber(std::vector<Node> &entry, const char *key, uint32_t value)
    {
        Node node;
        node.type = 2;
        node.key = key;
        node.raw = {static_cast<uint8_t>(value), static_cast<uint8_t>(value >> 8), static_cast<uint8_t>(value >> 16),
                    static_cast<uint8_t>(value >> 24)};
        if (Node *existing = find(entry, key); existing != nullptr)
            *existing = std::move(node);
        else
            entry.push_back(std::move(node));
    }

    uint32_t numberOf(const Node &node)
    {
        return node.raw.size() == 4 ? static_cast<uint32_t>(node.raw[0] | node.raw[1] << 8 | node.raw[2] << 16 | node.raw[3] << 24) : 0u;
    }

    static uint32_t crc32(const std::string &text)
    {
        uint32_t crc = 0xFFFFFFFFu;
        for (unsigned char c : text)
        {
            crc ^= c;
            for (int bit = 0; bit < 8; ++bit)
                crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));
        }
        return ~crc;
    }

    Node *ourEntry(std::vector<Node> &shortcuts, const std::string &exe)
    {
        Node *named = nullptr;
        for (Node &entry : shortcuts)
        {
            if (entry.type != 0)
                continue;
            if (const Node *target = find(entry.kids, "Exe"); target != nullptr && samePath(unquoted(target->text), exe))
                return &entry;
            if (const Node *name = find(entry.kids, "AppName"); named == nullptr && name != nullptr && name->text == kAppName)
                named = &entry;
        }
        return named;
    }

    // shortcuts.vdf read into the "shortcuts" map's entries. A missing file is an empty list; a file in a format
    // we don't know is false.
    bool readShortcuts(const std::vector<uint8_t> &data, std::vector<Node> &root)
    {
        root.clear();
        if (data.empty())
        {
            Node shortcuts;
            shortcuts.key = "shortcuts";
            root.push_back(std::move(shortcuts));
            return true;
        }
        size_t p = 0;
        if (!readMap(data, p, root, 0))
            return false;
        if (find(root, "shortcuts") == nullptr)
        {
            Node shortcuts;
            shortcuts.key = "shortcuts";
            root.push_back(std::move(shortcuts));
        }
        return find(root, "shortcuts")->type == 0;
    }

    bool addEntry(std::vector<Node> &root, const fs::path &exePath, const fs::path &startDir, const fs::path &grid, uint32_t &appid)
    {
        std::vector<Node> &shortcuts = find(root, "shortcuts")->kids;
        const std::string exe = utf8(exePath);
        const std::string exeQuoted = "\"" + exe + "\"";
        Node *entry = ourEntry(shortcuts, exe);
        const bool update = entry != nullptr;
        if (entry == nullptr)
        {
            int index = 0;
            while (find(shortcuts, std::to_string(index).c_str()) != nullptr)
                ++index;
            Node fresh;
            fresh.key = std::to_string(index);
            shortcuts.push_back(std::move(fresh));
            entry = &shortcuts.back();
        }
        // Steam's id for a shortcut: kept once it has one (its art, play time and collections hang on it).
        appid = 0;
        if (const Node *id = find(entry->kids, "appid"); id != nullptr && id->type == 2)
            appid = numberOf(*id);
        if (appid == 0)
        {
            appid = crc32(exeQuoted + kAppName) | 0x80000000u;
            setNumber(entry->kids, "appid", appid);
        }
        setText(entry->kids, "AppName", kAppName);
        setText(entry->kids, "Exe", exeQuoted);
        setText(entry->kids, "StartDir", "\"" + utf8(startDir) + "\"");
        setText(entry->kids, "icon", utf8(grid / (std::to_string(appid) + "_icon.png"))); // written with the art
        if (!update)
        {
            setText(entry->kids, "ShortcutPath", "");
            setText(entry->kids, "LaunchOptions", "");
            setNumber(entry->kids, "IsHidden", 0);
            setNumber(entry->kids, "AllowDesktopConfig", 1);
            setNumber(entry->kids, "AllowOverlay", 1);
            setNumber(entry->kids, "OpenVR", 0);
            setNumber(entry->kids, "Devkit", 0);
            setText(entry->kids, "DevkitGameID", "");
            setNumber(entry->kids, "DevkitOverrideAppID", 0);
            setNumber(entry->kids, "LastPlayTime", 0);
            setText(entry->kids, "FlatpakAppID", "");
            Node tags;
            tags.key = "tags";
            entry->kids.push_back(std::move(tags));
        }
        return update;
    }
}
