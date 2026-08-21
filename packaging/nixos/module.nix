# NixOS module: system integration for fcitx5-skey.
#
# What it does (the pieces that the RPM/DEB %post scripts handle manually
# on other distros):
#   * loads the uinput kernel module (hardware.uinput.enable)
#   * makes the systemd template unit from the package known to systemd
#     (systemd.packages) and enables one uinput-server instance per user
#   * adds the polkit rule so each user can restart their own instance
#     without a password prompt (the settings GUI and skey-setup call
#     `systemctl try-restart fcitx5-skey-uinput-server@$USER.service`)
#   * puts the package (settings GUI, skey-setup, skey-restart-fcitx5)
#     on PATH
#
# Enabling the fcitx5 addon itself is done the normal NixOS way, see
# flake.nix header or README.md.
{
  config,
  lib,
  pkgs,
  ...
}:

let
  cfg = config.services.fcitx5-skey;
in
{
  options.services.fcitx5-skey = {
    enable = lib.mkEnableOption "SKey Vietnamese input method system integration (uinput server, polkit rule)";

    package = lib.mkPackageOption pkgs "fcitx5-skey" { };

    users = lib.mkOption {
      type = lib.types.listOf lib.types.str;
      default = [ ];
      example = [ "huy" ];
      description = ''
        Users to run a `fcitx5-skey-uinput-server` instance for.  The
        server injects BackSpace/Unicode keystrokes via /dev/uinput and
        runs as the unprivileged `skey_uinput` system user, so one
        instance per user must be enabled explicitly.
      '';
    };
  };

  config = lib.mkIf cfg.enable {
    hardware.uinput.enable = true;

    environment.systemPackages = [ cfg.package ];

    # Dedicated system user the uinput server runs as.  The udev rule
    # (shipped in the package) grants it an rw ACL on /dev/uinput — never
    # group "input", which would let it read every /dev/input/event*
    # device.  No user needs group membership: the socket is chmod 0666
    # in a 0771 RuntimeDirectory; SO_PEERCRED + /proc/pid/exe checks at
    # accept time are the real authorization.
    users.users.skey_uinput = {
      isSystemUser = true;
      group = "skey_uinput";
    };
    users.groups.skey_uinput = { };

    services.udev.packages = [ cfg.package ];

    # Load the template unit shipped in the package
    # ($out/lib/systemd/system/fcitx5-skey-uinput-server@.service).
    systemd.packages = [ cfg.package ];

    # Enable one instance per user.  NixOS treats `...@<user>` as a
    # literal instance unit, which systemd merges over the template above.
    systemd.services = lib.listToAttrs (
      map (user:
        lib.nameValuePair "fcitx5-skey-uinput-server@${user}" {
          wantedBy = [ "multi-user.target" ];
        }
      ) cfg.users
    );

    # No group membership needed for users: the server socket is chmod
    # 0666 in a 0771 RuntimeDirectory, and authorization happens at
    # accept time (SO_PEERCRED + /proc/pid/exe).

    # Polkit rule from the package: each user may (re)start only their
    # own fcitx5-skey-uinput-server@<user>.service without admin auth.
    # On NixOS, /etc/polkit-1/rules.d is generated — rules must be
    # injected via extraConfig.
    security.polkit.extraConfig = builtins.readFile
      "${cfg.package}/etc/polkit-1/rules.d/60-fcitx5-skey-uinput.rules";
  };
}
