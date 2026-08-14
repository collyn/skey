# fcitx5-skey trên NixOS

| File | Nội dung |
|---|---|
| `flake.nix` | Package `fcitx5-skey` + NixOS module |
| `default.nix` | Package derivation (dùng được không cần flake) |
| `module.nix` | `services.fcitx5-skey` — uinput server, polkit rule |

## Cách dùng (flake)

```nix
# flake.nix của bạn
{
  inputs.skey.url = "github:collyn/skey";

  outputs = { nixpkgs, skey, ... }: {
    nixosConfigurations.hostname = nixpkgs.lib.nixosSystem {
      modules = [
        skey.nixosModules.default
        {
          i18n.inputMethod = {
            enable = true;
            type = "fcitx5";
            fcitx5.addons = [ skey.packages.x86_64-linux.fcitx5-skey ];
          };
          services.fcitx5-skey = {
            enable = true;
            users = [ "yourusername" ];
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

- `hardware.uinput.enable = true` — load module + udev rule cho `/dev/uinput`
- `systemd.packages` — nạp unit template `fcitx5-skey-uinput-server@.service`
  từ package và enable một instance cho mỗi user trong `users`
- Polkit rule (qua `security.polkit.extraConfig` — trên NixOS
  `/etc/polkit-1/rules.d` là file được generate, không sửa trực tiếp) để
  user restart instance của mình không cần mật khẩu
- Đưa `fcitx5-skey-settings`, `skey-setup`, `skey-restart-fcitx5` vào PATH

Các biến môi trường (`GTK_IM_MODULE`, `QT_IM_MODULE`, …) do module
`i18n.inputMethod` của NixOS quản lý — không cần `profile.d/fcitx5-skey.sh`.

Sau `nixos-rebuild switch`, chạy `fcitx5 -r -d` (hoặc logout/login), rồi
bật SKey trong `fcitx5-configtool`.

## Đóng gói riêng (không dùng flake)

```nix
pkgs.callPackage (builtins.fetchTarball "https://github.com/collyn/skey/archive/refs/heads/main.tar.gz" + "/packaging/nixos/default.nix") { }
```

## Ghi chú kỹ thuật

- **skey-engine (Rust)** được build bằng `buildRustPackage` riêng vì bước
  cargo trong CMake cần network (bị cấm trong sandbox nix). CMake được
  truyền `-DSKEY_ENGINE_PREBUILT_LIB` để bỏ qua bước cargo và
  `-DSKEY_ENGINE_SOURCE_DIR` để lấy `skey_engine.h`. Hash crate lấy từ
  `Cargo.lock` của skey-engine nên không cần `cargoHash`.
- **Path distro-specific** (`/lib/systemd/system`, `/etc/polkit-1/rules.d`,
  `/etc/profile.d`) được override qua cache vars `SKEY_SYSTEMD_UNIT_DIR`,
  `SKEY_POLKIT_RULES_DIR`, `SKEY_PROFILE_D_DIR` để trỏ vào `$out` — mặc định
  giữ nguyên như cũ cho Fedora/Debian/Arch.
- **Icon**: code có fallback `FCITX_SKEY_ICON_PATH` compile-time trỏ vào
  `$out/share/icons/...` nên tray icon hoạt động dù `/usr/share/icons`
  không tồn tại trên NixOS.
- `qt6.qtwayland` được wrap vào settings GUI (wrapQtAppsHook) để chạy được
  trên Wayland; `qt6.qtsvg` để QIcon load SVG.
