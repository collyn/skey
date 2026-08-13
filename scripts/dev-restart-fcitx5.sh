#!/usr/bin/env bash
# Dev-loop helper: gentle fcitx5 restart with KWin reconnect.
# Thin wrapper around skey-restart-fcitx5 (same file as the packaged
# /usr/bin/skey-restart-fcitx5) — keeps the dev loop and the postinst /
# settings GUI paths on one implementation.
exec "$(dirname "$0")/skey-restart-fcitx5" "$@"
