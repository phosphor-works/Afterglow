#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 fuddlesworth
# SPDX-License-Identifier: GPL-3.0-or-later
"""Native Luau -> control -> shader uniform acceptance, isolated from user terminals."""
import argparse
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import time
from nested_desktop import NestedDesktop, wait_for

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("binary", type=Path)
parser.add_argument("--private-bus", action="store_true", help=argparse.SUPPRESS)
args = parser.parse_args()
binary = str(args.binary.resolve())
if not args.private_bus:
    sys.exit(subprocess.call(["dbus-run-session", "--", sys.executable, __file__, binary, "--private-bus"]))
artifacts = Path("/tmp/afterglow-shader-parameters")
artifacts.mkdir(exist_ok=True)
with tempfile.TemporaryDirectory(prefix="afterglow-v11-") as temp:
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
 metadata={actions={{id="read",title="Inspect shader parameters"}}},
 run_action=function(_,ev) return afterglow.request("read",afterglow.shaders(ev.pane)) end,
 on_result=function(ev)
  if ev.request_id=="read" and ev.ok then
   return afterglow.request("saved",afterglow.state_write(ev.data,revision))
  elseif ev.request_id=="saved" and ev.ok then revision=ev.data.revision end
 end,
}
''')
    writer = config / "scripts/writer.luau"
    writer.write_text('''
local remembered
return {
 metadata={actions={{id="pane",title="Pane"},{id="tab",title="Tab"},{id="window",title="Window"},
          {id="bad",title="Invalid"},{id="stale",title="Stale"},{id="reset",title="Reset"}}},
 run_action=function(action,ev)
  if action=="stale" then return afterglow.request("done",afterglow.shader_parameters(ev.pane,remembered,{strength=1.2})) end
  return afterglow.request(action,afterglow.shaders(ev.pane))
 end,
 on_result=function(ev)
  if ev.request_id=="done" then return afterglow.log(ev.ok and "WRITE_OK" or "WRITE_REJECTED") end
  if not ev.ok then return afterglow.log("QUERY_FAILED") end
  local shader=ev.data.shaders[ev.request_id=="window" and 3 or 1]
  remembered=shader.instance
  local scope=ev.request_id=="tab" and "tab" or ev.request_id=="window" and "window" or "pane"
  local values=ev.request_id=="reset" and {} or {strength=ev.request_id=="bad" and 999 or 1.2}
  return afterglow.request("done",afterglow.shader_parameters(ev.pane,shader.instance,values,scope))
 end,
}
''')
    (root / ".bashrc").write_text("PS1='V1> '\n")
    settings = {"Terminal": {"shell": "/bin/bash", "env": {"HOME": temp}, "confirmClose": False},
        "Control": {"allowReadText": True}, "QuickTerminal": {"shortcut": ""}, "Audio": {"enabled": False},
        "Animation": {"enabled": False}, "Window": {"width": 900, "height": 600},
        "Shaders": {"pane": ["bloom", "bloom"], "window": ["bloom"], "params": {"bloom": {"strength": 0.1}}}}
    settings_path = config / "config.json"
    settings_path.write_text(json.dumps(settings))
    with NestedDesktop(root) as desktop, (artifacts / "application.log").open("w") as log:
        env = dict(desktop.env, QT_FORCE_STDERR_LOGGING="1", QT_LOGGING_TO_CONSOLE="1", QT_LOGGING_RULES="afterglow.scripts.info=true")
        app = subprocess.Popen([binary, "--title", "V1.1 live shader parameters"], env=env, stdout=log, stderr=subprocess.STDOUT)
        def ctl(*args):
            result = subprocess.run([binary, "ctl", *map(str, args)], env=env, text=True, capture_output=True, timeout=5)
            if result.returncode: raise RuntimeError(result.stderr)
            return result.stdout.strip()
        def panes():
            try: return json.loads(ctl("ls", "--json"))
            except RuntimeError: return []
        def action(pane, name):
            ctl("action", "script:writer." + name, "--pane", pane)
            time.sleep(0.2)
        state = config / "state/scripts/inspect.json"
        def inspect(pane):
            previous = json.loads(state.read_text())["revision"] if state.exists() else 0
            ctl("action", "script:inspect.read", "--pane", pane)
            wait_for(lambda: state.exists() and json.loads(state.read_text())["revision"] > previous)
            return json.loads(state.read_text())["value"]["shaders"]
        def near(value, expected): assert abs(value - expected) < 0.001, (value, expected)
        try:
            first = wait_for(panes)[0]["id"]
            wait_for(lambda: "V1>" in ctl("get-text", "--pane", first, "--screen"))
            second = int(ctl("split", "--pane", first, "--right"))
            for pane in [first, second]:
                ctl("send-text", "--pane", pane, "printf 'PHOSPHOR LIVE PARAMETERS\\n'\\n")
            desktop.activate()
            time.sleep(0.4)
            before = inspect(first)
            assert len(before) == 3 and len({s["instance"] for s in before}) == 3
            desktop.screenshot(artifacts / "baseline.png")
            action(first, "pane")
            near(inspect(first)[0]["values"]["strength"], 1.2)
            near(inspect(first)[1]["values"]["strength"], 0.1)
            near(inspect(second)[0]["values"]["strength"], 0.1)
            desktop.screenshot(artifacts / "pane-override.png")
            action(first, "bad")
            near(inspect(first)[0]["values"]["strength"], 1.2)
            action(first, "reset")
            near(inspect(first)[0]["values"]["strength"], 0.1)
            action(first, "tab")
            near(inspect(second)[0]["values"]["strength"], 1.2)
            action(first, "window")
            ctl("focus", "--pane", second)
            near(inspect(second)[2]["values"]["strength"], 1.2)
            settings["Shaders"]["params"]["bloom"]["strength"] = 0.2
            settings_path.write_text(json.dumps(settings))
            time.sleep(0.6)
            after = inspect(first)
            assert before[0]["instance"] != after[0]["instance"]
            action(first, "stale")
            near(inspect(first)[0]["values"]["strength"], 0.2)
            action(first, "tab")
            writer.unlink()
            time.sleep(0.5)
            near(inspect(second)[0]["values"]["strength"], 0.2)
            assert "WRITE_REJECTED" in (artifacts / "application.log").read_text()
            desktop.screenshot(artifacts / "reset.png")
            print("PASS: native discovery, duplicate instances, pane/tab/window updates, invalid atomic rejection, focus independence, reset, reload/stale ID rejection, script uninstall cleanup")
        except Exception:
            print((artifacts / "application.log").read_text()[-5000:], file=sys.stderr)
            raise
        finally:
            app.terminate()
            try: app.wait(timeout=5)
            except subprocess.TimeoutExpired: app.kill(); app.wait()
