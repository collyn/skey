#!/bin/bash
# Install fcitx5-skey APT repository
# Usage: curl -fsSL https://collyn.github.io/skey/install.sh | sudo bash
set -e

echo "Adding fcitx5-skey APT repository..."

# Install GPG key
sudo mkdir -p /etc/apt/keyrings
curl -fsSL "https://collyn.github.io/skey/key.asc" | sudo gpg --dearmor --yes -o /etc/apt/keyrings/fcitx5-skey.gpg

# Add apt source (arch=amd64 avoids i386 warnings)
echo "deb [arch=amd64 signed-by=/etc/apt/keyrings/fcitx5-skey.gpg] https://collyn.github.io/skey stable main" \
    | sudo tee /etc/apt/sources.list.d/fcitx5-skey.list > /dev/null

# Update
sudo apt update

echo ""
echo "✓ fcitx5-skey repository installed!"
echo "  Install with: sudo apt install fcitx5-skey"
