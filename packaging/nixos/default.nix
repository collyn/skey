# fcitx5-skey — Nix package.
#
# Usable standalone via `pkgs.callPackage ./default.nix { }` or through the
# flake in this directory.  The skey-engine Rust library defaults to the
# prebuilt libskey_engine.a from the skey-engine GitHub release — that skips
# the whole Rust toolchain (rustc/cargo/LLVM, ≈ 470 MB download / 1.7 GiB
# unpacked).  Pass `skeyEngineLib = null` to build it from source instead
# (buildRustPackage, needed because the CMake cargo step requires network
# access, which nix sandboxes forbid).
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
  # Content of ../../CMakeLists.txt, flattened, so the version parsing below
  # (package version, pinned engine tag) always matches the source.
  cmakeLists ? builtins.replaceStrings [ "\n" ] [ " " ] (builtins.readFile ../../CMakeLists.txt),
  # Engine tag pinned by SKEY_ENGINE_VERSION in the top-level CMakeLists.txt.
  # The two fetches below follow it: bumping the tag updates both URLs, and
  # the stale hashes then fail loudly until refreshed (nix store prefetch-file
  # --unpack <tarball-url> / plain prefetch for the release asset).
  engineVersion ? let
    m = builtins.match ".*set\\(SKEY_ENGINE_VERSION \"([^\"]+)\"\\).*" cmakeLists;
  in if m == null
     then throw "fcitx5-skey: cannot parse SKEY_ENGINE_VERSION from ../../CMakeLists.txt"
     else builtins.head m,
  # Source of https://github.com/collyn/skey-engine (skey_engine.h + the
  # source-build path).  Overridable so a caller can pass its own
  # fetchFromGitHub result (pinned with a hash).
  skeyEngineSrc ? builtins.fetchTarball {
    url = "https://github.com/collyn/skey-engine/archive/refs/tags/${engineVersion}.tar.gz";
    sha256 = "sha256-wtbL+cjr28DKTbNKoQiQwQyCKiSIs08/ghG0uw4l/08=";
  },
  # Prebuilt libskey_engine.a from the skey-engine GitHub release — the
  # default.  skips the Rust toolchain entirely.  Pass null to build the
  # engine from source instead:
  #   pkgs.fcitx5-skey.override { skeyEngineLib = null; }
  skeyEngineLib ? builtins.fetchurl {
    url = "https://github.com/collyn/skey-engine/releases/download/${engineVersion}/libskey_engine.a";
    sha256 = "sha256-pJNZpGldejtQxhe4C+zazdKavPt1UPcjL8p2gaOPzx4=";
  },
  # Dev channel: set to the dev prerelease version (e.g. "0.7.7-dev.3") to
  # build the package with that version string and the dev-build counter
  # (SKEY_APP_VERSION / SKEY_DEV_BUILD), so the updater recognizes the
  # build instead of re-offering the same dev tag forever.  null = stable.
  devVersion ? null,
}:

let
  # Version parsed from the top-level CMakeLists.txt (mirrors PKGBUILD's
  # pkgver()) so a release bump never has to touch two files.  project()
  # VERSION stays purely numeric, so the derived string is always a valid
  # Nix version.
  stableVersion = let
    m = builtins.match ".*project\\(fcitx5-skey VERSION ([0-9.]+)\\).*" cmakeLists;
  in if m == null
     then throw "fcitx5-skey: cannot parse version from ../../CMakeLists.txt"
     else builtins.head m;

  # Only built when skeyEngineLib is null (source-build override).
  skeyEngine = rustPlatform.buildRustPackage {
    pname = "skey-engine";
    version = lib.removePrefix "v" engineVersion;
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
  version = if devVersion == null then stableVersion else devVersion;

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
    # prebuilt static lib — CMake never invokes cargo.  skeyEngineLib
    # (default) is the release asset; null builds from source instead.
    "-DSKEY_ENGINE_SOURCE_DIR=${skeyEngineSrc}"
    "-DSKEY_ENGINE_PREBUILT_LIB=${
      if skeyEngineLib == null
      then "${skeyEngine}/lib/libskey_engine.a"
      else skeyEngineLib
    }"
    # Distro-specific files: NixOS has no /lib or /etc at build time.
    # systemd.packages in the NixOS module picks the unit up from here.
    "-DSKEY_SYSTEMD_UNIT_DIR=${placeholder "out"}/lib/systemd/system"
    "-DSKEY_POLKIT_RULES_DIR=${placeholder "out"}/etc/polkit-1/rules.d"
    "-DSKEY_UDEV_RULES_DIR=${placeholder "out"}/lib/udev/rules.d"
    "-DSKEY_SYSUSERS_DIR=${placeholder "out"}/lib/sysusers.d"
  ] ++ lib.optionals (devVersion != null) [
    # Dev channel: make the GUI/updater report the dev prerelease version
    # (the "-dev.N" suffix is what the updater parses to compare counters)
    # and mark this as a dev build.  The counter is the last component.
    "-DSKEY_APP_VERSION=${devVersion}"
    "-DSKEY_DEV_BUILD=${toString (lib.toInt (lib.last (lib.splitString "." devVersion)))}"
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
