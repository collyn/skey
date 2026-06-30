# SKey — Simple Key

**Bộ gõ Tiếng Việt đơn giản, nhẹ, và nhanh cho Linux.**

SKey (Simple Key) là bộ gõ Tiếng Việt cho Linux, xây dựng trên nền tảng [fcitx5](https://fcitx-im.org/). Lấy cảm hứng từ [fcitx5-lotus](https://github.com/LotusInputMethod/fcitx5-lotus), SKey sử dụng engine [bamboo-core](https://github.com/nickolasburr/bamboo-core) (Rust) qua FFI — mặc định chạy monolithic không cần server; riêng output mode Uinput có server tùy chọn để thay thế text mượt hơn.

---

## Tính năng

- 🇻🇳 **Phương thức gõ:** Telex, Telex W, VNI
- ✨ **Surrounding Text** — gõ trực tiếp, không gạch chân, mượt mà như Windows/macOS
- 📝 **Preedit** — gạch chân, tương thích cao với mọi ứng dụng
- 🔄 **Auto-restore thông minh** — tự động hoàn nguyên từ không phải tiếng Việt (`telegram` vẫn là `telegram`), giữ nguyên viết tắt tiếng Việt (`đc`, `đk`) nhờ ddFreeStyle
- ✅ **Kiểm tra tính hợp lệ** theo thời gian thực qua bamboo-core
- 🎯 **Vị trí dấu thanh:** Kiểu mới (hoà) và Kiểu cũ (hòa)
- 🖱️ **Menu system tray** để chuyển đổi nhanh các tùy chọn
- ⏱️ **Deferred commit (80ms)** chống nhấp nháy khi thay thế text
- ⚡ **Uinput mode** dùng server tùy chọn kiểu Lotus để tránh delay 80ms khi replace text
- 🚀 **Nhẹ:** mặc định một file `.so`, không daemon, không cần `/dev/uinput`
- ⚙️ **Cấu hình** qua `fcitx5-configtool` hoặc menu tray

---

## Cài đặt

### Từ file .deb (Ubuntu/Debian)

Tải file `.deb` mới nhất từ [GitHub Releases](https://github.com/collyn/skey/releases):

```bash
sudo dpkg -i fcitx5-skey_*.deb
sudo apt install -f  # cài dependencies nếu cần
```

Package tự động chạy `skey-setup` để cấu hình fcitx5 — bộ gõ SKey sẵn sàng sử dụng ngay.

### Build từ source

#### Yêu cầu

- Linux với fcitx5
- CMake ≥ 3.16, Extra CMake Modules (ECM)
- Fcitx5 development headers
- Rust toolchain (cargo)
- GCC/G++ (C++17)

#### Cài dependencies

**Ubuntu/Debian:**

```bash
sudo apt install cmake extra-cmake-modules libfcitx5core-dev fcitx5
```

**Fedora:**

```bash
sudo dnf install cmake extra-cmake-modules fcitx5-devel fcitx5
```

**Arch Linux:**

```bash
sudo pacman -S cmake extra-cmake-modules fcitx5
```

#### Build & Install

```bash
git clone https://github.com/collyn/skey.git
cd skey
mkdir build && cd build
cmake .. -DCMAKE_INSTALL_PREFIX=/usr
make -j$(nproc)
sudo make install
skey-setup
```

Script `skey-setup` sẽ cấu hình fcitx5 profile, bật `ActiveByDefault`, `ShareInputState`, và tự động restart fcitx5.

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
| `uw` hoặc `w` | ư | Dấu móc |
| `ow` | ơ | Dấu móc |
| `aw` | ă | Dấu trăng |
| `dd` | đ | D đét |
| `s` | dấu sắc | á, é, ó... |
| `f` | dấu huyền | à, è, ò... |
| `r` | dấu hỏi | ả, ẻ, ỏ... |
| `x` | dấu ngã | ã, ẽ, õ... |
| `j` | dấu nặng | ạ, ẹ, ọ... |
| `z` | xóa dấu | bỏ dấu thanh |

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

Mở **fcitx5-configtool** hoặc sử dụng **menu tray** để thay đổi cài đặt:

| Tùy chọn | Giá trị | Mô tả |
|----------|---------|-------|
| **Input Method** | Telex / Telex W / VNI | Phương thức gõ |
| **Output Mode** | Surrounding Text / Preedit / Surrounding Text (slow) / Uinput | Chế độ hiển thị text đang gõ |
| **Tone Mark Position** | Modern (hoà) / Traditional (hòa) | Vị trí đặt dấu thanh |
| **Free Marking** | true / false | Cho phép đặt dấu tự do |
| **Auto Restore** | true / false | Tự động hoàn nguyên từ không phải tiếng Việt |
| **Show Preedit** | true / false | Hiển thị text đang soạn |

File cấu hình lưu tại `~/.config/fcitx5/conf/skey.conf`. Thay đổi có hiệu lực ngay sau khi save.

### Output mode Uinput

Mode **Uinput** dùng cùng ý tưởng với fcitx5-lotus: addon gửi số lần Backspace cho `fcitx5-skey-uinput-server` qua Unix socket, server phát Backspace bằng `/dev/uinput`, rồi addon commit phần text mới ngay khi nhận Backspace cuối. Cách này tránh delay 80ms cố định của **Surrounding Text (slow)**.

Bật service trước khi chọn mode Uinput:

```bash
sudo systemctl enable --now fcitx5-skey-uinput-server@$USER.service
```

Service chạy root để mở `/dev/uinput`, nhưng nhận username từ instance `@$USER` để tạo socket đúng user đang chạy fcitx5. Kiểm tra trạng thái:

```bash
systemctl status fcitx5-skey-uinput-server@$USER.service
```

Khi server không chạy hoặc không mở được `/dev/uinput`, SKey tự fallback về đường deferred commit 80ms để tránh mất text.

---

## Auto-restore thông minh

SKey tự động nhận diện và hoàn nguyên các từ không phải tiếng Việt, dựa trên cách tiếp cận của fcitx5-lotus: **autoNonVnRestore** kết hợp **ddFreeStyle**.

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
│       │                                         │
│       ▼                                         │
│  SKeyState (per-window)                         │
│       │                                         │
│   ┌───┴───┐                                     │
│   │       │                                     │
│   ▼       ▼                                     │
│ Preedit  Surrounding Text                       │
│  Mode      Mode                                 │
│   │       │                                     │
│   └───┬───┘                                     │
│       ▼                                         │
│  bamboo-core (Rust FFI)                         │
│  ┌─────────────────────────────────────────┐    │
│  │ bamboo_engine_process_key_buf()         │    │
│  │ skey_engine_is_valid()                  │    │
│  │ skey_engine_restore_last_word()         │    │
│  └─────────────────────────────────────────┘    │
└─────────────────────────────────────────────────┘
```

### So sánh với fcitx5-lotus

| | fcitx5-lotus | SKey |
|---|---|---|
| **Kiến trúc** | Client-Server (addon + server daemon) | Monolithic addon mặc định; Uinput mode có server nhỏ tùy chọn |
| **Engine** | bamboo-core (Go/C) | bamboo-core (Rust FFI) |
| **Server riêng** | Có (daemon process) | Chỉ cần khi dùng Uinput mode |
| **Giao tiếp** | IPC (pipe/socket) | Direct function call; Uinput mode dùng Unix socket cho Backspace |
| **Quyền `/dev/uinput`** | Cần | Chỉ cần với `fcitx5-skey-uinput-server` |
| **Systemd service** | Cần | Không bắt buộc |
| **Auto-restore** | autoNonVnRestore + ddFreeStyle | autoNonVnRestore + ddFreeStyle |
| **Deferred commit** | Có | Có (80ms timer) + Uinput mode không dùng timer khi server sẵn sàng |
| **Cài đặt** | Phức tạp (server + service) | `make install` + `skey-setup`; bật systemd service nếu dùng Uinput |

---

## Debug

SKey ghi log vào `/tmp/skey.log`. Để theo dõi realtime:

```bash
tail -f /tmp/skey.log
```

Log bao gồm: trạng thái activation/deactivation, phím nhấn (old/new composed text), hoạt động surrounding text, deferred commit schedule/flush.

---

## License

[GPL-3.0](https://www.gnu.org/licenses/gpl-3.0.html)

---

## Credits

- [fcitx5-lotus](https://github.com/LotusInputMethod/fcitx5-lotus) — nguồn cảm hứng kiến trúc và cơ chế auto-restore
- [bamboo-core](https://github.com/nickolasburr/bamboo-core) — engine xử lý tiếng Việt (Rust)
- [fcitx5](https://fcitx-im.org/) — input method framework cho Linux
