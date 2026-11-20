// The program folder and the play folder (paths.h).
#include "kamite/paths.h"

#include <algorithm>
#include <cstdlib>
#include <system_error>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

namespace kamite::paths
{
    namespace fs = std::filesystem;

    namespace
    {
        fs::path findProgramDir()
        {
#if defined(_WIN32)
            std::wstring path(32768, L'\0');
            const DWORD n = GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
            if (n > 0 && n < path.size())
            {
                path.resize(n);
                return fs::path(path).parent_path();
            }
#else
            // The kernel's own link to the running binary: right whatever the current folder or argv[0] is, and
            // inside an AppImage it is the mounted copy ($APPDIR/usr/lib/ghr/GHR), next to its resources.
            std::error_code error;
            const fs::path exe = fs::read_symlink("/proc/self/exe", error);
            if (!error && !exe.empty())
                return exe.parent_path();
#endif
            std::error_code ignored;
            return fs::current_path(ignored);
        }

        struct Folders
        {
            fs::path program, home, resources;
            std::string note;
        };

        const Folders &folders()
        {
            static const Folders f = []
            {
                Folders out;
                out.program = findProgramDir();
                std::error_code error;
#if defined(_WIN32)
                // Windows keeps everything next to GHR.exe, as it always has: KAMITE_HOME is the Linux AppImage's
                // (its AppRun sets it), and a narrow getenv would mangle a non-ANSI path there anyway.
                out.home = resolveHome(nullptr, out.program, fs::current_path(error), out.note);
#else
                out.home = resolveHome(std::getenv("KAMITE_HOME"), out.program, fs::current_path(error), out.note);
#endif
                out.resources = out.program / "resources";
                return out;
            }();
            return f;
        }
    }

    fs::path resolveHome(const char *variable, const fs::path &programDir, const fs::path &cwd, std::string &note)
    {
        note.clear();
        if (variable == nullptr || *variable == '\0')
            return programDir;
        fs::path home = fs::path(variable); // the system's narrow encoding (UTF-8 on Linux)
        if (home.is_relative())
            home = cwd / home;
        home = home.lexically_normal();
        // "/x/y/" normalises to "/x/y/" (an empty last part): dropped so home / "GHR.ini" and the log read cleanly.
        if (!home.has_filename() && home.has_parent_path() && home != home.root_path())
            home = home.parent_path();
        std::error_code error;
        fs::create_directories(home, error);
        if (!fs::is_directory(home, error))
        {
            const std::u8string text = home.u8string();
            note = "KAMITE_HOME=" + std::string(text.begin(), text.end()) +
                   " could not be created; the program folder is the play folder instead";
            return programDir;
        }
        return home;
    }

    std::vector<fs::path> discSearchFolders(const char *home, const char *user, const fs::path &mediaRoot)
    {
        std::vector<fs::path> out;
        std::error_code error;
        const auto add = [&](const fs::path &folder)
        {
            if (fs::is_directory(folder, error) && std::find(out.begin(), out.end(), folder) == out.end())
                out.push_back(folder);
        };
        if (home && *home)
        {
            add(fs::path(home) / "Games");
            add(fs::path(home) / "Downloads");
        }
        const fs::path userMedia = user && *user ? mediaRoot / user : fs::path();
        // Sorted, so the same disc is found first every time whatever order the directory lists in.
        const auto children = [&](const fs::path &parent)
        {
            std::vector<fs::path> list;
            for (const auto &entry : fs::directory_iterator(parent, error))
                if (entry.is_directory(error) && entry.path() != userMedia)
                    list.push_back(entry.path());
            std::sort(list.begin(), list.end());
            return list;
        };
        if (!userMedia.empty())
            for (const fs::path &card : children(userMedia))
                add(card);
        for (const fs::path &card : children(mediaRoot))
            add(card);
        return out;
    }

    const fs::path &programDir() { return folders().program; }
    const fs::path &home() { return folders().home; }
    const fs::path &resources() { return folders().resources; }
    const std::string &homeNote() { return folders().note; }
}
