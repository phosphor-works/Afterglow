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
from nested_desktop import NestedDesktop, check, wait_for

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("binary", type=Path)
parser.add_argument("--artifacts", type=Path,
                    help="Directory for the log and screenshots (default: a new temporary directory)")
parser.add_argument("--private-bus", action="store_true", help=argparse.SUPPRESS)
args = parser.parse_args()
binary = str(args.binary.resolve())
if not args.private_bus:
    # The top-level process: the run under the private bus has its own timeouts.
    artifacts = args.artifacts or Path(tempfile.mkdtemp(prefix="afterglow-shader-parameters-"))
    artifacts.mkdir(parents=True, exist_ok=True)
    print(f"Artifacts: {artifacts}", flush=True)
    sys.exit(subprocess.call(["dbus-run-session", "--", sys.executable, __file__, binary,
                              "--artifacts", str(artifacts.resolve()), "--private-bus"]))
if not args.artifacts:
    parser.error("--private-bus needs --artifacts")
artifacts = args.artifacts


with tempfile.TemporaryDirectory(prefix="afterglow-shader-parameters-") as temp:
    root = Path(temp)
    config = root / "afterglow"
    inspector = config / "scripts/inspect"
    inspector.mkdir(parents=True)
    (inspector / "package.json").write_text(json.dumps({"id": "inspect", "version": "1.0.0", "api_version": 1,
        "state_version": 1, "entry": "main", "modules": {"main": "init.luau"}}))
    (inspector / "init.luau").write_text('''
local revision = 0
return {
 on_load=function(ev) revision=ev.storage.revision end,
 metadata={actions={{id="read",title="Inspect shader parameters"},{id="scene",title="Inspect scene"}}},
 run_action=function(action,ev) return afterglow.request("read",action=="scene" and afterglow.scene_state(ev.pane) or afterglow.shaders(ev.pane)) end,
 on_result=function(ev)
  if ev.request_id=="read" and ev.ok then
   return afterglow.request("saved",afterglow.state_write(ev.data,revision))
  elseif ev.request_id=="saved" and ev.ok then revision=ev.data.revision end
 end,
}
''')
    source = Path(__file__).resolve().parents[2]
    shutil.copytree(source / "data/examples/packages/scene-controls", config / "scripts/scene-controls")
    # Every pack from the checkout, so an installed copy of a bundled pack
    # (the default overview, pane effects and tab switch too) cannot answer.
    shutil.copytree(source / "data/shaders", config / "shaders")
    writer = config / "scripts/writer.luau"
    writer.write_text('''
return {
 metadata={actions={{id="pane",title="Pane"},{id="tab",title="Tab"},{id="window",title="Window"},
          {id="transition",title="Transition"},{id="bad",title="Invalid"},{id="reset",title="Reset"}}},
 run_action=function(action,ev) return afterglow.request(action,afterglow.shaders(ev.pane)) end,
 on_result=function(ev)
  -- Each line names its action, so the harness waits for the answer to that action.
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
    with NestedDesktop(root) as desktop, (artifacts / "application.log").open("w") as log:
        env = dict(desktop.env, QT_FORCE_STDERR_LOGGING="1", QT_LOGGING_TO_CONSOLE="1", QT_LOGGING_RULES="afterglow.scripts.info=true")
        app = subprocess.Popen([binary, "--title", "Live shader parameters"], env=env, stdout=log, stderr=subprocess.STDOUT)
        def ctl(*args):
            result = subprocess.run([binary, "ctl", *map(str, args)], env=env, text=True, capture_output=True, timeout=5)
            if result.returncode: raise RuntimeError(result.stderr)
            return result.stdout.strip()
        def panes():
            try: return json.loads(ctl("ls", "--json"))
            except RuntimeError: return []
        def log_text():
            log.flush()
            return (artifacts / "application.log").read_text(errors="replace")
        def count(kind, name):
            return log_text().count(f"{kind}:{name}\n")
        def answers(name):
            return sum(count(kind, name) for kind in ("WRITE_OK", "WRITE_REJECTED", "QUERY_FAILED"))
        def action(pane, name, accepted=True):
            """Runs a writer action, waits until the writer logs the answer to its write,
            and checks that the write was accepted or rejected as expected."""
            before = answers(name)
            expected = "WRITE_OK" if accepted else "WRITE_REJECTED"
            matching = count(expected, name)
            ctl("action", "script:writer." + name, "--pane", pane)
            wait_for(lambda: answers(name) > before)
            check(f"QUERY_FAILED:{name}" not in log_text(), name)
            check(count(expected, name) > matching,
                  f"The {name} write was " + ("rejected" if accepted else "accepted"))
        def settles(pane, index, expected):
            """Waits until a shader value reaches the expected value."""
            wait_for(lambda: abs(inspect(pane)[index]["values"]["strength"] - expected) < 0.001)
        state = config / "state/scripts/inspect.json"
        def inspect(pane, kind="read"):
            previous = json.loads(state.read_text())["revision"] if state.exists() else 0
            ctl("action", "script:inspect." + kind, "--pane", pane)
            wait_for(lambda: state.exists() and json.loads(state.read_text())["revision"] > previous)
            return json.loads(state.read_text())["value"]["scene" if kind == "scene" else "shaders"]
        def near(value, expected): check(abs(value - expected) < 0.001, value, expected)
        try:
            first = wait_for(panes)[0]["id"]
            wait_for(lambda: "PARAMS>" in ctl("get-text", "--pane", first, "--screen"))
            second = int(ctl("split", "--pane", first, "--right"))
            for pane in [first, second]:
                ctl("send-text", "--pane", pane, "printf 'PHOSPHOR LIVE PARAMETERS\\n'\\n")
            desktop.activate()
            time.sleep(0.4)
            before = inspect(first)
            check(len(before) == 3 and len({s["instance"] for s in before}) == 3, before)
            desktop.screenshot(artifacts / "baseline.png")
            action(first, "pane")
            near(inspect(first)[0]["values"]["strength"], 1.2)
            near(inspect(first)[1]["values"]["strength"], 0.1)
            near(inspect(second)[0]["values"]["strength"], 0.1)
            desktop.screenshot(artifacts / "pane-override.png")
            action(first, "bad", accepted=False)
            near(inspect(first)[0]["values"]["strength"], 1.2)
            action(first, "reset")
            near(inspect(first)[0]["values"]["strength"], 0.1)
            # A 3 s linear transition from 0.1 to 1.2. A read right after the
            # write lands inside it however fast the round trip is, and a read
            # 0.3 s later (about 0.11 further) shows that it moves.
            action(first, "transition")
            midway = inspect(first)[0]["values"]["strength"]
            check(0.1001 < midway < 1.1999, midway)
            time.sleep(0.3)
            later = inspect(first)[0]["values"]["strength"]
            check(midway + 0.05 < later < 1.1999, midway, later)
            settles(first, 0, 1.2)
            action(first, "tab")
            near(inspect(second)[0]["values"]["strength"], 1.2)
            action(first, "window")
            ctl("focus", "--pane", second)
            near(inspect(second)[2]["values"]["strength"], 1.2)
            settings["Shaders"]["params"]["bloom"]["strength"] = 0.2
            settings_path.write_text(json.dumps(settings))
            time.sleep(0.6)
            after = inspect(first)
            check(before[0]["instance"] != after[0]["instance"], before, after)
            # A write to an instance id from before the reload must fail. A
            # separate script carries the old id, so a reload of the writer
            # cannot lose it.
            stale = config / "scripts/stale.luau"
            stale.write_text('return {metadata={actions={{id="write",title="Stale"}}},'
                ' run_action=function(_,ev) return afterglow.request("done",afterglow.shader_parameters(ev.pane,'
                + json.dumps(before[0]["instance"]) + ',{strength=1.2})) end,'
                ' on_result=function(ev) return afterglow.log(ev.ok and "STALE_OK" or "STALE_REJECTED") end}\n')
            wait_for(lambda: any(s["id"] == "stale" for s in json.loads(ctl("scripts"))))
            ctl("action", "script:stale.write", "--pane", first)
            wait_for(lambda: "STALE_REJECTED" in log_text())
            check("STALE_OK" not in log_text(), "A stale instance id was accepted")
            near(inspect(first)[0]["values"]["strength"], 0.2)
            # The tab override must land before the uninstall can remove it.
            action(first, "tab")
            settles(second, 0, 1.2)
            writer.unlink()
            settles(second, 0, 0.2)
            desktop.screenshot(artifacts / "reset.png")
            ctl("new-tab", "--pane", first)
            time.sleep(0.3)
            empty = inspect(first, "scene")
            ctl("send-text", "--pane", first, "seq 1 200\\n")
            time.sleep(0.4)
            hidden = inspect(first, "scene")
            check(hidden["tab_id"] == empty["tab_id"] and hidden["history_rows"] == empty["history_rows"], hidden, empty)
            ctl("focus", "--pane", first)
            active = inspect(first, "scene")
            check(active["tab_id"] != hidden["tab_id"] and active["history_rows"] > 100, active, hidden)
            check(not active["audio_available"] and active["overall"] == 0, active)
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
            time.sleep(0.6)
            near(inspect(first)[0]["values"]["strength"], 0.8)
            menu("Save current")
            desktop.key("Return")
            time.sleep(0.3)
            check(list((config / "shader-presets/surface").glob("*.json")), "No preset was saved")
            menu("Reset my")
            time.sleep(0.3)
            desktop.screenshot(artifacts / "preset-reset.png")
            near(inspect(first)[0]["values"]["strength"], 0.2)
            app.terminate(); app.wait(timeout=5)
            app = subprocess.Popen([binary, "--title", "Persisted preset"], env=env, stdout=log, stderr=subprocess.STDOUT)
            first = wait_for(panes)[0]["id"]
            time.sleep(0.4)
            desktop.activate()
            menu("Choose preset")
            desktop.text("My scene")
            desktop.key("Return")
            time.sleep(0.6)
            near(inspect(first)[0]["values"]["strength"], 0.8)
            print("PASS: current-tab-only history, unavailable audio, native preset picker/apply/save/reset, restart persistence")
            print("PASS: native discovery, duplicate instances, pane/tab/window updates, invalid atomic rejection, focus independence, reset, reload/stale ID rejection, script uninstall cleanup")
        except Exception:
            print((artifacts / "application.log").read_text()[-5000:], file=sys.stderr)
            raise
        finally:
            app.terminate()
            try: app.wait(timeout=5)
            except subprocess.TimeoutExpired: app.kill(); app.wait()
