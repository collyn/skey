<div align="center">

<img src="data/icons/fcitx-skey.svg" alt="SKey" width="128" height="128">

# SKey — Simple Key

**Bộ gõ Tiếng Việt đơn giản, nhẹ, và nhanh cho Linux.**

[![Release](https://img.shields.io/github/v/release/collyn/skey?label=release&sort=semver)](https://github.com/collyn/skey/releases)
[![License](https://img.shields.io/github/license/collyn/skey?label=license)](LICENSE)
[![Downloads](https://img.shields.io/github/downloads/collyn/skey/total?label=downloads)](https://github.com/collyn/skey/releases)

</div>

SKey (Simple Key) là bộ gõ Tiếng Việt cho Linux trên nền tảng [fcitx5](https://fcitx-im.org/), sử dụng engine [bamboo-core](https://github.com/nguyen10t2/bamboo_core) (Rust) qua FFI. Mặc định chạy ở chế độ **Auto** — tự động chọn giữa Surrounding Text và Uinput dựa trên khả năng của ứng dụng. Bộ gõ chạy monolithic không cần server; riêng chế độ Uinput đi kèm một server tùy chọn để tối ưu hóa việc xóa/thay thế chữ.

---

## Tính năng

- **Phương thức gõ:** Telex (tuỳ chọn w→ư, ][→ươ), VNI
- **Auto Mode (mặc định)** — tự động chọn giữa Surrounding Text và Uinput dựa trên capability flags của ứng dụng. Không cần cấu hình thủ công cho từng app.
- **Surrounding Text** — gõ trực tiếp không gạch chân qua API fcitx5. Ổn định trên hầu hết ứng dụng native (Qt/GTK) và web (Chrome/Firefox).
- **Uinput** — gửi phím Backspace qua kernel `/dev/uinput`. Phù hợp cho terminal app (Tabby, Konsole) và ứng dụng không hỗ trợ Surrounding Text API.
- **Preedit** — hiển thị chữ gạch chân. Phù hợp cho thanh địa chỉ Chromium.
- **Per-app Mode Override** — ghi đè chế độ gõ cho từng ứng dụng qua menu phím tắt `` ` `` hoặc Settings UI. Cài đặt được lưu vĩnh viễn.
- **Loại trừ ứng dụng (App Exclusion)** — tắt gõ tiếng Việt cho từng ứng dụng qua menu phím tắt `` ` ``.
- **Auto-restore** — tự động hoàn nguyên từ không hợp lệ (ví dụ: gõ `telegram` giữ nguyên `telegram` thay vì `tẻlegam`).
- **Kiểm tra chính tả** — kiểm tra tính hợp lệ âm tiết theo thời gian thực.
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

Để gõ tiếng Việt trên các ứng dụng AppImage hoặc ứng dụng chạy qua IBus frontend, thêm vào `~/.profile`:

```bash
export GTK_IM_MODULE=fcitx
export QT_IM_MODULE=fcitx
export XMODIFIERS=@im=fcitx
export SDL_IM_MODULE=fcitx
export GLFW_IM_MODULE=ibus
```

Trên KDE Wayland, thêm dòng sau vào `~/.config/environment.d/fcitx.conf` để systemd user session nhận biến môi trường:

```
GTK_IM_MODULE=fcitx
QT_IM_MODULE=fcitx
XMODIFIERS=@im=fcitx
SDL_IM_MODULE=fcitx
GLFW_IM_MODULE=ibus
```

### Từ file .deb

Tải file `.deb` mới nhất từ [GitHub Releases](https://github.com/collyn/skey/releases):

```bash
sudo dpkg -i fcitx5-skey_*.deb
sudo apt install -f  # cài dependencies nếu cần
```

### Build từ source

#### Yêu cầu

- Linux với fcitx5 ≥ 5.0
- CMake ≥ 3.16, Extra CMake Modules (ECM)
- Fcitx5 development headers (`libfcitx5core-dev`)
- Rust toolchain (cargo, rustc)
- GCC/G++ (C++17)
- Qt6 (cho Settings GUI, tùy chọn)
- systemd (cho uinput server, tùy chọn)

#### Cài dependencies

**Ubuntu/Debian:**

```bash
sudo apt install cmake extra-cmake-modules libfcitx5core-dev \
  libfcitx5utils-dev fcitx5 fcitx5-modules \
  qt6-base-dev libqt6svg6-dev libgl1-mesa-dev \
  curl rustc cargo
```

**Fedora:**

```bash
sudo dnf install cmake extra-cmake-modules fcitx5-devel fcitx5 \
  qt6-qtbase-devel qt6-qtsvg-devel \
  rust cargo gcc-c++
```

**Arch Linux:**

```bash
sudo pacman -S cmake extra-cmake-modules fcitx5 \
  qt6-base qt6-svg \
  rust cargo gcc
```

#### Build & Install

```bash
git clone https://github.com/collyn/skey.git
cd skey
mkdir build && cd build
cmake .. -DCMAKE_INSTALL_PREFIX=/usr
make -j$(nproc)
sudo make install
```

Sau khi build và install:

```bash
# Cấu hình fcitx5 profile và biến môi trường
sudo skey-setup

# Restart fcitx5
fcitx5 -r -d
```

Script `skey-setup` sẽ:
1. Thêm SKey vào fcitx5 profile (ActiveByDefault, ShareInputState)
2. Export biến môi trường `GTK_IM_MODULE`, `QT_IM_MODULE`, `XMODIFIERS`, `SDL_IM_MODULE`, `GLFW_IM_MODULE`
3. Trên Wayland + KDE/GNOME: tự động reconnect compositor virtual keyboard
4. Inject biến môi trường fcitx vào systemd user session

#### Build .deb

```bash
./build.sh
```

Tạo file `fcitx5-skey_<version>_amd64.deb` sẵn sàng để cài đặt.

---

## Sử dụng

Sau khi cài đặt + chạy `skey-setup`, SKey là bộ gõ mặc định. Chuyển đổi bằng **Ctrl+Space**.

### Bảng gõ Telex

| Gõ | Kết quả | Mô tả |
|----|---------|-------|
| `aa` | â | Dấu mũ |
| `oo` | ô | Dấu mũ |
| `ee` | ê | Dấu mũ |
| `uw` | ư | Dấu móc |
| `ow` | ơ | Dấu móc |
| `aw` | ă | Dấu trăng |
| `dd` | đ | D đét |
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
- **Tab Info**: phiên bản, kiểm tra cập nhật (tự động tải + cài .deb từ GitHub Releases), **nút khởi động lại Fcitx5** (có xử lý Wayland reconnect)

Mở từ menu ứng dụng hoặc terminal: `fcitx5-skey-settings`

### Chế độ Auto (mặc định)

Chế độ **Auto** tự động chọn giữa Surrounding Text và Uinput khi focus vào một ứng dụng, dựa trên các tín hiệu từ fcitx5 capability flags:

| Điều kiện | Mode được chọn | Ví dụ app |
|-----------|---------------|-----------|
| Có `Terminal` flag | Uinput | Konsole, Alacritty |
| Là Chromium + **không** có `SpellCheck` | Uinput | Tabby (Electron terminal) |
| Có `SurroundingText` capability | Surrounding Text | Chrome, Firefox, Telegram, Antigravity |
| Không có `SurroundingText` capability | Uinput | App X11 cũ |

Nếu Auto chọn Surrounding Text nhưng surrounding text không thực sự hoạt động (cache rỗng sau vài lần gõ), engine tự động hạ cấp xuống Uinput.

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

Đối với chế độ **Surrounding Text** và **Uinput**, SKey sử dụng EWMA (Exponentially Weighted Moving Average) để đo thời gian round-trip thực tế của phím Backspace:
- Tự động tính toán thời gian phản hồi qua các lệnh xóa phím
- Sử dụng công thức tự thích ứng để giảm thời gian chờ xuống mức tối ưu nhất (thường chỉ khoảng ~10-15ms)
- Nếu ứng dụng có hỗ trợ đầy đủ API Surrounding Text gốc (như Qt/GTK), SKey sẽ thực hiện xóa và commit ngay lập tức (0ms)
- Có safety timeout 150ms để tránh freeze nếu BS events bị thất lạc

### Output mode Uinput

Chế độ **Uinput** gửi yêu cầu Backspace trực tiếp đến trình điều khiển hệ thống qua service `fcitx5-skey-uinput-server`. Phương pháp này giúp khắc phục lỗi mất chữ trên một số ứng dụng đặc thù hoặc terminal app không hỗ trợ Surrounding Text.

Bật service trước khi chọn chế độ Uinput:

```bash
sudo systemctl enable --now fcitx5-skey-uinput-server@$USER.service
```

Service chạy dưới quyền root để tương tác với `/dev/uinput`, nhưng nhận tên người dùng từ instance `@$USER` để mở socket đúng quyền người dùng chạy Fcitx5. Kiểm tra trạng thái:

```bash
systemctl status fcitx5-skey-uinput-server@$USER.service
```

Khi server không hoạt động hoặc gặp sự cố mở `/dev/uinput`, SKey tự động chuyển đổi an toàn sang chế độ trì hoãn tự thích ứng của **Surrounding Text** để tránh mất chữ.

---

## Auto-restore thông minh

SKey tự động nhận diện và hoàn nguyên các từ không phải tiếng Việt bằng cách kết hợp cơ chế hoàn nguyên nguyên âm và giữ nguyên phụ âm cuối (`ddFreeStyle`).

### Cách hoạt động

Khi người dùng kết thúc từ (nhấn Space, Enter, hoặc ký tự đặc biệt), SKey kiểm tra:

1. **Từ có biến đổi nguyên âm tiếng Việt không?** (ví dụ: `ee` → `ê`, `ow` → `ơ`)
2. **Kết quả có phải âm tiết tiếng Việt hợp lệ không?** (kiểm tra qua bamboo-core)
3. Nếu có biến đổi nhưng **không hợp lệ** → **hoàn nguyên** về text gốc
4. Nếu chỉ có `dd` → `đ` (không kèm biến đổi nguyên âm) → **ddFreeStyle giữ nguyên**

### Bảng ví dụ

| Người dùng gõ | Kết quả | Lý do |
|--------------|---------|-------|
| `telegram` | telegram | Từ không hợp lệ + có biến đổi nguyên âm → hoàn nguyên |
| `google` | google | Từ không hợp lệ + có biến đổi nguyên âm → hoàn nguyên |
| `facebook` | facebook | Không có biến đổi nào → giữ nguyên |
| `ddc` | đc | Chỉ có `dd` → `đ`, không biến đổi nguyên âm → ddFreeStyle giữ |
| `vieetj` | việt | Âm tiết tiếng Việt hợp lệ → giữ kết quả |

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
│      │                                          │
│      ▼                                          │
│  bamboo-core (Rust FFI)                         │
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

Phát hành theo giấy phép [MIT](LICENSE).

---

## Credits

Xin chân thành cảm ơn:

- **[bamboo-core](https://github.com/nguyen10t2/bamboo_core)** — engine xử lý tiếng Việt (Rust) là trái tim của SKey. Toàn bộ logic biến đổi Telex/VNI, kiểm tra âm tiết và khôi phục từ đều dựa trên bamboo-core. Cảm ơn tác giả đã xây dựng và chia sẻ một engine gõ tiếng Việt mạnh mẽ và mã nguồn mở.
- **[fcitx5](https://fcitx-im.org/)** — input method framework cho Linux.

