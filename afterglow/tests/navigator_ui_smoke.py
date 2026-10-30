#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 fuddlesworth
# SPDX-License-Identifier: GPL-3.0-or-later
"""Real picker -> layout -> reuse -> persistence, in an isolated nested desktop."""
import argparse
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import time
from nested_desktop import NestedDesktop, wait_for

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("binary", type=Path)
parser.add_argument("--artifacts", type=Path, default=Path("/tmp/afterglow-navigator-ui"))
parser.add_argument("--private-bus", action="store_true", help=argparse.SUPPRESS)
args = parser.parse_args()
binary = str(args.binary.resolve())
if not args.private_bus:
    sys.exit(subprocess.call(["dbus-run-session", "--", sys.executable, __file__, binary,
                             "--artifacts", str(args.artifacts), "--private-bus"]))
args.artifacts.mkdir(parents=True, exist_ok=True)
with tempfile.TemporaryDirectory(prefix="afterglow-navigator-") as temp:
    root = Path(temp)
    config = root / "afterglow"
    config.mkdir()
    (root / ".bashrc").write_text("PS1='L3> '\n")
    (config / "config.json").write_text(json.dumps({
        "Terminal": {"shell": "/bin/bash", "env": {"HOME": temp}, "confirmClose": False},
        "Control": {"allowReadText": True}, "QuickTerminal": {"shortcut": ""},
        "Audio": {"enabled": False}, "Animation": {"enabled": False},
        "Window": {"width": 900, "height": 600}}))
    repositories = root / "repositories"
    alpha, beta = repositories / "alpha", repositories / "beta"
    for repo in (alpha, beta):
        repo.mkdir(parents=True)
        subprocess.run(["git", "init", "-q", str(repo)], check=True)
    package = config / "scripts/project-navigator"
    source = Path(__file__).resolve().parents[2] / "data/examples/packages/project-navigator"
    with NestedDesktop(root) as desktop, (args.artifacts / "application.log").open("w") as log:
        env = dict(desktop.env, QT_FORCE_STDERR_LOGGING="1", QT_LOGGING_TO_CONSOLE="1",
                   QT_LOGGING_RULES="afterglow.scripts.info=true")
        def start():
            return subprocess.Popen([binary, "--title", "L3 navigator acceptance"], env=env,
                                    stdout=log, stderr=subprocess.STDOUT)
        app = start()
        def stop():
            app.terminate()
            try:
                app.wait(timeout=5)
            except subprocess.TimeoutExpired:
                app.kill()
                app.wait()
        def ctl(*values):
            return subprocess.check_output([binary, "ctl", *map(str, values)], env=env,
                                           text=True, stderr=subprocess.PIPE, timeout=5).strip()
        def panes():
            try:
                return json.loads(ctl("ls", "--json"))
            except subprocess.CalledProcessError:
                return []
        def state():
            return json.loads((config / "state/scripts/project-navigator.json").read_text())
        def status():
            return json.loads(ctl("scripts"))
        def configured(roots):
            (package / "config.json").write_text(json.dumps({"roots": list(map(str, roots)), "fd": True, "zoxide": False}))
        def action(pane, name="open"):
            ctl("focus", "--pane", pane)
            ctl("action", "script:project-navigator." + name, "--pane", pane)
            time.sleep(0.7)  # asynchronous provider and compositor frames
        def choose(text, capture=None):
            desktop.text(text)
            if capture:
                desktop.screenshot(args.artifacts / (capture + ".png"))
            desktop.key("Return")
            time.sleep(0.6)
        try:
            first = wait_for(panes)[0]["id"]
            wait_for(lambda: "L3>" in ctl("get-text", "--pane", first, "--screen"))
            desktop.activate()
            # Install into a previously nonexistent scripts directory while running.
            package.parent.mkdir()
            shutil.copytree(source, package)
            configured([repositories])
            wait_for(lambda: status() and status()[0]["status"] == "active")
            second = int(ctl("split", "--pane", first, "--right"))
            wait_for(lambda: len(panes()) == 2)
            ctl("save-project", "two-pane", "--pane", first)
            # Discovery is real fd, selections are real keyboard/QML events.
            action(first)
            choose("alpha", "discovery")
            choose("two-pane", "layout")
            wait_for(lambda: len(panes()) == 4)
            wait_for(lambda: state()["value"].get("templates", {}).get(str(alpha)) == "two-pane")
            opened = [p for p in panes() if p["cwd"] == str(alpha)]
            assert len(opened) == 2, panes()
            active = opened[0]["id"]
            ctl("send-text", "--pane", active, "printf 'PID_%s_END\\n' $$\\n")
            import re
            pid = wait_for(lambda: re.findall(r"PID_(\d+)_END", ctl("get-text", "--pane", active, "--screen")))[-1]
            action(active, "favorite")
            wait_for(lambda: state()["value"]["favorites"].get(str(alpha)))
            revision = state()["revision"]
            action(first)
            choose("alpha", "favorite-running")
            wait_for(lambda: state()["revision"] > revision)
            assert len(panes()) == 4, "Opening a running directory duplicated its window"
            assert pid in ctl("get-text", "--pane", active, "--screen")
            # Cancelling the picker must leave actions usable and storage unchanged.
            revision = state()["revision"]
            action(active)
            desktop.key("Escape")
            time.sleep(0.3)
            assert state()["revision"] == revision
            # Reload provider configuration, then choose a newly discoverable repo.
            external = root / "external"
            gamma = external / "gamma"
            gamma.mkdir(parents=True)
            subprocess.run(["git", "init", "-q", str(gamma)], check=True)
            configured([external])
            time.sleep(0.7)
            action(first)
            choose("gamma", "config-reload")
            choose("shell", "shell-layout")
            wait_for(lambda: len(panes()) == 5)
            wait_for(lambda: state()["value"]["recent"][0]["value"] == str(gamma))
            # Uninstall while its selector is open: UI disappears and state survives.
            action(first)
            before = (config / "state/scripts/project-navigator.json").read_bytes()
            shutil.move(package, root / "removed-package")
            wait_for(lambda: status() == [])
            desktop.screenshot(args.artifacts / "uninstalled.png")
            ctl("send-text", "--pane", first, "printf 'UNINSTALLED_OK\\n'\\n")
            wait_for(lambda: "UNINSTALLED_OK" in ctl("get-text", "--pane", first, "--screen"))
            assert (config / "state/scripts/project-navigator.json").read_bytes() == before
            shutil.move(root / "removed-package", package)
            wait_for(lambda: status() and status()[0]["status"] == "active")
            # Reinstall/restart preserves favorites and remembered templates.
            stop()
            app = start()
            first = wait_for(panes)[0]["id"]
            wait_for(lambda: "L3>" in ctl("get-text", "--pane", first, "--screen"))
            assert state()["value"]["favorites"][str(alpha)]
            action(first)
            choose("alpha", "restored-favorites")
            # Last-used layout is first: Enter alone should restore two panes.
            desktop.screenshot(args.artifacts / "remembered-layout.png")
            desktop.key("Return")
            wait_for(lambda: len(panes()) == 3)
            assert len([p for p in panes() if p["cwd"] == str(alpha)]) == 2
            desktop.screenshot(args.artifacts / "restored-layout.png")
            print("PASS: navigator UI and package lifecycle", flush=True)
            # Companion packages: real periodic saves, native restore picker and Git status.
            for name in ("project-autosave", "tab-status"):
                shutil.copytree(source.parent / name, package.parent / name)
            (package.parent / "project-autosave/config.json").write_text(json.dumps({"interval_seconds": 10}))
            wait_for(lambda: len(status()) == 3 and all(s["status"] == "active" for s in status()))
            active = next(p["id"] for p in panes() if p["cwd"] == str(alpha))
            ctl("save-project", "autosave-demo", "--pane", active)
            ctl("action", "script:project-autosave.enable", "--pane", active)
            autosave_state = config / "state/scripts/project-autosave.json"
            wait_for(lambda: autosave_state.exists() and json.loads(autosave_state.read_text())["value"].get("enabled", {}).get("autosave-demo"))
            third = int(ctl("split", "--pane", active, "--down"))
            def leaf_count(node):
                return 1 if "pane" in node else leaf_count(node["first"]) + leaf_count(node["second"])
            def saved_panes():
                layout = json.loads((config / "projects/autosave-demo.json").read_text())
                return sum(leaf_count(tab["tree"]) for tab in layout["tabs"])
            wait_for(lambda: saved_panes() == 3, timeout=20)
            stop()
            app = start()
            first = wait_for(panes)[0]["id"]
            wait_for(lambda: "L3>" in ctl("get-text", "--pane", first, "--screen"))
            ctl("focus", "--pane", first)
            ctl("action", "script:project-autosave.restore", "--pane", first)
            time.sleep(0.5)
            choose("autosave-demo", "autosave-restore")
            wait_for(lambda: len(panes()) == 4)
            active = next(p["id"] for p in panes() if p["cwd"] == str(alpha))
            ctl("focus", "--pane", active)
            ctl("new-tab", "--pane", active, "--cwd", alpha)
            ctl("focus", "--pane", active)
            ctl("send-text", "--pane", active, "sleep 3; false\\n")
            time.sleep(1.5)
            desktop.screenshot(args.artifacts / "status-running.png")
            time.sleep(5)  # command finishes, then all three tabs receive a refresh
            desktop.screenshot(args.artifacts / "status-failed.png")
            ctl("action", "script:tab-status.toggle", "--pane", active)
            status_state = config / "state/scripts/tab-status.json"
            wait_for(lambda: status_state.exists() and json.loads(status_state.read_text())["value"].get("enabled") is False)
            time.sleep(2)
            desktop.screenshot(args.artifacts / "status-disabled.png")
            ctl("action", "script:project-autosave.disable", "--pane", active)
            wait_for(lambda: not json.loads(autosave_state.read_text())["value"].get("enabled", {}).get("autosave-demo"))
            print("PASS: periodic project autosave, restart/native restore, tab status Git/job rendering and durable toggles", flush=True)
            errors = (args.artifacts / "application.log").read_text()
            assert not any(e in errors for e in ("TypeError", "ReferenceError", "QML failed", "Cannot assign")), errors
            print("PASS: live installation, fd discovery, keyboard picker/layout, running-window reuse, favorites, cancellation, config reload, uninstall with open selector, reinstall, restart and remembered split layout")
        except BaseException:
            desktop.screenshot(args.artifacts / "failure.png")
            print("PANES", panes(), file=sys.stderr)
            raise
        finally:
            stop()
