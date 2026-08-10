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

%cmake -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX=%{_prefix}
%cmake_build

%install
DESTDIR=%{buildroot} %cmake_install

%check
%ctest

%post
# Icon cache update
if [ -x /usr/bin/gtk-update-icon-cache ]; then
    /usr/bin/gtk-update-icon-cache %{_datadir}/icons/hicolor >/dev/null 2>&1 || :
    /usr/bin/gtk-update-icon-cache %{_datadir}/icons/breeze >/dev/null 2>&1 || :
    /usr/bin/gtk-update-icon-cache %{_datadir}/icons/breeze-dark >/dev/null 2>&1 || :
fi

# systemd daemon-reload for the new uinput service unit
if [ -x /usr/bin/systemctl ]; then
    /usr/bin/systemctl daemon-reload 2>/dev/null || :
fi

# Run skey-setup for the console user (SUDO_USER not available in rpm scripts)
if [ -x /usr/bin/loginctl ] && [ -x /usr/bin/skey-setup ]; then
    CONSOLE_USER=$(/usr/bin/loginctl list-sessions --no-legend 2>/dev/null | \
        awk '$4=="seat0" {print $3}' | sort -u | head -1)
    if [ -n "$CONSOLE_USER" ] && [ "$CONSOLE_USER" != "root" ]; then
        su -s /bin/bash "$CONSOLE_USER" -c /usr/bin/skey-setup >/dev/null 2>&1 || :
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
fi

%files
%license LICENSE
%doc README.md
%{_libdir}/fcitx5/skey.so
%{_bindir}/fcitx5-skey-uinput-server
%{_bindir}/fcitx5-skey-settings
%{_bindir}/skey-setup
%{_datadir}/fcitx5/addon/skey.conf
%{_datadir}/fcitx5/inputmethod/skey-im.conf
%{_datadir}/icons/hicolor/scalable/apps/fcitx-skey.svg
%{_datadir}/icons/hicolor/scalable/status/fcitx-skey.svg
%{_datadir}/icons/hicolor/*/apps/fcitx-skey.png
%{_datadir}/icons/hicolor/*/status/fcitx-skey.png
%{_datadir}/icons/breeze/status/*/fcitx-skey.svg
%{_datadir}/icons/breeze/apps/*/fcitx-skey.svg
%{_datadir}/icons/breeze-dark/status/*/fcitx-skey.svg
%{_datadir}/icons/breeze-dark/apps/*/fcitx-skey.svg
%{_datadir}/pixmaps/fcitx-skey.*
# Preset icons (v-blue, v-dark)
%{_datadir}/icons/hicolor/scalable/apps/fcitx-skey-v-blue.svg
%{_datadir}/icons/hicolor/scalable/status/fcitx-skey-v-blue.svg
%{_datadir}/icons/hicolor/*/apps/fcitx-skey-v-blue.png
%{_datadir}/icons/hicolor/*/status/fcitx-skey-v-blue.png
%{_datadir}/icons/breeze/status/*/fcitx-skey-v-blue.svg
%{_datadir}/icons/breeze/apps/*/fcitx-skey-v-blue.svg
%{_datadir}/icons/breeze-dark/status/*/fcitx-skey-v-blue.svg
%{_datadir}/icons/breeze-dark/apps/*/fcitx-skey-v-blue.svg
%{_datadir}/pixmaps/fcitx-skey-v-blue.*
%{_datadir}/icons/hicolor/scalable/apps/fcitx-skey-v-dark.svg
%{_datadir}/icons/hicolor/scalable/status/fcitx-skey-v-dark.svg
%{_datadir}/icons/hicolor/*/apps/fcitx-skey-v-dark.png
%{_datadir}/icons/hicolor/*/status/fcitx-skey-v-dark.png
%{_datadir}/icons/breeze/status/*/fcitx-skey-v-dark.svg
%{_datadir}/icons/breeze/apps/*/fcitx-skey-v-dark.svg
%{_datadir}/icons/breeze-dark/status/*/fcitx-skey-v-dark.svg
%{_datadir}/icons/breeze-dark/apps/*/fcitx-skey-v-dark.svg
%{_datadir}/pixmaps/fcitx-skey-v-dark.*
%{_datadir}/applications/fcitx5-skey-settings.desktop
/lib/systemd/system/fcitx5-skey-uinput-server@.service
%config %{_sysconfdir}/profile.d/fcitx5-skey.sh

%changelog
* Sat Aug 01 2026 Huy <collyn094@gmail.com> - 0.4.4-1
- Initial RPM packaging for Fedora and openSUSE
