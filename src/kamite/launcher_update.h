#pragma once

// The launcher's in-place updates (claude/release.md): at every start the GitHub releases are read (silently nothing
// when offline); a newer one shows "What's new" and "Update" above the volume. Update downloads the release zip,
// writes its program files next to the old ones, swaps them in (the running GHR.exe and its DLLs are renamed aside,
// removed at the next start) and starts the game again. Never touched: GHR.ini and controls.ini (only keys they lack
// are added), memory cards, save states, cases, logs, texture packs, mods, the disc image.
// Release builds check; a dev build (testers build main) only with [Launcher] UpdateTest=1, as version 0.0.0.
// Linux (update_archive.h picks the download): a tar.gz folder is updated as the zip is (same kept folders, same
// settings merge, the files' x bits kept); an AppImage ($APPIMAGE set) is downloaded next to itself and renamed over
// itself. The settings files are the play folder's (paths.h), the rest the program folder's. A Flatpak is offered
// the release's .flatpak but never installs it: Update says to update through Flatpak.
#include <atomic>
#include <cstdint>
#include <filesystem>
#include <string>
#include <thread>
#include <vector>

namespace kamite::launcher::update
{
    // "v0.6.9 Beta", or with the commit for a dev build.
    std::string versionLabel();
    // The newspaper's date line (claude/newspaper.md): "BETA EDITION" (KAMITE_VERSION_LABEL) or, in a dev build,
    // "DEV EDITION"; the dev build's commit ("" in a release build).
    std::string editionName();
    std::string devCommit();

    // One release as the newspaper prints it.
    struct Edition
    {
        std::string tag;  // "v0.7.0"
        std::string date; // "NOV. 24, 2026": when it went up on GitHub ("" unknown)
        std::string body; // its notes (the release's description)
    };

    struct Release
    {
        std::string tag;   // "v0.7.0"
        std::string notes; // every newer release's notes, newest first: "# v0.7.0" then its body's lines
        std::string zipUrl;   // the download this build installs: the zip, the tar.gz or the AppImage
        uint64_t zipSize = 0;
        std::vector<Edition> editions; // every release newer than ours, newest first
    };

    enum class CheckState : int
    {
        Off,      // not checked (a dev build)
        Checking,
        UpToDate,
        Available,
        Failed, // offline, GitHub unreachable: shown as up to date
    };
    struct Checker
    {
        std::atomic<int> state{static_cast<int>(CheckState::Off)};
        Release release; // valid once state is Available
        // This version's release (a dev build without its own: the newest), valid once the state is UpToDate or
        // Available and `hasCurrent`.
        Edition current;
        bool hasCurrent = false;
        std::thread worker;
    };
    // test: a dev build's [Launcher] UpdateTest (any release counts as newer). Dev builds read the releases too (the
    // newspaper's notes), but only offer an update with `test`.
    // tester: [Launcher] Channel=tester (the tester zip's GHR.ini): the rolling tester-build release instead, whose zip
    // is a build of main (tools/win/Publish-Tester.ps1, after each of K's builds); a newer commit's build is offered.
    void startCheck(Checker &checker, bool test, bool tester);

    enum class Stage : int
    {
        Downloading,
        Installing,
        Done,   // swapped in: start the game again
        Failed, // nothing changed; `error` says why
    };
    struct Installer
    {
        std::atomic<int> stage{static_cast<int>(Stage::Downloading)};
        std::atomic<uint64_t> received{0};
        uint64_t total = 0;
        std::string error;
        std::thread worker;
    };
    // exeDir: the program folder (paths::programDir()).
    void startInstall(Installer &installer, const Release &release, const std::filesystem::path &exeDir);

    // The launcher's start: removes the old program files a finished update renamed aside (on a thread: the old
    // process may still be closing).
    void cleanup(const std::filesystem::path &exeDir);
}
