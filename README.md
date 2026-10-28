<div align="center">

<img src="data/icons/fcitx-skey.svg" alt="SKey" width="96" height="96">

# SKey — Simple Key

**Bộ gõ tiếng Việt cho Linux, tích hợp Fcitx5.**

[![Release](https://img.shields.io/github/v/release/collyn/skey?label=release&sort=semver)](https://github.com/collyn/skey/releases)
[![License](https://img.shields.io/github/license/collyn/skey?label=license)](LICENSE)

</div>

SKey dùng [skey-engine](https://github.com/collyn/skey-engine) (Rust) để xử lý tiếng Việt, addon C++ tích hợp với Fcitx5 và giao diện cấu hình Qt6. Hỗ trợ X11 và Wayland; khả năng gõ phụ thuộc vào ứng dụng, frontend Fcitx5 và compositor.

## Tính năng

- **Telex, VNI**, tùy chọn `w → ư`, `][ → ươ` và đánh dấu tự do.
- **Tự động khôi phục** từ không hợp lệ trong tiếng Việt, từ điển và gõ tắt (macro).
- **14 bảng mã**, gồm Unicode, Unicode NFD, TCVN3, VNI Windows và VIQR.
- **Cấu hình theo ứng dụng:** chọn chế độ xuất, tắt tiếng Việt, chỉnh delay; có cấu hình riêng cho thanh địa chỉ Chromium.
- **Giao diện cài đặt:** quản lý kiểu gõ, chế độ xuất, từ điển, macro, biểu tượng và cập nhật Stable/Dev.

| Chế độ xuất | Cách hoạt động |
| --- | --- |
| **Auto** (mặc định) | Chọn theo khả năng ứng dụng và ngữ cảnh ô nhập; có xử lý riêng cho trình duyệt, terminal và ứng dụng văn phòng. |
| **Surrounding Text** | Thay thế văn bản qua API surrounding text của Fcitx5. |
| **Uinput** | Gửi Backspace qua `/dev/uinput` ngay trong fcitx5, dùng quyền user của phiên desktop. |
| **Native** | Gửi Backspace qua XTest trên X11 hoặc libei/portal trên Wayland. Đây là chế độ tùy chọn và mặc định tắt; backend Wayland cần thư viện và compositor hỗ trợ, cùng quyền từ portal. |
| **Preedit** | Soạn văn bản trong vùng preedit trước khi commit vào ứng dụng. |

Native mặc định tắt để tránh hộp thoại xin quyền input trên Wayland. Trong `fcitx5-skey-settings`, bật **Bật chế độ Native** trước; sau đó có thể bật **Dùng Native thay Uinput** để dùng Native trong các trường hợp Auto chọn Uinput. Khi Native tắt, mode Native không xuất hiện trong menu mode và SKey không khởi tạo portal libei.

## Cài đặt

### Ubuntu / Debian

```bash
curl -fsSL https://collyn.github.io/skey/install.sh | sudo bash
sudo apt install fcitx5-skey
skey-setup
```

Hoặc tải gói `.deb` từ [Releases](https://github.com/collyn/skey/releases), cài bằng `sudo apt install ./fcitx5-skey_*.deb`, rồi chạy `skey-setup`.

### Fedora / openSUSE / Arch Linux

Mỗi lệnh dưới đây thêm repository, cài gói và chạy `skey-setup` bằng tài khoản đang dùng desktop:

```bash
# Fedora
curl -fsSL https://collyn.github.io/skey/install-fedora.sh | sudo bash && sudo dnf install fcitx5-skey && skey-setup

# openSUSE
curl -fsSL https://collyn.github.io/skey/install-opensuse.sh | sudo bash && sudo zypper install fcitx5-skey && skey-setup

# Arch Linux
curl -fsSL https://collyn.github.io/skey/install-arch.sh | sudo bash && sudo pacman -S fcitx5-skey && skey-setup
```

Cài thêm frontend GTK/Qt của Fcitx5 phù hợp với ứng dụng của bạn.

### NixOS

SKey có flake và module NixOS tích hợp sẵn. Thêm input vào flake của hệ thống
và bật module cùng Fcitx5:

```nix
{
  inputs.skey = {
    url = "github:collyn/skey";
    inputs.nixpkgs.follows = "nixpkgs";
  };

  outputs = { nixpkgs, skey, ... }: {
    nixosConfigurations.hostname = nixpkgs.lib.nixosSystem {
      modules = [
        skey.nixosModules.default
        ({ config, ... }: {
          services.fcitx5-skey.enable = true;
          i18n.inputMethod = {
            enable = true;
            type = "fcitx5";
            # Dùng package từ module để kênh Stable/Dev đồng bộ.
            fcitx5.addons = [ config.services.fcitx5-skey.package ];
          };
        })
      ];
    };
  };
}
```

Áp dụng cấu hình:

```bash
sudo nixos-rebuild switch --flake /etc/nixos
fcitx5 -r -d
```

Module tự bật `hardware.uinput`, cài udev rule cấp quyền cho `/dev/uinput`,
đưa addon/settings GUI vào hệ thống và không cần chạy `skey-setup`. Trên KDE
Wayland chọn **Fcitx 5** trong System Settings → Virtual Keyboard.

Để build package riêng hoặc xem tùy chọn `services.fcitx5-skey.devVersion`,
xem [hướng dẫn NixOS đầy đủ](packaging/nixos/README.md). Có thể kiểm tra package
trước khi rebuild bằng:

```bash
nix --extra-experimental-features 'nix-command flakes' \
  build github:collyn/skey#fcitx5-skey
```

## Sử dụng

- `skey-setup` cấu hình Fcitx5, biến môi trường; đăng xuất/đăng nhập lại nếu script yêu cầu.
- **Ctrl+Space** bật/tắt bộ gõ theo cấu hình Fcitx5. Phím **backtick** (`` ` ``) mở menu chế độ cho ứng dụng hiện tại; có thể đổi trong cài đặt.
- Mở `fcitx5-skey-settings` để chỉnh cấu hình. Auto Restore mặc định bật; tắt nếu muốn giữ các từ viết tắt không hợp lệ trong tiếng Việt. Native mặc định tắt; chỉ bật khi cần dùng XTest/libei.
- Trên **KDE Plasma Wayland**, chọn **Fcitx 5** trong System Settings → Virtual Keyboard.

Nếu không gõ được sau khi cài hoặc cập nhật, chạy lại `skey-setup`. Với chế độ Uinput, kiểm tra quyền thiết bị:

```bash
getfacl /dev/uinput
# Thử lại bước cấp quyền/migration nếu cần: sudo skey-uinput-access configure
```

Bật **Ghi log debug** trong cài đặt để theo dõi bằng `tail -f /tmp/skey.log`.

## Build từ mã nguồn

Cần CMake ≥ 3.16, ECM, trình biên dịch C++17, Rust/Cargo, pkg-config, Fcitx5 development headers, D-Bus, XCB, X11 và XTest. Qt6 (Widgets/Core/Network/DBus) cùng gettext dùng cho giao diện cài đặt; libei ≥ 1.0 và libportal ≥ 0.7.1 bật backend Native trên Wayland.

Ví dụ dependencies trên Ubuntu/Debian. Cài thêm Rust/Cargo stable bằng [rustup](https://rustup.rs/) nếu máy chưa có:

```bash
sudo apt update
sudo apt install build-essential git cmake extra-cmake-modules pkg-config fcitx5 \
  libfcitx5core-dev libfcitx5utils-dev libdbus-1-dev libxcb1-dev \
  libx11-dev libxtst-dev libgl1-mesa-dev libxkbcommon-dev libfontconfig1-dev \
  qt6-base-dev libqt6svg6 gettext librsvg2-bin
```

`libxtst-dev` cung cấp XTest (`xtst.pc`); đây là dependency build bắt buộc, kể cả khi chạy desktop Wayland. Có thể kiểm tra các thư viện backend trước khi build:

```bash
pkg-config --modversion dbus-1 xcb x11 xtst
```

Để bật thêm Native qua libei/portal trên Wayland, cài các gói sau trên Ubuntu 24.04+ hoặc Debian 13+:

```bash
sudo apt install libei-dev libportal-dev
pkg-config --atleast-version=1.0 libei-1.0
pkg-config --atleast-version=0.7.1 libportal
```

Nếu distro chưa cung cấp đủ phiên bản, có thể bỏ các gói libei/libportal; CMake sẽ tắt backend này, còn XTest trên X11 vẫn build bình thường. `libqt6svg6-dev` là tùy chọn nếu muốn liên kết Qt SVG trực tiếp; `librsvg2-bin` tạo icon PNG dự phòng khi build.

```bash
git clone --depth=1 --single-branch https://github.com/collyn/skey.git
cd skey
cmake -S . -B build -DCMAKE_INSTALL_PREFIX=/usr -DCMAKE_BUILD_TYPE=Release \
  -DBUILD_TESTING=OFF
cmake --build build -j"$(nproc)"
sudo cmake --install build
skey-setup
```

Cài package hoặc cài source trực tiếp vào `/usr`/`/usr/local` tự dừng và dọn service, rule Uinput cũ. Bản sao cấu hình cũ nằm tại `/var/lib/fcitx5-skey/legacy-uinput/`. Cài qua `DESTDIR` chỉ đóng gói file; chạy `sudo skey-uinput-access configure` trên máy đích sau khi triển khai.

CMake tự tải phiên bản skey-engine được ghim trong dự án; bước build cần mạng để lấy engine và các crate. Để dùng engine cục bộ, thêm `-DSKEY_ENGINE_SOURCE_DIR=/path/to/skey-engine` khi cấu hình CMake.

Đóng gói `.deb`: `./build.sh` (cần thêm `dpkg-dev`). Muốn build và chạy test, cấu hình riêng với `-DBUILD_TESTING=ON`; xem [hướng dẫn kiểm thử và kiểm tra bằng Clang](scripts/TESTING.md).

## Mã nguồn & giấy phép

- `src/`: addon Fcitx5, xử lý văn bản, backend xuất và Uinput injector.
- `src/settings/`: giao diện Qt6.
- `data/`, `scripts/`: tài nguyên, service và công cụ thiết lập/kiểm thử.
- `debian/`, `packaging/`, `.github/workflows/`: đóng gói và CI.

Phát hành theo [GPL-3.0](LICENSE). Cảm ơn [Fcitx5](https://github.com/fcitx/fcitx5), [skey-engine](https://github.com/collyn/skey-engine) và [bamboo-core](https://github.com/nguyen10t2/bamboo_core), engine gốc của dự án.
