#!/bin/bash
set -e

# Define project variables
PKG_NAME="fcitx5-skey"
PKG_VERSION="0.1.1"
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
    -DCMAKE_INSTALL_PREFIX=/usr

echo "--> Compiling..."
cmake --build build-deb --config Release -j$(nproc)

# Install to temporary packaging directory
echo "--> Staging files for Debian package..."
DESTDIR="$(pwd)/$PKG_DIR" cmake --install build-deb

# Create DEBIAN folder and control file
echo "--> Generating DEBIAN/control..."
mkdir -p "$PKG_DIR/DEBIAN"
cat <<EOF > "$PKG_DIR/DEBIAN/control"
Package: $PKG_NAME
Version: $PKG_VERSION
Section: utils
Priority: optional
Architecture: $PKG_ARCH
Depends: fcitx5, systemd
Maintainer: Huy
Description: Vietnamese SKey input method addon for Fcitx5
 This package provides the skey input method engine for fcitx5,
 supporting Telex, Telex W, and VNI with advanced surrounding text editing capabilities.
EOF

# Create postinst script to run skey-setup after installation
cat <<'POSTINST' > "$PKG_DIR/DEBIAN/postinst"
#!/bin/bash
# Run skey-setup and start the optional uinput server for the user who invoked sudo.
if [ -n "$SUDO_USER" ]; then
    su - "$SUDO_USER" -c "skey-setup" 2>/dev/null || true
    if command -v systemctl >/dev/null 2>&1; then
        systemctl daemon-reload 2>/dev/null || true
        systemctl enable --now "fcitx5-skey-uinput-server@${SUDO_USER}.service" 2>/dev/null || true
    fi
elif command -v systemctl >/dev/null 2>&1; then
    systemctl daemon-reload 2>/dev/null || true
fi
POSTINST
chmod 755 "$PKG_DIR/DEBIAN/postinst"

# Build the .deb package
echo "--> Packaging as .deb..."
dpkg-deb --build "$PKG_DIR"

# Clean up
echo "--> Cleaning up staging directory and build-deb..."
rm -rf "$PKG_DIR"
rm -rf build-deb

echo "=== Success! Generated ${PKG_DIR}.deb ==="
