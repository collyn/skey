#!/usr/bin/env python3
"""Check setup and generated login scripts without touching the desktop."""
import json
import os
from pathlib import Path
import subprocess
import tempfile
import unittest

SOURCE = (Path(__file__).parent / "skey-setup").read_text()
ENVIRONMENT = SOURCE[
    SOURCE.index("# ── Determine required environment variables"):
    SOURCE.index("# ── Stop ibus if it's running")
]
APPLY = SOURCE[
    SOURCE.index("# ── Now update session environment"):
    SOURCE.index("# ── Wayland: disable classicui")
]
MODULES = ("GTK_IM_MODULE", "QT_IM_MODULE", "SDL_IM_MODULE")
REPORT = "import json,os; print(json.dumps({k:os.environ[k] for k in " + repr(MODULES) + " if k in os.environ}))"

# Model the manager environment, including the effect of dbus-update's
# --systemd option. No command in the test contacts the real session bus.
MOCK = '''#!/usr/bin/python3
import json, os, sys
from pathlib import Path
args = sys.argv[1:]
with open(os.environ["SKEY_TEST_CALLS"], "a") as log:
    log.write(json.dumps([Path(sys.argv[0]).name] + args) + "\\n")
path = Path(os.environ["SKEY_TEST_MANAGER"])
state = json.loads(path.read_text())
if "show-environment" in args:
    print("\\n".join(k + "=" + v for k, v in sorted(state.items())))
elif "unset-environment" in args:
    for key in args[args.index("unset-environment") + 1:]:
        state.pop(key, None)
else:
    for arg in args:
        if "=" in arg:
            key, value = arg.split("=", 1)
            state[key] = value
path.write_text(json.dumps(state))
'''


class KdeEnvironmentTest(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory(prefix="skey-env-test-", dir="/tmp")
        self.addCleanup(self.tmp.cleanup)
        self.root = Path(self.tmp.name)
        bindir = self.root / "bin"
        bindir.mkdir()
        for name in ("systemctl", "dbus-update-activation-environment"):
            command = bindir / name
            command.write_text(MOCK)
            command.chmod(0o755)
        self.manager = self.root / "manager.json"
        self.calls = self.root / "calls.jsonl"
        self.manager.write_text(json.dumps({"KEEP_ME": "unchanged"}))
        self.env = dict(os.environ, PATH=str(bindir) + ":/usr/bin:/bin",
                        SKEY_TEST_HOME=str(self.root),
                        SKEY_TEST_MANAGER=str(self.manager), SKEY_TEST_CALLS=str(self.calls),
                        USER_DE="KDE Plasma", SESSION_TYPE="wayland", USER_SHELL="zsh", WRITE_ENV="1")
        for key in MODULES:
            self.env.pop(key, None)
        self.login = self.root / ".config/plasma-workspace/env/fcitx5-skey.sh"

    def pollute(self, value):
        state = {**json.loads(self.manager.read_text()), **dict.fromkeys(MODULES, value)}
        self.manager.write_text(json.dumps(state))
        self.env.update(dict.fromkeys(MODULES, value))

    def run_shell(self, script):
        result = subprocess.run(["bash", "-c", script], env=self.env,
                                text=True, capture_output=True, timeout=10)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        return json.loads(result.stdout.splitlines()[-1])

    def setup_environment(self):
        # Exercise the actual environment sections; redirect only config
        # paths, and bypass unrelated system-wide legacy cleanup helpers.
        script = '''set -e
BEGIN_MARKER="# >>> fcitx5-skey >>>"
END_MARKER="# <<< fcitx5-skey <<<"
remove_shell_env() { :; }
remove_stale_profile_d() { :; }
remove_stale_system_env() { :; }
'''
        script += (ENVIRONMENT + APPLY).replace("$HOME", "$SKEY_TEST_HOME")
        script += '\npython3 -c "$SKEY_TEST_REPORT"\n'
        self.env["SKEY_TEST_REPORT"] = REPORT
        return self.run_shell(script)

    def assert_native_environment(self, child):
        self.assertEqual(child, {})
        state = json.loads(self.manager.read_text())
        self.assertEqual(state["KEEP_ME"], "unchanged")
        self.assertEqual(state["XMODIFIERS"], "@im=fcitx")
        self.assertEqual(state["GLFW_IM_MODULE"], "ibus")
        self.assertTrue(all(key not in state for key in MODULES), state)

    def test_setup_and_login_remove_old_values(self):
        self.pollute("ibus")
        self.login.parent.mkdir(parents=True)
        self.login.write_text("# fcitx5-skey\nexport QT_IM_MODULE=\n")
        self.assert_native_environment(self.setup_environment())
        for _ in range(2):
            self.pollute("fcitx")
            child = self.run_shell('. "$SKEY_TEST_HOME/.config/plasma-workspace/env/fcitx5-skey.sh"\n'
                                   'python3 -c "$SKEY_TEST_REPORT"')
            self.assert_native_environment(child)
        calls = [json.loads(line) for line in self.calls.read_text().splitlines()]
        self.assertTrue(all(not any(arg.startswith(tuple(key + "=" for key in MODULES))
                                    for arg in call) for call in calls), calls)

    def test_empty_session_values_are_repaired_without_rewriting_login(self):
        self.setup_environment()
        before = self.login.stat().st_mtime_ns
        self.pollute("")
        self.assert_native_environment(self.setup_environment())
        self.assertEqual(self.login.stat().st_mtime_ns, before)

    def test_clean_setup_is_a_noop(self):
        self.setup_environment()
        before = self.login.stat().st_mtime_ns
        self.calls.write_text("")
        self.assert_native_environment(self.setup_environment())
        self.assertEqual(self.login.stat().st_mtime_ns, before)
        calls = [json.loads(line) for line in self.calls.read_text().splitlines()]
        self.assertTrue(all("show-environment" in call for call in calls), calls)

    def test_no_env_preserves_configuration(self):
        self.pollute("")
        self.env["WRITE_ENV"] = "0"
        self.assertEqual(self.setup_environment(), dict.fromkeys(MODULES, ""))
        self.assertFalse(self.login.exists())
        state = json.loads(self.manager.read_text())
        self.assertTrue(all(key in state for key in MODULES))

    def test_other_sessions_keep_their_toolkit_modules(self):
        for desktop, session in (("GNOME", "wayland"), ("KDE Plasma", "x11")):
            with self.subTest(desktop=desktop, session=session):
                self.env.update(USER_DE=desktop, SESSION_TYPE=session, WRITE_ENV="1")
                self.assertEqual(self.setup_environment(), dict.fromkeys(MODULES, "fcitx5"))
                state = json.loads(self.manager.read_text())
                self.assertEqual({key: state[key] for key in MODULES}, dict.fromkeys(MODULES, "fcitx5"))


if __name__ == "__main__":
    unittest.main()
