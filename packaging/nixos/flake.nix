# fcitx5-skey flake — package + NixOS module.
#
# Flakes are only discovered at the repository root; the flake.nix at the
# repo root re-exports this file so that a plain URL works:
#   inputs.skey.url = "github:collyn/skey";
#   # or, for a local checkout:
#   # inputs.skey.url = "path:/path/to/skey";
# (For this subdirectory directly, use "github:collyn/skey?dir=packaging/nixos".)
#
#   i18n.inputMethod = {
#     enable = true;
#     type = "fcitx5";
#     fcitx5.addons = [ inputs.skey.packages.${pkgs.system}.fcitx5-skey ];
#   };
#   services.fcitx5-skey = {
#     enable = true;
#     users = [ "yourname" ];
#   };
{
  description = "SKey — Vietnamese input method for Fcitx5";

  inputs = {
    nixpkgs.url = "github:NixOS/nixpkgs/nixos-unstable";
  };

  outputs = { self, nixpkgs }:
    let
      systems = [ "x86_64-linux" "aarch64-linux" ];
      forAllSystems = f:
        nixpkgs.lib.genAttrs systems (system: f system (import nixpkgs { inherit system; }));
    in
    {
      packages = forAllSystems (system: pkgs: {
        fcitx5-skey = pkgs.callPackage ./default.nix { };
        default = self.packages.${system}.fcitx5-skey;
      });

      nixosModules = {
        # Wrapper module: overlays the package into pkgs so that
        # module.nix's `lib.mkPackageOption pkgs "fcitx5-skey"` resolves
        # (the package lives in this flake, not in nixpkgs), then imports
        # the module itself.  Consumers of skey.nixosModules.default get
        # pkgs.fcitx5-skey for free.
        fcitx5-skey = {
          nixpkgs.overlays = [
            (final: prev: {
              fcitx5-skey = self.packages.${final.stdenv.hostPlatform.system}.fcitx5-skey;
            })
          ];
          imports = [ ./module.nix ];
        };
        default = self.nixosModules.fcitx5-skey;
      };
    };
}
