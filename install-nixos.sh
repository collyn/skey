#!/bin/bash
# fcitx5-skey NixOS installer — wires the skey flake input + module into
# your system configuration, then tells you to rebuild.
#
# On a fresh NixOS (classic channel-based config, no flake) it generates a
# minimal flake.nix wrapping the existing configuration.nix, keeping the
# nixpkgs branch of the current channel so the system is not upgraded.
# Idempotent: re-running is a no-op.
set -e

FLAKE=/etc/nixos/flake.nix
CONFIG=/etc/nixos/configuration.nix
HWCONFIG=/etc/nixos/hardware-configuration.nix
USER_NAME="${SUDO_USER:-$USER}"
MISSING=0
GENERATED_FLAKE=0

if [ ! -f /etc/NIXOS ] && ! grep -q '^ID=nixos$' /etc/os-release 2>/dev/null; then
    echo "✗ This script is for NixOS only."
    exit 1
fi
if [ ! -f "$CONFIG" ]; then
    echo "✗ $CONFIG not found."
    exit 1
fi
if [ ! -w "$CONFIG" ]; then
    echo "✗ $CONFIG is not writable — run via: curl -fsSL https://collyn.github.io/skey/install-nixos.sh | sudo bash"
    exit 1
fi

if [ ! -f "$FLAKE" ]; then
    # Fresh NixOS: generate a minimal flake wrapping the existing config.
    # nixpkgs follows the current channel so the system is not upgraded.
    NIXPKGS_REF="nixos-unstable"
    CHANNEL=$(nix-channel --list 2>/dev/null | awk '/^nixos / {print $2; exit}')
    if [ -n "$CHANNEL" ]; then
        # https://nixos.org/channels/nixos-24.11 → nixos-24.11
        NIXPKGS_REF=$(basename "$CHANNEL")
    elif grep -q '^VERSION_ID=' /etc/os-release 2>/dev/null; then
        NIXPKGS_REF="nixos-$(grep -oP '^VERSION_ID="?\K[0-9.]+' /etc/os-release)"
    fi
    HOST="$(cat /etc/hostname 2>/dev/null || true)"
    HOST="${HOST:-$(hostname 2>/dev/null || true)}"
    HOST="${HOST:-nixos}"
    if [ -f "$HWCONFIG" ]; then
        MODULES=$'./configuration.nix\n        ./hardware-configuration.nix'
    else
        MODULES='./configuration.nix'
    fi
    cat > "$FLAKE" <<EOF
{
  description = "NixOS configuration with fcitx5-skey";

  inputs = {
    nixpkgs.url = "github:NixOS/nixpkgs/$NIXPKGS_REF";
    skey.url = "github:collyn/skey";
    # Reuse this flake's nixpkgs so skey does not download a second copy.
    skey.inputs.nixpkgs.follows = "nixpkgs";
  };

  outputs = { nixpkgs, skey, ... }: {
    nixosConfigurations."$HOST" = nixpkgs.lib.nixosSystem {
      modules = [
        skey.nixosModules.default
        $MODULES
      ];
    };
  };
}
EOF
    echo "✓ Created $FLAKE (nixpkgs: $NIXPKGS_REF, host: $HOST)"
    GENERATED_FLAKE=1
fi
if [ ! -w "$FLAKE" ]; then
    echo "✗ $FLAKE is not writable — run via: curl -fsSL https://collyn.github.io/skey/install-nixos.sh | sudo bash"
    exit 1
fi

# 1) flake input: skey.url = "github:collyn/skey";
#    + skey.inputs.nixpkgs.follows = "nixpkgs" so the skey flake reuses the
#    system's nixpkgs instead of downloading a second one (~45 MB source
#    tarball + ~300 MB unpacked, on top of a mismatched duplicate set).
if grep -q 'skey\.url' "$FLAKE"; then
    echo "✓ skey input already present in $FLAKE"
else
    if grep -q '^[[:space:]]*inputs[[:space:]]*=[[:space:]]*{[[:space:]]*$' "$FLAKE"; then
        if grep -q 'nixpkgs\.url' "$FLAKE"; then
            sed -i '/^[[:space:]]*inputs[[:space:]]*=[[:space:]]*{[[:space:]]*$/a\    skey.url = "github:collyn/skey";\n    skey.inputs.nixpkgs.follows = "nixpkgs";' "$FLAKE"
            echo "✓ Added skey input to $FLAKE (nixpkgs follows the system's)"
        else
            sed -i '/^[[:space:]]*inputs[[:space:]]*=[[:space:]]*{[[:space:]]*$/a\    skey.url = "github:collyn/skey";' "$FLAKE"
            echo "✓ Added skey input to $FLAKE (no nixpkgs input found — follows skipped)"
        fi
    else
        echo "⚠ Could not find 'inputs = {' in $FLAKE — add manually:"
        echo '    skey.url = "github:collyn/skey";'
        echo '    skey.inputs.nixpkgs.follows = "nixpkgs";'
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

# Append a config block to configuration.nix, inserting it BEFORE the
# attrset's closing brace when the file ends with one — the default
# nixos-generate-config layout — so the block lands INSIDE the config
# (appending blindly would put it after the final "}" and break eval).
append_config_block() {
    local blockfile
    blockfile=$(mktemp)
    cat > "$blockfile"
    awk -v blockfile="$blockfile" '
        { lines[NR] = $0 }
        END {
            # Skip trailing blank/comment lines to find the last real line.
            last = NR
            while (last > 0 && (lines[last] ~ /^[[:space:]]*$/ ||
                                lines[last] ~ /^[[:space:]]*#/)) last--
            if (last > 0 && lines[last] ~ /^[[:space:]]*}[[:space:]]*$/) {
                # Lone closing brace: insert the block before it.
                for (i = 1; i < last; i++) print lines[i]
                while ((getline line < blockfile) > 0) print line
                close(blockfile)
                for (i = last; i <= NR; i++) print lines[i]
            } else if (last > 0 && lines[last] ~ /}[[:space:]]*$/) {
                # Inline close ("...; }"): strip the brace, insert, restore.
                sub(/}[[:space:]]*$/, "", lines[last])
                for (i = 1; i <= last; i++) print lines[i]
                while ((getline line < blockfile) > 0) print line
                close(blockfile)
                print "}"
            } else {
                for (i = 1; i <= NR; i++) print lines[i]
                while ((getline line < blockfile) > 0) print line
                close(blockfile)
            }
        }
    ' "$CONFIG" > "$CONFIG.tmp" && mv "$CONFIG.tmp" "$CONFIG"
    rm -f "$blockfile"
}

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
    append_config_block <<EOF

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

# 5) lock the inputs so the first rebuild does not fail on eval
if command -v nix >/dev/null 2>&1; then
    if [ -f /etc/nixos/flake.lock ]; then
        if (cd /etc/nixos && nix --extra-experimental-features 'nix-command flakes' flake update skey); then
            echo "✓ Locked skey input in flake.lock"
        else
            echo "⚠ Could not lock the skey input now — the first nixos-rebuild will fetch it automatically."
        fi
    else
        # Fresh flake: lock every input (nixpkgs + skey).
        if (cd /etc/nixos && nix --extra-experimental-features 'nix-command flakes' flake lock); then
            echo "✓ Created flake.lock"
        else
            echo "⚠ Could not lock the inputs now — the first nixos-rebuild will fetch them automatically."
        fi
    fi
fi

if [ "$MISSING" = "1" ]; then
    echo "⚠ Some steps need manual attention — see messages above."
fi
echo ""
echo "✓ Done. Rebuild your system:"
echo "  sudo nixos-rebuild switch --flake /etc/nixos"
if [ "$GENERATED_FLAKE" = "1" ]; then
    echo "  (flake.nix was generated — from now on use --flake for rebuilds)"
fi
echo ""
echo "  Restart fcitx5: fcitx5 -r -d  (or logout/login)"
echo "  Updates: open fcitx5-skey-settings → Check Update"
