// GHR's unit tests: the pure logic of the operating-system layer (paths, the Steam Deck, update downloads and the
// tar.gz, Steam's shortcut list). One plain program, no framework: each CHECK prints the failures, the exit code is
// the number of them. Needs neither the game code nor a display. Run: ctest (or the ghr_unit_tests program itself).
#include "kamite/paths.h"
#include "kamite/steam_deck.h"
#include "kamite/steam_shortcuts.h"
#include "kamite/update_archive.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "fixtures.inc"

namespace fs = std::filesystem;
namespace archive = kamite::launcher::update::archive;
namespace vdf = kamite::launcher::steam::vdf;

namespace
{
    int g_failures = 0;
    int g_checks = 0;

#define CHECK(condition)                                                                                               \
    do                                                                                                                 \
    {                                                                                                                  \
        ++g_checks;                                                                                                    \
        if (!(condition))                                                                                              \
        {                                                                                                              \
            ++g_failures;                                                                                              \
            std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #condition);                                  \
        }                                                                                                              \
    } while (0)

#define CHECK_EQ(a, b)                                                                                                 \
    do                                                                                                                 \
    {                                                                                                                  \
        ++g_checks;                                                                                                    \
        const auto &va_ = (a);                                                                                         \
        const auto &vb_ = (b);                                                                                         \
        if (!(va_ == vb_))                                                                                             \
        {                                                                                                              \
            ++g_failures;                                                                                              \
            std::ostringstream text_;                                                                                  \
            text_ << va_ << " != " << vb_;                                                                             \
            std::fprintf(stderr, "FAIL %s:%d: %s == %s (%s)\n", __FILE__, __LINE__, #a, #b, text_.str().c_str());      \
        }                                                                                                              \
    } while (0)

    // A fresh, empty folder under the system's temporary folder, removed when the test ends.
    struct TempDir
    {
        fs::path path;
        TempDir()
        {
            static int counter = 0;
            const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
            path = fs::temp_directory_path() / ("ghr_unit_" + std::to_string(stamp) + "_" + std::to_string(counter++));
            fs::remove_all(path);
            fs::create_directories(path);
        }
        ~TempDir()
        {
            std::error_code ignored;
            fs::remove_all(path, ignored);
        }
    };

    void writeText(const fs::path &path, const std::string &text)
    {
        fs::create_directories(path.parent_path());
        std::ofstream(path, std::ios::binary) << text;
    }

    std::vector<uint8_t> bytes(const unsigned char *data, size_t size) { return std::vector<uint8_t>(data, data + size); }

    // ---- paths ------------------------------------------------------------------------------------------------------
    void testPaths()
    {
        TempDir tmp;
        const fs::path program = tmp.path / "program";
        fs::create_directories(program);
        std::string note;

        CHECK(kamite::paths::resolveHome(nullptr, program, tmp.path, note) == program);
        CHECK(note.empty());
        CHECK(kamite::paths::resolveHome("", program, tmp.path, note) == program);

        const fs::path absolute = tmp.path / "data" / "GodHandRearmed";
        CHECK(kamite::paths::resolveHome(absolute.string().c_str(), program, tmp.path, note) == absolute);
        CHECK(fs::is_directory(absolute));
        CHECK(note.empty());

        // A trailing slash is dropped, so <home>/GHR.ini reads cleanly.
        const std::string slashed = (tmp.path / "slashed").string() + "/";
        CHECK(kamite::paths::resolveHome(slashed.c_str(), program, tmp.path, note) == tmp.path / "slashed");

        // Relative: against the given current folder.
        CHECK(kamite::paths::resolveHome("rel/home", program, tmp.path, note) == tmp.path / "rel" / "home");
        CHECK(fs::is_directory(tmp.path / "rel" / "home"));

        // Not creatable (a file is in the way): the program folder, and a note for the log.
        writeText(tmp.path / "blocker", "a file");
        const fs::path blocked = tmp.path / "blocker" / "home";
        CHECK(kamite::paths::resolveHome(blocked.string().c_str(), program, tmp.path, note) == program);
        CHECK(!note.empty());

        // The real ones: the test program's own folder (/proc/self/exe), resources/ under it.
        CHECK(!kamite::paths::programDir().empty());
        CHECK(fs::is_directory(kamite::paths::programDir()));
        CHECK(kamite::paths::resources() == kamite::paths::programDir() / "resources");
        const char *home = std::getenv("KAMITE_HOME");
        if (home == nullptr || *home == '\0')
            CHECK(kamite::paths::home() == kamite::paths::programDir());
    }

    void testDiscSearchFolders()
    {
        TempDir tmp;
        const fs::path home = tmp.path / "home" / "deck", media = tmp.path / "run" / "media";
        fs::create_directories(home / "Games");
        fs::create_directories(media / "deck" / "SDCARD");
        fs::create_directories(media / "deck" / "ANOTHER");
        fs::create_directories(media / "mmcblk0p1");
        writeText(media / "not-a-folder", "x");
        std::vector<fs::path> got = kamite::paths::discSearchFolders(home.string().c_str(), "deck", media);
        // ~/Downloads does not exist: left out. <media>/deck itself is not a drive.
        std::vector<fs::path> want = {home / "Games", media / "deck" / "ANOTHER", media / "deck" / "SDCARD", media / "mmcblk0p1"};
        CHECK(got == want);
        CHECK(kamite::paths::discSearchFolders(nullptr, nullptr, tmp.path / "missing").empty());
        // Without a user name the user's folder under the media root reads as one more drive.
        got = kamite::paths::discSearchFolders(nullptr, nullptr, media);
        CHECK_EQ(got.size(), size_t{2});
    }

    // ---- the Steam Deck ---------------------------------------------------------------------------------------------
    void writeDmi(const fs::path &root, const std::string &vendor, const std::string &product)
    {
        const fs::path dmi = root / "sys" / "devices" / "virtual" / "dmi" / "id";
        writeText(dmi / "board_vendor", vendor);
        writeText(dmi / "product_name", product);
    }

    void testSteamDeck()
    {
        TempDir empty;
        CHECK(kamite::steam_deck::detect("1", empty.path));
        CHECK(!kamite::steam_deck::detect("0", empty.path));
        CHECK(!kamite::steam_deck::detect(nullptr, empty.path));
        CHECK(!kamite::steam_deck::detect("", empty.path));

        TempDir lcd, oled, other, notValve;
        writeDmi(lcd.path, "Valve\n", "Jupiter\n");
        writeDmi(oled.path, "Valve\n", "Galileo\n");
        writeDmi(other.path, "Valve\n", "Steam Machine\n");
        writeDmi(notValve.path, "LENOVO\n", "Jupiter\n");
        CHECK(kamite::steam_deck::detect(nullptr, lcd.path));
        CHECK(kamite::steam_deck::detect(nullptr, oled.path));
        CHECK(!kamite::steam_deck::detect(nullptr, other.path));
        CHECK(!kamite::steam_deck::detect(nullptr, notValve.path));

        const std::string ini = "[Game]\nAspectRatio=keep\n# StartFullscreen=0 in a comment\n[Video]\r\n"
                                "AspectRatio=16:9\r\nStartMaximized=1\r\n  StartFullscreen = 0\r\nWindowSize=auto\r\n"
                                "[Run]\nStartFullscreen=0";
        const std::string want = "[Game]\nAspectRatio=keep\n# StartFullscreen=0 in a comment\n[Video]\r\n"
                                 "AspectRatio=auto\r\nStartMaximized=0\r\n  StartFullscreen =1\r\nWindowSize=auto\r\n"
                                 "[Run]\nStartFullscreen=0";
        CHECK_EQ(kamite::steam_deck::withDeckDefaults(ini), want);

        // The shipped GHR.ini still has the three keys in [Video] (a rename would make the Deck defaults silently do
        // nothing).
        std::ifstream file(fs::path(KAMITE_SOURCE_DIR) / "GHR.ini", std::ios::binary);
        const std::string shipped((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
        CHECK(!shipped.empty());
        const std::string deck = kamite::steam_deck::withDeckDefaults(shipped);
        CHECK(deck.find("\nAspectRatio=auto") != std::string::npos);
        CHECK(deck.find("\nStartFullscreen=1") != std::string::npos);
        CHECK(deck.find("\nStartMaximized=0") != std::string::npos);
        CHECK_EQ(deck.size(), shipped.size() - std::string("16:9").size() + std::string("auto").size());
    }

    // ---- update downloads -------------------------------------------------------------------------------------------
    void testPickAsset()
    {
        using archive::Package;
        const std::vector<std::string> all = {"GHR-v0.7.0-Linux-x64.AppImage.zsync", "GHR-v0.7.0-Linux-x64.tar.gz",
                                              "GHR-v0.7.0-Linux-x64.AppImage", "GHR-v0.7.0.zip", "SHA256SUMS.txt"};
        CHECK_EQ(archive::pickAsset(all, Package::WindowsZip), 3);
        CHECK_EQ(archive::pickAsset(all, Package::LinuxFolder), 1);
        CHECK_EQ(archive::pickAsset(all, Package::LinuxAppImage), 2);
        // Case does not matter.
        CHECK_EQ(archive::pickAsset({"ghr-V0.7.0-LINUX-X64.TAR.GZ"}, Package::LinuxFolder), 0);
        CHECK_EQ(archive::pickAsset({"GHR-V0.7.0.ZIP"}, Package::WindowsZip), 0);
        // Windows never takes a Linux download, whatever it is packed in.
        const std::vector<std::string> linuxOnly = {"GHR-v0.7.0-Linux-x64.tar.gz", "GHR-v0.7.0-Linux-x64.AppImage",
                                                    "GHR-v0.7.0-Linux-x64.zip", "GHR-v0.7.0-Linux-x64.AppImage.zip"};
        CHECK_EQ(archive::pickAsset(linuxOnly, Package::WindowsZip), -1);
        // Linux never takes the Windows zip; each Linux form only its own.
        CHECK_EQ(archive::pickAsset({"GHR-v0.7.0.zip"}, Package::LinuxFolder), -1);
        CHECK_EQ(archive::pickAsset({"GHR-v0.7.0.zip"}, Package::LinuxAppImage), -1);
        CHECK_EQ(archive::pickAsset({"GHR-v0.7.0-Linux-x64.AppImage"}, Package::LinuxFolder), -1);
        CHECK_EQ(archive::pickAsset({"GHR-v0.7.0-Linux-x64.tar.gz"}, Package::LinuxAppImage), -1);
        CHECK_EQ(archive::pickAsset({"GHR-v0.7.0-Linux-arm64.tar.gz", "GHR-v0.7.0-Linux-arm64.AppImage"}, Package::LinuxFolder), -1);
        CHECK_EQ(archive::pickAsset({}, Package::WindowsZip), -1);
        // The tester zips stay a zip (Windows' rule as it was).
        CHECK_EQ(archive::pickAsset({"GHR-tester-1a2b3c4-1795100000.zip"}, Package::WindowsZip), 0);
#if defined(_WIN32)
        CHECK(archive::current() == Package::WindowsZip);
#else
        const char *appImage = std::getenv("APPIMAGE");
        CHECK(archive::current() == (appImage && *appImage ? Package::LinuxAppImage : Package::LinuxFolder));
#endif
    }

    const archive::TarEntry *entry(const std::vector<archive::TarEntry> &entries, const std::string &name)
    {
        for (const archive::TarEntry &e : entries)
            if (e.name == name)
                return &e;
        return nullptr;
    }

    std::string contents(const std::vector<uint8_t> &tar, const archive::TarEntry *e)
    {
        return e ? std::string(reinterpret_cast<const char *>(tar.data()) + e->data, e->size) : std::string("(missing)");
    }

    void checkGoodArchive(const unsigned char *data, size_t size, const char *label)
    {
        std::fprintf(stderr, "  archive %s\n", label);
        std::vector<uint8_t> tar;
        std::string error;
        CHECK(archive::gunzip(bytes(data, size), tar, 64u << 20, error));
        CHECK(error.empty());
        CHECK_EQ(tar.size() % 512, size_t{0});
        std::vector<archive::TarEntry> entries;
        size_t skipped = 99;
        CHECK(archive::readTar(tar, entries, skipped, error));
        CHECK_EQ(skipped, size_t{1}); // the symbolic link
        CHECK(entry(entries, "libfake.so.0") == nullptr);
        // The top folder ("God Hand Rearmed/") is stripped; nothing keeps it.
        for (const archive::TarEntry &e : entries)
            CHECK(e.name.rfind("God Hand Rearmed", 0) != 0);
        const archive::TarEntry *ghr = entry(entries, "GHR");
        CHECK(ghr && ghr->executable && !ghr->folder);
        CHECK_EQ(contents(tar, ghr), std::string("\x7f" "ELF fake program\n"));
        const archive::TarEntry *ini = entry(entries, "GHR.ini");
        CHECK(ini && !ini->executable);
        CHECK_EQ(contents(tar, ini), std::string("[Video]\nAspectRatio=16:9\n"));
        CHECK_EQ(contents(tar, entry(entries, "resources/readme.txt")), std::string("hello from the resources\n"));
        CHECK_EQ(contents(tar, entry(entries, "memcards/mc0/save.bin")), std::string("must never be written by an update\n"));
        std::string longName = "resources/";
        for (int i = 0; i < 22; ++i)
            longName += "deep/";
        longName += "file.txt";
        CHECK_EQ(contents(tar, entry(entries, longName)), std::string("long name\n"));
        const archive::TarEntry *folder = entry(entries, "resources");
        CHECK(folder && folder->folder && folder->size == 0);
    }

    void testTarGz()
    {
        CHECK_EQ(archive::crc32(reinterpret_cast<const uint8_t *>("123456789"), 9), 0xCBF43926u);
        checkGoodArchive(kGnuTarGz, sizeof(kGnuTarGz), "GNU tar --format=gnu");
        checkGoodArchive(kPaxTarGz, sizeof(kPaxTarGz), "GNU tar --format=pax");

        std::vector<uint8_t> tar;
        std::vector<archive::TarEntry> entries;
        size_t skipped = 0;
        std::string error;

        // Damaged downloads: a flipped byte in the deflate data, a cut-off file, a wrong trailer, not gzip at all.
        std::vector<uint8_t> flipped = bytes(kGnuTarGz, sizeof(kGnuTarGz));
        flipped[flipped.size() / 2] ^= 0x55u;
        CHECK(!archive::gunzip(flipped, tar, 64u << 20, error));
        CHECK(!error.empty());
        std::vector<uint8_t> cut = bytes(kGnuTarGz, sizeof(kGnuTarGz) / 2);
        CHECK(!archive::gunzip(cut, tar, 64u << 20, error));
        std::vector<uint8_t> badSize = bytes(kGnuTarGz, sizeof(kGnuTarGz));
        badSize[badSize.size() - 4] ^= 0x01u;
        CHECK(!archive::gunzip(badSize, tar, 64u << 20, error));
        std::vector<uint8_t> badCrc = bytes(kGnuTarGz, sizeof(kGnuTarGz));
        badCrc[badCrc.size() - 8] ^= 0x01u;
        CHECK(!archive::gunzip(badCrc, tar, 64u << 20, error));
        CHECK(error.find("checksum") != std::string::npos);
        CHECK(!archive::gunzip(std::vector<uint8_t>(64, 0x50u), tar, 64u << 20, error));
        CHECK(!archive::gunzip({}, tar, 64u << 20, error));
        // Bigger than allowed.
        CHECK(!archive::gunzip(bytes(kGnuTarGz, sizeof(kGnuTarGz)), tar, 1024, error));

        // Paths out of the folder: refused whole.
        CHECK(archive::gunzip(bytes(kDotDotTarGz, sizeof(kDotDotTarGz)), tar, 64u << 20, error));
        CHECK(!archive::readTar(tar, entries, skipped, error));
        CHECK(error.find("outside") != std::string::npos);
        CHECK(archive::gunzip(bytes(kAbsoluteTarGz, sizeof(kAbsoluteTarGz)), tar, 64u << 20, error));
        CHECK(!archive::readTar(tar, entries, skipped, error));
        CHECK(error.find("outside") != std::string::npos);

        // A header whose checksum is wrong, and one cut off.
        CHECK(archive::gunzip(bytes(kGnuTarGz, sizeof(kGnuTarGz)), tar, 64u << 20, error));
        std::vector<uint8_t> badHeader = tar;
        badHeader[0] ^= 0x20u;
        CHECK(!archive::readTar(badHeader, entries, skipped, error));
        std::vector<uint8_t> shortTar(tar.begin(), tar.begin() + 512 + 100);
        CHECK(!archive::readTar(shortTar, entries, skipped, error));
        // An empty tar (just the end blocks) is valid and empty.
        CHECK(archive::readTar(std::vector<uint8_t>(1024, 0), entries, skipped, error));
        CHECK(entries.empty());
    }

    // ---- Steam's shortcut list --------------------------------------------------------------------------------------
    const vdf::Node *kid(const vdf::Node &node, const char *key)
    {
        for (const vdf::Node &k : node.kids)
            if (k.key == key)
                return &k;
        return nullptr;
    }

    void testSteamShortcuts()
    {
        std::vector<vdf::Node> root;
        CHECK(vdf::readShortcuts({}, root));
        CHECK(root.size() == 1 && root[0].key == "shortcuts" && root[0].type == 0 && root[0].kids.empty());

        const fs::path exe = "/home/deck/Games/GHR.AppImage";
        uint32_t appid = 0;
        CHECK(!vdf::addEntry(root, exe, exe.parent_path(), "/grid", appid));
        CHECK_EQ(appid, 0xce8c6123u); // crc32("\"<exe>\"God Hand: Rearmed") | 0x80000000, as Steam names it
        const vdf::Node &entry0 = root[0].kids.at(0);
        CHECK_EQ(entry0.key, std::string("0"));
        CHECK_EQ(kid(entry0, "AppName")->text, std::string("God Hand: Rearmed"));
        CHECK_EQ(kid(entry0, "Exe")->text, std::string("\"/home/deck/Games/GHR.AppImage\""));
        CHECK_EQ(kid(entry0, "StartDir")->text, std::string("\"/home/deck/Games\""));
        // The icon path is the grid folder joined with the platform's separator ("\" on Windows, as Steam's own
        // Windows entries have it).
        CHECK_EQ(kid(entry0, "icon")->text, (fs::path("/grid") / (std::to_string(appid) + "_icon.png")).string());
        CHECK_EQ(vdf::numberOf(*kid(entry0, "appid")), appid);

        // The bytes Steam reads: map "shortcuts", map "0", the appid (type 2) first, the strings, three map ends.
        std::vector<uint8_t> written;
        vdf::writeMap(written, root);
        static const char kHead[] = "\x00shortcuts\x00\x00" "0\x00\x02" "appid\x00";
        const std::string head(kHead, sizeof(kHead) - 1);
        CHECK_EQ(head.size(), size_t{21});
        CHECK(written.size() > head.size() + 4);
        CHECK(std::equal(head.begin(), head.end(), written.begin()));
        CHECK(written[21] == 0x23 && written[22] == 0x61 && written[23] == 0x8c && written[24] == 0xce); // little-endian
        CHECK(written[25] == 0x01); // "AppName" string follows
        CHECK(written.size() >= 3 && written[written.size() - 1] == 0x08 && written[written.size() - 2] == 0x08 &&
              written[written.size() - 3] == 0x08);

        // Read back: the same tree, and our entry found by its program.
        std::vector<vdf::Node> again;
        CHECK(vdf::readShortcuts(written, again));
        std::vector<uint8_t> rewritten;
        vdf::writeMap(rewritten, again);
        CHECK(rewritten == written);
        CHECK(vdf::ourEntry(again[0].kids, exe.string()) == &again[0].kids[0]);

        // Adding again updates the one entry (same id); a moved program re-points the entry named God Hand: Rearmed.
        uint32_t second = 0;
        CHECK(vdf::addEntry(again, exe, exe.parent_path(), "/grid", second));
        CHECK_EQ(second, appid);
        CHECK_EQ(again[0].kids.size(), size_t{1});
        const fs::path moved = "/run/media/deck/SDCARD/GHR.AppImage";
        CHECK(vdf::addEntry(again, moved, moved.parent_path(), "/grid", second));
        CHECK_EQ(second, appid); // its art and play time stay with it
        CHECK_EQ(kid(again[0].kids[0], "Exe")->text, std::string("\"/run/media/deck/SDCARD/GHR.AppImage\""));
        CHECK_EQ(kid(again[0].kids[0], "StartDir")->text, std::string("\"/run/media/deck/SDCARD\""));

        // Other entries, and value types we do not write ourselves, round-trip untouched.
        vdf::Node other;
        other.key = "1";
        vdf::Node wide;
        wide.type = 7;
        wide.key = "LastPlayTime64";
        wide.raw = {1, 2, 3, 4, 5, 6, 7, 8};
        other.kids.push_back(wide);
        vdf::Node name;
        name.type = 1;
        name.key = "AppName";
        name.text = "Something else";
        other.kids.push_back(name);
        again[0].kids.push_back(other);
        std::vector<uint8_t> withOther;
        vdf::writeMap(withOther, again);
        std::vector<vdf::Node> third;
        CHECK(vdf::readShortcuts(withOther, third));
        CHECK_EQ(third[0].kids.size(), size_t{2});
        CHECK(third[0].kids[1].kids[0].raw == wide.raw);
        uint32_t ignored = 0;
        CHECK(vdf::addEntry(third, moved, moved.parent_path(), "/grid", ignored));
        CHECK_EQ(third[0].kids.size(), size_t{2}); // no third entry

        // Unknown types and cut-off data are refused (nothing would be written).
        static const char kUnknown[] = "\x00shortcuts\x00\x09odd\x00\x08\x08";
        const std::string unknown(kUnknown, sizeof(kUnknown) - 1);
        CHECK(!vdf::readShortcuts(std::vector<uint8_t>(unknown.begin(), unknown.end()), root));
        std::vector<uint8_t> cut(written.begin(), written.begin() + static_cast<std::ptrdiff_t>(written.size() / 2));
        CHECK(!vdf::readShortcuts(cut, root));

        // loginusers.vdf: MostRecent wins over a later Timestamp; the id is the SteamID64 minus the individual base.
        const std::string users = "\"users\"\n{\n"
                                  "\t\"76561198000000001\"\n\t{\n\t\t\"AccountName\"\t\t\"old\"\n\t\t\"MostRecent\"\t\t\"0\"\n"
                                  "\t\t\"Timestamp\"\t\t\"1800000000\"\n\t}\n"
                                  "\t\"76561198083722517\"\n\t{\n\t\t\"AccountName\"\t\t\"deck\"\n\t\t\"MostRecent\"\t\t\"1\"\n"
                                  "\t\t\"Timestamp\"\t\t\"1700000000\"\n\t}\n}\n";
        CHECK_EQ(vdf::recentAccount(std::vector<uint8_t>(users.begin(), users.end())), std::string("123456789"));
        CHECK(vdf::recentAccount({}).empty());

#if defined(_WIN32)
        CHECK(vdf::samePath("C:/Games/GHR.exe", "c:\\games\\ghr.EXE"));
#else
        CHECK(vdf::samePath("/home/deck/GHR", "/home/deck/GHR"));
        CHECK(!vdf::samePath("/home/deck/GHR", "/home/deck/ghr")); // Linux names are case-sensitive
#endif
    }
}

int main()
{
    const struct
    {
        const char *name;
        void (*run)();
    } tests[] = {
        {"paths", testPaths},
        {"disc search folders", testDiscSearchFolders},
        {"steam deck", testSteamDeck},
        {"update asset selection", testPickAsset},
        {"tar.gz extraction", testTarGz},
        {"steam shortcuts.vdf", testSteamShortcuts},
    };
    for (const auto &test : tests)
    {
        const int before = g_failures;
        std::fprintf(stderr, "[ RUN  ] %s\n", test.name);
        test.run();
        std::fprintf(stderr, "[ %s ] %s\n", g_failures == before ? " OK " : "FAIL", test.name);
    }
    std::fprintf(stderr, "%d checks, %d failed\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
