#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 fuddlesworth
# SPDX-License-Identifier: GPL-3.0-or-later
"""Native Luau -> control -> shader uniform acceptance, isolated from user terminals.

Requires the nested_desktop dependencies and dbus-run-session. Logs and
screenshots stay in the artifacts directory.
"""
import argparse
import json
import shutil
from pathlib import Path
import subprocess
import sys
import tempfile
import time
from nested_desktop import NestedDesktop, Session, check, exit_on_sigterm, install_inspector, run_on_private_bus, wait_for

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("binary", type=Path)
parser.add_argument("--artifacts", type=Path,
                    help="Directory for the log and screenshots (default: a new temporary directory)")
parser.add_argument("--timeout", type=float, default=300, help="Seconds for the whole run (default: 300)")
parser.add_argument("--private-bus", action="store_true", help=argparse.SUPPRESS)
args = parser.parse_args()
binary = str(args.binary.resolve())
if not args.private_bus:
    # The top-level process: --timeout bounds the run under the private bus.
    artifacts = args.artifacts or Path(tempfile.mkdtemp(prefix="afterglow-shader-parameters-"))
    artifacts.mkdir(parents=True, exist_ok=True)
    print(f"Artifacts: {artifacts}", flush=True)
    sys.exit(run_on_private_bus([sys.executable, __file__, binary, "--artifacts", artifacts.resolve(), "--private-bus"],
                                args.timeout))
if not args.artifacts:
    parser.error("--private-bus needs --artifacts")
exit_on_sigterm()
artifacts = args.artifacts


with tempfile.TemporaryDirectory(prefix="afterglow-shader-parameters-") as temp:
    root = Path(temp)
    config = root / "afterglow"
    read_state = install_inspector(config)
    source = Path(__file__).resolve().parents[2]
    shutil.copytree(source / "data/examples/packages/scene-controls", config / "scripts/scene-controls")
    # Every pack from the checkout, so an installed copy of a bundled pack
    # (the default overview, pane effects and tab switch too) cannot answer.
    shutil.copytree(source / "data/shaders", config / "shaders")
    writer = config / "scripts/writer.luau"
    writer.write_text('''
return {
 metadata={commands={pane={summary="Pane",palette="Pane"},tab={summary="Tab",palette="Tab"},window={summary="Window",palette="Window"},transition={summary="Transition",palette="Transition"},bad={summary="Invalid",palette="Invalid"},reset={summary="Reset",palette="Reset"}}},
 run_command=function(command,ev) return afterglow.request(command,afterglow.shaders(ev.pane)) end,
 on_result=function(ev)
  -- Each line names its command, so the harness waits for the answer to that command.
  local done=string.match(ev.request_id,"^done:(.+)$")
  if done then return afterglow.log((ev.ok and "WRITE_OK:" or "WRITE_REJECTED:")..done) end
  if not ev.ok then return afterglow.log("QUERY_FAILED:"..ev.request_id) end
  local shader=ev.data.shaders[ev.request_id=="window" and 3 or 1]
  local scope=ev.request_id=="tab" and "tab" or ev.request_id=="window" and "window" or "pane"
  local values=ev.request_id=="reset" and {} or {strength=ev.request_id=="bad" and 999 or 1.2}
  return afterglow.request("done:"..ev.request_id,afterglow.shader_parameters(ev.pane,shader.instance,values,scope,ev.request_id=="transition" and {duration_ms=3000,curve="linear"} or nil))
 end,
}
''')
    (root / ".bashrc").write_text("PS1='PARAMS> '\n")
    settings = {"Terminal": {"shell": "/bin/bash", "env": {"HOME": temp}, "confirmClose": False},
        "Control": {"allowReadText": True}, "QuickTerminal": {"shortcut": ""}, "Audio": {"enabled": False},
        "Animation": {"cursor": False, "scroll": False, "splits": False},
        "Window": {"initialColumns": 90, "initialRows": 28},
        "Shaders": {"pane": ["bloom", "bloom"], "window": ["bloom"], "params": {"bloom": {"strength": 0.1}},
                    "tabTransition": "", "paneEffects": ""}}
    settings_path = config / "config.json"
    settings_path.write_text(json.dumps(settings))
    with NestedDesktop(root) as desktop:
        env = dict(desktop.env, QT_FORCE_STDERR_LOGGING="1", QT_LOGGING_TO_CONSOLE="1", QT_LOGGING_RULES="afterglow.scripts.info=true")
        session = Session(binary, env, artifacts / "application.log")
        session.start("Live shader parameters")
        ctl = session.ctl
        def panes():
            try: return json.loads(ctl("ls", "--json"))
            except (subprocess.CalledProcessError, subprocess.TimeoutExpired): return []
        def screen_lines(pane):
            return [line.strip() for line in ctl("get-text", "--pane", pane, "--screen").splitlines()]
        def log_text():
            session.log.flush()
            return (artifacts / "application.log").read_text(errors="replace")
        def count(kind, name):
            return log_text().count(f"{kind}:{name}\n")
        def answers(name):
            return sum(count(kind, name) for kind in ("WRITE_OK", "WRITE_REJECTED", "QUERY_FAILED"))
        def command(pane, name, accepted=True):
            """Runs a writer command, waits until the writer logs the answer to its write,
            and checks that the write was accepted or rejected as expected."""
            before = answers(name)
            expected = "WRITE_OK" if accepted else "WRITE_REJECTED"
            matching = count(expected, name)
            ctl("action", "script:writer." + name, "--pane", pane)
            wait_for(lambda: answers(name) > before)
            check(f"QUERY_FAILED:{name}" not in log_text(), name)
            check(count(expected, name) > matching,
                  f"The {name} write was " + ("rejected" if accepted else "accepted"))
        def inspect(pane, kind="shaders"):
            """The shaders or the scene of a pane. aglow run returns after the state is written."""
            return read_state(session.run, pane)[kind]
        def until(pane, predicate, kind="shaders", timeout=10):
            """Reads the pane until predicate(data) is true, and returns the data.
            install_inspector paces the reads under the script's 20 effects a second."""
            deadline = time.monotonic() + timeout
            while not predicate(data := inspect(pane, kind)):
                check(time.monotonic() < deadline, f"no {kind} state matched", data)
                time.sleep(0.25)
            return data
        def settles(pane, index, expected):
            """Waits until a shader value reaches the expected value."""
            until(pane, lambda shaders: abs(shaders[index]["values"]["strength"] - expected) < 0.001)
        def near(value, expected): check(abs(value - expected) < 0.001, value, expected)
        try:
            first = wait_for(panes)[0]["id"]
            wait_for(lambda: "PARAMS>" in ctl("get-text", "--pane", first, "--screen"))
            second = int(ctl("split", "--pane", first, "--right"))
            for pane in [first, second]:
                ctl("send-text", "--pane", pane, "printf 'PHOSPHOR LIVE PARAMETERS\\n'\\n")
            desktop.activate()
            # The chains exist once both panes have drawn.
            before = until(first, lambda shaders: len(shaders) == 3)
            check(len({s["instance"] for s in before}) == 3, before)
            desktop.screenshot(artifacts / "baseline.png")
            command(first, "pane")
            near(inspect(first)[0]["values"]["strength"], 1.2)
            near(inspect(first)[1]["values"]["strength"], 0.1)
            near(inspect(second)[0]["values"]["strength"], 0.1)
            desktop.screenshot(artifacts / "pane-override.png")
            command(first, "bad", accepted=False)
            near(inspect(first)[0]["values"]["strength"], 1.2)
            command(first, "reset")
            near(inspect(first)[0]["values"]["strength"], 0.1)
            # A 3 s linear transition from 0.1 to 1.2. A read right after the
            # write lands inside it however fast the round trip is, and a read
            # 0.3 s later (about 0.11 further) shows that it moves.
            command(first, "transition")
            midway = inspect(first)[0]["values"]["strength"]
            check(0.1001 < midway < 1.1999, midway)
            time.sleep(0.3)
            later = inspect(first)[0]["values"]["strength"]
            check(midway + 0.05 < later < 1.1999, midway, later)
            settles(first, 0, 1.2)
            command(first, "tab")
            near(inspect(second)[0]["values"]["strength"], 1.2)
            command(first, "window")
            ctl("focus", "--pane", second)
            near(inspect(second)[2]["values"]["strength"], 1.2)
            settings["Shaders"]["params"]["bloom"]["strength"] = 0.2
            settings_path.write_text(json.dumps(settings))
            # The reload builds the chains again, with new instance ids.
            until(first, lambda shaders: shaders[0]["instance"] != before[0]["instance"])
            # A write to an instance id from before the reload must fail. A
            # separate script carries the old id, so a reload of the writer
            # cannot lose it.
            stale = config / "scripts/stale.luau"
            stale.write_text('return {metadata={commands={write={summary="Stale",palette="Stale"}}},'
                ' run_command=function(_,ev) return afterglow.request("done",afterglow.shader_parameters(ev.pane,'
                + json.dumps(before[0]["instance"]) + ',{strength=1.2})) end,'
                ' on_result=function(ev) return afterglow.log(ev.ok and "STALE_OK" or "STALE_REJECTED") end}\n')
            wait_for(lambda: any(s["id"] == "stale" for s in json.loads(ctl("scripts"))))
            ctl("action", "script:stale.write", "--pane", first)
            wait_for(lambda: "STALE_REJECTED" in log_text())
            check("STALE_OK" not in log_text(), "A stale instance id was accepted")
            near(inspect(first)[0]["values"]["strength"], 0.2)
            # The tab override must land before the uninstall can remove it.
            command(first, "tab")
            settles(second, 0, 1.2)
            writer.unlink()
            settles(second, 0, 0.2)
            desktop.screenshot(artifacts / "reset.png")
            old_tab = inspect(first, "scene")["tab_id"]
            ctl("new-tab", "--pane", first)
            empty = until(first, lambda scene: scene["tab_id"] != old_tab, "scene")
            ctl("send-text", "--pane", first, "seq 1 200\\n")
            # The output of the hidden tab is there, and the scene of the current tab must not count it.
            wait_for(lambda: "200" in screen_lines(first))
            hidden = inspect(first, "scene")
            check(hidden["tab_id"] == empty["tab_id"] and hidden["history_rows"] == empty["history_rows"], hidden, empty)
            ctl("focus", "--pane", first)
            active = inspect(first, "scene")
            check(active["tab_id"] != hidden["tab_id"] and active["history_rows"] > 100, active, hidden)
            check(not active["audio_available"] and active["overall"] == 0, active)
            # A script dialog shows nothing that the control socket reports, so the
            # keys wait for each dialog to open.
            def menu(choice):
                ctl("action", "script:scene-controls.open", "--pane", first)
                time.sleep(0.3)
                desktop.key("Return")
                time.sleep(0.5)
                desktop.text(choice)
                desktop.key("Return")
                time.sleep(0.3)
            menu("Choose preset")
            desktop.text("Bright")
            desktop.screenshot(artifacts / "preset-picker.png")
            desktop.key("Return")
            settles(first, 0, 0.8)
            menu("Save current")
            desktop.key("Return")
            wait_for(lambda: list((config / "shader-presets/surface").glob("*.json")))
            menu("Reset my")
            settles(first, 0, 0.2)
            desktop.screenshot(artifacts / "preset-reset.png")
            session.stop()
            session.start("Persisted preset")
            first = wait_for(panes)[0]["id"]
            wait_for(lambda: "PARAMS>" in ctl("get-text", "--pane", first, "--screen"))
            desktop.activate()
            menu("Choose preset")
            desktop.text("My scene")
            desktop.key("Return")
            settles(first, 0, 0.8)
            print("PASS: current-tab-only history, unavailable audio, native preset picker/apply/save/reset, restart persistence")
            print("PASS: native discovery, duplicate instances, pane/tab/window updates, invalid atomic rejection, focus independence, reset, reload/stale ID rejection, script uninstall cleanup")
        except Exception:
            print((artifacts / "application.log").read_text()[-5000:], file=sys.stderr)
            raise
        finally:
            session.close()
