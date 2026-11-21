#pragma once

// The pure parts of the launcher's updates (launcher_update.cpp), kept apart so the unit tests can reach them: which
// download of a release fits this build, and the Linux tar.gz's unpacking.
#include <cstdint>
#include <string>
#include <vector>

namespace kamite::launcher::update::archive
{
    // What the running build was shipped as, which decides the release download it takes.
    enum class Package
    {
        WindowsZip,    // GHR-v<ver>.zip
        LinuxFolder,   // GHR-v<ver>-Linux-x64.tar.gz (a writable folder, like the zip)
        LinuxAppImage, // GHR-v<ver>-Linux-x64.AppImage (one file, replaced whole)
        LinuxFlatpak,  // GHR-v<ver>-Linux-x64.flatpak (flatpak owns /app: updated through flatpak, never by GHR)
    };
    // Windows: the zip. Linux: linuxPackage() of $APPIMAGE, /.flatpak-info and /proc/self/exe.
    Package current();
    // Linux's rule: a Flatpak when the sandbox's /.flatpak-info exists and the program runs from /app (a tar.gz folder
    // or an AppImage started by Steam's own Flatpak is in a sandbox too, but not under /app; an $APPIMAGE inherited
    // from whatever started the Flatpak is not ours); else an AppImage when $APPIMAGE is set (the AppImage runtime
    // sets it); else the tar.gz.
    Package linuxPackage(const char *appImage, bool flatpakInfo, const std::string &exe);
    const char *name(Package package);

    // The index of the release asset (by its file name, any case) `package` installs, -1 when there is none. The
    // Windows zip never matches a Linux download, nor a Linux build the zip.
    int pickAsset(const std::vector<std::string> &assetNames, Package package);

    // CRC-32 (the gzip / zip one).
    uint32_t crc32(const uint8_t *data, size_t size);

    // A .gz (one member: what tar czf writes) inflated into `out`; false when it is not gzip, it is damaged (the
    // trailer's CRC or size disagree) or bigger than maxBytes. `error` says which.
    bool gunzip(const std::vector<uint8_t> &gz, std::vector<uint8_t> &out, size_t maxBytes, std::string &error);

    struct TarEntry
    {
        std::string name;        // relative, '/' separated, the archive's one top folder (if all share one) removed
        bool folder = false;
        bool executable = false; // any of the mode's x bits
        size_t data = 0, size = 0; // the file's bytes in the tar
    };
    // A tar (ustar or GNU; GNU long names and pax "path" records too) read into its regular files and folders.
    // Symbolic links, hard links and devices are left out and counted in `skipped`. False (and `error`) when the tar
    // is damaged (a header checksum, a size past the end) or names a path outside the folder (absolute, or "..").
    bool readTar(const std::vector<uint8_t> &tar, std::vector<TarEntry> &entries, size_t &skipped, std::string &error);
}
