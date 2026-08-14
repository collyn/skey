#ifndef FCITX5_SKEY_X11_APP_NAME_H
#define FCITX5_SKEY_X11_APP_NAME_H

#include <string>

/// Resolve the focused X11 window's WM_CLASS class string.
///
/// The IBus frontend reports an empty program name for apps that only
/// speak the ibus protocol (AppImages, some Electron apps) — the client
/// name is generic and the D-Bus sender PID cannot always be resolved.
/// Those apps are X11/XWayland clients, so the focused X11 window's
/// WM_CLASS identifies them the same way the X11 frontend names XIM
/// clients (instance\0class\0 — the class string is used).
///
/// Returns "" when no X server is reachable or no WM_CLASS is found —
/// callers then fall back to the shared empty "(IBus app)" key.
std::string x11FocusedWmClass();

#endif // FCITX5_SKEY_X11_APP_NAME_H
