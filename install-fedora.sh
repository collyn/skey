#!/bin/bash
# Install fcitx5-skey RPM repository (Fedora)
# Usage: curl -fsSL https://collyn.github.io/skey/install-fedora.sh | sudo bash
set -e

echo "Adding fcitx5-skey RPM repository for Fedora..."

# Import GPG key
sudo rpm --import https://collyn.github.io/skey/key.gpg

# Add repo
cat << 'REPO' | sudo tee /etc/yum.repos.d/fcitx5-skey.repo > /dev/null
[fcitx5-skey]
name=fcitx5-skey — Vietnamese SKey input method
baseurl=https://collyn.github.io/skey/rpm/fedora/
enabled=1
gpgcheck=1
repo_gpgcheck=1
gpgkey=https://collyn.github.io/skey/key.gpg
REPO

echo "✓ fcitx5-skey repository installed!"
echo "  Install with: sudo dnf install fcitx5-skey"
echo "  Frontends:    sudo dnf install fcitx5-gtk fcitx5-qt"
