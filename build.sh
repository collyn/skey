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
Depends: fcitx5, systemd, hicolor-icon-theme, libqt6widgets6, libqt6svg6, libxcb1, libx11-6, libxtst6, libei1, libportal1
Maintainer: Huy
Description: Vietnamese SKey input method addon for Fcitx5
 This package provides the skey input method engine for fcitx5,
 supporting Telex, Telex W, and VNI with advanced surrounding text editing capabilities.
EOF

# Share the Debian maintainer hook so release packages also restart on upgrade.
install -m 755 debian/postinst "$PKG_DIR/DEBIAN/postinst"

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

# ── Remove per-user config on removal (not upgrade/deconfigure) ──
# Mirror of debian/prerm: skey-setup -u strips the env-var blocks from
# shell rc files, removes skey-im from the fcitx5 profile and cleans the
# live session.  It must run as the USER (HOME + session D-Bus resolve
# correctly) and while /usr/bin/skey-setup still exists — prerm is the
# right hook.  Purge also runs prerm with $1="remove".
if [ "$1" = "remove" ] && [ -n "${SUDO_USER:-}" ] && [ "$SUDO_USER" != "root" ] \
   && command -v skey-setup >/dev/null 2>&1; then
    if command -v systemd-run >/dev/null 2>&1 && \
       systemd-run --user --machine="${SUDO_USER}@.host" \
           --wait --pipe true 2>/dev/null; then
        # KillMode=none: skey-setup -u restarts fcitx5 -d, which
        # daemonizes inside the transient unit.
        systemd-run --user --machine="${SUDO_USER}@.host" \
            --wait --pipe -p KillMode=none skey-setup -u 2>/dev/null || true
    else
        su - "$SUDO_USER" -c "skey-setup -u" 2>/dev/null || true
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
if [ "$1" = "purge" ]; then
    # Remove the dedicated sysuser (sysusers only creates, never deletes).
    # Order: drop the /dev/uinput ACL entry first (references the name),
    # evict group members, then userdel + groupdel safety net.
    if command -v setfacl >/dev/null 2>&1 && [ -c /dev/uinput ]; then
        setfacl -x u:skey_uinput /dev/uinput 2>/dev/null || true
    fi
    if command -v gpasswd >/dev/null 2>&1 && getent group skey_uinput >/dev/null 2>&1; then
        for member in $(getent group skey_uinput | cut -d: -f4 | tr ',' ' '); do
            [ -n "$member" ] && gpasswd -d "$member" skey_uinput 2>/dev/null || true
        done
    fi
    if getent passwd skey_uinput >/dev/null 2>&1; then
        userdel skey_uinput 2>/dev/null || true
    fi
    if getent group skey_uinput >/dev/null 2>&1; then
        groupdel skey_uinput 2>/dev/null || true
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
