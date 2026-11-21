// The launcher's in-place updates (see launcher_update.h).
#include "kamite/launcher_update.h"
#include "kamite/git_commit.h"
#include "kamite/ini_file.h"
#include "kamite/json_reader.h"
#include "kamite/launcher_platform.h"
#include "kamite/log.h"
#include "kamite/paths.h"
#include "kamite/update_archive.h"
#include "kamite/version.h"

#include "raylib.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <chrono>
#include <cstring>
#include <fstream>
#include <functional>
#include <sstream>
#include <string_view>
#include <vector>

// raylib's own inflate (rcore.c builds sinfl.h with SUPPORT_COMPRESSION_API): the zip's deflate streams.
extern "C" int sinflate(void *out, int cap, const void *in, int size);

#if !defined(_WIN32)
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace kamite::launcher::update
{
    namespace
    {
        namespace fs = std::filesystem;

        constexpr const char *kReleasesApi = "https://api.github.com/repos/God-Hand-Rearmed/God-Hand-Rearmed/releases?per_page=30";
        constexpr const char *kCleanupList = "update-cleanup.txt"; // the files renamed aside, removed at the next start
        constexpr const char *kNewSuffix = ".update-new";
        constexpr const char *kOldSuffix = ".update-old";
        // Folders an update never writes into (the zip has none of them; checked anyway).
        constexpr const char *kKeptFolders[] = {"memcards", "mc0", "mc1", "states", "cases", "logs", "cache", "game_data", "mods",
                                                "shadercache_pcsx2", "texture replacements", "textures", "game", "rom", "update"};
        // Settings files: written only when missing, else only the keys they lack are added.
        constexpr const char *kSettingsFiles[] = {"ghr.ini", "controls.ini"};

        fs::path fromUtf8(const std::string &text) { return fs::path(std::u8string(text.begin(), text.end())); }
        std::string utf8(const fs::path &path)
        {
            const std::u8string text = path.u8string();
            return std::string(text.begin(), text.end());
        }

        std::string lower(std::string s)
        {
            for (char &c : s)
                c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            return s;
        }

        using json::Json;
        using json::JsonReader;

        std::string text(const Json *value) { return value && value->kind == Json::Kind::String ? value->text : std::string(); }
        bool flag(const Json *value) { return value && value->kind == Json::Kind::Bool && value->flag; }

        // ---- versions ---------------------------------------------------------------------------------------------
        // "v0.6.9" -> {0, 6, 9}; anything after the numbers ("-beta") is ignored.
        std::vector<int> parseVersion(const std::string &tag)
        {
            std::vector<int> parts;
            size_t at = (!tag.empty() && (tag[0] == 'v' || tag[0] == 'V')) ? 1 : 0;
            while (at < tag.size() && std::isdigit(static_cast<unsigned char>(tag[at])))
            {
                int n = 0;
                while (at < tag.size() && std::isdigit(static_cast<unsigned char>(tag[at])))
                    n = n * 10 + (tag[at++] - '0');
                parts.push_back(n);
                if (at < tag.size() && tag[at] == '.')
                    ++at;
                else
                    break;
            }
            while (parts.size() > 1 && parts.back() == 0)
                parts.pop_back();
            return parts;
        }

        bool newer(const std::vector<int> &a, const std::vector<int> &b)
        {
            return std::lexicographical_compare(b.begin(), b.end(), a.begin(), a.end());
        }

        // GitHub's "2026-11-19T10:12:00Z" as the newspaper's date: "NOV. 19, 2026".
        std::string paperDate(const std::string &iso)
        {
            static const char *const kMonths[] = {"JAN.", "FEB.", "MARCH", "APRIL", "MAY", "JUNE", "JULY", "AUG.", "SEPT.", "OCT.", "NOV.", "DEC."};
            int year = 0, month = 0, day = 0;
            if (std::sscanf(iso.c_str(), "%d-%d-%d", &year, &month, &day) != 3 || month < 1 || month > 12 || day < 1)
                return {};
            return std::string(kMonths[month - 1]) + " " + std::to_string(day) + ", " + std::to_string(year);
        }

        void check(Checker &checker, bool test)
        {
#if KAMITE_RELEASE
            constexpr bool kRelease = true;
#else
            constexpr bool kRelease = false;
#endif
            const std::vector<int> ours = parseVersion(KAMITE_VERSION);
            const std::vector<int> current = test ? std::vector<int>{0} : ours;
            std::vector<uint8_t> bytes;
            Json list;
            if (!platform::httpGet(kReleasesApi, bytes, 8u << 20, "application/vnd.github+json") ||
                !JsonReader(std::string(bytes.begin(), bytes.end())).read(list) || list.kind != Json::Kind::Array)
            {
                KM_INFO(Boot, "update: the release list could not be read (offline?)");
                checker.state.store(static_cast<int>(CheckState::Failed));
                return;
            }
            struct Found
            {
                std::vector<int> version;
                std::string tag, body, url, date;
                uint64_t size = 0;
            };
            std::vector<Found> found;
            Found newest; // the newest release of all (a dev build's paper when it has none of its own)
            for (const Json &item : list.items)
            {
                if (flag(item["draft"]))
                    continue;
                Found f;
                f.tag = text(item["tag_name"]);
                f.version = parseVersion(f.tag);
                if (f.version.empty())
                    continue;
                f.body = text(item["body"]);
                f.date = paperDate(text(item["published_at"]));
                if (f.version == ours && !checker.hasCurrent)
                {
                    checker.current = Edition{f.tag, f.date, f.body};
                    checker.hasCurrent = true;
                }
                if (newest.version.empty() || newer(f.version, newest.version))
                    newest = f;
                if (!newer(f.version, current))
                    continue;
                // The download this build installs (archive::pickAsset: Windows the .zip; Linux the tar.gz, or the
                // AppImage or the .flatpak when it runs as one).
                if (const Json *assets = item["assets"]; assets && assets->kind == Json::Kind::Array)
                {
                    std::vector<std::string> names;
                    for (const Json &asset : assets->items)
                        names.push_back(text(asset["name"]));
                    if (const int pick = archive::pickAsset(names, archive::current()); pick >= 0)
                    {
                        const Json &asset = assets->items[static_cast<size_t>(pick)];
                        f.url = text(asset["browser_download_url"]);
                        const Json *size = asset["size"];
                        f.size = size && size->kind == Json::Kind::Number ? static_cast<uint64_t>(size->number) : 0u;
                    }
                }
                found.push_back(std::move(f));
            }
            if (!checker.hasCurrent && !kRelease && !newest.version.empty())
            {
                checker.current = Edition{newest.tag, newest.date, newest.body};
                checker.hasCurrent = true;
            }
            std::sort(found.begin(), found.end(), [](const Found &a, const Found &b) { return newer(a.version, b.version); });
            // The newest release with a zip is the one installed; the notes cover every release newer than ours. A dev
            // build only reads (testers build main), unless it is UpdateTest's.
            const auto target = std::find_if(found.begin(), found.end(), [](const Found &f) { return !f.url.empty(); });
            if (target == found.end() || (!kRelease && !test))
            {
                KM_INFO(Boot, "update: up to date (v" << KAMITE_VERSION << ")");
                checker.state.store(static_cast<int>(CheckState::UpToDate));
                return;
            }
            // Started by RecompLauncher (it sets RECOMPLAUNCHER=1 for every game it starts): it keeps this folder up to
            // date itself, so ours stands down (K, Nov 20: one updater is enough). The newspaper still has its notes.
            if (const char *launcher = std::getenv("RECOMPLAUNCHER"); launcher && *launcher && *launcher != '0')
            {
                KM_INFO(Boot, "update: " << target->tag << " is out; RecompLauncher installs it");
                checker.state.store(static_cast<int>(CheckState::UpToDate));
                return;
            }
            Release &release = checker.release;
            release.tag = target->tag;
            release.zipUrl = target->url;
            release.zipSize = target->size;
            for (auto it = target; it != found.end(); ++it)
            {
                std::string body = it->body;
                body.erase(std::remove(body.begin(), body.end(), '\r'), body.end());
                release.notes += "# " + it->tag + "\n" + body + "\n\n";
                release.editions.push_back(Edition{it->tag, it->date, body});
            }
            KM_INFO(Boot, "update: " << release.tag << " is out (" << release.zipSize / (1024 * 1024) << " MB)");
            checker.state.store(static_cast<int>(CheckState::Available));
        }

        // ---- the tester channel -----------------------------------------------------------------------------------
        // [Launcher] Channel=tester (the tester zip's GHR.ini): the rolling pre-release tagged tester-build, whose zip
        // tools/win/Publish-Tester.ps1 replaces after each of K's builds of main, named
        // GHR-tester-<commit>-<commit time, Unix seconds>.zip. A build of a newer commit than ours is offered. Players'
        // checks skip that release (its tag is not a version).
        constexpr const char *kTesterApi = "https://api.github.com/repos/God-Hand-Rearmed/God-Hand-Rearmed/releases/tags/tester-build";
        std::atomic<bool> g_testerChannel{false};

        // "ghr-tester-1a2b3c4-1795100000.zip" (lower case) -> commit, time.
        bool parseTesterAsset(const std::string &name, std::string &commit, uint64_t &time)
        {
            constexpr std::string_view kPrefix = "ghr-tester-", kSuffix = ".zip";
            if (name.size() <= kPrefix.size() + kSuffix.size() || name.compare(0, kPrefix.size(), kPrefix) != 0 ||
                name.compare(name.size() - kSuffix.size(), kSuffix.size(), kSuffix) != 0)
                return false;
            const std::string middle = name.substr(kPrefix.size(), name.size() - kPrefix.size() - kSuffix.size());
            const size_t dash = middle.find('-');
            if (dash == std::string::npos || dash == 0 || dash + 1 == middle.size())
                return false;
            commit = middle.substr(0, dash);
            time = 0;
            for (size_t i = dash + 1; i < middle.size(); ++i)
            {
                if (!std::isdigit(static_cast<unsigned char>(middle[i])))
                    return false;
                time = time * 10u + static_cast<uint64_t>(middle[i] - '0');
            }
            return true;
        }

        // Short commits of different lengths name the same commit when one starts the other.
        bool sameCommit(const std::string &a, const std::string &b)
        {
            const size_t n = std::min(a.size(), b.size());
            return n >= 7 && lower(a).compare(0, n, lower(b), 0, n) == 0;
        }

        void checkTester(Checker &checker)
        {
#if !defined(_WIN32)
            // The tester builds are Windows zips (tools/win/Publish-Tester.ps1): nothing a Linux build can install.
            KM_INFO(Boot, "update: the tester channel has Windows builds only; not checked on Linux");
            checker.state.store(static_cast<int>(CheckState::UpToDate));
            return;
#endif
            std::vector<uint8_t> bytes;
            Json item;
            if (!platform::httpGet(kTesterApi, bytes, 8u << 20, "application/vnd.github+json") ||
                !JsonReader(std::string(bytes.begin(), bytes.end())).read(item) || item.kind != Json::Kind::Object)
            {
                KM_INFO(Boot, "update: the tester build could not be read (offline, or none published yet)");
                checker.state.store(static_cast<int>(CheckState::Failed));
                return;
            }
            std::string commit, url, date;
            uint64_t time = 0, size = 0;
            if (const Json *assets = item["assets"]; assets && assets->kind == Json::Kind::Array)
                for (const Json &asset : assets->items)
                {
                    std::string c;
                    uint64_t t = 0;
                    if (!parseTesterAsset(lower(text(asset["name"])), c, t) || (!url.empty() && t <= time))
                        continue;
                    commit = c;
                    time = t;
                    url = text(asset["browser_download_url"]);
                    date = paperDate(text(asset["updated_at"]));
                    const Json *bytesJson = asset["size"];
                    size = bytesJson && bytesJson->kind == Json::Kind::Number ? static_cast<uint64_t>(bytesJson->number) : 0u;
                }
            if (url.empty())
            {
                KM_INFO(Boot, "update: no tester build published yet");
                checker.state.store(static_cast<int>(CheckState::UpToDate));
                return;
            }
            std::string body = text(item["body"]);
            body.erase(std::remove(body.begin(), body.end(), '\r'), body.end());
            const Edition edition{"Build " + commit, date, body};
            const std::string ours = KAMITE_GIT_COMMIT;
            const uint64_t ourTime = KAMITE_GIT_TIME;
            if (sameCommit(commit, ours) || (ourTime != 0 && time <= ourTime))
            {
                checker.current = edition;
                checker.hasCurrent = true;
                KM_INFO(Boot, "update: tester build " << commit << " is not newer than ours (" << ours << ")");
                checker.state.store(static_cast<int>(CheckState::UpToDate));
                return;
            }
            Release &release = checker.release;
            release.tag = edition.tag;
            release.zipUrl = url;
            release.zipSize = size;
            release.notes = "# " + edition.tag + "\n" + body + "\n\n";
            release.editions.push_back(edition);
            KM_INFO(Boot, "update: tester build " << commit << " is out (" << size / (1024 * 1024) << " MB)");
            checker.state.store(static_cast<int>(CheckState::Available));
        }

        // ---- the zip --------------------------------------------------------------------------------------------
        uint16_t u16(const uint8_t *p) { return static_cast<uint16_t>(p[0] | (p[1] << 8)); }
        uint32_t u32(const uint8_t *p) { return static_cast<uint32_t>(p[0] | (p[1] << 8) | (p[2] << 16)) | (static_cast<uint32_t>(p[3]) << 24); }

        struct Entry
        {
            std::string name; // relative, '/' separated, the zip's top folder (if all share one) removed
            bool folder = false;
            uint16_t method = 0;
            uint32_t crc = 0;
            uint32_t packed = 0, size = 0;
            size_t data = 0; // offset of the packed bytes
        };

        bool readZip(const std::vector<uint8_t> &zip, std::vector<Entry> &entries, std::string &error)
        {
            error = "the download is not a valid zip";
            if (zip.size() < 22)
                return false;
            size_t end = zip.size() - 22;
            const size_t stop = zip.size() > 22 + 0xFFFF ? zip.size() - 22 - 0xFFFF : 0;
            while (u32(&zip[end]) != 0x06054B50u)
            {
                if (end == stop)
                    return false;
                --end;
            }
            const uint16_t count = u16(&zip[end + 10]);
            size_t at = u32(&zip[end + 16]);
            for (uint16_t i = 0; i < count; ++i)
            {
                if (at + 46 > zip.size() || u32(&zip[at]) != 0x02014B50u)
                    return false;
                const uint8_t *h = &zip[at];
                Entry e;
                const uint16_t flags = u16(h + 8);
                e.method = u16(h + 10);
                e.crc = u32(h + 16);
                e.packed = u32(h + 20);
                e.size = u32(h + 24);
                const uint16_t nameLength = u16(h + 28), extra = u16(h + 30), comment = u16(h + 32);
                const size_t local = u32(h + 42);
                if (at + 46 + nameLength > zip.size() || (flags & 1u) || (e.method != 0 && e.method != 8))
                    return false;
                e.name.assign(reinterpret_cast<const char *>(h + 46), nameLength);
                std::replace(e.name.begin(), e.name.end(), '\\', '/'); // Windows PowerShell's Compress-Archive
                e.folder = !e.name.empty() && e.name.back() == '/';
                if (local + 30 > zip.size() || u32(&zip[local]) != 0x04034B50u)
                    return false;
                e.data = local + 30 + u16(&zip[local + 26]) + u16(&zip[local + 28]);
                if (e.data + e.packed > zip.size())
                    return false;
                entries.push_back(std::move(e));
                at += 46 + nameLength + extra + comment;
            }
            // One top folder for everything ("God Hand Rearmed/..."): stripped.
            std::string top;
            bool shared = !entries.empty();
            for (const Entry &e : entries)
            {
                const size_t slash = e.name.find('/');
                const std::string first = slash == std::string::npos ? std::string() : e.name.substr(0, slash + 1);
                if (first.empty() || (!top.empty() && first != top))
                {
                    shared = false;
                    break;
                }
                top = first;
            }
            if (shared)
                for (Entry &e : entries)
                    e.name.erase(0, top.size());
            entries.erase(std::remove_if(entries.begin(), entries.end(), [](const Entry &e) { return e.name.empty(); }), entries.end());
            for (const Entry &e : entries)
            {
                // Nothing may leave the game's folder.
                if (e.name[0] == '/' || e.name.find(':') != std::string::npos || ("/" + e.name + "/").find("/../") != std::string::npos)
                {
                    error = "the zip names a file outside the game's folder (" + e.name + ")";
                    return false;
                }
            }
            error.clear();
            return true;
        }

        bool unpack(const std::vector<uint8_t> &zip, const Entry &e, std::vector<uint8_t> &out)
        {
            out.assign(static_cast<size_t>(e.size) + 64u, 0u);
            int got = 0;
            if (e.method == 0)
            {
                if (e.packed != e.size)
                    return false;
                std::memcpy(out.data(), &zip[e.data], e.size);
                got = static_cast<int>(e.size);
            }
            else
                got = sinflate(out.data(), static_cast<int>(out.size()), &zip[e.data], static_cast<int>(e.packed));
            if (got != static_cast<int>(e.size))
                return false;
            out.resize(e.size);
            return ComputeCRC32(out.data(), static_cast<int>(out.size())) == e.crc;
        }

        bool writeFile(const fs::path &path, const std::vector<uint8_t> &bytes)
        {
            std::ofstream file(path, std::ios::binary | std::ios::trunc);
            file.write(reinterpret_cast<const char *>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
            return static_cast<bool>(file);
        }

        // Adds the keys the player's settings file lacks, from the release's copy (values and order of theirs kept).
        void mergeSettings(const fs::path &mine, const std::vector<uint8_t> &theirs)
        {
            IniFile ini;
            if (!ini.load(mine))
                return;
            const int added = ini.addMissing(std::string(theirs.begin(), theirs.end()));
            if (added > 0)
            {
                ini.save();
                KM_INFO(Boot, "update: " << added << " new setting(s) added to " << mine.filename().string());
            }
        }

        // One file or folder of a release archive (the zip or the tar.gz), its bytes read when they are needed.
        struct Item
        {
            std::string name; // relative, '/' separated
            bool folder = false;
            bool executable = false; // the tar's x bit (the zip has none: Windows needs none)
            std::function<bool(std::vector<uint8_t> &)> read;
        };

        // The AppImage (Linux, $APPIMAGE set): one file, downloaded next to the running one and renamed over it. The
        // running program keeps its old copy open (its squashfs stays mounted) until it quits; the relaunch starts
        // the new one. Settings and saves live in the play folder, outside the AppImage, so nothing else changes.
        void installAppImage(Installer &installer, const Release &release, const std::function<void(const std::string &)> &fail)
        {
#if defined(_WIN32)
            (void)installer;
            (void)release;
            fail("This build cannot install an AppImage.");
#else
            const char *appImage = std::getenv("APPIMAGE");
            const fs::path target = appImage ? fs::path(appImage) : fs::path();
            if (target.empty())
                return fail("The AppImage's own file is not known (APPIMAGE is not set).");
            fs::path fresh = target;
            fresh += kNewSuffix;
            std::vector<uint8_t> bytes;
            if (!platform::httpGet(release.zipUrl, bytes, 2048u << 20, nullptr, &installer.received))
                return fail("The download did not finish. Check your connection and try again.");
            if (release.zipSize != 0 && bytes.size() != release.zipSize)
                return fail("The download came out the wrong size. Try again.");
            installer.stage.store(static_cast<int>(Stage::Installing));
            // An AppImage is an ELF program with the squashfs after it: anything else is not one.
            static constexpr uint8_t kElf[4] = {0x7F, 'E', 'L', 'F'};
            if (bytes.size() < 4096 || std::memcmp(bytes.data(), kElf, 4) != 0)
                return fail("The download is not an AppImage. Try again.");
            std::error_code error;
            if (!writeFile(fresh, bytes))
            {
                fs::remove(fresh, error);
                return fail("Couldn't write the update next to " + utf8(target.filename()) + " (is the disk full, or the folder read-only?).");
            }
            if (::chmod(fresh.c_str(), 0755) != 0)
            {
                fs::remove(fresh, error);
                return fail("Couldn't make the update runnable.");
            }
            // rename() replaces the old file in one step: there is never a moment without an AppImage there.
            fs::rename(fresh, target, error);
            if (error)
            {
                fs::remove(fresh, error);
                return fail("Couldn't replace " + utf8(target.filename()) + ". Is its folder read-only?");
            }
            KM_INFO(Boot, "update: " << release.tag << " installed (AppImage " << utf8(target) << ")");
            installer.stage.store(static_cast<int>(Stage::Done));
#endif
        }

        void install(Installer &installer, const Release &release, const fs::path &exeDir)
        {
            const auto fail = [&](const std::string &why)
            {
                installer.error = why;
                KM_INFO(Boot, "update: failed: " << why);
                installer.stage.store(static_cast<int>(Stage::Failed));
            };
            const archive::Package package = archive::current();
            // flatpak owns /app (read-only to the app, its files checked against the installed commit): the offer
            // stays so the player learns of the release, and Update says where updates come from.
            if (package == archive::Package::LinuxFlatpak)
                return fail("This is the Flatpak: update it through Flatpak (install the release's new .flatpak "
                            "over it: flatpak install --user GHR-...-Linux-x64.flatpak).");
            if (package == archive::Package::LinuxAppImage)
                return installAppImage(installer, release, fail);
            std::vector<uint8_t> zip;
            if (!platform::httpGet(release.zipUrl, zip, 2048u << 20, nullptr, &installer.received))
                return fail("The download did not finish. Check your connection and try again.");
            if (release.zipSize != 0 && zip.size() != release.zipSize)
                return fail("The download came out the wrong size. Try again.");
            installer.stage.store(static_cast<int>(Stage::Installing));
            std::vector<Item> items;
            std::vector<Entry> entries;
            std::vector<uint8_t> tar;
            std::vector<archive::TarEntry> tarEntries;
            std::string why;
            if (package == archive::Package::WindowsZip)
            {
                if (!readZip(zip, entries, why))
                    return fail("The update could not be read: " + why + ".");
                for (const Entry &e : entries)
                    items.push_back(Item{e.name, e.folder, false, [&zip, &e](std::vector<uint8_t> &out) { return unpack(zip, e, out); }});
            }
            else
            {
                size_t skipped = 0;
                if (!archive::gunzip(zip, tar, 2048u << 20, why) || !archive::readTar(tar, tarEntries, skipped, why))
                    return fail("The update could not be read: " + why + ".");
                zip.clear();
                zip.shrink_to_fit();
                if (skipped != 0)
                    KM_INFO(Boot, "update: " << skipped << " link(s) or special file(s) in the tar.gz left out");
                for (const archive::TarEntry &e : tarEntries)
                    items.push_back(Item{e.name, e.folder, e.executable, [&tar, &e](std::vector<uint8_t> &out)
                                         {
                                             out.assign(tar.begin() + static_cast<std::ptrdiff_t>(e.data),
                                                        tar.begin() + static_cast<std::ptrdiff_t>(e.data + e.size));
                                             return true;
                                         }});
            }

            // Each new file is written next to its old one first; nothing is replaced until all of them are there.
            struct Swap
            {
                fs::path target, fresh, old;
                bool done = false;
            };
            std::vector<Swap> swaps;
            std::vector<std::pair<fs::path, std::vector<uint8_t>>> merges;
            std::error_code error;
            const auto removeFresh = [&]
            {
                for (const Swap &s : swaps)
                    fs::remove(s.fresh, error);
            };
            std::vector<uint8_t> bytes;
            for (const Item &e : items)
            {
                const std::string name = lower(e.name);
                const std::string head = name.substr(0, name.find('/'));
                if (std::find(std::begin(kKeptFolders), std::end(kKeptFolders), head) != std::end(kKeptFolders) && name.find('/') != std::string::npos)
                    continue;
                const bool settingsFile = std::find(std::begin(kSettingsFiles), std::end(kSettingsFiles), name) != std::end(kSettingsFiles);
                // The settings files belong to the play folder (paths.h), the rest to the program folder; the same
                // folder unless KAMITE_HOME names another.
                const fs::path target = (settingsFile ? paths::home() : exeDir) / fromUtf8(e.name);
                if (e.folder)
                {
                    fs::create_directories(target, error);
                    continue;
                }
                if (!e.read(bytes))
                {
                    removeFresh();
                    return fail("The update is damaged (" + e.name + "). Try again.");
                }
                if (settingsFile && fs::exists(target, error))
                {
                    merges.emplace_back(target, std::move(bytes));
                    bytes = {};
                    continue;
                }
                fs::create_directories(target.parent_path(), error);
                Swap s;
                s.target = target;
                s.fresh = target;
                s.fresh += kNewSuffix;
                bool written = writeFile(s.fresh, bytes);
#if !defined(_WIN32)
                // The tar's x bit (GHR itself): kept, else the new GHR would not start.
                if (written)
                    written = ::chmod(s.fresh.c_str(), e.executable ? 0755 : 0644) == 0;
#endif
                if (!written)
                {
                    removeFresh();
                    fs::remove(s.fresh, error);
                    return fail("Couldn't write " + e.name + " (is the disk full, or the folder read-only?).");
                }
                swaps.push_back(std::move(s));
            }
            items.clear();
            zip.clear();
            zip.shrink_to_fit();
            tar.clear();
            tar.shrink_to_fit();

            // The swap: the old file renamed aside (Windows lets a running program and its DLLs be renamed), the new
            // one into its place; undone in full if any step fails.
            std::vector<fs::path> renamedAside;
            for (Swap &s : swaps)
            {
                if (fs::exists(s.target, error))
                {
                    s.old = s.target;
                    s.old += kOldSuffix;
                    for (int n = 2; fs::exists(s.old, error) && !fs::remove(s.old, error); ++n)
                    {
                        s.old = s.target;
                        s.old += kOldSuffix + std::to_string(n);
                    }
                    fs::rename(s.target, s.old, error);
                    if (error)
                    {
                        s.old.clear();
                        why = s.target.filename().string();
                        break;
                    }
                }
                fs::rename(s.fresh, s.target, error);
                if (error)
                {
                    if (!s.old.empty())
                        fs::rename(s.old, s.target, error);
                    why = s.target.filename().string();
                    break;
                }
                s.done = true;
                if (!s.old.empty())
                    renamedAside.push_back(s.old);
            }
            if (!why.empty())
            {
                for (auto it = swaps.rbegin(); it != swaps.rend(); ++it)
                    if (it->done)
                    {
                        std::error_code ignored;
                        fs::remove(it->target, ignored);
                        if (!it->old.empty())
                            fs::rename(it->old, it->target, ignored);
                    }
                removeFresh();
                return fail("Couldn't replace " + why + ". Close anything that uses the game's files and try again.");
            }
            for (const auto &[path, theirs] : merges)
                mergeSettings(path, theirs);
            std::ofstream list(exeDir / kCleanupList, std::ios::app);
            for (const fs::path &old : renamedAside)
                list << utf8(old) << '\n';
            KM_INFO(Boot, "update: " << release.tag << " installed (" << swaps.size() << " files, " << archive::name(package) << ")");
            installer.stage.store(static_cast<int>(Stage::Done));
        }
    }

    std::string versionLabel()
    {
#if KAMITE_RELEASE
        return std::string("v") + KAMITE_VERSION + " " + KAMITE_VERSION_LABEL;
#else
        return std::string("v") + KAMITE_VERSION + " " + KAMITE_VERSION_LABEL + " (dev " + KAMITE_GIT_COMMIT + ")";
#endif
    }

    std::string editionName()
    {
#if KAMITE_RELEASE
        std::string name = KAMITE_VERSION_LABEL;
        for (char &c : name)
            c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
        return name + " EDITION";
#else
        return g_testerChannel.load() ? "TESTER EDITION" : "DEV EDITION";
#endif
    }

    std::string devCommit()
    {
#if KAMITE_RELEASE
        return {};
#else
        return KAMITE_GIT_COMMIT;
#endif
    }

    void startCheck(Checker &checker, bool test, bool tester)
    {
        g_testerChannel.store(tester);
        checker.state.store(static_cast<int>(CheckState::Checking));
        checker.worker = std::thread([&checker, test, tester] { tester ? checkTester(checker) : check(checker, test); });
    }

    void startInstall(Installer &installer, const Release &release, const std::filesystem::path &exeDir)
    {
        if (installer.worker.joinable())
            installer.worker.join();
        installer.stage.store(static_cast<int>(Stage::Downloading));
        installer.received.store(0);
        installer.total = release.zipSize;
        installer.error.clear();
        installer.worker = std::thread([&installer, release, exeDir] { install(installer, release, exeDir); });
    }

    void cleanup(const std::filesystem::path &exeDir)
    {
        const fs::path listPath = exeDir / kCleanupList;
        std::error_code error;
        if (!fs::exists(listPath, error))
            return;
        std::thread([listPath]
                    {
                        std::vector<fs::path> files;
                        {
                            std::ifstream list(listPath);
                            std::string line;
                            while (std::getline(list, line))
                                if (!line.empty())
                                    files.push_back(fromUtf8(line));
                        }
                        // The old process may still be closing: a few tries over two seconds; what is left waits for
                        // the next start.
                        std::error_code ignored;
                        for (int attempt = 0; attempt < 20 && !files.empty(); ++attempt)
                        {
                            files.erase(std::remove_if(files.begin(), files.end(),
                                                       [&](const fs::path &f)
                                                       {
                                                           std::error_code e;
                                                           return fs::remove(f, e) || !fs::exists(f, e);
                                                       }),
                                        files.end());
                            if (!files.empty())
                                std::this_thread::sleep_for(std::chrono::milliseconds(100));
                        }
                        if (files.empty())
                            fs::remove(listPath, ignored);
                        else
                        {
                            std::ofstream list(listPath, std::ios::trunc);
                            for (const fs::path &f : files)
                                list << utf8(f) << '\n';
                        }
                    })
            .detach();
    }
}
