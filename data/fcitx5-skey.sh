# fcitx5-skey: environment variables for fcitx5 input method
# This file is sourced by /etc/profile.d/ for login shells and display managers.
#
# On KDE Plasma Wayland, QT_IM_MODULE is deliberately NOT set because KWin
# handles Qt input natively via zwp_input_method_v2.  Forcing QT_IM_MODULE=fcitx5
# would bypass KWin's IM handling and break Vietnamese input in Qt apps.

if [ "$XDG_SESSION_TYPE" = "wayland" ]; then
    case "${XDG_CURRENT_DESKTOP:-}" in
        KDE|kde|KDE-Plasma|plasma)
            export GTK_IM_MODULE=fcitx5
            export XMODIFIERS=@im=fcitx
            export SDL_IM_MODULE=fcitx5
            export GLFW_IM_MODULE=ibus
            ;;
        *)
            export GTK_IM_MODULE=fcitx5
            export QT_IM_MODULE=fcitx5
            export XMODIFIERS=@im=fcitx
            export SDL_IM_MODULE=fcitx5
            export GLFW_IM_MODULE=ibus
            ;;
    esac
else
    export GTK_IM_MODULE=fcitx5
    export QT_IM_MODULE=fcitx5
    export XMODIFIERS=@im=fcitx
    export SDL_IM_MODULE=fcitx5
    export GLFW_IM_MODULE=ibus
fi
