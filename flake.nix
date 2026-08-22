# Root flake for SKey.
#
# Flakes are only discovered at the repository root, so this file re-exports
# the real packaging (package derivation + NixOS module) that lives in
# packaging/nixos/flake.nix.  Without this, `inputs.skey.url =
# "github:collyn/skey"` would fail with "does not provide attribute flake.nix".
#
# Usage in your own flake:
#   inputs.skey.url = "github:collyn/skey";
#
#   outputs = { nixpkgs, skey, ... }: {
#     nixosConfigurations.hostname = nixpkgs.lib.nixosSystem {
#       modules = [
#         skey.nixosModules.default
#         {
#           i18n.inputMethod = {
#             enable = true;
#             type = "fcitx5";
#             fcitx5.addons = [ skey.packages.${pkgs.system}.fcitx5-skey ];
#           };
#           services.fcitx5-skey = {
#             enable = true;
#             users = [ "yourusername" ];
#           };
#         }
#       ];
#     };
#   };
#
# See packaging/nixos/README.md for details.
{
  description = "SKey — Vietnamese input method for Fcitx5";

  inputs = {
    nixpkgs.url = "github:NixOS/nixpkgs/nixos-unstable";
  };

  outputs = { self, nixpkgs }:
    (import ./packaging/nixos/flake.nix).outputs { inherit self nixpkgs; };
}
