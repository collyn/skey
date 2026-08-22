#!/bin/bash
# fcitx5-skey NixOS installer — wires the skey flake input + module into
# your system configuration, then tells you to rebuild.
#
# Requirements: NixOS with a flake configuration (/etc/nixos/flake.nix).
# Idempotent: re-running is a no-op.
set -e

FLAKE=/etc/nixos/flake.nix
CONFIG=/etc/nixos/configuration.nix
USER_NAME="${SUDO_USER:-$USER}"
MISSING=0

if [ ! -f /etc/NIXOS ] && ! grep -q '^ID=nixos$' /etc/os-release 2>/dev/null; then
    echo "✗ This script is for NixOS only."
    exit 1
fi
if [ ! -f "$FLAKE" ]; then
    echo "✗ $FLAKE not found — a flake configuration is required."
    echo "  See https://github.com/collyn/skey/blob/main/packaging/nixos/README.md"
    exit 1
fi
if [ ! -w "$FLAKE" ]; then
    echo "✗ $FLAKE is not writable — run via: curl -fsSL https://collyn.github.io/skey/install-nixos.sh | sudo bash"
    exit 1
fi

# 1) flake input: skey.url = "github:collyn/skey";
if grep -q 'skey\.url' "$FLAKE"; then
    echo "✓ skey input already present in $FLAKE"
else
    if grep -q '^[[:space:]]*inputs[[:space:]]*=[[:space:]]*{[[:space:]]*$' "$FLAKE"; then
        sed -i '/^[[:space:]]*inputs[[:space:]]*=[[:space:]]*{[[:space:]]*$/a\    skey.url = "github:collyn/skey";' "$FLAKE"
        echo "✓ Added skey input to $FLAKE"
    else
        echo "⚠ Could not find 'inputs = {' in $FLAKE — add manually:"
        echo '    skey.url = "github:collyn/skey";'
        MISSING=1
    fi
fi

# 2) outputs signature: add `skey` next to `nixpkgs`
if grep -q 'outputs[[:space:]]*=[[:space:]]*{[^}]*skey,' "$FLAKE"; then
    echo "✓ skey already in outputs"
else
    sed -i -E 's|(outputs[[:space:]]*=[[:space:]]*\{[^}]*nixpkgs,)([^}]*\})|\1 skey,\2|' "$FLAKE"
    if grep -q 'outputs[[:space:]]*=[[:space:]]*{[^}]*skey,' "$FLAKE"; then
        echo "✓ Added skey to the outputs function signature"
    else
        echo "⚠ Could not update the outputs signature — add 'skey' next to 'nixpkgs' manually."
        MISSING=1
    fi
fi

# 3) module import: skey.nixosModules.default in every nixosConfiguration
if grep -q 'skey\.nixosModules\.default' "$FLAKE"; then
    echo "✓ skey module already imported"
else
    if grep -q '^[[:space:]]*modules[[:space:]]*=[[:space:]]*\[' "$FLAKE"; then
        awk '
            /^[[:space:]]*modules[[:space:]]*=[[:space:]]*\[.*\];[[:space:]]*$/ {
                # one-line list: split after "[" and insert the module first
                sub(/\[[[:space:]]*/, "[\n        skey.nixosModules.default\n        ")
                print
                next
            }
            {
                print
            }
            /^[[:space:]]*modules[[:space:]]*=[[:space:]]*\[/ {
                # multi-line list: insert right after the opening bracket line
                print "        skey.nixosModules.default"
            }
        ' "$FLAKE" > "$FLAKE.tmp" && mv "$FLAKE.tmp" "$FLAKE"
        echo "✓ Imported skey.nixosModules.default in $FLAKE"
    else
        echo "⚠ No 'modules = [' found in $FLAKE — add 'skey.nixosModules.default' to your nixosConfigurations modules list manually."
        MISSING=1
    fi
fi

# 4) configuration.nix: uinput server + fcitx5 addon
if grep -q 'services\.fcitx5-skey' "$CONFIG"; then
    echo "✓ $CONFIG already configures services.fcitx5-skey"
else
    if grep -q 'i18n\.inputMethod' "$CONFIG"; then
        echo "⚠ $CONFIG already configures i18n.inputMethod — merge the fcitx5 addon block manually to avoid option conflicts."
        MISSING=1
    fi
    if [ "$USER_NAME" = "root" ]; then
        echo "⚠ Could not determine your user (script ran as root) — edit users in $CONFIG after the rebuild fails."
    fi
    cat >> "$CONFIG" <<EOF

# ── fcitx5-skey (added by install script) ──
services.fcitx5-skey = {
  enable = true;
  users = [ "$USER_NAME" ];
};
i18n.inputMethod = {
  enable = true;
  type = "fcitx5";
  # Reference the module's package (not pkgs directly) so the dev channel
  # (services.fcitx5-skey.devVersion) applies to the addon too.
  fcitx5.addons = [ config.services.fcitx5-skey.package ];
};
# ── end fcitx5-skey ──
EOF
    echo "✓ Added skey config to $CONFIG (uinput server for user: $USER_NAME)"
fi

# 5) lock the new input so the first rebuild does not fail on eval
if command -v nix >/dev/null 2>&1; then
    if (cd /etc/nixos && nix --extra-experimental-features 'nix-command flakes' flake update skey); then
        echo "✓ Locked skey input in flake.lock"
    else
        echo "⚠ Could not lock the skey input now — the first nixos-rebuild will fetch it automatically."
    fi
fi

if [ "$MISSING" = "1" ]; then
    echo "⚠ Some steps need manual attention — see messages above."
fi
echo ""
echo "✓ Done. Rebuild your system:"
echo "  sudo nixos-rebuild switch --flake /etc/nixos"
echo ""
echo "  Restart fcitx5: fcitx5 -r -d  (or logout/login)"
echo "  Updates: open fcitx5-skey-settings → Check Update"
