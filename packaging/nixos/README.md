# fcitx5-skey trên NixOS

| File | Nội dung |
|---|---|
| `../../flake.nix` | Flake ở repo root, re-export thư mục này (flakes chỉ được phát hiện ở root) |
| `flake.nix` | Package `fcitx5-skey` + NixOS module |
| `default.nix` | Package derivation (dùng được không cần flake) |
| `module.nix` | `services.fcitx5-skey` — module uinput và quyền uaccess |

## Cài đặt nhanh

Thêm vào `/etc/nixos/configuration.nix`:

```nix
{ config, pkgs, ... }:
let
  skey = pkgs.callPackage (builtins.fetchTarball
    "https://github.com/collyn/skey/archive/refs/heads/main.tar.gz"
    + "/packaging/nixos/default.nix") {};
in {
  hardware.uinput.enable = true;
  services.udev.packages = [ skey ];
  i18n.inputMethod = {
    enable = true;
    type = "fcitx5";
    fcitx5.addons = [ skey ];
  };
  # Hiện biểu tượng Fcitx5/SKey trên top panel GNOME.
  services.desktopManager.gnome.sessionPath = [ pkgs.gnomeExtensions.appindicator ];
}
```

Áp dụng:

```bash
sudo nixos-rebuild switch --no-flake
fcitx5 -r -d
```

Nếu `/etc/nixos` không có `flake.nix`, bỏ `--no-flake`. Mở
`fcitx5-configtool` để thêm SKey vào nhóm bộ gõ.
Đăng xuất rồi đăng nhập lại sau lần cài đầu tiên để GNOME nạp AppIndicator
và tự khởi chạy Fcitx5.

## Cập nhật lên bản mới (từ GitHub)

```bash
# Lấy commit mới nhất của skey.  Nhớ sudo: /etc/nixos thuộc root nên
# `nix flake update` không có sudo sẽ fail "Permission denied" khi ghi
# flake.lock.  --extra-experimental-features cho máy chưa bật
# nix-command/flakes trong /etc/nix/nix.conf.
sudo nix --extra-experimental-features 'nix-command flakes' flake update
sudo nixos-rebuild switch --flake /etc/nixos#hostname

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
nix --extra-experimental-features 'nix-command flakes' \
  build .#fcitx5-skey --no-link
```
