# fcitx5-skey trên NixOS

| File | Nội dung |
|---|---|
| `../../flake.nix` | Flake ở repo root, re-export thư mục này (flakes chỉ được phát hiện ở root) |
| `flake.nix` | Package `fcitx5-skey` + NixOS module |
| `default.nix` | Package derivation (dùng được không cần flake) |
| `module.nix` | `services.fcitx5-skey` — module uinput và quyền uaccess |

## Cách dùng (flake)

```nix
# flake.nix của bạn
{
  inputs.skey = {
    url = "github:collyn/skey";
    # Dùng chung nixpkgs với hệ thống — không tải nixpkgs thứ hai
    # (~45 MB tarball + ~300 MB giải nén).
    inputs.nixpkgs.follows = "nixpkgs";
  };

  outputs = { nixpkgs, skey, ... }: {
    nixosConfigurations.hostname = nixpkgs.lib.nixosSystem {
      modules = [
        skey.nixosModules.default
        { config, ... }: {
          i18n.inputMethod = {
            enable = true;
            type = "fcitx5";
            # Tham chiếu qua module (không phải pkgs trực tiếp) để
            # services.fcitx5-skey.devVersion (kênh Dev) áp dụng cho cả
            # addon và cấu hình quyền thiết bị.
            fcitx5.addons = [ config.services.fcitx5-skey.package ];
          };
          services.fcitx5-skey = {
            enable = true;
          };
        }
      ];
    };
  };
}
```

`fcitx5.addons` làm cho wrapper `fcitx5-with-addons` set `FCITX_ADDON_DIRS`
và `XDG_DATA_DIRS` trỏ vào package — đây là cách fcitx5 tìm thấy `skey.so`,
`skey.conf` và `skey-im.conf` trong nix store (read-only).

Module `services.fcitx5-skey` xử lý những việc mà trên Fedora/Debian do
`%post` của RPM/DEB làm:

- `hardware.uinput.enable = true` — nạp module uinput.
- `services.udev.packages` — cài `70-skey-uinput.rules` trước `73-seat-late.rules`;
  logind cấp ACL cho user của local seat đang active. Không thêm user vào group `input`.
- Đưa settings GUI và các helper vào PATH. Uinput chạy trong fcitx5, không cần service.

Option `services.fcitx5-skey.users` cũ vẫn được chấp nhận và phát cảnh báo deprecation;
không còn cần khai báo danh sách user. Khi switch từ cấu hình cũ, NixOS dừng các unit
đã bị bỏ khỏi hệ thống. Sau rollback, rebuild generation cũ rồi restart fcitx5.

Các biến môi trường (`GTK_IM_MODULE`, `QT_IM_MODULE`, …) do module
`i18n.inputMethod` của NixOS quản lý — package không còn ship
`profile.d/fcitx5-skey.sh` (đã bỏ từ 0.7.x; skey-setup ghi đúng 1 file
user-level theo ngữ cảnh session).

Sau `nixos-rebuild switch`, chạy `fcitx5 -r -d` (hoặc logout/login), rồi
bật SKey trong `fcitx5-configtool`.

## Cập nhật lên bản mới (từ GitHub)

```bash
# Lấy commit mới nhất của skey.  Nhớ sudo: /etc/nixos thuộc root nên
# `nix flake update` không có sudo sẽ fail "Permission denied" khi ghi
# flake.lock.  --extra-experimental-features cho máy chưa bật
# nix-command/flakes trong /etc/nix/nix.conf.
sudo nix --extra-experimental-features 'nix-command flakes' flake update
sudo nixos-rebuild switch --flake /etc/nixos

# Kiểm tra quyền của user trên local seat đang active:
getfacl /dev/uinput
# Log fcitx5 phải báo: Uinput: ready
```

Lưu ý: `flake update` chỉ lấy được các thay đổi đã **commit + push** lên
GitHub — thay đổi local chưa push sẽ không xuất hiện.

## Kênh Dev

Settings GUI (tab Thông tin → Kênh cập nhật: Thử nghiệm) hoạt động trên
NixOS bằng cách:

- pin flake input vào đúng tag prerelease
  (`github:collyn/skey/v0.7.7-dev.3`) qua `nix flake update --override-input`
  — source build ra **y hệt** source của dev package trên các distro khác
- ghi `services.fcitx5-skey.devVersion = "0.7.7-dev.3";` vào
  `configuration.nix` (kèm marker comment do GUI quản lý) — build mang
  version string dev + counter (`SKEY_DEV_BUILD`) nên updater nhận diện
  đúng bản dev, không lặp lại offer cùng tag

Quay lại kênh Stable, GUI xóa block devVersion và reset input về
`github:collyn/skey`.

Thủ công tương đương:

```bash
# Bật dev (thay tag bằng dev prerelease mới nhất):
# thêm vào configuration.nix:
#   services.fcitx5-skey.devVersion = "0.7.7-dev.3";
sudo nix flake lock --override-input skey github:collyn/skey/v0.7.7-dev.3
sudo nixos-rebuild switch --flake /etc/nixos

# Về stable:
# xóa dòng devVersion khỏi configuration.nix
sudo nix flake lock --override-input skey github:collyn/skey
sudo nixos-rebuild switch --flake /etc/nixos
```

Điều kiện để dev channel áp dụng đúng: addon phải tham chiếu
`config.services.fcitx5-skey.package` (như ví dụ trên) — nếu dùng
`pkgs.fcitx5-skey` trực tiếp, engine addon vẫn là bản stable.

## Đóng gói riêng (không dùng flake)

```nix
pkgs.callPackage (builtins.fetchTarball "https://github.com/collyn/skey/archive/refs/heads/main.tar.gz" + "/packaging/nixos/default.nix") { }
```

## Ghi chú kỹ thuật

- **`nixosModules.default` là wrapper module**: overlay `fcitx5-skey` vào
  pkgs (để `services.fcitx5-skey.package` mặc định resolve được — package
  nằm trong flake chứ không phải nixpkgs) rồi mới import `module.nix`.
  Nếu dùng `module.nix` trực tiếp (không qua flake), phải tự overlay
  hoặc set `services.fcitx5-skey.package` thủ công.
- **skey-engine (Rust)** mặc định lấy **prebuilt `libskey_engine.a`** từ
  GitHub Release của skey-engine (`builtins.fetchurl` + sha256) — bỏ hẳn
  Rust toolchain (rustc/cargo/LLVM ≈ 470 MB download / 1.7 GiB unpacked).
  CMake được truyền `-DSKEY_ENGINE_PREBUILT_LIB` để bỏ qua bước cargo và
  `-DSKEY_ENGINE_SOURCE_DIR` để lấy `skey_engine.h` (tag lấy theo
  `SKEY_ENGINE_VERSION` trong CMakeLists.txt, bump tag là 2 URL tự đổi,
  hash cũ sẽ fail lớn cho tới khi refresh). Muốn build từ source:
  `pkgs.fcitx5-skey.override { skeyEngineLib = null; }` — dùng
  `buildRustPackage` (vì bước cargo trong CMake cần network, bị cấm trong
  sandbox nix); hash crate lấy từ `Cargo.lock` của skey-engine nên không
  cần `cargoHash`.
- **Path distro-specific**: `SKEY_UDEV_RULES_DIR` và `SKEY_MODULES_LOAD_DIR`
  trỏ vào `$out`. Rule chỉ dùng `TAG+="uaccess"`, không gọi đường dẫn `/usr/bin`;
  dùng trực tiếp qua `services.udev.packages`.
- **Icon**: cả addon (tray) lẫn settings GUI đều resolve icon qua
  `FCITX_SKEY_ICON_DIR` / `FCITX_SKEY_ICON_PATH` compile-time trỏ vào
  `$out/share/icons/...` nên icon mặc định **và các preset theme** hoạt
  động dù `/usr/share/icons` không tồn tại trên NixOS.
- **Save config**: settings GUI tạo `~/.config/fcitx5/conf` nếu thiếu và
  tự tạo `~/.config/fcitx5/config` (với section `[Hotkey/TriggerKeys]`)
  khi chưa tồn tại — trên NixOS config hệ thống nằm ở `/etc/xdg/fcitx5`
  (environment.etc) nên file user thường không có sẵn.
- `qt6.qtwayland` được wrap vào settings GUI (wrapQtAppsHook) để chạy được
  trên Wayland; `qt6.qtsvg` để QIcon load SVG.

## Kiểm tra không cần máy NixOS

Trên máy không có nix, có thể validate syntax + eval bằng container:

```sh
docker run --rm -v "$PWD":/src -w /src nixos/nix \
  nix-instantiate --parse packaging/nixos/default.nix packaging/nixos/module.nix flake.nix
docker run --rm -v "$PWD":/src -w /src nixos/nix \
  nix build .#fcitx5-skey --no-link
```
