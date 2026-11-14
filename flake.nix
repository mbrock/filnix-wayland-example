{
  inputs.filnix.url = "github:mbrock/filnix";

  outputs =
    { filnix, ... }:
    let
      # Every library in this package set is compiled with Fil-C.
      fil = filnix.legacyPackages.x86_64-linux.pkgsFilc;

      # Tools that run on your machine during the build (ordinary native builds).
      nativeBuildInputs = with fil.buildPackages; [
        cmake
        ninja
        pkg-config
        python3
        wayland-scanner
      ];

      # Libraries the program links against (Fil-C builds).
      buildInputs = with fil; [
        dbus
        libxkbcommon
        wayland
        wayland-protocols
        # pkg-config also asks for these, via dbus-1.pc and wayland-client.pc.
        systemdLibs
        libffi
      ];
    in
    {
      # `nix develop` opens a shell for building the project yourself.
      devShells.x86_64-linux.default = fil.mkShell {
        inherit nativeBuildInputs buildInputs;
      };

      # `nix build` and `nix run` build the program with CMake in the Nix sandbox.
      packages.x86_64-linux.default = fil.stdenv.mkDerivation {
        pname = "hello-wayland";
        version = "0.1";
        src = ./.;
        inherit nativeBuildInputs buildInputs;
        meta.mainProgram = "hello-wayland";
      };
    };
}
