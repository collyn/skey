#!/bin/bash
set -e

# Define project variables
PKG_NAME="fcitx5-skey"
PKG_VERSION=$(grep -oP 'project\(fcitx5-skey VERSION \K[0-9.]+' CMakeLists.txt)
if [ -z "$PKG_VERSION" ]; then
    PKG_VERSION="0.1.0"
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
Depends: fcitx5, systemd, hicolor-icon-theme, libqt6widgets6
Maintainer: Huy
Description: Vietnamese SKey input method addon for Fcitx5
 This package provides the skey input method engine for fcitx5,
 supporting Telex, Telex W, and VNI with advanced surrounding text editing capabilities.
EOF

# Create postinst script to run skey-setup after installation
cat <<'POSTINST' > "$PKG_DIR/DEBIAN/postinst"
#!/bin/bash
# Update icon cache so the skey icon appears in taskbar after installation
if command -v gtk-update-icon-cache >/dev/null 2>&1; then
    gtk-update-icon-cache /usr/share/icons/hicolor 2>/dev/null || true
    # Also update breeze caches (icons installed to breeze/status/)
    gtk-update-icon-cache /usr/share/icons/breeze 2>/dev/null || true
    gtk-update-icon-cache /usr/share/icons/breeze-dark 2>/dev/null || true
fi

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

# Create prerm script to stop and disable systemd service instances on remove
cat <<'PRERM' > "$PKG_DIR/DEBIAN/prerm"
#!/bin/bash
set -e
if [ "$1" = "remove" ] || [ "$1" = "deconfigure" ]; then
    if command -v systemctl >/dev/null 2>&1; then
        # Stop all running instances of the service
        systemctl stop "fcitx5-skey-uinput-server@*.service" 2>/dev/null || true
        
        # Disable all enabled instances
        for link in /etc/systemd/system/multi-user.target.wants/fcitx5-skey-uinput-server@*; do
            if [ -L "$link" ]; then
                instance=$(basename "$link")
                systemctl disable "$instance" 2>/dev/null || true
            fi
        done
    fi
fi
PRERM
chmod 755 "$PKG_DIR/DEBIAN/prerm"

# Create postrm script to daemon-reload systemd after package removal
cat <<'POSTRM' > "$PKG_DIR/DEBIAN/postrm"
#!/bin/bash
set -e
if [ "$1" = "remove" ] || [ "$1" = "purge" ]; then
    if command -v gtk-update-icon-cache >/dev/null 2>&1; then
        gtk-update-icon-cache /usr/share/icons/hicolor 2>/dev/null || true
        gtk-update-icon-cache /usr/share/icons/breeze 2>/dev/null || true
        gtk-update-icon-cache /usr/share/icons/breeze-dark 2>/dev/null || true
    fi
    if command -v systemctl >/dev/null 2>&1; then
        systemctl daemon-reload 2>/dev/null || true
    fi
fi
POSTRM
chmod 755 "$PKG_DIR/DEBIAN/postrm"

# Build the .deb package
echo "--> Packaging as .deb..."
dpkg-deb --build "$PKG_DIR"

# Clean up
echo "--> Cleaning up staging directory and build-deb..."
rm -rf "$PKG_DIR"
rm -rf build-deb

echo "=== Success! Generated ${PKG_DIR}.deb ==="
