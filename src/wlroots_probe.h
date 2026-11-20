#ifndef SKEY_WLROOTS_PROBE_H
#define SKEY_WLROOTS_PROBE_H
#include <string_view>

namespace skey {

// KDE Plasma and GNOME keep the Libei portal for Native mode: KWin offers
// no virtual keyboard to ordinary clients and Mutter has neither protocol.
bool desktopUsesLibei(std::string_view xdgCurrentDesktop);

// True when the compositor advertises zwp_input_method_manager_v2 together
// with zwp_virtual_keyboard_manager_v1 (Sway, Hyprland, river, labwc,
// Wayfire, niri, ...).  Fcitx5 then speaks input-method-v2 and its
// forwardKey() rides its own virtual keyboard, which the compositor keeps
// out of the input-method grab: no loopback, no portal permission.
// Opens a short-lived connection to $WAYLAND_DISPLAY; false without one.
bool compositorHasInputMethodKeyboard();

} // namespace skey
#endif
