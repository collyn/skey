<div align="center">

<img src="data/icons/fcitx-skey.svg" alt="SKey" width="128" height="128">

# SKey — Simple Key

**Bộ gõ Tiếng Việt đơn giản, nhẹ, và nhanh cho Linux.**

[![Release](https://img.shields.io/github/v/release/collyn/skey?label=release&sort=semver)](https://github.com/collyn/skey/releases)
[![License](https://img.shields.io/github/license/collyn/skey?label=license)](LICENSE)
[![Stars](https://img.shields.io/github/stars/collyn/skey?label=stars)](https://github.com/collyn/skey/stargazers)

</div>

SKey (Simple Key) là bộ gõ Tiếng Việt cho Linux trên nền tảng [fcitx5](https://fcitx-im.org/), sử dụng engine [skey-engine](https://github.com/collyn/skey-engine) (Rust) qua FFI. Mặc định chạy ở chế độ **Auto** — tự động chọn giữa Surrounding Text và Uinput dựa trên khả năng của ứng dụng. Gõ **tự do như UniKey**: double-letter transform (dd→đ, oo→ô...) hoạt động ở mọi vị trí, đổi dấu thanh liên tục không lỗi, viết tắt thoải mái. Bộ gõ chạy monolithic không cần server; riêng chế độ Uinput đi kèm một server tùy chọn để tối ưu hóa việc xóa/thay thế chữ.

> **v0.5.4:** Engine xử lý tiếng Việt đã chuyển từ bamboo-core sang [skey-engine](https://github.com/collyn/skey-engine) — một Rust engine được fork và tối ưu riêng cho SKey, hỗ trợ cú pháp C ABI trực tiếp không cần wrapper.

---

## Tính năng

- **Phương thức gõ:** Telex (tuỳ chọn w→ư, ][→ươ), VNI
- **Auto Mode (mặc định)** — tự động chọn giữa Surrounding Text và Uinput dựa trên capability flags của ứng dụng. Không cần cấu hình thủ công cho từng app.
- **Surrounding Text** — gõ trực tiếp không gạch chân qua API fcitx5. Ổn định trên hầu hết ứng dụng native (Qt/GTK) và web (Chrome/Firefox).
- **Uinput** — gửi phím Backspace qua kernel `/dev/uinput`. Phù hợp cho terminal app (Tabby, Konsole) và ứng dụng không hỗ trợ Surrounding Text API.
- **Preedit** — hiển thị chữ gạch chân. Phù hợp cho thanh địa chỉ Chromium.
- **Per-app Mode Override** — ghi đè chế độ gõ cho từng ứng dụng qua menu phím tắt `` ` `` hoặc Settings UI. Cài đặt được lưu vĩnh viễn.
- **Loại trừ ứng dụng (App Exclusion)** — tắt gõ tiếng Việt cho từng ứng dụng qua menu phím tắt `` ` ``.
- **Gõ tự do (Unikey-style)** — transform hoạt động ở mọi vị trí: `dd→đ`, `oo→ô`, `aa→â`, `ee→ê`, `aw→ă`, `ow→ơ`, `ww/uw→ư`. Viết tắt (`vcđ`, `nđm`, `NĐ`), gõ chữ không dấu rồi thêm dấu sau, đổi dấu liên tục (`x→s→f→x`) đều hoạt động.
- **Double-tone undo** — bấm cùng phím dấu 2 lần liên tiếp để hiện raw form (vd: `vãi` → `x` → `vaix`).
- **Auto-restore** — tự động hoàn nguyên từ không phải tiếng Việt. Hai cơ chế: real-time (all-ASCII, giữa chừng) và commit-time (khi kết thúc từ bằng space/enter/tab/punctuation). Mặc định bật.
- **Kiểm tra chính tả** — kiểm tra tính hợp lệ âm tiết theo thời gian thực qua skey-engine.
- **Free marking** — cho phép đặt dấu tự do, không bó buộc vị trí.
- **Vị trí dấu thanh:** Kiểu mới (hoà) hoặc kiểu cũ (hòa).
- **Menu cấu hình** — chuyển đổi nhanh tùy chọn qua phím tắt `` ` `` và menu hệ thống.
- **Nhẹ & Gọn** — mặc định là một thư viện liên kết động (.so), không chạy daemon ngầm trừ khi bật chế độ Uinput.
- **Cấu hình** — thiết lập qua `fcitx5-skey-settings` (Qt6 GUI) hoặc `fcitx5-configtool`.

---

## Cài đặt

### APT Repository (khuyến nghị)

Thêm APT repository để tự động nhận cập nhật qua `apt update`:

```bash
curl -fsSL https://collyn.github.io/skey/install.sh | sudo bash
sudo apt install fcitx5-skey
```

Package tự động chạy `skey-setup` để cấu hình fcitx5, bật `ActiveByDefault` và `ShareInputState`, export biến môi trường (`GTK_IM_MODULE`, `QT_IM_MODULE`, `XMODIFIERS`) cho cả X11 và Wayland. Bộ gõ SKey sẵn sàng sử dụng ngay sau khi cài — chuyển đổi bằng **Ctrl+Space**.

> ⚠️ **Wayland:** Sau khi cài, vào System Settings → Input Devices → Virtual Keyboard chọn **Fcitx 5**. Nếu không thấy tuỳ chọn này, logout/login rồi kiểm tra lại.

Nếu gõ không được sau khi cài hoặc update, chạy lại:

```bash
skey-setup
```

### Từ file .deb

Tải file `.deb` mới nhất từ [GitHub Releases](https://github.com/collyn/skey/releases):

```bash
sudo dpkg -i fcitx5-skey_*.deb
sudo apt install -f  # cài dependencies nếu cần
skey-setup       # cấu hình fcitx5 và uinput server
```

> ⚠️ **Wayland:** Chọn Virtual Keyboard là **Fcitx 5** trong System Settings (xem [Khắc phục sự cố](#khắc-phục-sự-cố-sau-khi-cài-đặt--cập-nhật)).

### RPM Repository — Fedora

```bash
curl -fsSL https://collyn.github.io/skey/install-fedora.sh | sudo bash
sudo dnf install fcitx5-skey
```

Cập nhật tự động qua `dnf update`. Cần thêm frontends để gõ trên GTK/Qt:

```bash
sudo dnf install fcitx5-gtk fcitx5-qt
```

### RPM Repository — openSUSE

```bash
curl -fsSL https://collyn.github.io/skey/install-opensuse.sh | sudo bash
sudo zypper install fcitx5-skey
```

Cập nhật tự động qua `zypper up`. Cần thêm frontend GTK:

```bash
sudo zypper install fcitx5-gtk
```

### Arch Linux

```bash
curl -fsSL https://collyn.github.io/skey/install-arch.sh | sudo bash
sudo pacman -S fcitx5-skey
```

Cập nhật tự động qua `pacman -Syu`. Cần thêm frontends:

```bash
sudo pacman -S fcitx5-gtk fcitx5-qt
```

> **GPG key fingerprint:** Tất cả repository (APT, RPM, Arch) dùng chung một GPG key. Public key tại `https://collyn.github.io/skey/key.asc`. Import thủ công: `curl -fsSL https://collyn.github.io/skey/key.asc | gpg --import`

### Build từ source

#### Yêu cầu

| Thành phần | Bắt buộc | Ghi chú |
|-----------|----------|---------|
| CMake ≥ 3.16, ECM, pkg-config | ✅ | Hệ thống build |
| GCC/G++ (C++17) + make | ✅ | Biên dịch C++ |
| Rust toolchain (rustc, cargo) | ✅ | Build skey-engine (Rust static library) |
| Fcitx5 development headers | ✅ | `Fcitx5::Core`, `fcitx-utils` |
| dbus-1 development headers | ✅ | AT-SPI2 (phát hiện thanh địa chỉ Chromium) |
| gettext | ✅ | i18n |
| Qt6 (Widgets, Core, Network, DBus) | ❌ | Settings GUI (`fcitx5-skey-settings`) |
| librsvg2 (rsvg-convert) | ❌ | Sinh PNG icon cho system tray (KDE/GNOME) |
| systemd | ❌ | uinput server |
| dpkg-dev / rpm-build / base-devel | ❌ | Build package (`.deb`, `.rpm`, `.pkg.tar.zst`) |

#### Cài dependencies

Chọn lệnh phù hợp với nền tảng của bạn. Các gói được chia thành **bắt buộc** (build core engine) và **tùy chọn** (settings GUI, icon, đóng gói).

**Ubuntu / Debian:**

```bash
# Bắt buộc — toolchain + fcitx5 + dbus
sudo apt install build-essential cmake extra-cmake-modules pkg-config gettext \
  libfcitx5core-dev libfcitx5utils-dev fcitx5 \
  libdbus-1-dev librsvg2-bin curl

# Tùy chọn — Settings GUI (Qt6)
sudo apt install qt6-base-dev

# Tùy chọn — Build .deb
sudo apt install dpkg-dev
```

> **Rust toolchain:** Trên Ubuntu < 24.04, `rustc`/`cargo` từ apt quá cũ. Dùng rustup:
> ```bash
> curl --proto '=https' --tlsv1.2 -sSf https://sh.rustup.rs | sh -s -- -y --default-toolchain stable
> source "$HOME/.cargo/env"
> ```
> Trên Ubuntu ≥ 24.04, có thể dùng gói từ apt: `sudo apt install rustc cargo`

**Fedora:**

```bash
# Bắt buộc — toolchain + fcitx5 + dbus
sudo dnf install cmake extra-cmake-modules pkgconf-pkg-config gettext \
  fcitx5-devel fcitx5 dbus-devel \
  rust cargo gcc-c++ make

# Tùy chọn — Settings GUI (Qt6)
sudo dnf install qt6-qtbase-devel

# Tùy chọn — PNG icon
sudo dnf install librsvg2-tools
```

**Arch Linux:**

```bash
# Bắt buộc — toolchain + fcitx5 + dbus
sudo pacman -S base-devel cmake extra-cmake-modules pkgconf gettext \
  fcitx5 dbus \
  rust cargo

# Tùy chọn — Settings GUI (Qt6)
sudo pacman -S qt6-base

# Tùy chọn — PNG icon
sudo pacman -S librsvg
```

**openSUSE:**

```bash
# Bắt buộc — toolchain + fcitx5 + dbus
sudo zypper install cmake extra-cmake-modules pkgconf gettext \
  fcitx5-devel fcitx5 dbus-1-devel \
  rust cargo gcc-c++ make

# Tùy chọn — Settings GUI (Qt6)
sudo zypper install qt6-base-devel

# Tùy chọn — PNG icon
sudo zypper install rsvg-convert
```

> **Ghi chú chung về Rust:** Nếu rust/cargo từ repo distro quá cũ, cài đặt qua rustup để có toolchain mới nhất:
> ```bash
> curl --proto '=https' --tlsv1.2 -sSf https://sh.rustup.rs | sh -s -- -y --default-toolchain stable
> source "$HOME/.cargo/env"
> ```
> Rust ≥ 1.70 được khuyến nghị để build skey-engine.

#### Build & Install

```bash
# --single-branch: chỉ lấy nhánh main — gh-pages chứa package repo (~vài trăm MB),
# clone thường sẽ kéo cả nhánh đó xuống rất lâu. --depth=1 lấy nông cho nhanh.
git clone --depth=1 --single-branch https://github.com/collyn/skey.git
cd skey
cmake -B build -DCMAKE_INSTALL_PREFIX=/usr -DCMAKE_BUILD_TYPE=Release
cmake --build build -j$(nproc)
sudo cmake --install build
```

> **Build không có Qt6:** Nếu không cài `qt6-base-dev` (hoặc equivalent), CMake sẽ tự động bỏ qua Settings GUI và chỉ build engine core + uinput server.

Sau khi build và install:

```bash
# Cấu hình fcitx5 profile và biến môi trường
skey-setup
```

Script `skey-setup` sẽ:
1. Thêm SKey vào fcitx5 profile (ActiveByDefault, ShareInputState)
2. Export biến môi trường `GTK_IM_MODULE`, `QT_IM_MODULE`, `XMODIFIERS`, `SDL_IM_MODULE`, `GLFW_IM_MODULE`
3. Trên Wayland + KDE/GNOME: tự động reconnect compositor virtual keyboard
4. Enable + start `fcitx5-skey-uinput-server@$USER` service (cần cho chế độ Uinput)

### Khắc phục sự cố sau khi cài đặt / cập nhật

Nếu sau khi cài hoặc update mà **không gõ được tiếng Việt**, chạy lại:

```bash
skey-setup
```

**Lưu ý quan trọng cho Wayland:** Sau khi chạy `skey-setup`, vào cài đặt hệ thống chọn Virtual Keyboard là **"Fcitx 5"** (không phải IBus hay None):

| Desktop | Đường dẫn cài đặt |
|---------|------------------|
| **KDE Plasma** | System Settings → Input Devices → Virtual Keyboard → chọn **Fcitx 5** |
| **GNOME** | Settings → Keyboard → Input Sources → chọn **Vietnamese (Fcitx 5)** |
| **Hyprland / Sway / Niri** | Thêm `exec-once = fcitx5 -d` vào config file |

Nếu Wayland không có tuỳ chọn Virtual Keyboard, kiểm tra compositor đã hỗ trợ `zwp_input_method_v2` chưa. Với KDE ≥ 6.0 hoặc GNOME ≥ 45, tính năng này có sẵn mặc định.

**Sau khi update gặp lỗi lạ:** Thử logout/login hoàn toàn để session Wayland/X11 nhận biến môi trường mới. Nếu vẫn lỗi, kiểm tra:

```bash
# Kiểm tra fcitx5 đang chạy chưa
pidof fcitx5

# Kiểm tra uinput server
systemctl status fcitx5-skey-uinput-server@$USER.service

# Xem log để debug
tail -f /tmp/skey.log
```

#### Build .deb

```bash
./build.sh
```

Tạo file `fcitx5-skey_<version>_amd64.deb` sẵn sàng để cài đặt.

---

## Sử dụng

Sau khi cài đặt + chạy `skey-setup`, SKey là bộ gõ mặc định. Chuyển đổi bằng **Ctrl+Space**.

### Bảng gõ Telex

Các transform **hoạt động ở mọi vị trí** (đầu, giữa, cuối từ):

| Gõ | Kết quả | Mô tả |
|----|---------|-------|
| `aa` | â | Dấu mũ (mọi vị trí) |
| `oo` | ô | Dấu mũ (mọi vị trí) |
| `ee` | ê | Dấu mũ (mọi vị trí) |
| `uw` / `ww` | ư | Dấu móc (mọi vị trí) |
| `ow` | ơ | Dấu móc (mọi vị trí) |
| `aw` | ă | Dấu trăng (mọi vị trí) |
| `dd` | đ | D đét (mọi vị trí, kể cả viết tắt) |
| `s` | dấu sắc | á, é, ó... |
| `f` | dấu huyền | à, è, ò... |
| `r` | dấu hỏi | ả, ẻ, ỏ... |
| `x` | dấu ngã | ã, ẽ, õ... |
| `j` | dấu nặng | ạ, ẹ, ọ... |
| `z` | xóa dấu | bỏ dấu thanh |

**Tuỳ chọn mở rộng cho Telex** (bật trong cấu hình, mặc định tắt):

| Gõ | Kết quả | Tuỳ chọn |
|----|---------|----------|
| `w` | ư | **Gõ w thành ư** — phím `w` đơn lẻ ra `ư` |
| `[` | ơ | **Gõ ][ thành ư ơ** — kiểu UniKey |
| `]` | ư | **Gõ ][ thành ư ơ** — kiểu UniKey |

Ví dụ khi bật **Gõ ][ thành ư ơ**: gõ `dd][cj` → `được`, `ng][if` → `người`.

### Bảng gõ VNI

| Gõ | Kết quả |
|----|---------|
| `1` | dấu sắc |
| `2` | dấu huyền |
| `3` | dấu hỏi |
| `4` | dấu ngã |
| `5` | dấu nặng |
| `6` | dấu mũ (â, ê, ô) |
| `7` | dấu móc (ơ, ư) |
| `8` | dấu trăng (ă) |
| `9` | d đét (đ) |
| `0` | xóa dấu |

---

## Cấu hình

Mở **fcitx5-skey-settings** (khuyến nghị) hoặc **fcitx5-configtool** để thay đổi cài đặt:

| Tùy chọn | Giá trị | Mô tả |
|----------|---------|-------|
| **Input Method** | Telex / VNI | Phương thức gõ |
| **Gõ w thành ư** | true / false | Chỉ Telex: gõ w đơn lẻ ra ư |
| **Gõ ][ thành ư ơ** | true / false | Chỉ Telex: gõ [ ra ơ, ] ra ư (giống UniKey) |
| **Output Mode** | **Auto** / Surrounding Text / Preedit / Uinput | Chế độ xuất. Mặc định Auto — tự động chọn giữa Surrounding Text và Uinput |
| **Chromium Address Bar** | **Auto** / Uinput / Surrounding Text / Preedit / No Vietnamese | Chế độ gõ riêng cho thanh địa chỉ Chromium (Chrome, Edge, Brave...) |
| **Free Marking** | true / false | Cho phép đặt dấu tự do |
| **Auto Restore** | true / false | Tự động hoàn nguyên từ không phải tiếng Việt |
| **Show Preedit** | true / false | Hiển thị text đang soạn |

File cấu hình lưu tại `~/.config/fcitx5/conf/skey.conf`. Thay đổi có hiệu lực ngay sau khi save.

### Settings GUI

SKey đi kèm ứng dụng **fcitx5-skey-settings** (Qt6) để cấu hình trực quan:

- **Tab Chung**: tất cả tùy chọn gõ (phương thức, chế độ output, bảng mã, dấu thanh...)
- **Tab Ứng dụng**: cấu hình chế độ gõ riêng cho từng ứng dụng (Auto / Uinput / Surrounding Text / Preedit / Excluded)
- **Tab Info**: phiên bản, kiểm tra cập nhật, **kênh cập nhật (Stable/Dev)**, **nút khởi động lại Fcitx5**

Mở từ menu ứng dụng hoặc terminal: `fcitx5-skey-settings`

### Cập nhật từ Settings GUI

SKey hỗ trợ **cập nhật tự động ngay trong giao diện** — không cần mở terminal:

1. Mở `fcitx5-skey-settings` → **Tab Info**
2. Bấm **"Kiểm tra cập nhật"** — app truy vấn GitHub Releases API để so sánh phiên bản
3. Nếu có phiên bản mới → hiện dialog với **version + release notes** → bấm **"Cập nhật ngay"**
4. App tự động **tải .deb** → **cài qua `pkexec dpkg -i`** → **chạy `skey-setup`** → **restart Fcitx5** (có reconnect Wayland virtual keyboard)
5. Settings GUI tự khởi động lại với phiên bản mới sau khi cài xong

Toàn bộ quy trình diễn ra trong GUI — người dùng chỉ cần bấm 2 nút: "Kiểm tra cập nhật" → "Cập nhật ngay". Sau khi cập nhật, Fcitx5 được restart tự động và sẵn sàng sử dụng ngay.

> 💡 **Sau khi update**, nếu gặp lỗi (không gõ được, mất biểu tượng...), chạy `skey-setup` trong terminal. Trên Wayland, kiểm tra lại Virtual Keyboard vẫn đang chọn **Fcitx 5**.

### Kênh phát hành: Stable / Dev

Tab Info có combo **"Kênh cập nhật"** với 2 lựa chọn:

- **Ổn định (Stable)** — mặc định. Kiểm tra bản phát hành chính thức (`releases/latest` trên GitHub). Các bản Dev dạng prerelease không bao giờ xuất hiện ở kênh này.
- **Thử nghiệm (Dev)** — build tự động từ nhánh `dev`, mỗi lần push lên `dev` là một build mới (version dạng `0.7.5~dev.123`). Có thể chưa ổn định. Chỉ giữ **3 bản Dev mới nhất** trên GitHub; bản cũ bị xóa tự động.

Quy tắc khi check update:

| Bản đang dùng | Kênh đang chọn | Kết quả |
|---|---|---|
| Stable | Stable | Mời bản stable mới hơn (như cũ) |
| Stable | Dev | Mời bản Dev mới hơn (chuyển sang Dev) |
| Dev | Dev | Mời bản Dev mới hơn (so theo số build) |
| Dev | Stable | Mời khi stable ra bản **mới hơn** base của Dev; nếu stable bằng/thấp hơn sẽ báo "đang dùng bản Dev mới hơn Stable" (không tự hạ cấp) |

Lựa chọn kênh được lưu trong `skey.conf` (`UpdateChannel=`), có hiệu lực ngay, không cần khởi động lại.

**Dành cho maintainer — cắt bản stable mới:** merge `dev` → `main`, bump `project(fcitx5-skey VERSION ...)` trong `CMakeLists.txt`, push tag `vX.Y.Z` (workflow tự build + tạo release + publish repo gh-pages). Luôn bump version trên nhánh `dev` ≥ main để build Dev không bao giờ thấp hơn bản stable đã phát hành.

### Chế độ Auto (mặc định)

Chế độ **Auto** tự động chọn giữa Surrounding Text và Uinput khi focus vào một ứng dụng. Logic được đánh giá theo thứ tự ưu tiên (tiered cascade) — rule nào khớp trước sẽ quyết định, không cần kiểm tra các rule sau:

```
1. surroundText trước đó đã fail?         → Uinput  (runtime degradation)
2. Không có SurroundingText capability?    → Uinput  (app X11 cũ, WINE...)
3. Terminal app? (flag Terminal hoặc tên)  → Uinput  (Konsole, Alacritty, Kitty...)
4. Đang ở thanh địa chỉ Chromium?         → Uinput  (tránh xung đột omnibox autocomplete)
5. Electron app không có content hints?    → Uinput  (Tabby, VS Code terminal pane...)
6. Còn lại                                  → SurroundingText
```

**Giải thích từng tầng:**

| # | Điều kiện | Lý do |
|---|-----------|-------|
| 1 | `surroundingTextFailed_` | SurroundingText đã được thử nhưng `isValid()` trả về `false` — API không thực sự hoạt động dù capability flag có bật. Hạ cấp vĩnh viễn cho input context này. |
| 2 | Thiếu `SurroundingText` cap | Ứng dụng không quảng bá hỗ trợ surrounding text — không có cách nào đọc/ghi văn bản qua API, phải dùng uinput. |
| 3 | Terminal app | Terminal có internal buffer riêng, SurroundingText API không đồng bộ đúng. Phát hiện qua `CapabilityFlag::Terminal` hoặc match tên (`konsole`, `alacritty`, `kitty`, `wezterm`, `foot`, `xterm`...). |
| 4 | Chromium address bar | Omnibox autocomplete thay đổi text giữa lúc delete và commit, gây corrupt nếu dùng SurroundingText. Phát hiện qua AT-SPI2 (`A11yMonitor`) — chỉ trigger trên browser thực sự (Chrome, Edge, Brave...), không nhầm với Electron app. |
| 5 | Electron không content hints | Electron app gửi SurroundingText cap nhưng bare flags (`0x72`) — không có content hints như `UppercaseWords`, `Alpha`, `Email`... Chứng tỏ đây là terminal/text input, không phải editor thực sự. Browser thực sự (Chrome, Firefox) không bị ảnh hưởng vì có `Url` cap hoặc content hints đầy đủ. |

**Ví dụ cụ thể:**

| App | Cap flags | Tầng khớp | Kết quả |
|-----|----------|-----------|---------|
| Chrome/Firefox (web content) | `SurroundingText` + content hints | #6 | SurroundingText |
| Chrome address bar (X11) | `SurroundingText` + AT-SPI2 detect | #4 | Uinput |
| Tabby (Electron terminal) | `SurroundingText` bare, no hints | #5 | Uinput |
| VS Code (Electron editor) | `SurroundingText` + content hints | #6 | SurroundingText |
| Konsole, Alacritty | `Terminal` flag | #3 | Uinput |
| App X11 cũ (GTK2, WINE) | Không có `SurroundingText` | #2 | Uinput |
| App có S/T cap nhưng API fail | `SurroundingText` nhưng `isValid()=false` | #1 | Uinput |

Kết quả detect được cache cho mỗi input context, không quét lại `/proc` mỗi keystroke.

Để ghi đè cho một ứng dụng cụ thể: bấm `` ` `` → chọn mode mong muốn. Cài đặt được lưu vĩnh viễn trong `~/.config/fcitx5/conf/skey-app-modes.conf`.

### Chế độ riêng cho thanh địa chỉ Chromium

Trên X11, thanh địa chỉ Chrome/Chromium không hỗ trợ Surrounding Text API — việc xóa và thay thế văn bản qua D-Bus gây ra xung đột với autocomplete của omnibox, dẫn đến mất chữ hoặc gõ sai. SKey sử dụng AT-SPI2 để tự động phát hiện khi đang ở thanh địa chỉ Chromium và chuyển sang chế độ gõ được cấu hình riêng (mặc định: **Auto**).

| Chế độ | Phù hợp khi |
|--------|------------|
| **Auto** (mặc định) | Dùng chung logic tự động như web content |
| **Preedit** | Ổn định nhất — hiển thị gạch chân, không xung đột với autocomplete |
| **Surrounding Text** | Muốn gõ trực tiếp không gạch chân |
| **Uinput** | Muốn gõ trực tiếp qua `/dev/uinput` (cần bật service uinput server) |
| **No Vietnamese** | Tắt hoàn toàn tiếng Việt trong thanh địa chỉ, chỉ gõ trên web |

Trên Wayland, Chrome hỗ trợ `CapabilityFlag::Url` nên việc phát hiện thanh địa chỉ là tức thời và chính xác.

### Chế độ Trì hoãn Tự Thích ứng (Adaptive Delay)

Đối với chế độ **Uinput**, SKey sử dụng kỹ thuật **Backspace đồng bộ (Sync BS)** lấy cảm hứng từ fcitx5-lotus và tối ưu phần delay thay vì tạo ra nhiều chế độ Smooth, Super Smooth, Slow. Thật sự hồi mới dùng mình không biết phải chọn cái nào nên khi phát triển skey mình chỉ để chung 1 chế độ Uinput duy nhất và sử dụng các thuật toán để tự động đánh gía độ delay tránh mất chữ khi gõ:

- Gửi N+1 phím Backspace qua `/dev/uinput`: N phím để xoá chữ cũ, 1 phím neo (anchor) làm điểm đồng bộ
- N phím BS đầu tiên đi thẳng vào ứng dụng để xoá văn bản. Phím neo thứ N+1 quay trở lại fcitx5 — lúc này kernel/X11 đã serialize xong toàn bộ N phím trước đó, đảm bảo thứ tự BS → commit chính xác
- **EWMA** (Exponentially Weighted Moving Average) đo thời gian round-trip thực tế của phím BS để tính sleep thích ứng:
  - App native (Qt/GTK): **3–30ms**
  - Chromium/Electron: **6–60ms** (×2.0, bù cho multi-process pipeline)
  - Thanh địa chỉ Chromium: **10–50ms** (conservative, tránh xung đột với autocomplete)
- **Commit đồng bộ** ngay sau sleep — không cần timer async, không race condition
- Safety timeout **150ms** để tránh freeze nếu BS events bị thất lạc
- Trên X11, sync BS giải quyết vấn đề BS (uinput kernel→X11) và commit (D-Bus) đi qua 2 kênh khác nhau, không đảm bảo thứ tự. Wayland serialize tự nhiên qua compositor nên delay thấp hơn (2–18ms)

### Output mode Uinput

Chế độ **Uinput** gửi yêu cầu Backspace trực tiếp đến trình điều khiển hệ thống qua service `fcitx5-skey-uinput-server`. Phương pháp này giúp khắc phục lỗi mất chữ trên một số ứng dụng đặc thù hoặc terminal app không hỗ trợ Surrounding Text.

`skey-setup` tự động enable và start service. Nếu cần bật thủ công:

```bash
sudo systemctl enable --now fcitx5-skey-uinput-server@$USER.service
```

Service chạy hoàn toàn không quyền root dưới tài khoản hệ thống `skey_uinput` (tạo tự động qua sysusers.d khi cài). Quyền ghi `/dev/uinput` được cấp qua ACL trong udev rule `99-skey-uinput.rules` — không dùng group `input` để tránh mở quyền đọc toàn bộ thiết bị input. Instance nhận tên người dùng từ `@$USER` để mở socket đúng user chạy Fcitx5; socket đặt trong `/run/skey-uinput-$USER` (do systemd `RuntimeDirectory` tạo). Kiểm tra trạng thái:

```bash
systemctl status fcitx5-skey-uinput-server@$USER.service
```

Khi server không hoạt động hoặc gặp sự cố mở `/dev/uinput`, cần khởi động lại service (`systemctl status` để kiểm tra). Nếu cần dùng SKey trong lúc chờ, chọn thủ công chế độ **Surrounding Text** hoặc **Preedit** trong Settings GUI.

---

## Gõ tự do & Auto-restore

SKey hỗ trợ gõ tự do kiểu UniKey: tất cả double-letter transform (`dd→đ`, `oo→ô`, `aa→â`, `ee→ê`, `aw→ă`, `ow→ơ`, `ww/uw→ư`) hoạt động ở **mọi vị trí** trong từ, không giới hạn ở đầu âm tiết.

### Gõ viết tắt & tự do dấu

| Gõ | Kết quả | Mô tả |
|----|---------|-------|
| `vcdd` | vcđ | `dd→đ` sau phụ âm |
| `nddm` | nđm | Viết tắt nhiều chữ |
| `aloo` | alô | `oo→ô` sau phụ âm |
| `NDD` | NĐ | Chữ hoa |
| `xij` + `x` | xĩ | Đổi dấu tại chỗ |
| `vãi` + `s` + `f` + `x` | vãi | Đổi dấu liên tục, về đúng dấu cũ |
| `vãi` + `x` + `x` | vaix | Double-tone = undo ra raw form |
| `reboo` + `t` | rebôt → **Space** | Gõ "reboot": oo→ô tạo "rebôt", ấn Space → auto-restore thành "reboot" |

### Cách hoạt động auto-restore

Auto-restore có **hai cơ chế** hoạt động song song, tuân theo triết lý Unikey: transform luôn áp dụng khi gõ, chỉ hoàn nguyên khi chắc chắn không phải tiếng Việt.

#### 1. Real-time (all-ASCII, giữa chừng)

Chạy sau mỗi phím gõ trong `maybeAutoRestoreRealTime`. Chỉ kích hoạt khi kết quả **toàn ASCII** — tức engine tự undo một transform trước đó (vd: tone-key undo). Các ký tự tiếng Việt (ô, â, ê, đ...) được **giữ nguyên** giữa chừng, vì có thể người dùng đang gõ dở một từ tiếng Việt.

| Người dùng gõ | Kết quả giữa chừng | Lý do |
|--------------|---------|-------|
| `address` + `ss` | address | Engine undo sắc → `addres` (all-ASCII) → restore ngay |
| `ook` | ôk | `ô` là ký tự Việt → giữ, chưa restore |
| `vaai` | vâi | `â` là ký tự Việt → giữ |
| `ddc` | đc | `đ` là ký tự Việt → giữ |
| `wood` | wôd | `ô` là ký tự Việt → giữ |

#### 2. Commit-time (khi kết thúc từ)

Chạy khi người dùng ấn **Space, Enter, Tab, dấu câu, hoặc phím modifier**. Gọi `autoRestore()` — kiểm tra toàn bộ từ đã gõ xong. Nếu từ **không hợp lệ trong tiếng Việt** (theo `skey_engine_is_valid()`), hoàn nguyên về raw form, **kể cả khi có ký tự Việt**.

| Gõ | Kết quả khi gõ | Sau Space | Lý do |
|----|---------------|-----------|-------|
| `reboo` + `t` | rebôt | **reboot** | "rebôt" không valid VN → restore về raw |
| `world` | wỏld | **world** | "wỏld" không valid VN → restore |
| `computer` | computẻ | **computer** | "computẻ" không valid VN → restore |
| `đc` + Space | đc | **ddc** | "đc" không valid VN → restore |
| `cô` + Space | cô | **cô** | "cô" valid VN → giữ nguyên |
| `lỗi` + Space | lỗi | **lỗi** | "lỗi" valid VN → giữ nguyên |
| `nđm` + Space | nđm | **nddm** | "nđm" không valid VN → restore |

> 💡 **Mẹo:** Nếu bạn thường xuyên gõ viết tắt (như `nđm`, `vcđ`), hãy **tắt Auto Restore** trong Settings (`fcitx5-skey-settings` → bỏ chọn "Tự động khôi phục"). Khi tắt, mọi transform được giữ nguyên, không bị hoàn nguyên khi kết thúc từ.

#### Cơ chế undo thủ công

Nếu không muốn dùng auto-restore, bạn luôn có thể **undo thủ công** bằng cách gõ lặp trigger key:

| Gõ | Kết quả | Cơ chế |
|----|---------|---------|
| `ooo` | oo | Lặp `o` lần 3 → undo `oo→ô` |
| `ddd` | dd | Lặp `d` lần 3 → undo `dd→đ` |
| `aaa` | aa | Lặp `a` lần 3 → undo `aa→â` |
| `eee` | ee | Lặp `e` lần 3 → undo `ee→ê` |

Undo hoạt động ở mọi vị trí, không phụ thuộc vào auto-restore.

---

## Kiến trúc

### Tổng quan

```
┌─────────────────────────────────────────────────┐
│        Ứng dụng (Chrome, VS Code, Terminal...)  │
└────────────────────┬────────────────────────────┘
                     │ KeyEvent / CommitString
                     ▼
┌─────────────────────────────────────────────────┐
│               fcitx5 Framework                  │
│          (InputContextManager, Addons)          │
└────────────────────┬────────────────────────────┘
                     │
                     ▼
┌─────────────────────────────────────────────────┐
│        SKey Addon  (skey.so — shared library)   │
│                                                 │
│  SKeyEngine ─── SKeyConfig                      │
│      │      ├── A11yMonitor (AT-SPI2 phát hiện  │
│      │      │   thanh địa chỉ Chromium trên X11)│
│      │      └── Settings GUI (fcitx5-skey-settings)│
│      ▼                                          │
│  SKeyState (per-window)                         │
│      │                                          │
│      ├── Output: Preedit / Surrounding Text /   │
│      │           Uinput                         │
│      │              │ (Uinput mode)             │
│      │              ▼                           │
│      │      fcitx5-skey-uinput-server           │
│      │      (tiến trình riêng ↔ /dev/uinput)    │
│      ▼                                          │
│  VietnameseEngine  (wrapper C++)                │
│      │  - post-processing: double-letter        │
│      │    transforms ở non-start position       │
│      │  - tone key dedup (cùng key lặp)        │
│      │  - double-tone undo (xx → raw form)     │
│      │  - auto-restore (real-time + commit-time)│
│      ▼                                          │
│  skey-engine (Rust FFI)                         │
│  ┌─────────────────────────────────────────┐    │
│  │ skey_engine_process_string()            │    │
│  │ skey_engine_is_valid()                  │    │
│  │ skey_engine_set_method() / _reset()     │    │
│  │ skey_engine_set_free_marking()          │    │
│  │ skey_engine_set_std_tone_style()        │    │
│  └─────────────────────────────────────────┘    │
└─────────────────────────────────────────────────┘
```

---

## Debug

SKey ghi log vào `/tmp/skey.log`. Để theo dõi realtime:

```bash
tail -f /tmp/skey.log
```

Log bao gồm: trạng thái activation/deactivation, phím nhấn (old/new composed text), hoạt động surrounding text, deferred commit schedule/flush.

---

## License

Phát hành theo giấy phép [GPL-3.0](LICENSE).

---

## Credits

Xin chân thành cảm ơn:

- **[skey-engine](https://github.com/collyn/skey-engine)** — engine xử lý tiếng Việt (Rust) là trái tim của SKey. Toàn bộ logic biến đổi Telex/VNI, kiểm tra âm tiết và khôi phục từ đều dựa trên skey-engine. Engine được viết và tối ưu riêng cho SKey với C ABI trực tiếp, không cần wrapper trung gian.
- **[bamboo-core](https://github.com/nguyen10t2/bamboo_core)** — engine gốc ban đầu. Cảm ơn tác giả đã xây dựng và chia sẻ một engine gõ tiếng Việt mạnh mẽ và mã nguồn mở. Từ v0.5.4, SKey chuyển sang dùng skey-engine.
- **[fcitx5](https://fcitx-im.org/)** — input method framework cho Linux.
