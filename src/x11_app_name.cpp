#include "x11_app_name.h"

#include <xcb/xcb.h>

#include <cstdlib>
#include <cstring>
#include <unordered_map>

namespace {

/// Lazily connect to the default X display ($DISPLAY — XWayland in a
/// Wayland session).  Returns nullptr when no X server is reachable.
xcb_connection_t *x11Connection() {
  static xcb_connection_t *conn = xcb_connect(nullptr, nullptr);
  if (!conn || xcb_connection_has_error(conn)) {
    return nullptr;
  }
  return conn;
}

/// Intern an atom with a per-process cache (roundtrip only once).
xcb_atom_t internAtom(xcb_connection_t *conn, const char *name) {
  static std::unordered_map<std::string, xcb_atom_t> cache;
  auto it = cache.find(name);
  if (it != cache.end()) {
    return it->second;
  }
  auto cookie = xcb_intern_atom(conn, 0, strlen(name), name);
  auto *reply = xcb_intern_atom_reply(conn, cookie, nullptr);
  xcb_atom_t atom = XCB_ATOM_NONE;
  if (reply) {
    atom = reply->atom;
  }
  free(reply);
  cache[name] = atom;
  return atom;
}

/// WM_CLASS of `win`; walks up the parent chain (bounded) because the X
/// input focus may sit on a child widget while WM_CLASS lives on the
/// toplevel.  Returns "" when no WM_CLASS is found.
std::string wmClassOf(xcb_connection_t *conn, xcb_window_t win, int depth) {
  if (!win || depth > 10) {
    return "";
  }
  auto cookie = xcb_get_property(conn, 0, win, internAtom(conn, "WM_CLASS"),
                                 XCB_ATOM_STRING, 0, 64);
  auto *reply = xcb_get_property_reply(conn, cookie, nullptr);
  std::string result;
  if (reply && reply->type != XCB_ATOM_NONE && reply->format == 8) {
    const char *data = static_cast<const char *>(xcb_get_property_value(reply));
    int len = xcb_get_property_value_length(reply);
    // WM_CLASS is "instance\0class\0".  Prefer the class string (what the
    // X11 frontend uses as program name); fall back to the instance when
    // only one string is present.
    const char *firstEnd = static_cast<const char *>(
        memchr(data, '\0', static_cast<size_t>(len)));
    if (firstEnd) {
      const char *second = firstEnd + 1;
      int secondLen = len - static_cast<int>(second - data);
      const char *secondEnd = static_cast<const char *>(
          memchr(second, '\0', static_cast<size_t>(secondLen)));
      if (secondEnd && secondEnd > second) {
        result.assign(second, secondEnd - second);
      } else if (firstEnd > data) {
        result.assign(data, firstEnd - data);
      }
    }
  }
  free(reply);
  if (!result.empty()) {
    return result;
  }
  // No WM_CLASS on this window — try the parent.
  auto treeCookie = xcb_query_tree(conn, win);
  auto *treeReply = xcb_query_tree_reply(conn, treeCookie, nullptr);
  xcb_window_t parent = treeReply ? treeReply->parent : 0;
  free(treeReply);
  if (!parent || parent == win) {
    return "";
  }
  return wmClassOf(conn, parent, depth + 1);
}

} // namespace

std::string x11FocusedWmClass() {
  xcb_connection_t *conn = x11Connection();
  if (!conn) {
    return "";
  }
  auto cookie = xcb_get_input_focus(conn);
  auto *reply = xcb_get_input_focus_reply(conn, cookie, nullptr);
  if (!reply) {
    return "";
  }
  xcb_window_t focus = reply->focus;
  free(reply);
  return wmClassOf(conn, focus, 0);
}
