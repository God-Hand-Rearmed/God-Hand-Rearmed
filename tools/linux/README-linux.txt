God Hand: Rearmed {version} for Linux and the Steam Deck
=========================================================

An unofficial, fan-made native PC port of Clover Studio's God Hand (PlayStation 2, 2006).
No game files are included: it plays your own disc image (USA, SLUS-21503; DISC_HASHES.md on GitHub has the
checksums). https://github.com/God-Hand-Rearmed/God-Hand-Rearmed

What you need
-------------
- A 64-bit x86 PC (any CPU with SSE4.1) or a Steam Deck.
- Linux with glibc 2.31 or newer (SteamOS, Debian 11+, Ubuntu 20.04+, Fedora, Arch, ...).
- A Vulkan driver for the game's renderer (Mesa's RADV/ANV, or NVIDIA's). The launcher itself uses OpenGL.

Three ways to run it
--------------------
The AppImage (one file):
    chmod +x GHR-v{version}-Linux-x64.AppImage
    ./GHR-v{version}-Linux-x64.AppImage
  Settings, saves, logs and mods go to ~/.local/share/GodHandRearmed. To keep them beside the AppImage instead, make a
  folder named GHR-data next to it before the first start.
  If it says FUSE is missing, run it with APPIMAGE_EXTRACT_AND_RUN=1 set, or install your distribution's fuse package.

The folder (GHR-v{version}-Linux-x64.tar.gz):
    tar xf GHR-v{version}-Linux-x64.tar.gz
    "God Hand Rearmed/GHR"
  Unpack it anywhere you can write to. Settings and saves live inside the folder, like the Windows version.
  ./install-desktop-entry.sh adds it to your application menu.

The Flatpak (GHR-v{version}-Linux-x64.flatpak):
    flatpak install --user GHR-v{version}-Linux-x64.flatpak
    flatpak run io.github.God_Hand_Rearmed.GHR
  The first install also fetches the Freedesktop runtime from Flathub. It runs in Flatpak's sandbox: settings and
  saves go to ~/.var/app/io.github.God_Hand_Rearmed.GHR/data/GodHandRearmed, and it can read your disc image only from
  ~/Games, ~/Downloads or an SD card / USB drive (it has no file chooser yet: it finds the image there by itself). For
  a disc image elsewhere:
    flatpak override --user --filesystem=/path/to/its/folder:ro io.github.God_Hand_Rearmed.GHR

The first start
---------------
The launcher opens: point it at your disc image (.iso) once, pick a preset and press Play. It also finds a disc image
by itself in a "game" folder next to the program, in ~/Games, ~/Downloads, or on an SD card.
The very first start compiles the graphics for your GPU, which can take a while: "Preparing shaders" shows with a
moving bar, the seconds and the shaders done so far. Once.

Steam Deck
----------
1. In Desktop Mode, put the AppImage (or the folder) somewhere in your home folder, and your disc image beside it or in
   ~/Games.
2. Start it once from the file manager, choose the disc image, then press "Add to Steam" in the launcher (close Steam
   first if it asks). It adds God Hand: Rearmed to your library with its artwork.
   The Flatpak can't reach Steam from its sandbox: add it with Steam's own "Add a Non-Steam Game" instead.
3. Back in Game Mode it is in your library under Non-Steam. Fullscreen and the Deck's 16:10 screen are picked for you.
The Deck's controls work through Steam Input with the Xbox-style layout; the button prompts follow your controller.

Where things are
----------------
AppImage: ~/.local/share/GodHandRearmed (or GHR-data next to it).  Folder: inside the folder.
Flatpak: ~/.var/app/io.github.God_Hand_Rearmed.GHR/data/GodHandRearmed.
  GHR.ini          every option (the launcher writes it)
  controls.ini     your controls
  memcards/mc0     the game's saves        states/   save states (F5 / F9)
  logs/            the logs; logs/kamite.log goes with a bug report
  textures/SLUS-21503   an HD texture pack in PCSX2's format

Updates: the launcher says when a new version is out and installs it in place (the AppImage replaces itself; the folder
updates its files). Your settings, saves and mods are never touched. The Flatpak only says so: install the new .flatpak
the same way as the first (flatpak install --user ...), which keeps your settings and saves.

Bugs: https://github.com/God-Hand-Rearmed/God-Hand-Rearmed/issues (attach logs/kamite.log).

God Hand is a trademark of Capcom. This project is not affiliated with or endorsed by Capcom or Clover Studio.
The code is GPL-3.0 (LICENSE); the licenses of everything it is built on are in licenses/.
