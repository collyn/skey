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
  extra-cmake-modules,
  gettext,
  librsvg, # rsvg-convert for PNG tray icons
  dbus,
  xorg, # libxcb (X11 WM_CLASS detection)
  fcitx5,
  qt6, # settings GUI (qtbase/qtwayland/qtsvg)
  wrapQtAppsHook,
  rustPlatform,
  # Source of https://github.com/collyn/skey-engine.  Overridable so a
  # caller can pass its own fetchFromGitHub result (pinned with a hash).
  skeyEngineSrc ? builtins.fetchTarball {
    url = "https://github.com/collyn/skey-engine/archive/refs/tags/v0.1.20.tar.gz";
  },
}:

let
  skeyEngine = rustPlatform.buildRustPackage {
    pname = "skey-engine";
    version = "0.1.20";
    src = skeyEngineSrc;
    # Cargo.lock carries the per-crate checksums, so no cargoHash is needed.
    cargoLock.lockFile = "${skeyEngineSrc}/Cargo.lock";
    doCheck = false;
    installPhase = ''
      runHook preInstall
      install -Dm644 target/release/libskey_engine.a "$out/lib/libskey_engine.a"
      install -Dm644 skey_engine.h "$out/include/skey_engine.h"
      runHook postInstall
    '';
  };
in
stdenv.mkDerivation (finalAttrs: {
  pname = "fcitx5-skey";
  version = "0.6.4";

  src = ../..;

  nativeBuildInputs = [
    cmake
    pkg-config
    extra-cmake-modules
    gettext
    librsvg
    wrapQtAppsHook
  ];

  buildInputs = [
    fcitx5
    dbus
    xorg.libxcb
    qt6.qtbase
    qt6.qtwayland # runtime platform plugin for the settings GUI on Wayland
    qt6.qtsvg # QIcon SVG loading at runtime
    acl # setfacl in the uinput server unit's ExecStartPre
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
    license = licenses.mit;
    maintainers = [ ];
    platforms = platforms.linux;
  };
})
