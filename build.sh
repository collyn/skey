#!/bin/bash
set -e

# Define project variables
PKG_NAME="fcitx5-skey"
# Respect an externally supplied version (CI dev builds); otherwise derive.
if [ -z "${PKG_VERSION:-}" ]; then
    PKG_VERSION=$(grep -oP 'project\(fcitx5-skey VERSION \K[0-9.]+' CMakeLists.txt)
    if [ -z "$PKG_VERSION" ]; then
        PKG_VERSION="0.1.0"
    fi
fi
PKG_ARCH=$(dpkg --print-architecture 2>/dev/null || echo "amd64")
PKG_DIR="${PKG_NAME}_${PKG_VERSION}_${PKG_ARCH}"

echo "=== Building $PKG_NAME v$PKG_VERSION for $PKG_ARCH ==="

# Check prerequisites
if ! command -v cmake &> /dev/null; then
    echo "Error: cmake is not installed. Please install it first." >&2
    exit 1
fi
if ! command -v cargo &> /dev/null; then
    echo "Error: cargo (Rust toolchain) is not installed. Please install it first." >&2
    exit 1
fi
if ! command -v dpkg-deb &> /dev/null; then
    echo "Error: dpkg-deb is not installed. This script is intended to run on Debian-based systems." >&2
    exit 1
fi

# Clean up previous temporary packaging folders and deb files
rm -rf "$PKG_DIR"
rm -f "${PKG_DIR}.deb"

# Configure and compile using CMake
echo "--> Configuring project with CMake..."
cmake -B build-deb -S . \
    -DCMAKE_BUILD_TYPE=Release \
    -DBUILD_TESTING=OFF \
    -DCMAKE_INSTALL_PREFIX=/usr \
    -DSKEY_APP_VERSION="${PKG_VERSION}" \
    -DSKEY_DEV_BUILD="${SKEY_DEV_BUILD:-0}"

echo "--> Compiling..."
cmake --build build-deb --config Release -j$(nproc)

# Install to temporary packaging directory
echo "--> Staging files for Debian package..."
DESTDIR="$(pwd)/$PKG_DIR" cmake --install build-deb

# Create DEBIAN folder and control file
# NOTE: dpkg control files don't allow '#' comments inside the heredoc —
# libqt6svg6 = Qt6 SVG image plugin: the settings GUI loads SVG icons via
# QIcon; without it the Icons tab (and system tray icon) render blank.
echo "--> Generating DEBIAN/control..."
mkdir -p "$PKG_DIR/DEBIAN"
cat <<EOF > "$PKG_DIR/DEBIAN/control"
Package: $PKG_NAME
Version: $PKG_VERSION
Section: utils
Priority: optional
Architecture: $PKG_ARCH
Depends: fcitx5, systemd, udev, kmod, acl, hicolor-icon-theme, libqt6widgets6, libqt6svg6, libxcb1, libx11-6, libxtst6, libei1, libportal1
Maintainer: Huy
Description: Vietnamese SKey input method addon for Fcitx5
 This package provides the skey input method engine for fcitx5,
 supporting Telex, Telex W, and VNI with advanced surrounding text editing capabilities.
EOF

# Share the Debian maintainer hook so release packages also restart on upgrade.
install -m 755 debian/postinst "$PKG_DIR/DEBIAN/postinst"

# Keep generated packages identical to the maintained Debian hooks.
install -m 755 debian/prerm "$PKG_DIR/DEBIAN/prerm"
install -m 755 debian/postrm "$PKG_DIR/DEBIAN/postrm"

# Build the .deb package
echo "--> Packaging as .deb..."
dpkg-deb --root-owner-group --build "$PKG_DIR"

# Clean up
echo "--> Cleaning up staging directory and build-deb..."
rm -rf "$PKG_DIR"
rm -rf build-deb

echo "=== Success! Generated ${PKG_DIR}.deb ==="
