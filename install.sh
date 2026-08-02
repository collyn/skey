#!/bin/bash
set -e
echo "Adding fcitx5-skey APT repository..."
sudo mkdir -p /etc/apt/keyrings
curl -fsSL https://collyn.github.io/skey/key.asc | sudo gpg --dearmor --yes -o /etc/apt/keyrings/fcitx5-skey.gpg
echo "deb [arch=amd64 signed-by=/etc/apt/keyrings/fcitx5-skey.gpg] https://collyn.github.io/skey stable main" | sudo tee /etc/apt/sources.list.d/fcitx5-skey.list > /dev/null
sudo apt update
echo "✓ Run: sudo apt install fcitx5-skey"
