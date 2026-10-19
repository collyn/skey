# NixOS integration: user-session Uinput inside fcitx5.
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
    enable = lib.mkEnableOption "SKey Vietnamese input method system integration (uinput device access)";

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
        Deprecated compatibility option; no longer needed. Uinput runs in
        fcitx5, and logind grants the active local seat access via uaccess.
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

    services.udev.packages = [ cfg.package ];
    warnings = lib.optional (cfg.users != [ ])
      "services.fcitx5-skey.users is deprecated and ignored; Uinput now runs inside fcitx5 using active-seat uaccess.";
  };
}
