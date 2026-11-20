{
  description = "God Hand: Rearmed - Linux build environment, checks and packaging";

  inputs.nixpkgs.url = "github:NixOS/nixpkgs/nixos-26.05";

  outputs = { self, nixpkgs }:
    let
      systems = [ "x86_64-linux" ];
      forAll = f: nixpkgs.lib.genAttrs systems (system: f nixpkgs.legacyPackages.${system});
    in
    {
      devShells = forAll (pkgs: {
        default = pkgs.mkShell {
          packages = with pkgs; [
            cmake ninja pkg-config python3 git gcc
            # raylib (GLFW, X11 + Wayland) and SDL3's video/input backends
            libx11 libxrandr libxinerama libxcursor libxi libxext libxfixes
            libxscrnsaver libxtst
            libGL wayland wayland-scanner wayland-protocols libxkbcommon libdecor
            alsa-lib libpulseaudio pipewire udev dbus
            vulkan-headers vulkan-loader
          ];
        };
      });
    };
}
