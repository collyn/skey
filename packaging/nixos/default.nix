# fcitx5-skey — Nix package.
#
# Usable standalone via `pkgs.callPackage ./default.nix { }` or through the
# flake in this directory.  The skey-engine Rust library is built by a
# separate derivation (buildRustPackage) because the CMake cargo step needs
# network access, which nix sandboxes forbid.
{
  lib,
  stdenv,
  cmake,
  pkg-config,
  kdePackages, # extra-cmake-modules (top-level alias removed after KDE Gear 5 EOL)
  gettext,
  librsvg, # rsvg-convert for PNG tray icons
  dbus,
  libxcb, # X11 WM_CLASS detection
  fcitx5,
  qt6, # settings GUI (qtbase/qtwayland/qtsvg) + wrapQtAppsHook
  rustPlatform,
  # Source of https://github.com/collyn/skey-engine.  Overridable so a
  # caller can pass its own fetchFromGitHub result (pinned with a hash).
  # Keep in sync with SKEY_ENGINE_VERSION in the top-level CMakeLists.txt.
  # The sha256 keeps the fetch legal under pure evaluation (flakes); it
  # must be updated whenever the tag changes (nix store prefetch-file
  # --unpack <tarball-url>).
  skeyEngineSrc ? builtins.fetchTarball {
    url = "https://github.com/collyn/skey-engine/archive/refs/tags/v0.1.23.tar.gz";
    sha256 = "sha256-wtbL+cjr28DKTbNKoQiQwQyCKiSIs08/ghG0uw4l/08=";
  },
}:

let
  skeyEngine = rustPlatform.buildRustPackage {
    pname = "skey-engine";
    version = "0.1.23";
    src = skeyEngineSrc;
    # Cargo.lock carries the per-crate checksums, so no cargoHash is needed.
    cargoLock.lockFile = "${skeyEngineSrc}/Cargo.lock";
    doCheck = false;
    installPhase = ''
      runHook preInstall
      # cargoBuildHook passes --target <rustcTarget>, so artifacts land in
      # target/<triple>/release/, not target/release/.
      install -Dm644 target/${stdenv.hostPlatform.rust.rustcTarget}/release/libskey_engine.a \
        "$out/lib/libskey_engine.a"
      install -Dm644 skey_engine.h "$out/include/skey_engine.h"
      runHook postInstall
    '';

    meta = with lib; {
      # skey-engine repo is GPL-3.0-or-later too (its LICENSE has the same
      # "at your option, any later version" grant).
      license = licenses.gpl3Plus;
      platforms = platforms.linux;
    };
  };
in
stdenv.mkDerivation (finalAttrs: {
  pname = "fcitx5-skey";
  version = "0.7.7";

  src = ../..;

  # The repo ships in-tree build dirs (build/, build-dev/, CMakeFiles/,
  # Testing/, compile_commands.json) whose CMakeCache.txt records the
  # original absolute source path.  Nix's cmake configure step would reuse
  # that stale cache and die with "does not match the source".  They are
  # local artifacts, never needed for a clean build.
  preConfigure = ''
    rm -rf build build-dev CMakeFiles Testing compile_commands.json
  '';

  nativeBuildInputs = [
    cmake
    pkg-config
    kdePackages.extra-cmake-modules
    gettext
    librsvg
    qt6.wrapQtAppsHook
  ];

  buildInputs = [
    fcitx5
    dbus
    libxcb
    qt6.qtbase
    qt6.qtwayland # runtime platform plugin for the settings GUI on Wayland
    qt6.qtsvg # QIcon SVG loading at runtime
    # No `acl` here: setfacl is only referenced as text inside the unit
    # and udev rule files, never invoked at build time.  The NixOS module
    # uses pkgs.acl directly for its ExecStartPre/ExecStartPost overrides.
  ];

  cmakeFlags = [
    # Engine: use the fetched source for skey_engine.h and link the
    # prebuilt static lib — CMake never invokes cargo.
    "-DSKEY_ENGINE_SOURCE_DIR=${skeyEngineSrc}"
    "-DSKEY_ENGINE_PREBUILT_LIB=${skeyEngine}/lib/libskey_engine.a"
    # Distro-specific files: NixOS has no /lib or /etc at build time.
    # systemd.packages in the NixOS module picks the unit up from here.
    "-DSKEY_SYSTEMD_UNIT_DIR=${placeholder "out"}/lib/systemd/system"
    "-DSKEY_POLKIT_RULES_DIR=${placeholder "out"}/etc/polkit-1/rules.d"
    "-DSKEY_UDEV_RULES_DIR=${placeholder "out"}/lib/udev/rules.d"
    "-DSKEY_SYSUSERS_DIR=${placeholder "out"}/lib/sysusers.d"
    "-DSKEY_PROFILE_D_DIR=${placeholder "out"}/etc/profile.d"
  ];

  meta = with lib; {
    description = "SKey Vietnamese input method engine addon for Fcitx5";
    homepage = "https://github.com/collyn/skey";
    # Root LICENSE: GPL-3.0-or-later ("at your option, any later version").
    license = licenses.gpl3Plus;
    maintainers = [ ];
    platforms = platforms.linux;
  };
})
