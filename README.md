# SKey — Bộ gõ Tiếng Việt cho Linux

SKey là bộ gõ Tiếng Việt cho Linux, xây dựng trên nền tảng [fcitx5](https://fcitx-im.org/). Lấy cảm hứng từ [fcitx5-lotus](https://github.com/LotusInputMethod/fcitx5-lotus), SKey sử dụng kiến trúc đơn giản hơn bằng cách nhúng engine Telex/VNI trực tiếp vào addon fcitx5 — không cần server daemon riêng, không cần `/dev/uinput`.

## Tính năng

- 🇻🇳 Hỗ trợ phương thức gõ **Telex** và **VNI**
- ✨ Chế độ **Surrounding Text** (không gạch chân, mượt mà, tương tự gõ trên Windows/macOS)
- 📝 Chế độ **Preedit** (gạch chân, tương thích cao với mọi ứng dụng)
- ⚙️ Cấu hình qua giao diện fcitx5-configtool
- 🔤 Hỗ trợ đặt dấu thanh theo **quy tắc mới** (hoà) và **quy tắc cũ** (hòa)
- 🚀 Nhẹ, không cần server daemon riêng, không cần quyền `/dev/uinput`

---

## Kiến trúc hệ thống

### Tổng quan

```
┌─────────────────────────────────────────────────────────┐
│                     Ứng dụng (Chrome, VS Code, ...)      │
└──────────────────────┬──────────────────────────────────┘
                       │ KeyEvent / CommitString / Preedit
                       ▼
┌─────────────────────────────────────────────────────────┐
│                      fcitx5 Framework                    │
│  ┌──────────────────────────────────────────────────┐   │
│  │          InputContextManager                      │   │
│  │  (quản lý per-window InputContext)               │   │
│  └──────────────────────────────────────────────────┘   │
└──────────────────────┬──────────────────────────────────┘
                       │ Forward key events
                       ▼
┌─────────────────────────────────────────────────────────┐
│              SKey Addon (Shared Library: skey.so)        │
│                                                         │
│  ┌─────────────────────────────────────────────────┐    │
│  │         SKeyEngineFactory (AddonFactory)         │    │
│  │         - Đăng ký addon với fcitx5               │    │
│  │         - Tạo SKeyEngine instance                │    │
│  └──────────────────────┬──────────────────────────┘    │
│                         │                               │
│                         ▼                               │
│  ┌─────────────────────────────────────────────────┐    │
│  │           SKeyEngine (InputMethodEngineV2)       │    │
│  │  - Route key events → SKeyState                 │    │
│  │  - Quản lý configuration (SKeyConfig)            │    │
│  │  - Handle activate/deactivate/reset/save         │    │
│  │  - FactoryFor<SKeyState>: tạo state per context  │    │
│  └──────────────────────┬──────────────────────────┘    │
│                         │                               │
│                         ▼                               │
│  ┌─────────────────────────────────────────────────┐    │
│  │       SKeyState (InputContextProperty)           │    │
│  │  - Mỗi cửa sổ/ứng dụng có 1 instance riêng       │    │
│  │  - Xử lý keyEvent chính (phím đặc biệt, chữ...) │    │
│  │  - Điều phối 2 chế độ output                     │    │
│  │  - Deferred commit timer (80ms)                  │    │
│  └──────────┬──────────────┬───────────────────────┘    │
│             │              │                             │
│     ┌───────▼──┐    ┌──────▼────────┐                   │
│     │ Preedit  │    │ Surrounding   │                   │
│     │ Mode     │    │ Text Mode     │                   │
│     │ (gạch    │    │ (gõ trực tiếp,│                   │
│     │  chân)   │    │  không gạch)  │                   │
│     └───────┬──┘    └──────┬────────┘                   │
│             │              │                             │
│             └──────┬───────┘                             │
│                    ▼                                     │
│  ┌─────────────────────────────────────────────────┐    │
│  │    skey::VietnameseEngine (Core Logic)           │    │
│  │    - Xử lý Telex/VNI input rules                 │    │
│  │    - Quản lý trạng thái soạn thảo (rawInput_)    │    │
│  │    - Recompose: rawInput → VChar[] → UTF-8       │    │
│  │    - Tone placement (Modern / Traditional)       │    │
│  │    - Unicode lookup tables                       │    │
│  └─────────────────────────────────────────────────┘    │
│                                                         │
│  ┌─────────────────────────────────────────────────┐    │
│  │       SKeyConfig (Configuration)                 │    │
│  │       - InputMethod (Telex/VNI)                  │    │
│  │       - OutputMode (SurroundingText/Preedit)     │    │
│  │       - TonePosition (Modern/Traditional)         │    │
│  │       - FreeMarking, AutoRestore, ShowPreedit    │    │
│  └─────────────────────────────────────────────────┘    │
└─────────────────────────────────────────────────────────┘
```

### Cấu trúc thư mục

```
fcitx5-skey/
├── CMakeLists.txt              # Root build file: tìm dependencies, add subdirectories
├── src/
│   ├── CMakeLists.txt          # Build config cho shared library skey.so
│   ├── engine.h                # Khai báo SKeyEngine, SKeyState, SKeyEngineFactory
│   ├── engine.cpp              # Triển khai fcitx5 integration + key event handling
│   ├── vietnamese.h            # Khai báo VietnameseEngine, VChar, enums
│   ├── vietnamese.cpp          # Core: Unicode table, recompose, tone placement
│   ├── config.h                # SKeyConfig: schema cấu hình fcitx5
│   ├── skey-addon.conf.in      # Template: metadata đăng ký addon với fcitx5
│   └── skey-im.conf.in         # Template: metadata đăng ký input method với fcitx5
├── data/
│   ├── CMakeLists.txt          # Cài đặt icon
│   └── icons/
│       └── fcitx-skey.svg      # Icon cho input method
├── po/                         # Thư mục cho translations (i18n)
└── build/                      # Build output (skey.so, config files)
```

### Phân tầng chi tiết

---

## Tầng 1: fcitx5 Integration (`engine.h` / `engine.cpp`)

### SKeyEngineFactory

```cpp
class SKeyEngineFactory : public AddonFactory {
    AddonInstance *create(AddonManager *manager) override;
};
```

- Điểm entry của addon, được fcitx5 gọi khi load shared library.
- Đăng ký translation domain `fcitx5-skey` cho i18n.
- Tạo một instance `SKeyEngine` duy nhất cho toàn bộ session.

**Đăng ký qua macro:**
```cpp
FCITX_ADDON_FACTORY(SKeyEngineFactory);
```

### SKeyEngine

```cpp
class SKeyEngine : public InputMethodEngineV2 { ... };
```

- Kế thừa `InputMethodEngineV2` — interface chính của fcitx5 cho input method engine.
- **Quản lý configuration**: `SKeyConfig config_` — load/save qua `readAsIni`/`saveAsIni`.
- **Factory pattern**: `FactoryFor<SKeyState> factory_` — tự động tạo một `SKeyState` cho mỗi `InputContext` (mỗi cửa sổ ứng dụng).
- **Lifecycle methods**:
  - `keyEvent()` → forward tới `SKeyState::keyEvent()`
  - `activate()` → reset state khi user chuyển sang bộ gõ này
  - `deactivate()` → flush deferred commit + commit buffer + clear UI
  - `reset()` → xóa state nhưng giữ deferred commit nếu có
  - `subMode()` → hiển thị label "Telex" hoặc "VNI" trên status bar

### SKeyState

```cpp
class SKeyState : public InputContextProperty { ... };
```

Đây là **trái tim của addon** — mỗi instance gắn với **một cửa sổ/ứng dụng**, giữ trạng thái soạn thảo riêng biệt.

**State members:**
| Member | Mô tả |
|--------|-------|
| `engine_` | Con trỏ về SKeyEngine (config, instance) |
| `ic_` | InputContext hiện tại (giao tiếp với ứng dụng) |
| `viet_` | `skey::VietnameseEngine` — engine xử lý tiếng Việt |
| `committedLen_` | Số ký tự Unicode đã commit trong SurroundingText mode |
| `deferredCommitTimer_` | Timer 80ms để trì hoãn commit (chống flicker) |
| `deferredCommitText_` | Text đang chờ commit (dùng với timer) |
| `deferredPrefix_` | Phần prefix ổn định không thay đổi |

**Luồng xử lý phím — `keyEvent()`:**

```
keyEvent(KeyEvent)
  │
  ├─ filter: bỏ qua key release
  │
  ├─ Kiểm tra deferred commit pending
  │   └─ Nếu key mới là letter/VNI digit → merge vào deferred
  │   └─ Nếu không → flush deferred commit ngay
  │
  ├─ Modifier keys (Ctrl, Alt, Meta, Super)
  │   └─ Nếu đang compose: flush commit + reset state → pass through
  │
  ├─ Backspace (đang compose)
  │   ├─ SurroundingText: delete surrounding 1 char
  │   └─ Preedit: viet_.backspace() + updatePreedit()
  │
  ├─ Escape → reset tất cả
  ├─ Enter / Space / Tab → commit buffer + kết thúc từ
  │
  └─ Printable ASCII [a-z][A-Z][0-9]
      ├─ Letter hoặc VNI digit: viet_.processKey(ch)
      │   ├─ Committed → commitString + clear committed buffer
      │   ├─ SurroundingText: surroundingCommit(old, new)
      │   └─ Preedit: updatePreedit()
      └─ Non-letter: commit buffer + pass through
```

---

## Tầng 2: Vietnamese Processing Engine (`vietnamese.h` / `vietnamese.cpp`)

### Data Model

```
rawInput_ (string)               "chuwas" (người dùng gõ)
       │
       ▼  recompose()
chars_ (vector<VChar>)           [{c,h},{u,w},{a,s}]
       │
       ▼  getComposed()
UTF-8 string                     "chứa" (kết quả hiển thị)
```

### VChar

```cpp
struct VChar {
    char base = 0;          // Ký tự gốc: a, e, i, o, u, y, d, ...
    Mark mark = Mark::None; // Dấu phụ: Hat(â), Breve(ă), Horn(ơ), Stroke(đ)
    Tone tone = Tone::None; // Thanh: Sac, Huyen, Hoi, Nga, Nang
    bool upper = false;
};
```

Mỗi ký tự tiếng Việt được biểu diễn phân tách thành 3 thành phần (base + mark + tone), cho phép áp dụng/xóa từng thành phần độc lập.

### Unicode Lookup Table

```cpp
kVowelTable[6][4][6]  // [base_vowel][mark][tone] → Unicode code point
```

- **6 base vowels**: a(0), e(1), i(2), o(3), u(4), y(5)
- **4 marks**: None(0), Hat(1), Breve(2), Horn(3)
- **6 tones**: None(0), Sac(1), Huyen(2), Hoi(3), Nga(4), Nang(5)
- Có bảng riêng cho lowercase (`kVowelTable`) và uppercase (`kVowelTableUpper`)
- Giá trị `0` = tổ hợp không hợp lệ (vd: Horn trên 'a')

### VietnameseEngine

```cpp
class VietnameseEngine {
    std::string rawInput_;         // Chuỗi người dùng gõ thực tế
    std::vector<VChar> chars_;     // Mảng ký tự đã phân tích
    std::string committed_;        // Text đã auto-commit
    int vowelStart_, vowelEnd_;   // Vị trí cụm nguyên âm
    int consonantEnd_;             // Vị trí phụ âm cuối
};
```

### Thuật toán Recompose — `recompose()`

Đây là thuật toán **cốt lõi**: xây dựng lại `chars_` từ `rawInput_` mỗi khi có thay đổi.

```
1. Duyệt từng ký tự trong rawInput_
   │
   ├─ Telex mode:
   │   ├─ dd → đ, aa → â, ee → ê, oo → ô
   │   ├─ aw → ă, ow → ơ, uw → ư
   │   ├─ s/f/r/x/j → tone (sắc/huyền/hỏi/ngã/nặng)
   │   │   └─ Nếu tone đã tồn tại → undo (bỏ dấu)
   │   ├─ z → xóa dấu (remove tone)
   │   └─ w standalone → horn cho o/u gần nhất
   │
   ├─ VNI mode:
   │   ├─ 1/2/3/4/5 → tone (sắc/huyền/hỏi/ngã/nặng)
   │   ├─ 6 → circumflex (â, ê, ô)
   │   ├─ 7 → horn (ơ, ư)
   │   ├─ 8 → breve (ă)
   │   ├─ 9 → stroke (đ)
   │   └─ 0 → xóa tất cả marks và tones
   │
   └─ Ký tự thường → thêm VChar vào tempChars

2. parseSyllable() — xác định cấu trúc âm tiết
   ├─ Tìm vowelStart_ (nguyên âm đầu tiên)
   ├─ Tìm vowelEnd_ (nguyên âm cuối trong cụm)
   └─ Tìm consonantEnd_ (phụ âm cuối nếu có)
```

### Tone Placement — `findToneTarget()`

Quy tắc đặt thanh trong tiếng Việt:

| Trường hợp | Vị trí đặt thanh | Ví dụ |
|------------|-----------------|-------|
| 1 nguyên âm | Nguyên âm đó | `tôi` → dấu ^ trên ô |
| Có nguyên âm mang dấu phụ (â,ê,ô,ơ,ư,ă) | Nguyên âm đó | `hoà` → dấu \` trên o (Modern) |
| 2 nguyên âm + phụ âm cuối | Nguyên âm thứ 2 | `chiến` → dấu ´ trên ê |
| 2 nguyên âm không phụ âm cuối | Nguyên âm thứ nhất | `chào` → dấu \` trên a |
| oa, oe, uy | Nguyên âm thứ 2 | `loà` → dấu \` trên a |
| 3 nguyên âm trở lên | Nguyên âm giữa | `tươi` → dấu ˀ trên ơ |

---

## Tầng 3: Hai chế độ Output

### Preedit Mode (chế độ gạch chân)

```
User gõ             → Preedit hiển thị         → Commit khi hoàn thành
"tieengs"           → tiếng (gạch chân)         → "tiếng"
```

- Sử dụng fcitx5 `InputPanel` API để hiển thị text đang soạn với gạch chân.
- `updatePreedit()` cập nhật preedit với text từ `viet_.getComposed()`.
- `commitBuffer()` gọi `ic_->commitString()` để gửi text cuối cùng.
- **Ưu điểm**: Tương thích với mọi ứng dụng.
- **Nhược điểm**: Hiển thị gạch chân, có thể gây khó chịu với một số ứng dụng.

### SurroundingText Mode (chế độ không gạch chân, mặc định)

```
User gõ "t"     → commit "t"         (buffer: "t")
User gõ "i"     → commit "i"         (buffer: "ti")
User gõ "e"     → commit "e"         (buffer: "tie")
User gõ "n"     → commit "n"         (buffer: "tien")
User gõ "g"     → commit "g"         (buffer: "tieng")
User gõ "s"     → delete 5 + commit "tiếng"  (buffer: "tiếng")
```

**Cơ chế thay thế (replacement):**
1. Lưu text đã commit trước đó (`oldComposed`).
2. Tính toán text mới (`newComposed`).
3. Xóa phần suffix của old không còn trong new (dùng `deleteSurroundingText` hoặc forward BackSpace).
4. Commit phần mới được thêm vào (dùng `commitString`).

**Deferred Commit (commit trì hoãn):**
- Khi cần xóa text cũ và commit text mới, thay vì commit ngay, sử dụng timer 80ms.
- Nếu có key mới đến trong 80ms và có thể merge → cập nhật deferred text thay vì commit.
- Nếu key không merge được → flush commit ngay.
- **Mục đích**: Tránh flicker khi thay thế text nhiều lần liên tiếp (vd: `hoa` → `hòa` → `hoà`).

```
Timeline:
  0ms: user gõ "f"  → schedule deferred "òa" (sau khi delete "oa")
 30ms: user gõ "f" lần nữa → update deferred "oà"
 80ms: timer fire → commit "oà"
```

---

## Luồng dữ liệu đầy đủ

### Ví dụ: Gõ "chào" bằng Telex

```
Bước 1: User gõ "c"
  keyEvent → sym='c' → letter → viet_.processKey('c')
  rawInput_ = "c"
  recompose() → chars_ = [{c}]
  getComposed() = "c"
  SurroundingText: commitString("c"), committedLen_ = 1

Bước 2: User gõ "h"
  keyEvent → sym='h' → letter → viet_.processKey('h')
  rawInput_ = "ch"
  recompose() → chars_ = [{c},{h}]
  getComposed() = "ch"
  isSimpleAppend: commit "h", committedLen_ = 2

Bước 3: User gõ "a"
  keyEvent → sym='a' → letter → viet_.processKey('a')
  rawInput_ = "cha"
  recompose() → chars_ = [{c},{h},{a}]
  getComposed() = "cha"
  isSimpleAppend: commit "a", committedLen_ = 3

Bước 4: User gõ "o"
  keyEvent → sym='o' → letter → viet_.processKey('o')
  rawInput_ = "chao"
  recompose() → chars_ = [{c},{h},{a},{o}]
  vowelStart_=2, vowelEnd_=3
  getComposed() = "chao"
  isSimpleAppend: commit "o", committedLen_ = 4

Bước 5: User gõ "f" (dấu huyền)
  keyEvent → sym='f' → letter → viet_.processKey('f')
  rawInput_ = "chaof"
  recompose():
    - Parse "c", "h", "a", "o" into tempChars
    - "f" → tone Huyen
    - findToneTarget(): 2 vowels (a,o) không phụ âm cuối → target=vowelStart_=2
    - chars_[2].tone = Huyen
  oldComposed = "chao", newComposed = "chào"
  commonPrefix = "ch" (2 bytes)
  deletedPart = "ao" (2 chars), addedPart = "ào" (2 chars)
  deleteSurroundingText(-2, 2) → xóa "ao"
  scheduleDeferredCommit("ào") → timer 80ms
  80ms sau: commitString("ào") → "chào"
```

---

## Tầng 4: Configuration (`config.h`)

```cpp
FCITX_CONFIGURATION(
    SKeyConfig,
    Option<SKeyInputMethod> inputMethod{...};    // Telex | VNI
    Option<SKeyOutputMode> outputMode{...};      // SurroundingText | Preedit
    Option<TonePosition> tonePosition{...};      // Modern | Traditional
    Option<bool> freeMarking{...};               // true | false
    Option<bool> autoRestore{...};               // true | false
    Option<bool> showPreedit{...};              // true | false
);
```

- Sử dụng macro `FCITX_CONFIGURATION` của fcitx5 để tự động sinh GUI cấu hình.
- Lưu trong `~/.config/fcitx5/conf/skey.conf` (định dạng INI).
- Thay đổi cấu hình có hiệu lực ngay sau khi save.

---

## So sánh kiến trúc với fcitx5-lotus

| | fcitx5-lotus | SKey |
|---|---|---|
| **Kiến trúc** | Client-Server (addon + server daemon) | Monolithic addon |
| **Server riêng** | Có (Python/C++ process) | Không |
| **Giao tiếp** | IPC (pipe/socket) | Direct function call |
| **Quyền `/dev/uinput`** | Cần (để forward key) | Không cần |
| **Systemd service** | Cần | Không cần |
| **Độ phức tạp** | Cao (2 processes, IPC, retry logic) | Thấp (1 shared library) |
| **SurroundingText** | Có (forward key engine) | Có (native API + fallback) |
| **Deferred commit** | Có (tối ưu IPC) | Có (tránh flicker) |
| **Cài đặt** | Phức tạp (server + service) | `make install` + restart |

---

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

| Tùy chọn | Giá trị | Mô tả |
|----------|---------|-------|
| **Input Method** | Telex / VNI | Phương thức gõ |
| **Output Mode** | Surrounding Text / Preedit | Cách hiển thị text đang gõ |
| **Tone Mark Position** | Modern (hoà) / Traditional (hòa) | Vị trí dấu thanh |
| **Free Marking** | true / false | Cho phép đặt dấu tự do |
| **Auto Restore** | true / false | Tự động khôi phục khi gõ sai |
| **Show Preedit** | true / false | Hiển thị text đang soạn |

## Debug

SKey ghi log vào `/tmp/skey.log` với timestamp. Log bao gồm:
- Trạng thái activation/deactivation
- Mỗi lần nhấn phím (old/new composed text, committedLen)
- Hoạt động của SurroundingText (append/replace/delete)
- Deferred commit schedule/flush

Để theo dõi realtime:
```bash
tail -f /tmp/skey.log
```

## License

GPL-3.0
