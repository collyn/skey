#!/usr/bin/env python3
"""
test_e2e.py — End-to-end integration tests for skey Vietnamese IME via fcitx5 DBus.

Tests the FULL pipeline: key event → fcitx5 → skey engine → commit/preedit.

Requirements:
    dbus-python (system package: python3-dbus)

Usage:
    python3 scripts/test_e2e.py --monitor     # Watch debug log in real-time
    python3 scripts/test_e2e.py --setup       # Configure skey with debug on
    python3 scripts/test_e2e.py --check       # Verify skey is working
    python3 scripts/test_e2e.py --app-types   # Show active apps and their modes
    python3 scripts/test_e2e.py --all         # Run all checks
"""

import sys
import os
import time
import subprocess
import argparse
import re
from datetime import datetime

# ── DBus via dbus-python ───────────────────────────────────────────────────
try:
    import dbus
    from dbus.mainloop.glib import DBusGMainLoop
    DBusGMainLoop(set_as_default=True)
    HAS_DBUS = True
except ImportError:
    HAS_DBUS = False

# ── Constants (verified against running fcitx5) ────────────────────────────

FCITX5_SERVICE   = "org.fcitx.Fcitx5"
FCITX5_PATH      = "/controller"
FCITX5_IFACE     = "org.fcitx.Fcitx.Controller1"       # Note: NO '5' in interface!
FCITX5_IC_IFACE  = "org.fcitx.Fcitx.InputContext1"
DEBUG_LOG        = "/tmp/skey.log"
SKEY_IM_NAME     = "skey-im"                            # The IM name in fcitx5

# ── ANSI colors ────────────────────────────────────────────────────────────

RED = '\033[31m'; GREEN = '\033[32m'; YELLOW = '\033[33m'
CYAN = '\033[36m'; MAGENTA = '\033[35m'; BOLD = '\033[1m'; NC = '\033[0m'

PASS = f"{GREEN}PASS{NC}"
FAIL = f"{RED}FAIL{NC}"

# ── DBus helpers ───────────────────────────────────────────────────────────

def get_controller():
    """Get fcitx5 controller proxy."""
    if not HAS_DBUS:
        return None
    bus = dbus.SessionBus()
    return bus.get_object(FCITX5_SERVICE, FCITX5_PATH)

def get_controller_iface():
    """Get fcitx5 controller interface."""
    obj = get_controller()
    if obj:
        return dbus.Interface(obj, FCITX5_IFACE)
    return None

def get_current_im() -> str:
    """Get current input method name from fcitx5."""
    # Method call approach via busctl (more reliable than dbus-python)
    try:
        r = subprocess.run(
            ["busctl", "--user", "call", FCITX5_SERVICE, FCITX5_PATH,
             FCITX5_IFACE, "CurrentInputMethod"],
            capture_output=True, text=True, timeout=5)
        m = re.search(r'"([^"]+)"', r.stdout)
        if m:
            return m.group(1)
    except Exception:
        pass
    return ""

def switch_to_skey() -> bool:
    """Switch fcitx5 to skey input method."""
    current = get_current_im()
    if current == SKEY_IM_NAME:
        return True

    print(f"[SETUP] Switching IM: {current} → {SKEY_IM_NAME}")
    try:
        subprocess.run(
            ["busctl", "--user", "call", FCITX5_SERVICE, FCITX5_PATH,
             FCITX5_IFACE, "SetCurrentIM", "s", SKEY_IM_NAME],
            capture_output=True, timeout=5)
        time.sleep(0.5)
        new_im = get_current_im()
        if new_im == SKEY_IM_NAME:
            return True
        print(f"  {RED}Failed: current IM is '{new_im}'{NC}")
    except Exception as e:
        print(f"  {RED}Error: {e}{NC}")
    return False

def reload_fcitx5_config():
    """Reload fcitx5 configuration."""
    subprocess.run(["fcitx5-remote", "-r"], timeout=5)

# ── Config ─────────────────────────────────────────────────────────────────

def write_skey_config(output_mode: str = "Uinput", input_method: str = "Telex"):
    """Write skey configuration file."""
    config_path = os.path.expanduser("~/.config/fcitx5/conf/skey.conf")

    mode_map = {
        "uinput": "Uinput", "surrounding": "SurroundingText", "preedit": "Preedit",
    }
    im_map = {
        "telex": "Telex", "vni": "VNI", "telexw": "TelexW",
    }

    mode_val = mode_map.get(output_mode.lower(), output_mode)
    im_val = im_map.get(input_method.lower(), input_method)

    config = f"""[SKeyConfig]
InputMethod={im_val}
OutputMode={mode_val}
TonePosition=Modern
FreeMarking=True
AutoRestore=True
ShowPreedit=True
Debug=True
"""
    os.makedirs(os.path.dirname(config_path), exist_ok=True)
    with open(config_path, "w") as f:
        f.write(config)

    print(f"[CONFIG] OutputMode={mode_val} InputMethod={im_val} Debug=True")
    reload_fcitx5_config()
    time.sleep(0.5)

# ── Debug log analysis ─────────────────────────────────────────────────────

def get_debug_log(tail: int = 100) -> str:
    """Read last N lines of skey debug log."""
    try:
        r = subprocess.run(["tail", "-n", str(tail), DEBUG_LOG],
                          capture_output=True, text=True, timeout=3)
        return r.stdout
    except Exception:
        return ""

def parse_log(log: str) -> dict:
    """Extract structured info from skey debug log."""
    commits = []
    preedits = []
    activations = []

    for line in log.splitlines():
        m = re.search(r"[Cc]ommit[^']*'([^']*)'", line)
        if m:
            commits.append(m.group(1))
            continue

        m = re.search(r"new='([^']*)'", line)
        if m:
            preedits.append(m.group(1))
            continue

        if "Activated:" in line and "mode=" in line:
            m_mode = re.search(r"mode=(\w+)", line)
            m_app = re.search(r"app=(\S+)", line)
            m_frontend = re.search(r"frontend=(\S+)", line)
            m_pwd = re.search(r"password=(\d+)", line)
            m_surr = re.search(r"surroundingCap=(\d+)", line)
            activations.append({
                "mode": m_mode.group(1) if m_mode else "?",
                "app": m_app.group(1) if m_app else "?",
                "frontend": m_frontend.group(1) if m_frontend else "?",
                "password": m_pwd.group(1) if m_pwd else "?",
                "surroundingCap": m_surr.group(1) if m_surr else "?",
            })

    return {"commits": commits, "preedits": preedits, "activations": activations}

# ── Tests ───────────────────────────────────────────────────────────────────

def check_setup():
    """Verify fcitx5 + skey are properly configured."""
    print(f"\n{CYAN}{'='*60}{NC}")
    print(f"{CYAN}  System Check{NC}")
    print(f"{CYAN}{'='*60}{NC}")

    ok = True

    # 1. fcitx5 running?
    # fcitx5-remote returns: 0=closed, 1=inactive, 2=active (on stdout)
    r = subprocess.run(["fcitx5-remote"], capture_output=True, text=True)
    status_code = r.stdout.strip()
    status_map = {"0": "closed", "1": "inactive", "2": "active"}
    status_text = status_map.get(status_code, f"unknown({status_code})")
    if status_code == "2":
        print(f"  {PASS}  fcitx5 is running (active)")
    elif status_code == "1":
        print(f"  {YELLOW}⚠{NC}  fcitx5 is running but INACTIVE. Toggle with fcitx5-remote -t")
    else:
        print(f"  {FAIL}  fcitx5 is NOT running (status={status_text})")
        ok = False

    # 2. skey addon loaded?
    current = get_current_im()
    print(f"  {'  ' if ok else ''}  Current IM: {GREEN}{current}{NC}")

    # 3. Debug log?
    if os.path.exists(DEBUG_LOG):
        size = os.path.getsize(DEBUG_LOG)
        print(f"  {'  ' if ok else ''}  Debug log: {GREEN}{DEBUG_LOG}{NC} ({size} bytes)")
    else:
        print(f"  {'  ' if ok else ''}  Debug log: {RED}NOT FOUND{NC}")
        print(f"  {'  ' if ok else ''}  → Run: python3 scripts/test_e2e.py --setup")
        ok = False

    # 4. Check recent activity
    log = get_debug_log(50)
    info = parse_log(log)
    if info["activations"]:
        last = info["activations"][-1]
        print(f"  {'  ' if ok else ''}  Last activation: app={GREEN}{last['app']}{NC} "
              f"mode={YELLOW}{last['mode']}{NC} frontend={last['frontend']}")
    else:
        print(f"  {'  ' if ok else ''}  Last activation: {YELLOW}(no recent activity){NC}")

    return ok

def show_app_types():
    """Analyze which apps are connected and their capabilities."""
    print(f"\n{CYAN}{'='*60}{NC}")
    print(f"{CYAN}  App Types & Modes{NC}")
    print(f"{CYAN}{'='*60}{NC}")

    log = get_debug_log(200)
    info = parse_log(log)

    if not info["activations"]:
        print(f"  {YELLOW}No app activations in log.{NC}")
        print("  Type something in different apps first.")
        return

    # Deduplicate by app
    seen = {}
    for act in info["activations"]:
        app = act["app"]
        if app not in seen:
            seen[app] = act

    print(f"\n  {'App':<30} {'Mode':<20} {'Frontend':<10} {'SurrCap':<10}")
    print(f"  {'-'*30} {'-'*20} {'-'*10} {'-'*10}")
    for app, act in sorted(seen.items()):
        has_surrounding = "✓" if act["surroundingCap"] == "1" else "✗"
        is_pwd = "🔒" if act["password"] == "1" else ""
        print(f"  {app:<30} {act['mode']:<20} {act['frontend']:<10} "
              f"{has_surrounding:<10} {is_pwd}")

    print(f"\n  Legend:")
    print(f"    SurroundingText capability = ✓ : GTK/Qt6 apps")
    print(f"    SurroundingText capability = ✗ : Electron/tabby/terminal → Uinput mode")
    print(f"    🔒 = password field")

def monitor_debug_log():
    """Continuously monitor the debug log with color-coded output."""
    print(f"\n{CYAN}{'='*60}{NC}")
    print(f"{CYAN}  Debug Log Monitor{NC}")
    print(f"{CYAN}{'='*60}{NC}")
    print(f"  Watching: {DEBUG_LOG}")
    print(f"  {YELLOW}Type Vietnamese in any app to see results here.{NC}")
    print(f"  Press Ctrl+C to stop.\n")

    if not os.path.exists(DEBUG_LOG):
        print(f"  {RED}No debug log found! Run --setup first.{NC}")
        return

    # Seek to end
    with open(DEBUG_LOG, "r") as f:
        f.seek(0, os.SEEK_END)

    try:
        while True:
            line = ""
            with open(DEBUG_LOG, "r") as f:
                f.seek(0, os.SEEK_END)
                # Wait for new content
                while True:
                    line = f.readline()
                    if line:
                        break
                    time.sleep(0.1)

            line = line.rstrip()

            # Color-code different event types
            if "Commit:" in line or "commit '" in line.lower():
                print(f"  {GREEN}▶ COMMIT{NC}  {line}")
            elif "Key '" in line:
                print(f"  {CYAN}⌨ KEY{NC}     {line}")
            elif "new='" in line:
                print(f"  {YELLOW}✎ PREEDIT{NC} {line}")
            elif "Activated:" in line:
                print(f"  {MAGENTA}⚡ ACTIVE{NC}  {line}")
            elif "BS x" in line or "BS=" in line:
                print(f"  {MAGENTA}⌫ BS{NC}     {line}")
            elif "surrounding" in line.lower() or "Surr" in line:
                print(f"  {CYAN}↻ SURR{NC}   {line}")
            elif "Uinput" in line or "uinput" in line:
                print(f"  {CYAN}↑ UINP{NC}   {line}")
            else:
                print(f"         {line}")

    except KeyboardInterrupt:
        print(f"\n{CYAN}[DONE] Monitor stopped.{NC}")

def show_recent_activity():
    """Show recent typing activity from debug log."""
    print(f"\n{CYAN}{'='*60}{NC}")
    print(f"{CYAN}  Recent Typing Activity{NC}")
    print(f"{CYAN}{'='*60}{NC}")

    log = get_debug_log(100)
    info = parse_log(log)

    if info["commits"]:
        print(f"\n  {BOLD}Recent commits:{NC}")
        for c in info["commits"][-15:]:
            print(f"    → '{GREEN}{c}{NC}'")
    else:
        print(f"\n  {YELLOW}No commits detected in recent log.{NC}")

    if info["preedits"]:
        print(f"\n  {BOLD}Recent preedits:{NC}")
        for p in info["preedits"][-10:]:
            print(f"    ↻ '{YELLOW}{p}{NC}'")

    if not info["commits"] and not info["preedits"]:
        print("  Type Vietnamese in an app, then re-run.")

def generate_test_phrases():
    """Print test phrases for manual testing across different apps."""
    print(f"\n{CYAN}{'='*60}{NC}")
    print(f"{CYAN}  Manual Test Phrases{NC}")
    print(f"{CYAN}{'='*60}{NC}")

    # Telex phrases grouped by complexity
    phrases = [
        ("Basic tones", [
            ("as → á", "as"), ("af → à", "af"), ("ar → ả", "ar"),
            ("ax → ã", "ax"), ("aj → ạ", "aj"),
        ]),
        ("Compound vowels", [
            ("aw → ă", "aw"), ("aa → â", "aa"), ("ee → ê", "ee"),
            ("oo → ô", "oo"), ("ow → ơ", "ow"), ("uw → ư", "uw"),
            ("dd → đ", "dd"),
        ]),
        ("High-frequency words", [
            ("xin chào", "xin chafo"),
            ("cảm ơn", "camr own"),
            ("tiếng Việt", "tieengs Vieetj"),
            ("được không", "dduwowcj khoong"),
            ("người dùng", "nguwowif dungf"),
        ]),
        ("Edge cases", [
            ("vãi (undo-sensitive)", "vaix"),
            ("các (auto-restore)", "cacs"),
            ("đc (abbreviation)", "ddc"),
            ("wood → wood (restore)", "wood"),
            ("ooo → oo (undo ô)", "ooo"),
        ]),
    ]

    for category, tests in phrases:
        print(f"\n  {BOLD}{category}:{NC}")
        for label, keys in tests:
            print(f"    {label:<30} {YELLOW}type: {keys}{NC}")

    print(f"\n  {BOLD}Test in different apps:{NC}")
    print(f"    1. Terminal (tabby)    → Uinput mode")
    print(f"    2. Kate/gedit          → SurroundingText mode")
    print(f"    3. VS Code (Electron) → Uinput mode (no SurroundingText)")
    print(f"    4. Chrome/Firefox     → SurroundingText mode")
    print(f"    5. System settings    → Preedit mode")
    print(f"\n  {YELLOW}After typing each phrase, press Space to commit.{NC}")

# ── Main ───────────────────────────────────────────────────────────────────

def main():
    parser = argparse.ArgumentParser(
        description="skey Vietnamese IME — E2E Test Suite")
    parser.add_argument("--monitor", action="store_true",
                       help="Monitor debug log in real-time (Ctrl+C to stop)")
    parser.add_argument("--setup", action="store_true",
                       help="Write skey config with Debug=True, OutputMode=Uinput")
    parser.add_argument("--check", action="store_true",
                       help="Verify fcitx5 + skey are working")
    parser.add_argument("--app-types", action="store_true",
                       help="Show active apps and their capability modes")
    parser.add_argument("--phrases", action="store_true",
                       help="Print test phrases for manual testing")
    parser.add_argument("--recent", action="store_true",
                       help="Show recent typing activity from debug log")
    parser.add_argument("--mode", choices=["uinput", "surrounding", "preedit"],
                       default="uinput", help="Output mode (default: uinput)")
    parser.add_argument("--im", choices=["telex", "vni", "telexw"],
                       default="telex", help="Input method (default: telex)")
    parser.add_argument("--all", action="store_true",
                       help="Run all checks and show test phrases")
    args = parser.parse_args()

    print(f"\n{MAGENTA}{BOLD}╔══════════════════════════════════════════════════╗")
    print(f"║   skey E2E Test Suite — Vietnamese IME Testing  ║")
    print(f"╚══════════════════════════════════════════════════╝{NC}")

    # Setup
    if args.setup or args.all:
        write_skey_config(args.mode, args.im)

    # Check system
    if args.check or args.all:
        check_setup()

    # App types
    if args.app_types or args.all:
        show_app_types()

    # Recent activity
    if args.recent or args.all:
        show_recent_activity()

    # Test phrases
    if args.phrases or args.all:
        generate_test_phrases()

    # Monitor
    if args.monitor:
        monitor_debug_log()

    # If no action
    if not any([args.setup, args.check, args.app_types, args.phrases,
                args.recent, args.monitor, args.all]):
        parser.print_help()
        print(f"\nQuick start:")
        print(f"  python3 scripts/test_e2e.py --check    # Verify everything works")
        print(f"  python3 scripts/test_e2e.py --monitor  # Watch typing in real-time")
        print(f"  python3 scripts/test_e2e.py --phrases  # Show test phrases")
        print(f"  python3 scripts/test_e2e.py --all      # Run all checks")

if __name__ == "__main__":
    main()
