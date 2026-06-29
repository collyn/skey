# SKey - Bộ gõ Tiếng Việt cho Linux

SKey là bộ gõ Tiếng Việt cho Linux, xây dựng trên nền tảng [fcitx5](https://fcitx-im.org/). Lấy cảm hứng từ [fcitx5-lotus](https://github.com/LotusInputMethod/fcitx5-lotus), SKey sử dụng kiến trúc đơn giản hơn bằng cách nhúng engine Telex/VNI trực tiếp vào addon.

## Tính năng

- 🇻🇳 Hỗ trợ phương thức gõ **Telex** và **VNI**
- ✨ Chế độ **Surrounding Text** (không gạch chân, mượt mà)
- 📝 Chế độ **Preedit** (gạch chân, tương thích cao)
- ⚙️ Cấu hình qua giao diện fcitx5
- 🔤 Hỗ trợ đặt dấu thanh theo **quy tắc mới** (hoà) và **quy tắc cũ** (hòa)
- 🚀 Nhẹ, không cần server daemon riêng

## Yêu cầu hệ thống

- Linux với fcitx5
- CMake ≥ 3.16
- Extra CMake Modules (ECM)
- Fcitx5 development headers (`libfcitx5core-dev` hoặc `fcitx5-devel`)
- GCC/G++ (C++17)

### Cài đặt dependencies

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

## Build & Cài đặt

```bash
# Clone repository
git clone https://github.com/your-username/fcitx5-skey.git
cd fcitx5-skey

# Build
mkdir build && cd build
cmake .. -DCMAKE_INSTALL_PREFIX=/usr
make -j$(nproc)

# Cài đặt
sudo make install

# Khởi động lại fcitx5
fcitx5 -r &
```

### Cài đặt cho user (không cần sudo)

```bash
cmake .. -DCMAKE_INSTALL_PREFIX=$HOME/.local
make -j$(nproc)
make install
fcitx5 -r &
```

## Sử dụng

1. Mở **fcitx5-configtool**
2. Thêm **SKey Vietnamese** vào danh sách Input Method
3. Chuyển đổi bộ gõ bằng phím tắt (mặc định: `Ctrl+Space`)

### Telex

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

### VNI

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

## Cấu hình

Mở fcitx5-configtool → Chọn SKey → Configure:

- **Input Method**: Telex hoặc VNI
- **Output Mode**: Surrounding Text (không gạch chân) hoặc Preedit (gạch chân)
- **Tone Mark Position**: Modern (hoà) hoặc Traditional (hòa)
- **Free Marking**: Cho phép đặt dấu tự do
- **Auto Restore**: Tự động khôi phục khi gõ sai tiếng Việt
- **Show Preedit**: Hiển thị text đang soạn

## Kiến trúc

```
Ứng dụng → Fcitx5 → SKeyEngine → SKeyState → VietnameseEngine
                                                    ↓
                                        Surrounding Text / Preedit
                                                    ↓
                                              Ứng dụng nhận text
```

Khác với fcitx5-lotus sử dụng kiến trúc client-server (addon + server riêng), SKey nhúng toàn bộ engine xử lý trực tiếp vào addon fcitx5, giúp:
- Không cần cài đặt server daemon
- Không cần quyền `/dev/uinput`
- Không cần systemd service
- Cài xong dùng ngay

## License

GPL-3.0
