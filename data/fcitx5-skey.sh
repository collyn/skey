# fcitx5-skey: environment variables for fcitx5 input method
# This file is sourced by /etc/profile.d/ for login shells and display managers

export GTK_IM_MODULE=fcitx
# QT_IM_MODULE=ibus: uses IBus D-Bus protocol (fcitx5 provides org.freedesktop.IBus).
# Works for AppImages that bundle Qt without the fcitx5 plugin (e.g. Viber),
# because the IBus plugin is included in standard Qt builds and most AppImages.
export QT_IM_MODULE=ibus
# XMODIFIERS must match fcitx5's XIM server name on the X display (see xprop -root | grep XIM_SERVERS).
export XMODIFIERS=@im=fcitx
export SDL_IM_MODULE=fcitx
export GLFW_IM_MODULE=ibus
