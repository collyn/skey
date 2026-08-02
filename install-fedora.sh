#!/bin/bash
set -e
echo "Adding fcitx5-skey RPM repository for Fedora..."
sudo rpm --import https://collyn.github.io/skey/key.gpg
cat << 'REPO' | sudo tee /etc/yum.repos.d/fcitx5-skey.repo > /dev/null
[fcitx5-skey]
name=fcitx5-skey — Vietnamese SKey input method
baseurl=https://collyn.github.io/skey/rpm/fedora/
enabled=1
gpgcheck=1
repo_gpgcheck=1
gpgkey=https://collyn.github.io/skey/key.gpg
REPO
echo "✓ Run: sudo dnf install fcitx5-skey"
