# fcitx5-skey flake — package + NixOS module.
#
# Usage:
#   inputs.skey.url = "github:collyn/skey";
#   # or, for a local checkout:
#   # inputs.skey.url = "path:/path/to/skey";
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
        fcitx5-skey = import ./module.nix;
        default = self.nixosModules.fcitx5-skey;
      };
    };
}
