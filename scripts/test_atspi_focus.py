#!/usr/bin/env python3
"""Test AT-SPI2 focus events from Chrome.
Run this, then switch between Chrome's address bar and web content.
Press Ctrl+C to stop.

Requires: python3-gi, gir1.2-atspi-2.0
Install: sudo apt install python3-gi gir1.2-atspi-2.0
"""
import gi
gi.require_version('Atspi', '2.0')
from gi.repository import Atspi

def get_ancestors(obj, max_depth=6):
    """Get role chain of ancestors."""
    roles = []
    current = obj
    for _ in range(max_depth):
        parent = current.get_parent()
        if parent is None:
            break
        roles.append(parent.get_role_name())
        current = parent
    return roles

def on_state_changed(event):
    """Handle state-changed events."""
    # Only care about focused state being gained
    if event.type != "object:state-changed:focused":
        return
    if event.detail1 != 1:  # detail1=1 means gained focus
        return

    obj = event.source
    role = obj.get_role_name()
    name = obj.get_name()
    try:
        app = obj.get_application()
        app_name = app.get_name() if app else "?"
    except:
        app_name = "?"

    ancestors = get_ancestors(obj)
    has_doc_web = any("document" in r.lower() or "web" in r.lower()
                      for r in ancestors)

    location = "ADDRESS_BAR" if not has_doc_web else "WEB_CONTENT"

    print(f"[{location}] app={app_name} role={role} name='{name}' "
          f"ancestors={ancestors}")

def main():
    Atspi.init()
    print("Monitoring AT-SPI2 focus events...")
    print("Switch between Chrome address bar and web content.")
    print("Press Ctrl+C to stop.\n")

    listener = Atspi.EventListener.new(on_state_changed)
    listener.register("object:state-changed:focused")

    try:
        Atspi.event_main()
    except KeyboardInterrupt:
        print("\nStopped.")

if __name__ == "__main__":
    main()
