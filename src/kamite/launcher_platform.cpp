#include "kamite/launcher_platform.h"

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <shellapi.h>
#include <objbase.h>
#include <shobjidl.h>
#include <winhttp.h>
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "winhttp.lib")
#else
#include "raylib.h"

#include <algorithm>
#include <cerrno>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <type_traits>

#include <dlfcn.h>
#include <fcntl.h>
#include <sys/wait.h>
#include <unistd.h>

extern char **environ;
#endif

namespace kamite::launcher::platform
{
#if !defined(_WIN32)
    // Linux: helper programs (zenity, kdialog, xdg-open) through fork + exec with an argument list (no shell, so a
    // path with quotes or spaces is passed as it is), downloads through the system's libcurl opened at run time.
    namespace
    {
        // The program `name` in PATH ("" when not there). Looked up before fork(): the child may only make
        // async-signal-safe calls, and execvp's own search is not one.
        std::string findProgram(const std::string &name)
        {
            const char *pathVariable = std::getenv("PATH");
            const std::string path = pathVariable && *pathVariable ? pathVariable : "/usr/local/bin:/usr/bin:/bin";
            size_t at = 0;
            while (at <= path.size())
            {
                const size_t colon = std::min(path.find(':', at), path.size());
                const std::string dir = path.substr(at, colon - at);
                at = colon + 1;
                if (dir.empty())
                    continue;
                const std::string candidate = dir + "/" + name;
                if (::access(candidate.c_str(), X_OK) == 0)
                    return candidate;
            }
            return {};
        }

        // The environment for a helper program: GHR's own, minus LD_LIBRARY_PATH / LD_PRELOAD entries inside the
        // AppImage ($APPDIR), whose libraries are GHR's and could break a system program that loaded them.
        std::vector<std::string> helperEnvironment()
        {
            std::vector<std::string> out;
            const char *appDir = std::getenv("APPDIR");
            const std::string inside = appDir && *appDir ? std::string(appDir) : std::string();
            for (char **e = environ; e && *e; ++e)
            {
                std::string entry = *e;
                if (!inside.empty() && (entry.rfind("LD_LIBRARY_PATH=", 0) == 0 || entry.rfind("LD_PRELOAD=", 0) == 0))
                {
                    const size_t equals = entry.find('=');
                    const char separator = entry[0] == 'L' && entry[3] == 'P' ? ' ' : ':'; // LD_PRELOAD: spaces too
                    std::string kept, list = entry.substr(equals + 1);
                    size_t at = 0;
                    while (at <= list.size())
                    {
                        size_t next = list.find_first_of(separator == ' ' ? ": " : ":", at);
                        if (next == std::string::npos)
                            next = list.size();
                        const std::string item = list.substr(at, next - at);
                        at = next + 1;
                        if (!item.empty() && item.rfind(inside, 0) != 0)
                            kept += (kept.empty() ? "" : ":") + item;
                    }
                    if (kept.empty())
                        continue;
                    entry = entry.substr(0, equals + 1) + kept;
                }
                out.push_back(std::move(entry));
            }
            return out;
        }

        std::vector<char *> pointers(std::vector<std::string> &strings)
        {
            std::vector<char *> out;
            for (std::string &s : strings)
                out.push_back(s.data());
            out.push_back(nullptr);
            return out;
        }

        // Starts `program` (a full path) with `args` (argv[1...]) in `cwd` ("": GHR's own), detached: a grandchild in
        // a session of its own, so it outlives GHR and leaves no zombie. True once exec has succeeded (a
        // close-on-exec pipe reports a failed exec).
        bool spawnDetached(const std::string &program, const std::vector<std::string> &args, const std::string &cwd,
                           bool helper)
        {
            std::vector<std::string> argv{program};
            argv.insert(argv.end(), args.begin(), args.end());
            std::vector<std::string> env = helper ? helperEnvironment() : std::vector<std::string>();
            std::vector<char *> argp = pointers(argv);
            std::vector<char *> envp = pointers(env);
            int report[2];
            if (::pipe2(report, O_CLOEXEC) != 0)
                return false;
            const long maxFd = std::min<long>(::sysconf(_SC_OPEN_MAX) > 0 ? ::sysconf(_SC_OPEN_MAX) : 1024, 65536);
            const pid_t child = ::fork();
            if (child < 0)
            {
                ::close(report[0]);
                ::close(report[1]);
                return false;
            }
            if (child == 0)
            {
                // Only async-signal-safe calls from here to exec (the parent has other threads).
                ::setsid();
                const pid_t grandchild = ::fork();
                if (grandchild != 0)
                    ::_exit(grandchild < 0 ? 127 : 0);
                sigset_t none;
                ::sigemptyset(&none);
                ::sigprocmask(SIG_SETMASK, &none, nullptr);
                for (int fd = 3; fd < maxFd; ++fd)
                    if (fd != report[1])
                        ::close(fd);
                if (!cwd.empty() && ::chdir(cwd.c_str()) != 0)
                {
                    const int e = errno;
                    (void)!::write(report[1], &e, sizeof(e));
                    ::_exit(127);
                }
                if (helper)
                    ::execve(argp[0], argp.data(), envp.data());
                else
                    ::execv(argp[0], argp.data());
                const int e = errno;
                (void)!::write(report[1], &e, sizeof(e));
                ::_exit(127);
            }
            ::close(report[1]);
            int status = 0;
            while (::waitpid(child, &status, 0) < 0 && errno == EINTR)
            {
            }
            int execError = 0;
            ssize_t got = 0;
            do
                got = ::read(report[0], &execError, sizeof(execError));
            while (got < 0 && errno == EINTR);
            ::close(report[0]);
            return WIFEXITED(status) && WEXITSTATUS(status) == 0 && got == 0;
        }

        // Runs a helper and returns what it printed on stdout, waiting for it (the file chooser). `exitCode` is its
        // exit status (-1 when it could not run).
        std::string runAndRead(const std::string &program, const std::vector<std::string> &args, int &exitCode)
        {
            exitCode = -1;
            std::vector<std::string> argv{program};
            argv.insert(argv.end(), args.begin(), args.end());
            std::vector<std::string> env = helperEnvironment();
            std::vector<char *> argp = pointers(argv);
            std::vector<char *> envp = pointers(env);
            int output[2];
            if (::pipe2(output, O_CLOEXEC) != 0)
                return {};
            const long maxFd = std::min<long>(::sysconf(_SC_OPEN_MAX) > 0 ? ::sysconf(_SC_OPEN_MAX) : 1024, 65536);
            const pid_t child = ::fork();
            if (child < 0)
            {
                ::close(output[0]);
                ::close(output[1]);
                return {};
            }
            if (child == 0)
            {
                sigset_t none;
                ::sigemptyset(&none);
                ::sigprocmask(SIG_SETMASK, &none, nullptr);
                ::dup2(output[1], STDOUT_FILENO); // dup2 clears close-on-exec on the copy
                for (int fd = 3; fd < maxFd; ++fd)
                    ::close(fd);
                ::execve(argp[0], argp.data(), envp.data());
                ::_exit(127);
            }
            ::close(output[1]);
            std::string text;
            char buffer[4096];
            for (;;)
            {
                const ssize_t got = ::read(output[0], buffer, sizeof(buffer));
                if (got < 0 && errno == EINTR)
                    continue;
                if (got <= 0)
                    break;
                text.append(buffer, static_cast<size_t>(got));
            }
            ::close(output[0]);
            int status = 0;
            while (::waitpid(child, &status, 0) < 0 && errno == EINTR)
            {
            }
            exitCode = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
            return text;
        }

        std::string homeFolder()
        {
            const char *home = std::getenv("HOME");
            return home && *home ? std::string(home) : std::string("/");
        }

        // ---- libcurl, opened at run time (curl.h's values: the ABI has kept them since libcurl 7) ----
        struct Curl
        {
            using Easy = void;
            using Slist = void;
            int (*globalInit)(long) = nullptr;
            Easy *(*init)() = nullptr;
            int (*setopt)(Easy *, int, ...) = nullptr;
            int (*perform)(Easy *) = nullptr;
            int (*getinfo)(Easy *, int, ...) = nullptr;
            void (*cleanup)(Easy *) = nullptr;
            Slist *(*slistAppend)(Slist *, const char *) = nullptr;
            void (*slistFree)(Slist *) = nullptr;
            const char *(*strerror)(int) = nullptr;
            bool ok = false;
        };
        constexpr int kOptWriteData = 10001, kOptUrl = 10002, kOptWriteFunction = 20011, kOptUserAgent = 10018,
                      kOptLowSpeedLimit = 19, kOptLowSpeedTime = 20, kOptHttpHeader = 10023, kOptFollowLocation = 52,
                      kOptMaxRedirs = 68, kOptConnectTimeout = 78, kOptNoSignal = 99;
        constexpr int kInfoResponseCode = 0x200000 + 2;
        constexpr long kGlobalDefault = 3;

        const Curl &curl()
        {
            static const Curl c = []
            {
                Curl out;
                void *library = nullptr;
                for (const char *name : {"libcurl.so.4", "libcurl-gnutls.so.4"})
                    if ((library = ::dlopen(name, RTLD_NOW | RTLD_LOCAL)) != nullptr)
                        break;
                if (library == nullptr)
                {
                    std::fprintf(stderr, "[launcher] downloads off: libcurl.so.4 is not installed (%s)\n", ::dlerror());
                    return out;
                }
                const auto load = [&](auto &fn, const char *symbol)
                {
                    fn = reinterpret_cast<std::remove_reference_t<decltype(fn)>>(::dlsym(library, symbol));
                    return fn != nullptr;
                };
                out.ok = load(out.globalInit, "curl_global_init") && load(out.init, "curl_easy_init") &&
                         load(out.setopt, "curl_easy_setopt") && load(out.perform, "curl_easy_perform") &&
                         load(out.getinfo, "curl_easy_getinfo") && load(out.cleanup, "curl_easy_cleanup") &&
                         load(out.slistAppend, "curl_slist_append") && load(out.slistFree, "curl_slist_free_all") &&
                         load(out.strerror, "curl_easy_strerror");
                // curl_global_init is not thread-safe in older libcurl: once, here, before any download thread uses it.
                if (out.ok && out.globalInit(kGlobalDefault) != 0)
                    out.ok = false;
                if (!out.ok)
                    std::fprintf(stderr, "[launcher] downloads off: the libcurl found lacks a function GHR needs\n");
                return out;
            }();
            return c;
        }

        struct Download
        {
            std::vector<uint8_t> *out;
            size_t maxBytes;
            std::atomic<uint64_t> *received;
            bool tooBig = false;
        };

        size_t onData(char *data, size_t size, size_t count, void *user)
        {
            auto *d = static_cast<Download *>(user);
            const size_t n = size * count;
            if (d->out->size() + n > d->maxBytes)
            {
                d->tooBig = true;
                return 0; // a short count stops the transfer (CURLE_WRITE_ERROR)
            }
            d->out->insert(d->out->end(), reinterpret_cast<const uint8_t *>(data), reinterpret_cast<const uint8_t *>(data) + n);
            if (d->received)
                d->received->store(d->out->size(), std::memory_order_relaxed);
            return n;
        }
    }
#endif

    namespace
    {
        enum class PickKind
        {
            Disc,
            Folder,
            Zip,
        };
    }

    static std::filesystem::path pickWith(void *window, PickKind kind)
    {
        const bool folder = kind == PickKind::Folder;
#if defined(_WIN32)
        const HRESULT init = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
        std::filesystem::path result;
        IFileOpenDialog *dialog = nullptr;
        if (SUCCEEDED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dialog))))
        {
            FILEOPENDIALOGOPTIONS options = 0;
            dialog->GetOptions(&options);
            dialog->SetOptions(options | FOS_FORCEFILESYSTEM | FOS_PATHMUSTEXIST | (folder ? FOS_PICKFOLDERS : FOS_FILEMUSTEXIST));
            if (folder)
                dialog->SetTitle(L"Choose the texture pack folder (the one holding replacements)");
            else if (kind == PickKind::Zip)
            {
                const COMDLG_FILTERSPEC filters[] = {{L"Mod (*.zip)", L"*.zip"}, {L"All files", L"*.*"}};
                dialog->SetFileTypes(2, filters);
                dialog->SetTitle(L"Choose a mod's zip to install");
            }
            else
            {
                const COMDLG_FILTERSPEC filters[] = {{L"PS2 disc image (*.iso, *.chd)", L"*.iso;*.chd"}, {L"All files", L"*.*"}};
                dialog->SetFileTypes(2, filters);
                dialog->SetTitle(L"Choose your disc image (USA, SLUS-21503)");
            }
            // A borderless window covering the whole monitor has its frames shown straight on screen, without the
            // desktop's composition, so the dialog opened over it stays invisible until something (the Start key)
            // brings composition back (the same cause as the Nov 17 black screen after Play). One pixel off the
            // bottom while the dialog is up gives composition back; the old size returns after it.
            const HWND owner = static_cast<HWND>(window);
            RECT kept{};
            bool shrunk = false;
            if (owner != nullptr && GetWindowRect(owner, &kept))
            {
                MONITORINFO monitor{};
                monitor.cbSize = sizeof(monitor);
                const RECT &m = monitor.rcMonitor;
                if (GetMonitorInfoW(MonitorFromWindow(owner, MONITOR_DEFAULTTONEAREST), &monitor) && kept.left <= m.left &&
                    kept.top <= m.top && kept.right >= m.right && kept.bottom >= m.bottom)
                {
                    shrunk = SetWindowPos(owner, nullptr, m.left, m.top, m.right - m.left, m.bottom - m.top - 1,
                                          SWP_NOZORDER | SWP_NOACTIVATE) != FALSE;
                }
            }
            const bool chosen = SUCCEEDED(dialog->Show(owner));
            if (shrunk)
                SetWindowPos(owner, nullptr, kept.left, kept.top, kept.right - kept.left, kept.bottom - kept.top,
                             SWP_NOZORDER | SWP_NOACTIVATE);
            if (chosen)
            {
                IShellItem *item = nullptr;
                if (SUCCEEDED(dialog->GetResult(&item)))
                {
                    PWSTR path = nullptr;
                    if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &path)))
                    {
                        result = std::filesystem::path(path);
                        CoTaskMemFree(path);
                    }
                    item->Release();
                }
            }
            dialog->Release();
        }
        if (SUCCEEDED(init))
            CoUninitialize();
        return result;
#else
        // zenity (GNOME, SteamOS) first, then kdialog (KDE); neither there: nothing (the disc search and the
        // Install screen's path box still work). Both block until the player chooses, as the Windows dialog does.
        (void)window;
        const std::string start = homeFolder() + "/";
        const std::string zenity = findProgram("zenity");
        if (!zenity.empty())
        {
            std::vector<std::string> args{"--file-selection"};
            if (folder)
                args.insert(args.end(), {"--directory", "--title=Choose the texture pack folder (the one holding replacements)"});
            else if (kind == PickKind::Zip)
                args.insert(args.end(), {"--title=Choose a mod's zip to install", "--file-filter=Mod (*.zip) | *.zip *.ZIP",
                                         "--file-filter=All files | *"});
            else
                args.insert(args.end(), {"--title=Choose your disc image (USA, SLUS-21503)",
                                         "--file-filter=PS2 disc image (*.iso, *.chd) | *.iso *.ISO *.chd *.CHD",
                                         "--file-filter=All files | *"});
            args.push_back("--filename=" + start);
            int code = -1;
            std::string chosen = runAndRead(zenity, args, code);
            while (!chosen.empty() && (chosen.back() == '\n' || chosen.back() == '\r'))
                chosen.pop_back();
            if (code == 0 && !chosen.empty())
                return std::filesystem::path(chosen);
            if (code == 0 || code == 1) // chosen, or cancelled
                return {};
        }
        const std::string kdialog = findProgram("kdialog");
        if (!kdialog.empty())
        {
            std::vector<std::string> args;
            if (folder)
                args = {"--title", "Choose the texture pack folder (the one holding replacements)", "--getexistingdirectory", start};
            else if (kind == PickKind::Zip)
                args = {"--title", "Choose a mod's zip to install", "--getopenfilename", start, "*.zip *.ZIP|Mod (*.zip)"};
            else
                args = {"--title", "Choose your disc image (USA, SLUS-21503)", "--getopenfilename", start,
                        "*.iso *.ISO *.chd *.CHD|PS2 disc image (*.iso, *.chd)"};
            int code = -1;
            std::string chosen = runAndRead(kdialog, args, code);
            while (!chosen.empty() && (chosen.back() == '\n' || chosen.back() == '\r'))
                chosen.pop_back();
            if (code == 0 && !chosen.empty())
                return std::filesystem::path(chosen);
            return {};
        }
        std::fprintf(stderr, "[launcher] no file chooser: install zenity or kdialog, or type the path\n");
        return {};
#endif
    }

    std::filesystem::path pick(void *window, bool folder) { return pickWith(window, folder ? PickKind::Folder : PickKind::Disc); }

    std::filesystem::path pickZip(void *window) { return pickWith(window, PickKind::Zip); }

    void open(const std::filesystem::path &path)
    {
#if defined(_WIN32)
        ShellExecuteW(nullptr, L"open", path.wstring().c_str(), nullptr, nullptr, SW_SHOWNORMAL);
#else
        const std::string opener = findProgram("xdg-open");
        if (opener.empty() || !spawnDetached(opener, {path.string()}, std::string(), true))
            std::fprintf(stderr, "[launcher] could not open %s (xdg-open missing?)\n", path.string().c_str());
#endif
    }

    bool shiftHeld()
    {
#if defined(_WIN32)
        return (GetAsyncKeyState(VK_SHIFT) & 0x8000) != 0;
#else
        // Best effort: X11 and Wayland give a program the keyboard only through its own window, so Shift counts
        // once the window is open; before that (the start-up check) it reads as not held. [Launcher] Show=1 brings
        // the launcher back there.
        return IsWindowReady() && (IsKeyDown(KEY_LEFT_SHIFT) || IsKeyDown(KEY_RIGHT_SHIFT));
#endif
    }

#if defined(_WIN32)
    namespace
    {
        std::wstring wide(const std::string &text)
        {
            const int n = MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0);
            std::wstring out(static_cast<size_t>(n), L'\0');
            MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), out.data(), n);
            return out;
        }
    }
#endif

    bool httpGet(const std::string &url, std::vector<uint8_t> &out, size_t maxBytes, const char *accept,
                 std::atomic<uint64_t> *received)
    {
        out.clear();
#if defined(_WIN32)
        const std::wstring address = wide(url);
        wchar_t host[256] = {}, path[2048] = {};
        URL_COMPONENTS parts{};
        parts.dwStructSize = sizeof(parts);
        parts.lpszHostName = host;
        parts.dwHostNameLength = 256;
        parts.lpszUrlPath = path;
        parts.dwUrlPathLength = 2048;
        if (!WinHttpCrackUrl(address.c_str(), 0, 0, &parts))
            return false;
        // The agent name is the User-Agent GitHub's API asks for.
        HINTERNET session = WinHttpOpen(L"GHR", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
        if (session == nullptr) // before Windows 8.1
            session = WinHttpOpen(L"GHR", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
        if (session == nullptr)
            return false;
        WinHttpSetTimeouts(session, 10000, 10000, 15000, 30000);
        const std::wstring headers = accept ? L"Accept: " + wide(accept) + L"\r\n" : std::wstring();
        bool ok = false;
        if (HINTERNET connection = WinHttpConnect(session, host, parts.nPort, 0))
        {
            if (HINTERNET request = WinHttpOpenRequest(connection, L"GET", path, nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
                                                       parts.nScheme == INTERNET_SCHEME_HTTPS ? WINHTTP_FLAG_SECURE : 0))
            {
                DWORD status = 0, size = sizeof(status);
                if (WinHttpSendRequest(request, headers.empty() ? WINHTTP_NO_ADDITIONAL_HEADERS : headers.c_str(),
                                       headers.empty() ? 0 : static_cast<DWORD>(-1L), WINHTTP_NO_REQUEST_DATA, 0, 0, 0) &&
                    WinHttpReceiveResponse(request, nullptr) &&
                    WinHttpQueryHeaders(request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX, &status,
                                        &size, WINHTTP_NO_HEADER_INDEX) &&
                    status == 200)
                {
                    ok = true;
                    for (;;)
                    {
                        DWORD available = 0, read = 0;
                        if (!WinHttpQueryDataAvailable(request, &available))
                        {
                            ok = false;
                            break;
                        }
                        if (available == 0)
                            break;
                        const size_t at = out.size();
                        if (at + available > maxBytes)
                        {
                            ok = false;
                            break;
                        }
                        out.resize(at + available);
                        if (!WinHttpReadData(request, out.data() + at, available, &read))
                        {
                            ok = false;
                            break;
                        }
                        out.resize(at + read);
                        if (received)
                            received->store(out.size(), std::memory_order_relaxed);
                    }
                }
                WinHttpCloseHandle(request);
            }
            WinHttpCloseHandle(connection);
        }
        WinHttpCloseHandle(session);
        return ok;
#else
        const Curl &c = curl();
        if (!c.ok)
            return false;
        Curl::Easy *easy = c.init();
        if (easy == nullptr)
            return false;
        Download download{&out, maxBytes, received};
        Curl::Slist *headers = nullptr;
        if (accept)
            headers = c.slistAppend(nullptr, (std::string("Accept: ") + accept).c_str());
        using WriteFn = size_t (*)(char *, size_t, size_t, void *);
        const WriteFn write = &onData;
        // The same limits as the WinHTTP version: 10 s to connect, a transfer stalled for 30 s ends. NOSIGNAL: libcurl
        // must not use SIGALRM for its timeouts on a thread of a program with its own signal handlers.
        c.setopt(easy, kOptUrl, url.c_str());
        c.setopt(easy, kOptUserAgent, "GHR"); // the User-Agent GitHub's API asks for
        c.setopt(easy, kOptFollowLocation, 1L);
        c.setopt(easy, kOptMaxRedirs, 10L);
        c.setopt(easy, kOptConnectTimeout, 10L);
        c.setopt(easy, kOptLowSpeedLimit, 1L);
        c.setopt(easy, kOptLowSpeedTime, 30L);
        c.setopt(easy, kOptNoSignal, 1L);
        c.setopt(easy, kOptWriteFunction, write);
        c.setopt(easy, kOptWriteData, static_cast<void *>(&download));
        if (headers)
            c.setopt(easy, kOptHttpHeader, headers);
        const int result = c.perform(easy);
        long status = 0;
        c.getinfo(easy, kInfoResponseCode, &status);
        c.cleanup(easy);
        if (headers)
            c.slistFree(headers);
        if (result != 0 || status != 200)
        {
            if (result != 0 && !download.tooBig)
                std::fprintf(stderr, "[launcher] download failed: %s (%s)\n", c.strerror(result), url.c_str());
            out.clear();
            return false;
        }
        return true;
#endif
    }

    std::filesystem::path thisExe()
    {
#if defined(_WIN32)
        std::wstring path(32768, L'\0');
        const DWORD n = GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
        path.resize(n);
        return std::filesystem::path(path);
#else
        // In an AppImage the program runs from a mount that is gone once it quits: the file the player has (what
        // Steam shortcuts and the updater must point at) is $APPIMAGE.
        if (const char *appImage = std::getenv("APPIMAGE"); appImage && *appImage)
            return std::filesystem::path(appImage);
        std::error_code error;
        const std::filesystem::path exe = std::filesystem::read_symlink("/proc/self/exe", error);
        return error ? std::filesystem::path() : exe;
#endif
    }

    bool relaunch(const std::filesystem::path &exe)
    {
#if defined(_WIN32)
        std::wstring command = L"\"" + exe.wstring() + L"\"";
        STARTUPINFOW startup{};
        startup.cb = sizeof(startup);
        PROCESS_INFORMATION process{};
        if (!CreateProcessW(exe.c_str(), command.data(), nullptr, nullptr, FALSE, 0, nullptr, exe.parent_path().c_str(),
                            &startup, &process))
            return false;
        CloseHandle(process.hThread);
        CloseHandle(process.hProcess);
        return true;
#else
        if (exe.empty())
            return false;
        // GHR's own environment goes along (KAMITE_HOME, Steam's variables); an AppImage sets its APPDIR afresh.
        return spawnDetached(exe.string(), {}, exe.parent_path().string(), false);
#endif
    }

#if !defined(_WIN32)
    bool spawn(const std::string &program, const std::vector<std::string> &args)
    {
        const std::string path = program.find('/') != std::string::npos ? program : findProgram(program);
        return !path.empty() && spawnDetached(path, args, std::string(), true);
    }
#endif
}
