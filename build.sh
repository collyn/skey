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
echo "--> Generating DEBIAN/control..."
mkdir -p "$PKG_DIR/DEBIAN"
cat <<EOF > "$PKG_DIR/DEBIAN/control"
Package: $PKG_NAME
Version: $PKG_VERSION
Section: utils
Priority: optional
Architecture: $PKG_ARCH
# libqt6svg6 = Qt6 SVG image plugin: the settings GUI loads SVG icons via
# QIcon; without it the Icons tab (and system tray icon) render blank.
Depends: fcitx5, systemd, hicolor-icon-theme, libqt6widgets6, libqt6svg6, libxcb1
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

# Mirror of debian/postinst's system-integration steps (this script stages
# its own maintainer scripts instead of using debian/): daemon-reload picks
# up the new unit, sysusers creates the unprivileged skey_uinput user the
# server runs as, udev reload applies the /dev/uinput ACL rule, and the
# service is enabled+started as ROOT before skey-setup runs — so the user
# never hits the unscopable manage-unit-files polkit prompt.
if command -v systemctl >/dev/null 2>&1; then
    systemctl daemon-reload 2>/dev/null || true
fi
if command -v systemd-sysusers >/dev/null 2>&1; then
    systemd-sysusers /usr/lib/sysusers.d/skey-uinput.conf 2>/dev/null || true
elif ! getent passwd skey_uinput >/dev/null 2>&1 && command -v useradd >/dev/null 2>&1; then
    useradd --system --no-create-home --user-group --shell /usr/sbin/nologin skey_uinput 2>/dev/null || true
fi
if command -v udevadm >/dev/null 2>&1; then
    udevadm control --reload-rules 2>/dev/null || true
    udevadm trigger --subsystem-match=misc 2>/dev/null || true
fi
if [ -n "$SUDO_USER" ] && [ "$SUDO_USER" != "root" ]; then
    systemctl enable --now "fcitx5-skey-uinput-server@${SUDO_USER}.service" 2>/dev/null || true
    # im-config deliberately NOT run here: it must run as the USER (it
    # writes ~/.xinputrc, not a system file) — skey-setup does it inside
    # the user's session below.  Running it as root writes the system-wide
    # default AND spawns a root-owned fcitx5 that steals the session
    # D-Bus name.  Keep this pkill as migration cleanup for machines that
    # already have such a zombie from older installs.
    pkill -u root -x fcitx5 2>/dev/null || true
    # Run skey-setup INSIDE the user's session via systemd-run when
    # possible: it carries the full session environment (DISPLAY,
    # XAUTHORITY, DBUS_SESSION_BUS_ADDRESS) which plain `su -` resets —
    # without the session bus fcitx5 starts but no app can reach it, and
    # fcitx5-remote inside skey-setup can't verify readiness.
    if command -v systemd-run >/dev/null 2>&1 && \
       systemd-run --user --machine="${SUDO_USER}@.host" \
           --wait --pipe true 2>/dev/null; then
        # KillMode=none: skey-setup starts fcitx5 -d, which daemonizes
        # inside the transient unit.
        systemd-run --user --machine="${SUDO_USER}@.host" \
            --wait --pipe -p KillMode=none skey-setup 2>/dev/null || true
    else
        # Fallback: no user systemd manager — bare su with X11 passthrough.
        su - "$SUDO_USER" -c "DISPLAY='${DISPLAY:-:0}' XAUTHORITY='${XAUTHORITY:-}' skey-setup" 2>/dev/null || true
    fi
else
    # Installed from a root session (no SUDO_USER): restart any running
    # instance so the new binary/unit takes effect (the self-healing
    # ExecStartPre recreates skey_uinput if needed).
    systemctl try-restart "fcitx5-skey-uinput-server@*.service" 2>/dev/null || true
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
