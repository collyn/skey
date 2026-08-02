#!/bin/bash
# Install fcitx5-skey RPM repository (openSUSE)
# Usage: curl -fsSL https://collyn.github.io/skey/install-opensuse.sh | sudo bash
set -e

echo "Adding fcitx5-skey RPM repository for openSUSE..."

# Import GPG key
sudo rpm --import https://collyn.github.io/skey/key.gpg

# Add repo
sudo zypper addrepo --refresh --check --gpgcheck     https://collyn.github.io/skey/rpm/opensuse/ fcitx5-skey

echo "✓ fcitx5-skey repository installed!"
echo "  Install with: sudo zypper install fcitx5-skey"
echo "  Frontends:    sudo zypper install fcitx5-gtk"
