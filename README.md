# SKey — Simple Key

**Bộ gõ Tiếng Việt đơn giản, nhẹ, và nhanh cho Linux.**

SKey (Simple Key) là bộ gõ Tiếng Việt cho Linux trên nền tảng [fcitx5](https://fcitx-im.org/), sử dụng engine [bamboo-core](https://github.com/nguyen10t2/bamboo_core) (Rust) qua FFI. Mặc định bộ gõ chạy monolithic không cần server; riêng chế độ Uinput đi kèm một server tùy chọn để tối ưu hóa việc xóa/thay thế chữ.

---

## Tính năng

- **Phương thức gõ:** Telex (tuỳ chọn w→ư, ][→ươ), VNI
- **Surrounding Text** — gõ trực tiếp không gạch chân. Tự động chuyển đổi giữa chế độ nhanh (Qt/GTK) và trì hoãn tự thích ứng (Electron/D-Bus).
- **Preedit** — hiển thị chữ gạch chân.
- **Loại trừ ứng dụng (App Exclusion)** — tắt gõ tiếng Việt cho từng ứng dụng qua menu phím tắt `` ` ``.
- **Auto-restore** — tự động hoàn nguyên từ không hợp lệ (ví dụ: gõ `telegram` giữ nguyên `telegram` thay vì `tẻlegam`).
- **Kiểm tra chính tả** — kiểm tra tính hợp lệ âm tiết theo thời gian thực.
- **Vị trí dấu thanh:** Kiểu mới (hoà) hoặc kiểu cũ (hòa).
- **Menu cấu hình** — chuyển đổi nhanh tùy chọn qua phím tắt `` ` `` và menu hệ thống.
- **Nhẹ & Gọn** — mặc định là một thư viện liên kết động (.so), không chạy daemon ngầm trừ khi bật chế độ Uinput.
- **Cấu hình** — thiết lập qua `fcitx5-configtool` hoặc menu hệ thống.

---

## Cài đặt

### APT Repository (khuyến nghị)

Thêm APT repository để tự động nhận cập nhật qua `apt update`:

```bash
curl -fsSL https://collyn.github.io/skey/install.sh | sudo bash
sudo apt install fcitx5-skey
```

Package tự động chạy `skey-setup` để cấu hình fcitx5 — bộ gõ SKey sẵn sàng sử dụng ngay.

### Từ file .deb

Tải file `.deb` mới nhất từ [GitHub Releases](https://github.com/collyn/skey/releases):

```bash
sudo dpkg -i fcitx5-skey_*.deb
sudo apt install -f  # cài dependencies nếu cần
```

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
| **Input Method** | Telex / VNI | Phương thức gõ |
| **Gõ w thành ư** | true / false | Chỉ Telex: gõ w đơn lẻ ra ư |
| **Gõ ][ thành ư ơ** | true / false | Chỉ Telex: gõ [ ra ơ, ] ra ư (giống UniKey) |
| **Output Mode** | Surrounding Text / Preedit / Uinput | Chế độ hiển thị text đang gõ |
| **Tone Mark Position** | Modern (hoà) / Traditional (hòa) | Vị trí đặt dấu thanh |
| **Free Marking** | true / false | Cho phép đặt dấu tự do |
| **Auto Restore** | true / false | Tự động hoàn nguyên từ không phải tiếng Việt |
| **Show Preedit** | true / false | Hiển thị text đang soạn |

File cấu hình lưu tại `~/.config/fcitx5/conf/skey.conf`. Thay đổi có hiệu lực ngay sau khi save.

### Chế độ Trì hoãn Tự Thích ứng (Adaptive Delay)
Đối với chế độ **Surrounding Text** trên các ứng dụng D-Bus (như các phần mềm viết bằng Electron/Chromium như VS Code, Discord, Chrome), SKey không sử dụng độ trễ cố định (80ms). Thay vào đó:
- Nó tự động tính toán thời gian phản hồi thực tế (Round-Trip Time) thông qua các lệnh xóa phím.
- Sử dụng công thức tự thích ứng để giảm thời gian chờ xuống mức tối ưu nhất (thường chỉ khoảng ~10-15ms).
- Nếu ứng dụng có hỗ trợ đầy đủ API Surrounding Text gốc (như Qt/GTK), SKey sẽ thực hiện xóa và commit ngay lập tức (0ms).

### Output mode Uinput

Chế độ **Uinput** gửi yêu cầu Backspace trực tiếp đến trình điều khiển hệ thống qua dịch vụ `fcitx5-skey-uinput-server`. Phương pháp này giúp khắc phục lỗi mất chữ trên một số ứng dụng đặc thù hoặc game chặn phím ảo từ D-Bus/X11.

Bật service trước khi chọn chế độ Uinput:

```bash
sudo systemctl enable --now fcitx5-skey-uinput-server@$USER.service
```

Dịch vụ chạy dưới quyền root để tương tác với `/dev/uinput`, nhưng nhận tên người dùng từ instance `@$USER` để mở socket đúng quyền người dùng chạy Fcitx5. Kiểm tra trạng thái:

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

- [bamboo-core](https://github.com/nguyen10t2/bamboo_core) — engine xử lý tiếng Việt (Rust)
- [fcitx5](https://fcitx-im.org/) — input method framework cho Linux
