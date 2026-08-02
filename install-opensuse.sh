#!/bin/bash
set -e
echo "Adding fcitx5-skey RPM repository for openSUSE..."
sudo rpm --import https://collyn.github.io/skey/key.asc
sudo zypper addrepo --refresh --check --gpgcheck https://collyn.github.io/skey/rpm/opensuse/ fcitx5-skey
echo "✓ Run: sudo zypper install fcitx5-skey"
