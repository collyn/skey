#!/bin/bash
# Install fcitx5-skey Arch Linux repository
# Usage: curl -fsSL https://collyn.github.io/skey/install-arch.sh | sudo bash
set -e

echo "Adding fcitx5-skey Arch Linux repository..."

# Download and import GPG key
curl -fsSL "https://collyn.github.io/skey/key.asc" -o /tmp/fcitx5-skey-key.asc
sudo pacman-key --add /tmp/fcitx5-skey-key.asc
sudo pacman-key --lsign-key collyn094@gmail.com
rm -f /tmp/fcitx5-skey-key.asc

# Add repo to pacman.conf
if ! grep -q '^\[fcitx5-skey\]' /etc/pacman.conf; then
    echo "" | sudo tee -a /etc/pacman.conf > /dev/null
    echo "[fcitx5-skey]" | sudo tee -a /etc/pacman.conf > /dev/null
    echo "Server = https://collyn.github.io/skey/arch/x86_64" | sudo tee -a /etc/pacman.conf > /dev/null
    echo "SigLevel = Optional TrustAll" | sudo tee -a /etc/pacman.conf > /dev/null
fi

# Update
sudo pacman -Syu

echo "✓ fcitx5-skey repository installed!"
echo "  Install with: sudo pacman -S fcitx5-skey"
echo "  Frontends:    sudo pacman -S fcitx5-gtk fcitx5-qt"
