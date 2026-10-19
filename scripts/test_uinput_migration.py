#!/usr/bin/env python3
"""Exercise install/upgrade cleanup in temporary roots; never touch the host."""
import os
from pathlib import Path
import re
import subprocess
import tempfile

SOURCE = (Path(__file__).parent / "skey-uinput-access").read_text()


def scenario(*, action="configure", merged=False, offline=False, stop_fail=False, orphan=False, loaded=True):
    with tempfile.TemporaryDirectory(prefix="skey-access-test-") as tmp:
        root = Path(tmp)
        bindir = root / "bin"
        bindir.mkdir()
        log = root / "calls"
        # Rewrite every managed absolute host path, leaving /dev/null alone.
        source = re.sub(r"(?<![A-Za-z0-9_])/(?:usr/|etc/|lib/|run/|var/|proc/|dev/uinput\b)",
                        lambda match: str(root) + match.group(), SOURCE)
        script = root / "helper"
        script.write_text(source)
        commands = {"id": "echo 0", "getent": "echo old-account"}
        for command in ("systemctl", "setfacl", "modprobe", "udevadm"):
            commands[command] = f'echo "{command} $*" >> "$CALLS"'
        commands["systemctl"] += '''
if [ "$1" = stop ] && [ "$STOP_FAIL" = 1 ]; then exit 1; fi
if [ "$1" = list-units ] && [ "$LOADED" = 1 ]; then
    echo "fcitx5-skey-uinput-server@alice.service loaded active running Legacy service"
fi
'''
        for name, body in commands.items():
            path = bindir / name
            path.write_text("#!/bin/sh\n" + body + "\n")
            path.chmod(0o755)
        if merged:
            (root / "usr/lib").mkdir(parents=True)
            (root / "lib").symlink_to("usr/lib")
        obsolete = [
            "etc/polkit-1/rules.d/60-fcitx5-skey-uinput.rules",
            "usr/share/polkit-1/rules.d/60-fcitx5-skey-uinput.rules.rpmsave",
            "etc/udev/rules.d/99-skey-uinput.rules",
            "etc/udev/rules.d/99-skey-uinput.rules.dpkg-old",
            "lib/udev/rules.d/99-skey-uinput.rules",
            "usr/local/lib/udev/rules.d/99-skey-uinput.rules",
            "usr/lib/systemd/system/fcitx5-skey-uinput-server@.service",
            "etc/systemd/system/fcitx5-skey-uinput-server@alice.service",
            "usr/local/lib/systemd/system/fcitx5-skey-uinput-server@.service.skey-legacy-disabled",
            "usr/lib/sysusers.d/skey-uinput.conf",
            "etc/sysusers.d/skey-uinput.conf",
            "usr/bin/fcitx5-skey-uinput-server",
            "usr/local/bin/fcitx5-skey-uinput-server",
        ]
        if not merged:
            obsolete.append("usr/lib/udev/rules.d/99-skey-uinput.rules")
        for name in obsolete:
            path = root / name
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text("local edits preserved: " + name)
        dropin = root / "etc/systemd/system/fcitx5-skey-uinput-server@.service.d"
        dropin.mkdir()
        (dropin / "override.conf").write_text("local unit override")
        unrelated = root / "etc/udev/rules.d/80-other-uinput.rules"
        unrelated.write_text("unrelated policy")
        (root / "dev").mkdir()
        (root / "dev/uinput").symlink_to("/dev/null")  # -c check, never opened
        links = []
        for suffix in ("wants", "requires"):
            directory = root / ("etc/systemd/system/multi-user.target." + suffix)
            directory.mkdir()
            link = directory / "fcitx5-skey-uinput-server@alice.service"
            link.symlink_to("/retired-unit")
            links.append(link)
        if not offline:
            (root / "run/systemd/system").mkdir(parents=True)
        (root / "proc/123").mkdir(parents=True)
        (root / "proc/123/exe").symlink_to(
            "/usr/local/bin/fcitx5-skey-uinput-server (deleted)" if orphan else "/bin/unrelated")
        runtime = root / "run/skey-uinput-alice"
        runtime.mkdir(parents=True)
        other_runtime = root / "run/skey-uinput-unexpected"
        other_runtime.mkdir()
        (other_runtime / "keep").write_text("unrelated runtime content")
        env = dict(os.environ, PATH=str(bindir) + ":/usr/bin:/bin", CALLS=str(log),
                   STOP_FAIL="1" if stop_fail else "0", LOADED="1" if loaded else "0")

        def run():
            return subprocess.run(["sh", str(script), action], env=env,
                                  text=True, capture_output=True)

        first = run()
        if stop_fail or orphan:
            assert first.returncode != 0 and "SKey:" in first.stderr, first
            assert (root / obsolete[0]).is_file(), "failed stop must leave migration untouched"
            assert not log.exists() or "modprobe" not in log.read_text()
            return
        assert first.returncode == 0, first.stderr
        archive = root / "var/lib/fcitx5-skey/legacy-uinput"
        # Source-path rewrite also affects the absolute path appended by archive().
        def backup(path):
            return archive / str(path).lstrip("/")
        for name in obsolete:
            path = root / name
            assert not path.exists(), name
            saved = backup(path)
            if merged and name.startswith("lib/"):
                # /usr/lib was visited first; /lib is the same file.
                saved = backup(root / ("usr/" + name))
            assert saved.read_text() == "local edits preserved: " + name, saved
        assert (backup(dropin) / "override.conf").read_text() == "local unit override"
        assert all(not link.is_symlink() for link in links)
        assert unrelated.read_text() == "unrelated policy"
        assert not runtime.exists() and (other_runtime / "keep").exists()
        before = sorted(str(path) for path in archive.rglob("*"))
        second = run()
        assert second.returncode == 0, second.stderr
        assert before == sorted(str(path) for path in archive.rglob("*")), "idempotent backups"
        calls = log.read_text()
        assert calls.count("systemctl disable fcitx5-skey-uinput-server@alice.service") == 2
        assert "systemctl enable" not in calls and "systemctl restart" not in calls
        assert calls.count("setfacl -x u:skey_uinput ") == 2
        assert calls.index("udevadm settle") < calls.index("setfacl -x"), "drain old udev RUNs before ACL removal"
        assert "--sysname-match=uinput" in calls
        assert ("modprobe uinput" in calls) == (action == "configure")
        assert ("systemctl stop" in calls) == (not offline and loaded)
        # An old source install written again must not overwrite preserved edits.
        path = root / obsolete[0]
        path.write_text("new local edits")
        assert run().returncode == 0
        assert backup(path).read_text() == "local edits preserved: " + obsolete[0]
        assert Path(str(backup(path)) + ".old").read_text() == "new local edits"


scenario()
scenario(merged=True)
scenario(offline=True)
scenario(loaded=False)
scenario(action="remove")
scenario(stop_fail=True)
scenario(orphan=True)
print("Uinput migration tests passed")
