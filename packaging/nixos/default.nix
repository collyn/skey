{
  lib,
  stdenv,
  cmake,
  pkg-config,
  kdePackages,
  gettext,
  librsvg,
  dbus,
  libxcb,
  libX11,
  libXtst,
  libei,
  libportal,
  fcitx5,
  qt6,
  rustPlatform,
  fetchFromGitHub,
  writeShellApplication,
  nix,
  git,
  coreutils,
  gnused,
  cmakeLists ? builtins.replaceStrings [ "\n" ] [ " " ] (builtins.readFile ../../CMakeLists.txt),
  engineVersion ? let
    m = builtins.match ".*set\\(SKEY_ENGINE_VERSION \"([^\"]+)\"\\).*" cmakeLists;
  in if m == null
     then throw "fcitx5-skey: cannot parse SKEY_ENGINE_VERSION from ../../CMakeLists.txt"
     else builtins.head m,
  skeyEngineSrc ? fetchFromGitHub {
    owner = "collyn";
    repo = "skey-engine";
    tag = engineVersion;
    hash = "sha256-L1RpMZCjeP3KhuO4Xjuq0jwKDCxCpoNcmQUu2LTg0JY=";
  },
  skeyEngineLib ? builtins.fetchurl {
    url = "https://github.com/collyn/skey-engine/releases/download/${engineVersion}/libskey_engine.a";
    sha256 = "sha256-YRQ8vbqfStvns1aX2hYrV8ZBkZoAZGTxbblxA+gI2SE=";
  },
  devVersion ? null,
}:

let
  stableVersion = let
    m = builtins.match ".*project\\(fcitx5-skey VERSION ([0-9.]+)\\).*" cmakeLists;
  in if m == null
     then throw "fcitx5-skey: cannot parse version from ../../CMakeLists.txt"
     else builtins.head m;

  skeyEngine = rustPlatform.buildRustPackage {
    pname = "skey-engine";
    version = lib.removePrefix "v" engineVersion;
    src = skeyEngineSrc;
    cargoLock.lockFile = "${skeyEngineSrc}/Cargo.lock";
    doCheck = false;
    installPhase = ''
      runHook preInstall
      install -Dm644 target/${stdenv.hostPlatform.rust.rustcTarget}/release/libskey_engine.a \
        "$out/lib/libskey_engine.a"
      install -Dm644 skey_engine.h "$out/include/skey_engine.h"
      runHook postInstall
    '';
    meta = with lib; {
      license = licenses.gpl3Plus;
      platforms = platforms.linux;
    };
  };
in
stdenv.mkDerivation (finalAttrs: {
  pname = "fcitx5-skey";
  version = if devVersion == null then stableVersion else devVersion;

  src = ../..;

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
    libX11
    libXtst
    libei
    libportal
    qt6.qtbase
    qt6.qtwayland
    qt6.qtsvg
  ];

  cmakeFlags = [
    "-DSKEY_ENGINE_SOURCE_DIR=${skeyEngineSrc}"
    "-DSKEY_ENGINE_PREBUILT_LIB=${
      if skeyEngineLib == null
      then "${skeyEngine}/lib/libskey_engine.a"
      else skeyEngineLib
    }"
    "-DSKEY_UDEV_RULES_DIR=${placeholder "out"}/lib/udev/rules.d"
    "-DSKEY_MODULES_LOAD_DIR=${placeholder "out"}/lib/modules-load.d"
  ] ++ lib.optionals (devVersion != null) [
    "-DSKEY_APP_VERSION=${devVersion}"
    "-DSKEY_DEV_BUILD=${toString (lib.toInt (lib.last (lib.splitString "." devVersion)))}"
  ];

  meta = with lib; {
    description = "SKey Vietnamese input method engine addon for Fcitx5";
    homepage = "https://github.com/collyn/skey";
    license = licenses.gpl3Plus;
    maintainers = [ ];
    platforms = platforms.linux;
  };

  passthru.updateScript = writeShellApplication {
    name = "fcitx5-skey-update-engine-hashes";
    runtimeInputs = [ nix git coreutils gnused ];
    text = ''
      set -euo pipefail

      dflt="''${1:-$(git rev-parse --show-toplevel)/packaging/nixos/default.nix}"
      cmake="$(dirname "$(dirname "$(dirname "$dflt")")")/CMakeLists.txt"
      version=$(sed -n 's/.*set(SKEY_ENGINE_VERSION "\([^"]*\)".*/\1/p' "$cmake")
      if [ -z "$version" ]; then
        echo "cannot parse SKEY_ENGINE_VERSION from $cmake" >&2
        exit 1
      fi
      echo "engine version: $version"

      tarball_hash=$(nix --extra-experimental-features 'nix-command flakes' \
        store prefetch-file --unpack \
        "https://github.com/collyn/skey-engine/archive/refs/tags/$version.tar.gz" \
        2>&1 | sed -n "s/.*hash '\(sha256-[^']*\)'.*/\1/p" | head -n1)
      asset_hash=$(nix --extra-experimental-features 'nix-command flakes' \
        store prefetch-file \
        "https://github.com/collyn/skey-engine/releases/download/$version/libskey_engine.a" \
        2>&1 | sed -n "s/.*hash '\(sha256-[^']*\)'.*/\1/p" | head -n1)

      if [ -w "$dflt" ]; then
        sed -i -E "s|(hash = )\"sha256-[^\"]*\"|\1\"$tarball_hash\"|" "$dflt"
        sed -i -E "s|(sha256 = )\"sha256-[^\"]*\"|\1\"$asset_hash\"|" "$dflt"
        echo "updated $dflt"
      else
        echo "$dflt not writable; paste into default.nix:" >&2
        printf '  skeyEngineSrc: hash = "%s"\n' "$tarball_hash"
        printf '  skeyEngineLib: sha256 = "%s"\n' "$asset_hash"
      fi
    '';
  };
})
