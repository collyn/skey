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

License:        GPL-3.0-or-later
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
BuildRequires:  pkgconfig(x11)
BuildRequires:  pkgconfig(xtst)
BuildRequires:  pkgconfig(libei-1.0) >= 1.0
BuildRequires:  pkgconfig(libportal) >= 0.7.1
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
Requires:       /usr/bin/udevadm
Requires:       kmod
Requires:       acl
# RPM generates shared-library requirements automatically, including the
# correct providers on openSUSE (libei1 / libportal1).

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

%{_bindir}/skey-uinput-access configure || exit $?

# Find the console user (SUDO_USER not available in rpm scripts)
CONSOLE_USER=""
if [ -x /usr/bin/loginctl ]; then
    CONSOLE_USER=$(/usr/bin/loginctl list-sessions --no-legend 2>/dev/null | \
        awk '$4=="seat0" {print $3}' | sort -u | head -1)
fi

# User configuration is performed manually with skey-setup.
if [ "$1" -ge 2 ]; then
    # ── Upgrade ──
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
    %{_bindir}/skey-uinput-access remove || exit $?

    # ── Remove per-user config on removal (not upgrade) ──
    # skey-setup -u strips the env-var blocks from shell rc files, removes
    # skey-im from the fcitx5 profile and cleans the live session.  It
    # must run as the USER (HOME + session D-Bus resolve correctly) and
    # while /usr/bin/skey-setup still exists — %preun is the right hook.
    CONSOLE_USER=""
    if [ -x /usr/bin/loginctl ]; then
        CONSOLE_USER=$(/usr/bin/loginctl list-sessions --no-legend 2>/dev/null | \
            awk '$4=="seat0" {print $3}' | sort -u | head -1)
    fi
    if [ -n "$CONSOLE_USER" ] && [ "$CONSOLE_USER" != "root" ] && \
       [ -x /usr/bin/skey-setup ]; then
        if [ -x /usr/bin/systemd-run ] && \
           /usr/bin/systemd-run --user --machine="${CONSOLE_USER}@.host" \
               --wait --pipe true 2>/dev/null; then
            # KillMode=none: skey-setup -u restarts fcitx5 -d, which
            # daemonizes inside the transient unit.
            /usr/bin/systemd-run --user --machine="${CONSOLE_USER}@.host" \
                --wait --pipe -p KillMode=none /usr/bin/skey-setup -u \
                2>/dev/null || :
        else
            su -s /bin/bash "$CONSOLE_USER" -c "/usr/bin/skey-setup -u" \
                >/dev/null 2>&1 || :
        fi
    fi
fi

%postun
if [ "$1" = "0" ]; then
    # Refresh icon cache after removal
    if [ -x /usr/bin/gtk-update-icon-cache ]; then
        /usr/bin/gtk-update-icon-cache %{_datadir}/icons/hicolor >/dev/null 2>&1 || :
        /usr/bin/gtk-update-icon-cache %{_datadir}/icons/breeze >/dev/null 2>&1 || :
        /usr/bin/gtk-update-icon-cache %{_datadir}/icons/breeze-dark >/dev/null 2>&1 || :
    fi
    # Keep the retired account; never recycle its uid during removal.
    if [ -x /usr/bin/udevadm ]; then
        /usr/bin/udevadm control --reload-rules 2>/dev/null || :
        /usr/bin/udevadm trigger --action=change --subsystem-match=misc --sysname-match=uinput 2>/dev/null || :
    fi
fi

%files
%license LICENSE
%doc README.md
%{_libdir}/fcitx5/skey.so
%{_bindir}/skey-uinput-access
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
# 128px PNG fallbacks (rsvg-convert at build time) — renderable without
# the Qt SVG image plugin.
%{_datadir}/icons/hicolor/128x128/apps/fcitx-skey*.png
%{_datadir}/pixmaps/fcitx-skey.*
%{_datadir}/pixmaps/fcitx-skey*.png
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
%{_datadir}/locale/*/LC_MESSAGES/fcitx5-skey.mo
%{_prefix}/lib/udev/rules.d/70-skey-uinput.rules
%{_prefix}/lib/modules-load.d/skey-uinput.conf

%changelog
* Sat Aug 01 2026 Huy <collyn094@gmail.com> - 0.4.4-1
- Initial RPM packaging for Fedora and openSUSE
