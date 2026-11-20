#include "wlroots_probe.h"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <string>

#ifdef SKEY_HAVE_WAYLAND_CLIENT
#include <wayland-client.h>
#endif

namespace skey {

bool desktopUsesLibei(std::string_view desktop) {
  std::string list = ":" + std::string(desktop) + ":";
  std::transform(list.begin(), list.end(), list.begin(),
                 [](unsigned char c) { return std::tolower(c); });
  for (const char *name : {":kde:", ":plasma:", ":kde-plasma:", ":gnome:",
                           ":gnome-classic:"})
    if (list.find(name) != std::string::npos) return true;
  return false;
}

#ifdef SKEY_HAVE_WAYLAND_CLIENT
namespace {
struct Globals {
  bool inputMethod = false;
  bool virtualKeyboard = false;
};
void global(void *data, wl_registry *, uint32_t, const char *interface, uint32_t) {
  auto &found = *static_cast<Globals *>(data);
  if (!std::strcmp(interface, "zwp_input_method_manager_v2")) found.inputMethod = true;
  if (!std::strcmp(interface, "zwp_virtual_keyboard_manager_v1")) found.virtualKeyboard = true;
}
void globalRemove(void *, wl_registry *, uint32_t) {}
const wl_registry_listener kListener = {global, globalRemove};
} // namespace

bool compositorHasInputMethodKeyboard() {
  wl_display *display = wl_display_connect(nullptr);
  if (!display) return false;
  Globals found;
  wl_registry *registry = wl_display_get_registry(display);
  wl_registry_add_listener(registry, &kListener, &found);
  wl_display_roundtrip(display);
  wl_registry_destroy(registry);
  wl_display_disconnect(display);
  return found.inputMethod && found.virtualKeyboard;
}
#else
bool compositorHasInputMethodKeyboard() { return false; }
#endif

} // namespace skey
