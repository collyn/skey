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
- **Giao diện cài đặt:** quản lý kiểu gõ, từ điển, macro, biểu tượng và cập nhật Stable/Dev.

| Chế độ xuất | Cách hoạt động |
| --- | --- |
| **Auto** (mặc định) | Chọn theo khả năng ứng dụng và ngữ cảnh ô nhập; có xử lý riêng cho trình duyệt, terminal và ứng dụng văn phòng. |
| **Surrounding Text** | Thay thế văn bản qua API surrounding text của Fcitx5. |
| **Uinput** | Gửi Backspace qua `/dev/uinput`, cần service đi kèm. |
| **Native** | Gửi Backspace qua XTest trên X11 hoặc libei/portal trên Wayland. Backend Wayland cần thư viện và compositor hỗ trợ, cùng quyền từ portal. |
| **Preedit** | Soạn văn bản trong vùng preedit trước khi commit vào ứng dụng. |

Có thể bật **Thay thế Uinput bằng Native** để dùng Native cho các trường hợp Auto chọn Uinput.

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

Dùng flake và module `services.fcitx5-skey` theo [hướng dẫn NixOS](packaging/nixos/README.md).

## Sử dụng

- `skey-setup` cấu hình Fcitx5, biến môi trường và service Uinput; đăng xuất/đăng nhập lại nếu script yêu cầu.
- **Ctrl+Space** bật/tắt bộ gõ theo cấu hình Fcitx5. Phím **backtick** (`` ` ``) mở menu chế độ cho ứng dụng hiện tại; có thể đổi trong cài đặt.
- Mở `fcitx5-skey-settings` để chỉnh cấu hình. Auto Restore mặc định bật; tắt nếu muốn giữ các từ viết tắt không hợp lệ trong tiếng Việt.
- Trên **KDE Plasma Wayland**, chọn **Fcitx 5** trong System Settings → Virtual Keyboard.

Nếu không gõ được sau khi cài hoặc cập nhật, chạy lại `skey-setup`. Với chế độ Uinput, kiểm tra service:

```bash
systemctl status "fcitx5-skey-uinput-server@$USER.service"
```

Bật **Ghi log debug** trong cài đặt để theo dõi bằng `tail -f /tmp/skey.log`.

## Build từ mã nguồn

Cần CMake ≥ 3.16, ECM, trình biên dịch C++17, Rust/Cargo, pkg-config, Fcitx5 development headers, D-Bus, XCB, X11 và XTest. Qt6 (Widgets/Core/Network/DBus) cùng gettext dùng cho giao diện cài đặt; libei ≥ 1.0 và libportal ≥ 0.7.1 bật backend Native trên Wayland.

Ví dụ dependencies trên Ubuntu/Debian (cần cài thêm Rust/Cargo):

```bash
sudo apt install build-essential git cmake extra-cmake-modules pkg-config \
  libfcitx5core-dev libfcitx5utils-dev libdbus-1-dev libxcb1-dev \
  libx11-dev libxtst-dev libei-dev libportal-dev qt6-base-dev gettext librsvg2-bin
```

```bash
git clone --depth=1 --single-branch https://github.com/collyn/skey.git
cd skey
cmake -S . -B build -DCMAKE_INSTALL_PREFIX=/usr -DCMAKE_BUILD_TYPE=Release
cmake --build build -j"$(nproc)"
ctest --test-dir build --output-on-failure
sudo cmake --install build
skey-setup
```

CMake tự tải phiên bản skey-engine được ghim trong dự án; bước build cần mạng để lấy engine và các crate. Để dùng engine cục bộ, thêm `-DSKEY_ENGINE_SOURCE_DIR=/path/to/skey-engine` khi cấu hình CMake.

Đóng gói `.deb`: `./build.sh`. Hướng dẫn kiểm thử và kiểm tra bằng Clang: [scripts/TESTING.md](scripts/TESTING.md).

## Mã nguồn & giấy phép

- `src/`: addon Fcitx5, xử lý văn bản, backend xuất và Uinput server.
- `src/settings/`: giao diện Qt6.
- `data/`, `scripts/`: tài nguyên, service và công cụ thiết lập/kiểm thử.
- `debian/`, `packaging/`, `.github/workflows/`: đóng gói và CI.

Phát hành theo [GPL-3.0](LICENSE). Cảm ơn [Fcitx5](https://github.com/fcitx/fcitx5), [skey-engine](https://github.com/collyn/skey-engine) và [bamboo-core](https://github.com/nguyen10t2/bamboo_core), engine gốc của dự án.
