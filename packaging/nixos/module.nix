# NixOS module: system integration for fcitx5-skey.
#
# What it does (the pieces that the RPM/DEB %post scripts handle manually
# on other distros):
#   * loads the uinput kernel module (hardware.uinput.enable)
#   * creates the dedicated skey_uinput system user and grants it
#     /dev/uinput access via the "uinput" group that hardware.uinput
#     already declares (0660 root:uinput).  The package's udev ACL rule
#     cannot be used on NixOS: it calls /usr/bin/setfacl, and nixpkgs'
#     udev rule processing rejects rules that reference /usr/bin programs
#     (there is no /usr/bin on NixOS anyway).
#   * makes the systemd template unit from the package known to systemd
#     (systemd.packages) and enables one uinput-server instance per user
#   * overrides the unit's setfacl ExecStartPre/ExecStartPost with nix
#     store paths (the template's /usr/bin/systemd-sysusers and
#     /usr/bin/setfacl don't exist on NixOS)
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

    devVersion = lib.mkOption {
      type = lib.types.nullOr lib.types.str;
      default = null;
      example = "0.7.7-dev.3";
      description = ''
        Dev channel: set to the dev prerelease version to build the package
        with that version string and dev-build counter.  The settings GUI
        writes this option (with marker comments) when the dev update
        channel is selected, together with pinning the flake input to the
        matching dev tag — keep `null` for stable releases.
      '';
    };

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

    # Dev channel: rebuild the package with the dev version string + counter
    # so the settings GUI / updater recognizes the build (otherwise the same
    # dev tag would be re-offered forever).  Overrides the mkPackageOption
    # default; stable (null) keeps plain pkgs.fcitx5-skey.
    services.fcitx5-skey.package = lib.mkIf (cfg.devVersion != null)
      (pkgs.fcitx5-skey.override { inherit (cfg) devVersion; });

    environment.systemPackages = [ cfg.package ];

    # Dedicated system user the uinput server runs as.  On other distros
    # the package's udev rule grants it an rw ACL on /dev/uinput; on NixOS
    # that rule is unusable (see header), so the user is put into the
    # "uinput" group that hardware.uinput.enable declares — the device is
    # 0660 root:uinput.  That grants exactly one privilege: opening
    # /dev/uinput.  Deliberately NOT group "input", which would let the
    # server read every /dev/input/event* device (a keylogging surface).
    # No real user needs group membership: the socket dir is 0700 with a
    # per-user named ACL (ExecStartPost override below), and accept-time
    # checks (SO_PEERCRED + /proc/pid/exe) are the real authorization.
    users.users.skey_uinput = {
      isSystemUser = true;
      group = "skey_uinput";
      extraGroups = [ "uinput" ];
    };
    users.groups.skey_uinput = { };

    # Load the template unit shipped in the package
    # ($out/lib/systemd/system/fcitx5-skey-uinput-server@.service).
    systemd.packages = [ cfg.package ];

    # Enable one instance per user.  NixOS treats `...@<user>` as a
    # literal instance unit, which systemd merges over the template above.
    #
    # The template's ExecStartPre/ExecStartPost call /usr/bin/systemd-sysusers
    # and /usr/bin/setfacl — neither exists on NixOS — so override them
    # with store paths.  sysusers is unnecessary here (the user above is
    # declarative); the /dev/uinput ACL is a belt-and-braces fallback for
    # the group-based access; the ExecStartPost ACL is what lets <user>
    # traverse the 0700 runtime directory to reach the fs socket (without
    # it clients silently fall back to the legacy abstract socket).
    systemd.services = lib.listToAttrs (
      map (user:
        lib.nameValuePair "fcitx5-skey-uinput-server@${user}" {
          wantedBy = [ "multi-user.target" ];
          serviceConfig = {
            ExecStartPre = [
              "-+${pkgs.acl}/bin/setfacl -m u:skey_uinput:rw /dev/uinput"
            ];
            ExecStartPost = [
              "-+${pkgs.acl}/bin/setfacl -m u:${user}:x /run/skey-uinput-${user}"
            ];
          };
        }
      ) cfg.users
    );

    # Polkit rule from the package: each user may (re)start only their
    # own fcitx5-skey-uinput-server@<user>.service without admin auth.
    # On NixOS, /etc/polkit-1/rules.d is generated — rules must be
    # injected via extraConfig.
    security.polkit.extraConfig = builtins.readFile
      "${cfg.package}/etc/polkit-1/rules.d/60-fcitx5-skey-uinput.rules";
  };
}
