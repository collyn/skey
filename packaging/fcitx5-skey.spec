# fcitx5-skey.spec — RPM spec for Fedora and openSUSE
#
# Build on Fedora:
#   sudo dnf builddep fcitx5-skey.spec
#   rpmbuild -bb fcitx5-skey.spec
#
# Build on openSUSE:
#   sudo zypper install <BuildRequires>
#   rpmbuild -bb fcitx5-skey.spec

Name:           fcitx5-skey
Version:        0.5.6
Release:        1%{?dist}
Summary:        Vietnamese SKey input method addon for Fcitx5

License:        MIT
URL:            https://github.com/collyn/skey
# Source is the git checkout; CI populates ~/rpmbuild/SOURCES or builds from checkout
Source0:        fcitx5-skey-%{version}.tar.gz

BuildRequires:  cmake
BuildRequires:  extra-cmake-modules
BuildRequires:  fcitx5-devel
BuildRequires:  fcitx5
BuildRequires:  gettext
BuildRequires:  gcc-c++
BuildRequires:  make
BuildRequires:  pkgconfig(dbus-1)
BuildRequires:  pkgconfig(xcb)
# rust + cargo provided by rustup in CI; use distro packages for local builds:
# BuildRequires:  rust
# BuildRequires:  cargo

%if 0%{?fedora} || 0%{?rhel}
BuildRequires:  qt6-qtbase-devel
BuildRequires:  librsvg2-tools
Requires:       qt6-qtsvg
%endif
%if 0%{?suse_version}
BuildRequires:  qt6-base-devel
BuildRequires:  rsvg-convert
Requires:       qt6-svg
%endif

# qt6-qtsvg / qt6-svg is needed at runtime for the settings GUI to load
# SVG icons via QIcon.  It's an explicit Requires (not BuildRequires)
# because the settings app builds fine without Qt6Svg headers — it just
# won't display SVGs at runtime if the plugin is missing.

Requires:       fcitx5
Requires:       hicolor-icon-theme
Requires:       systemd
Requires:       acl

%description
SKey (Simple Key) is a Vietnamese input method engine for Fcitx5,
using skey-engine (Rust) via FFI. It supports Telex, Telex W, and VNI
with surrounding text editing, auto mode switching, per-app mode override,
and spell checking.

%prep
%setup -q -n fcitx5-skey-%{version}

%build
# system rust/cargo from BuildRequires (CI); rustup fallback for local builds
export PATH="$HOME/.cargo/bin:$PATH"
export CARGO_HOME=%{_builddir}/.cargo
mkdir -p "$CARGO_HOME"

# SKEY_UDEV_RULES_DIR is pinned to %{_prefix}/lib/udev/rules.d (merged-usr):
# data/CMakeLists.txt defaults to /lib/udev/rules.d (Debian style), which
# rpmbuild's %files check would reject — it compares literal paths and
# does not resolve the /lib → /usr/lib symlink.
%cmake -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX=%{_prefix} \
    -DSKEY_UDEV_RULES_DIR=%{_prefix}/lib/udev/rules.d
%cmake_build

%install
DESTDIR=%{buildroot} %cmake_install

%check
%ctest

%post
# Icon cache update (always — icons may have changed)
if [ -x /usr/bin/gtk-update-icon-cache ]; then
    /usr/bin/gtk-update-icon-cache %{_datadir}/icons/hicolor >/dev/null 2>&1 || :
    /usr/bin/gtk-update-icon-cache %{_datadir}/icons/breeze >/dev/null 2>&1 || :
    /usr/bin/gtk-update-icon-cache %{_datadir}/icons/breeze-dark >/dev/null 2>&1 || :
fi

# systemd daemon-reload for the new/updated uinput service unit
if [ -x /usr/bin/systemctl ]; then
    /usr/bin/systemctl daemon-reload 2>/dev/null || :
fi

# Create the skey_uinput system user (the uinput server runs as this
# unprivileged user instead of root) and re-apply udev rules so the
# /dev/uinput ACL grants it rw access.
if [ -x /usr/bin/systemd-sysusers ]; then
    /usr/bin/systemd-sysusers %{_prefix}/lib/sysusers.d/skey-uinput.conf 2>/dev/null || :
elif ! getent passwd skey_uinput >/dev/null 2>&1 && [ -x /usr/sbin/useradd ]; then
    /usr/sbin/useradd --system --no-create-home --user-group --shell /usr/sbin/nologin skey_uinput 2>/dev/null || :
fi
if [ -x /usr/bin/udevadm ]; then
    /usr/bin/udevadm control --reload-rules 2>/dev/null || :
    /usr/bin/udevadm trigger --subsystem-match=misc 2>/dev/null || :
fi

# Find the console user (SUDO_USER not available in rpm scripts)
CONSOLE_USER=""
if [ -x /usr/bin/loginctl ]; then
    CONSOLE_USER=$(/usr/bin/loginctl list-sessions --no-legend 2>/dev/null | \
        awk '$4=="seat0" {print $3}' | sort -u | head -1)
fi

if [ "$1" -eq 1 ]; then
    # ── First install: enable the uinput server as root ──
    # Deb postinst and the NixOS module both enable the service during
    # packaging; without this, the first `skey-setup` run hits the
    # enable branch as the user, and `systemctl enable` uses the
    # manage-unit-files polkit action which cannot be scoped per unit —
    # the graphical polkit agent pops a root-password dialog that appears
    # to hang the terminal.  Do it here so the service is already enabled
    # by the time skey-setup runs.  %preun disables each instance on
    # removal, so fresh installs start clean.
    if [ -n "$CONSOLE_USER" ] && [ "$CONSOLE_USER" != "root" ] && \
       [ -x /usr/bin/systemctl ]; then
        # No group membership needed: the server socket sits in a 0700 RuntimeDirectory
        # with a per-user named ACL granted by ExecStartPost (traverse only);
        # authorization happens at accept time
        # (SO_PEERCRED + /proc/pid/exe) — so users work immediately after
        # an upgrade without a re-login.
        /usr/bin/systemctl enable "fcitx5-skey-uinput-server@${CONSOLE_USER}.service" 2>/dev/null || :
        /usr/bin/systemctl start "fcitx5-skey-uinput-server@${CONSOLE_USER}.service" 2>/dev/null || :
    fi
fi

if [ "$1" -eq 1 ]; then
    # ── First install ──
    # Run full skey-setup: configures fcitx5 profile, shell env vars,
    # KWin virtual keyboard, autostart, ibus disable, etc.
    #
    # It must run INSIDE the user's graphical session — su loses
    # XDG_SESSION_TYPE / XDG_CURRENT_DESKTOP, which made the KDE-Wayland
    # blocks (KWin reconnect, session env files) silently skip.  Route
    # through the user's systemd manager instead.
    if [ -n "$CONSOLE_USER" ] && [ "$CONSOLE_USER" != "root" ] && [ -x /usr/bin/skey-setup ]; then
        if [ -x /usr/bin/systemd-run ] && \
           /usr/bin/systemd-run --user --machine="${CONSOLE_USER}@.host" \
               --wait --pipe true 2>/dev/null; then
            # KillMode=none: skey-setup starts fcitx5 -d, which
            # daemonizes inside the transient unit.
            /usr/bin/systemd-run --user --machine="${CONSOLE_USER}@.host" \
                --wait --pipe -p KillMode=none /usr/bin/skey-setup \
                2>/dev/null || :
        else
            su -s /bin/bash "$CONSOLE_USER" -c /usr/bin/skey-setup >/dev/null 2>&1 || :
        fi
    fi
elif [ "$1" -ge 2 ]; then
    # ── Upgrade ──
    # Restart every running uinput server instance so it picks up the new
    # binary.  Instance-agnostic on purpose: CONSOLE_USER only finds a
    # seat0 session, so a headless/multi-seat or SSH-driven upgrade used to
    # skip the restart.  The glob only matches instances systemd has
    # already loaded.
    if [ -x /usr/bin/systemctl ]; then
        /usr/bin/systemctl try-restart "fcitx5-skey-uinput-server@*.service" 2>/dev/null || :
    fi
    # Restart fcitx5 to load the new skey.so and gently reconnect KWin's
    # zwp_input_method_v2.  The helper script must run INSIDE the user's
    # graphical session — `su -` loses XDG_SESSION_TYPE /
    # DBUS_SESSION_BUS_ADDRESS (the reconnect toggle silently never ran),
    # so route through the user's systemd manager.
    if [ -n "$CONSOLE_USER" ] && [ "$CONSOLE_USER" != "root" ]; then
        if [ -x /usr/bin/systemd-run ] && \
           /usr/bin/systemd-run --user --machine="${CONSOLE_USER}@.host" \
               --wait --pipe true 2>/dev/null; then
            # KillMode=none: fcitx5 -r -d daemonizes inside the transient
            # unit — default KillMode would kill it when the unit finishes.
            /usr/bin/systemd-run --user --machine="${CONSOLE_USER}@.host" \
                --wait --pipe -p KillMode=none \
                /usr/bin/skey-restart-fcitx5 2>/dev/null || :
        else
            # Fallback: no user systemd manager — bare restart (apps may
            # need a focus toggle to reconnect).
            su -s /bin/bash "$CONSOLE_USER" -c "fcitx5 -r -d 2>/dev/null" 2>/dev/null || :
        fi
    fi
fi

%preun
if [ "$1" = "0" ]; then
    # Stop all running uinput server instances before removal
    if [ -x /usr/bin/systemctl ]; then
        /usr/bin/systemctl stop "fcitx5-skey-uinput-server@*.service" 2>/dev/null || :
    fi
fi

%postun
if [ "$1" = "0" ]; then
    if [ -x /usr/bin/systemctl ]; then
        # Disable all enabled uinput server instances
        for link in /etc/systemd/system/multi-user.target.wants/fcitx5-skey-uinput-server@*; do
            if [ -L "$link" ]; then
                instance=$(basename "$link")
                /usr/bin/systemctl disable "$instance" 2>/dev/null || :
            fi
        done
        /usr/bin/systemctl daemon-reload 2>/dev/null || :
    fi
    # Refresh icon cache after removal
    if [ -x /usr/bin/gtk-update-icon-cache ]; then
        /usr/bin/gtk-update-icon-cache %{_datadir}/icons/hicolor >/dev/null 2>&1 || :
        /usr/bin/gtk-update-icon-cache %{_datadir}/icons/breeze >/dev/null 2>&1 || :
        /usr/bin/gtk-update-icon-cache %{_datadir}/icons/breeze-dark >/dev/null 2>&1 || :
    fi
    # Remove the dedicated sysuser created by sysusers.d — sysusers only
    # creates, never deletes.  %preun already stopped the service, so the
    # RuntimeDirectory is gone.  Order matters: drop the /dev/uinput ACL
    # entry first (it references the user by name), then evict group
    # members (groupdel refuses a non-empty group), then userdel, then
    # groupdel as a safety net.
    if [ -x /usr/bin/setfacl ] && [ -c /dev/uinput ]; then
        /usr/bin/setfacl -x u:skey_uinput /dev/uinput 2>/dev/null || :
    fi
    if [ -x /usr/bin/gpasswd ] && getent group skey_uinput >/dev/null 2>&1; then
        for member in $(getent group skey_uinput | cut -d: -f4 | tr ',' ' '); do
            [ -n "$member" ] && /usr/bin/gpasswd -d "$member" skey_uinput 2>/dev/null || :
        done
    fi
    if getent passwd skey_uinput >/dev/null 2>&1; then
        /usr/sbin/userdel skey_uinput 2>/dev/null || :
    fi
    if getent group skey_uinput >/dev/null 2>&1; then
        /usr/sbin/groupdel skey_uinput 2>/dev/null || :
    fi
fi

%files
%license LICENSE
%doc README.md
%{_libdir}/fcitx5/skey.so
%{_bindir}/fcitx5-skey-uinput-server
%{_bindir}/fcitx5-skey-settings
%{_bindir}/skey-setup
%{_bindir}/skey-restart-fcitx5
%{_datadir}/fcitx5/addon/skey.conf
%{_datadir}/fcitx5/inputmethod/skey-im.conf
%{_datadir}/icons/hicolor/scalable/apps/fcitx-skey.svg
%{_datadir}/icons/hicolor/scalable/status/fcitx-skey.svg
%{_datadir}/icons/breeze/status/*/fcitx-skey.svg
%{_datadir}/icons/breeze/apps/*/fcitx-skey.svg
%{_datadir}/icons/breeze-dark/status/*/fcitx-skey.svg
%{_datadir}/icons/breeze-dark/apps/*/fcitx-skey.svg
%{_datadir}/pixmaps/fcitx-skey.*
# Preset icons (v-blue, v-dark, v-light, v-red, vn-flag) — SVG only,
# loaded at runtime by icon_resolver via absolute path.
%{_datadir}/icons/hicolor/scalable/apps/fcitx-skey-v-*.svg
%{_datadir}/icons/hicolor/scalable/status/fcitx-skey-v-*.svg
%{_datadir}/icons/hicolor/scalable/apps/fcitx-skey-vn-flag.svg
%{_datadir}/icons/hicolor/scalable/status/fcitx-skey-vn-flag.svg
%{_datadir}/icons/breeze/status/*/fcitx-skey-v-*.svg
%{_datadir}/icons/breeze/apps/*/fcitx-skey-v-*.svg
%{_datadir}/icons/breeze-dark/status/*/fcitx-skey-v-*.svg
%{_datadir}/icons/breeze-dark/apps/*/fcitx-skey-v-*.svg
%{_datadir}/icons/breeze/status/*/fcitx-skey-vn-flag.svg
%{_datadir}/icons/breeze/apps/*/fcitx-skey-vn-flag.svg
%{_datadir}/icons/breeze-dark/status/*/fcitx-skey-vn-flag.svg
%{_datadir}/icons/breeze-dark/apps/*/fcitx-skey-vn-flag.svg
%{_datadir}/pixmaps/fcitx-skey-v-*.svg
%{_datadir}/pixmaps/fcitx-skey-vn-flag.svg
%{_datadir}/applications/fcitx5-skey-settings.desktop
/lib/systemd/system/fcitx5-skey-uinput-server@.service
%config(noreplace) /etc/polkit-1/rules.d/60-fcitx5-skey-uinput.rules
%{_prefix}/lib/udev/rules.d/99-skey-uinput.rules
%{_prefix}/lib/sysusers.d/skey-uinput.conf
%config %{_sysconfdir}/profile.d/fcitx5-skey.sh

%changelog
* Sat Aug 01 2026 Huy <collyn094@gmail.com> - 0.4.4-1
- Initial RPM packaging for Fedora and openSUSE
