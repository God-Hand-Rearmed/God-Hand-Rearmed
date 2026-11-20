#include "kamite/launcher_steam.h"
#include "kamite/launcher_platform.h"
#include "kamite/log.h"
#include "kamite/steam_shortcuts.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>
#include <system_error>
#include <thread>
#include <vector>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <shellapi.h>
#include <tlhelp32.h>
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "advapi32.lib")
#endif

namespace kamite::launcher::steam
{
    namespace
    {
        namespace fs = std::filesystem;

        // The library art. The cover ships in resources/images (steam_cover.jpg: masterlynx's art with our logo, used
        // with the artist's permission; README credit); the wide cover and the hero are K's SteamGridDB picks,
        // downloaded when the button is pressed, never shipped (README credits their makers; a dead link is a one-line
        // swap here). The logo and the icon are ours (resources/images/steam_logo.png: the launcher's layers
        // flattened; steam_icon.png: ghr.ico's 256 px frame). The icon is a PNG of its own: Steam reads GHR.exe's
        // PNG-compressed icon frames with a black background.
        struct Art
        {
            const char *name;   // for the popup: "Couldn't download the <name>."
            const char *suffix; // Steam's grid file: <appid><suffix>.<ext>
            const char *url;    // downloaded (the first of these that answers: a SteamGridDB page shows the picture's
            const char *alt[2]; // thumbnail, whose full size may be a .png or a .jpg), or
            const char *file;   // read from resources/images
        };
        constexpr Art kArt[] = {
            {"cover", "p", nullptr, {}, "steam_cover.jpg"}, // 600x900
            // K's pick (Nov 18): its full size, else its thumbnail
            {"wide cover", "", "https://cdn2.steamgriddb.com/grid/792d0821bdce2991d71967b4a95ed8b2.png",
             {"https://cdn2.steamgriddb.com/grid/792d0821bdce2991d71967b4a95ed8b2.jpg",
              "https://cdn2.steamgriddb.com/thumb/792d0821bdce2991d71967b4a95ed8b2.jpg"},
             nullptr},
            {"hero banner", "_hero", "https://cdn2.steamgriddb.com/hero/7cff4ce87d7f2179d73e7959bf213529.jpg", {}, nullptr}, // 3840x1240
        };

        std::string utf8(const fs::path &path)
        {
            const std::u8string text = path.u8string();
            return std::string(text.begin(), text.end());
        }

        // ---- The platform's pieces (Windows, Linux; elsewhere none: Steam reads as not installed) ----
#if defined(_WIN32)
        std::wstring wide(const std::string &text)
        {
            const int n = MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0);
            std::wstring out(static_cast<size_t>(n), L'\0');
            MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), out.data(), n);
            return out;
        }

        std::wstring registryText(const wchar_t *name)
        {
            wchar_t buffer[1024];
            DWORD size = sizeof(buffer);
            if (RegGetValueW(HKEY_CURRENT_USER, L"Software\\Valve\\Steam", name, RRF_RT_REG_SZ, nullptr, buffer, &size) != ERROR_SUCCESS)
                return {};
            return buffer;
        }

        fs::path steamRoot()
        {
            const std::wstring root = registryText(L"SteamPath");
            return root.empty() ? fs::path() : fs::path(root).make_preferred();
        }

        fs::path steamExe(const fs::path &root)
        {
            const std::wstring exe = registryText(L"SteamExe");
            return exe.empty() ? root / "steam.exe" : fs::path(exe).make_preferred();
        }

        fs::path thisExe()
        {
            std::wstring path(32768, L'\0');
            const DWORD n = GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
            path.resize(n);
            return fs::path(path);
        }

        bool steamRunning()
        {
            const HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
            if (snapshot == INVALID_HANDLE_VALUE)
                return false;
            PROCESSENTRY32W entry{};
            entry.dwSize = sizeof(entry);
            bool found = false;
            for (BOOL more = Process32FirstW(snapshot, &entry); more && !found; more = Process32NextW(snapshot, &entry))
                found = _wcsicmp(entry.szExeFile, L"steam.exe") == 0;
            CloseHandle(snapshot);
            return found;
        }

        void runSteam(const fs::path &exe, const wchar_t *arguments)
        {
            ShellExecuteW(nullptr, L"open", exe.c_str(), arguments, exe.parent_path().c_str(), SW_SHOWNORMAL);
        }

        bool download(const char *url, std::vector<uint8_t> &out)
        {
            return platform::httpGet(url, out, 64u << 20); // no library picture is that big
        }

#else
        // Linux: Steam's folder is one of three (the native package's two names for the same folder, and the
        // Flatpak's); the one with a signed-in account wins.
        fs::path homeDir()
        {
            const char *home = std::getenv("HOME");
            return home && *home ? fs::path(home) : fs::path();
        }

        fs::path flatpakRoot() { return homeDir() / ".var" / "app" / "com.valvesoftware.Steam" / "data" / "Steam"; }

        fs::path steamRoot()
        {
            const fs::path home = homeDir();
            if (home.empty())
                return {};
            std::error_code error;
            for (const fs::path &root : {home / ".local" / "share" / "Steam", home / ".steam" / "steam", home / ".steam" / "root", flatpakRoot()})
                if (fs::is_regular_file(root / "config" / "loginusers.vdf", error))
                {
                    // ~/.steam/steam is a link to ~/.local/share/Steam: the real path, so the files are named once.
                    const fs::path real = fs::canonical(root, error);
                    return error ? root : real;
                }
            return {};
        }

        // How Steam is started: the Flatpak through flatpak, the native one through the steam command (its launcher
        // script hands -shutdown to the running Steam).
        fs::path steamExe(const fs::path &root) { return root; }

        fs::path thisExe() { return platform::thisExe(); }

        bool steamRunning()
        {
            // Steam's main process is named "steam", natively and inside the Flatpak (whose processes /proc shows too).
            std::error_code error;
            for (const auto &entry : fs::directory_iterator("/proc", error))
            {
                const std::string pid = entry.path().filename().string();
                if (pid.empty() || !std::all_of(pid.begin(), pid.end(), [](char c) { return c >= '0' && c <= '9'; }))
                    continue;
                std::ifstream comm(entry.path() / "comm");
                std::string name;
                if (std::getline(comm, name) && name == "steam")
                    return true;
            }
            return false;
        }

        // `arguments` is the one Windows takes (nullptr or L"-shutdown"); the same meaning here.
        void runSteam(const fs::path &root, const wchar_t *arguments)
        {
            std::vector<std::string> args;
            std::error_code error;
            const fs::path flatpak = fs::canonical(flatpakRoot(), error);
            const bool isFlatpak = !error && root == flatpak;
            std::string program;
            if (isFlatpak)
            {
                program = "flatpak";
                args = {"run", "com.valvesoftware.Steam"};
            }
            else
                program = "steam";
            if (arguments != nullptr)
                args.push_back("-shutdown");
            if (!platform::spawn(program, args))
                KM_WARN(Boot, "launcher: steam: could not run " << program);
        }

        bool download(const char *url, std::vector<uint8_t> &out)
        {
            return platform::httpGet(url, out, 64u << 20); // no library picture is that big
        }
#endif
        using vdf::Node;
        using vdf::find;
        using vdf::ourEntry;

        // ---- Steam's files ----

        bool readFile(const fs::path &path, std::vector<uint8_t> &out)
        {
            std::ifstream file(path, std::ios::binary);
            if (!file)
                return false;
            out.assign(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
            return true;
        }

        bool writeFile(const fs::path &path, const void *data, size_t size)
        {
            const fs::path temp = fs::path(path).concat(".ghr-tmp");
            {
                std::ofstream file(temp, std::ios::binary | std::ios::trunc);
                if (!file || !file.write(static_cast<const char *>(data), static_cast<std::streamsize>(size)))
                    return false;
            }
            std::error_code error;
            fs::rename(temp, path, error);
            if (error)
                fs::remove(temp, error);
            return !error;
        }

        struct Account
        {
            fs::path root;   // Steam's folder
            fs::path config; // userdata/<account>/config
        };

        bool locate(Account &account)
        {
            account.root = steamRoot();
            std::error_code error;
            if (account.root.empty() || !fs::is_directory(account.root, error))
                return false;
            std::vector<uint8_t> loginUsers;
            if (!readFile(account.root / "config" / "loginusers.vdf", loginUsers))
                return false;
            const std::string id = vdf::recentAccount(loginUsers);
            if (id.empty() || !fs::is_directory(account.root / "userdata" / id, error))
                return false;
            account.config = account.root / "userdata" / id / "config";
            return true;
        }

        // shortcuts.vdf read into the "shortcuts" map's entries. A missing file is an empty list; a file that can't
        // be read, or in a format we don't know, is false.
        bool readShortcuts(const fs::path &file, std::vector<Node> &root)
        {
            std::vector<uint8_t> data;
            std::error_code error;
            if (fs::exists(file, error) && !readFile(file, data))
                return false;
            return vdf::readShortcuts(data, root);
        }

    }

    bool running() { return steamRunning(); }

    Status probe()
    {
        Status status;
#if !defined(_WIN32)
        // Started by Steam (it sets SteamGameId; Game Mode on a Deck always does): the game is in Steam already, and
        // closing Steam to write the list would end GHR with it. The button stays hidden.
        if (const char *game = std::getenv("SteamGameId"); game && *game)
            return status;
#endif
        Account account;
        if (!locate(account))
            return status;
        status.available = true;
        std::vector<Node> root;
        if (readShortcuts(account.config / "shortcuts.vdf", root))
            status.added = ourEntry(find(root, "shortcuts")->kids, utf8(thisExe())) != nullptr;
        return status;
    }

    Result addOrUpdate(const fs::path &resources, std::atomic<int> &stage)
    {
        Result result;
        const fs::path exePath = thisExe();
        const auto done = [&](bool ok, std::string message) {
            result.ok = ok;
            result.message = std::move(message);
            stage = static_cast<int>(Stage::Done);
            KM_INFO(Boot, "launcher: steam: " << (ok ? "" : "failed: ") << result.message);
            return result;
        };
        Account account;
        if (!locate(account))
            return done(false, "Steam couldn't be found on this PC.");

        // The art first, so Steam is closed only for the moment the files are written.
        stage = static_cast<int>(Stage::Downloading);
        std::vector<uint8_t> art[std::size(kArt)];
        bool got[std::size(kArt)] = {};
        for (size_t i = 0; i < std::size(kArt); ++i)
        {
            const auto picture = [&] {
                return art[i].size() > 8 && ((art[i][0] == 0x89 && art[i][1] == 'P' && art[i][2] == 'N' && art[i][3] == 'G') ||
                                             (art[i][0] == 0xFF && art[i][1] == 0xD8 && art[i][2] == 0xFF));
            };
            if (kArt[i].file)
                got[i] = readFile(resources / "images" / kArt[i].file, art[i]) && picture();
            else
                for (const char *url : {kArt[i].url, kArt[i].alt[0], kArt[i].alt[1]})
                    if (url && !got[i])
                        got[i] = download(url, art[i]) && picture();
            KM_INFO(Boot, "launcher: steam: " << kArt[i].name << (got[i] ? " ready (" : " missing (") << art[i].size() << " bytes)");
        }

        const bool wasRunning = steamRunning();
        const fs::path steam = steamExe(account.root);
        if (wasRunning)
        {
            stage = static_cast<int>(Stage::ClosingSteam);
            runSteam(steam, L"-shutdown");
            const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(30);
            while (steamRunning() && std::chrono::steady_clock::now() < until)
                std::this_thread::sleep_for(std::chrono::milliseconds(250));
            if (steamRunning())
                return done(false, "Steam didn't close, so nothing was changed.\nClose Steam yourself and try again.");
            std::this_thread::sleep_for(std::chrono::milliseconds(500)); // its last writes
        }
        const auto restart = [&] {
            if (wasRunning)
            {
                stage = static_cast<int>(Stage::StartingSteam);
                runSteam(steam, nullptr);
            }
        };

        stage = static_cast<int>(Stage::Writing);
        const fs::path list = account.config / "shortcuts.vdf";
        std::vector<Node> root;
        if (!readShortcuts(list, root))
        {
            restart();
            return done(false, "Steam's shortcut list couldn't be read, so nothing was changed.");
        }
        const fs::path grid = account.config / "grid";
        uint32_t appid = 0;
        // StartDir: the folder holding the program, as on Windows (the AppImage's own folder when it is one). GHR
        // finds its play folder by itself, wherever it starts from.
        const bool update = vdf::addEntry(root, exePath, exePath.parent_path(), grid, appid);

        std::error_code error;
        fs::create_directories(account.config, error);
        std::vector<uint8_t> current;
        if (fs::exists(list, error) && readFile(list, current))
            writeFile(fs::path(list).concat(".ghr-backup"), current.data(), current.size()); // the list as it was
        std::vector<uint8_t> bytes;
        vdf::writeMap(bytes, root);
        if (!writeFile(list, bytes.data(), bytes.size()))
        {
            restart();
            return done(false, "Steam's shortcut list couldn't be written, so nothing was changed.");
        }

        // The art: grid/<appid><suffix>.<ext>. A picture that didn't download keeps the one from last time.
        fs::create_directories(grid, error);
        const std::string id = std::to_string(appid);
        std::string missing;
        for (size_t i = 0; i < std::size(kArt); ++i)
        {
            if (!got[i])
            {
                missing += std::string("\nThe ") + kArt[i].name + (kArt[i].file ? " is missing from the resources folder." : " couldn't be downloaded.");
                continue;
            }
            const char *ext = art[i][0] == 0x89 ? ".png" : ".jpg";
            for (const char *old : {".png", ".jpg", ".jpeg"})
                if (std::strcmp(old, ext) != 0)
                    fs::remove(grid / (id + kArt[i].suffix + old), error);
            if (!writeFile(grid / (id + kArt[i].suffix + ext), art[i].data(), art[i].size()))
                missing += std::string("\nThe ") + kArt[i].name + " couldn't be saved.";
        }
        for (const char *ours : {"_logo", "_icon"})
        {
            std::vector<uint8_t> image;
            if (readFile(resources / "images" / (std::string("steam") + ours + ".png"), image) && !image.empty())
                writeFile(grid / (id + ours + ".png"), image.data(), image.size());
        }

        restart();
        std::string message = update ? "Steam entry updated." : "Added to Steam.";
        message += missing;
        if (!missing.empty())
            message += "\nUpdate the Steam entry later to try again.";
        if (!wasRunning)
            message += "\nIt shows up the next time Steam starts.";
        return done(true, message);
    }
}
