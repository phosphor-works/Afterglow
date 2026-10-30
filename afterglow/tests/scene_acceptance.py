#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""V1 native scene acceptance using real CAVA on an isolated, silent Pulse sink.

Requires the nested_desktop dependencies, pactl, paplay and cava. No sound is
routed to speakers. Temporary sink, desktop, app and files are cleaned up.
"""
import argparse
import json
import math
import os
from pathlib import Path
import shutil
import struct
import subprocess
import sys
import tempfile
import time
import wave
from nested_desktop import NestedDesktop, wait_for

parser=argparse.ArgumentParser(description=__doc__)
parser.add_argument('binary',type=Path)
parser.add_argument('--private-bus',action='store_true',help=argparse.SUPPRESS)
args=parser.parse_args(); binary=str(args.binary.resolve())
if not args.private_bus:
    sys.exit(subprocess.call(['dbus-run-session','--',sys.executable,__file__,binary,'--private-bus']))
repo=Path(__file__).resolve().parents[2]
out=Path('/tmp/afterglow-scene-acceptance');out.mkdir(exist_ok=True)
sink='afterglow_v1_'+str(os.getpid())
module=subprocess.check_output(['pactl','load-module','module-null-sink','sink_name='+sink],text=True).strip()
play=None
try:
 with tempfile.TemporaryDirectory(prefix='afterglow-scene-') as tmp:
    root=Path(tmp);config=root/'afterglow';config.mkdir()
    shutil.copytree(repo/'data/examples/phosphor/shaders',config/'shaders')
    shutil.copytree(repo/'data/examples/phosphor/scripts',config/'scripts')
    for package in ['scene-controls','contextual-scene']:
        shutil.copytree(repo/'data/examples/packages'/package,config/'scripts'/package)
    shutil.copytree(repo/'data/shaders/feedback-glow',config/'shaders/feedback-glow')
    inspector=config/'scripts/inspect';inspector.mkdir()
    (inspector/'package.json').write_text(json.dumps(dict(id='inspect',version='1.0.0',api_version=1,state_version=1,entry='main',modules={'main':'init.luau'})))
    (inspector/'init.luau').write_text('''
local revision, result=0,{}
return {
 on_load=function(ev) revision=ev.storage.revision end,
 metadata={actions={{id="read"}}},
 run_action=function(_,ev) return afterglow.request("scene",afterglow.scene_state(ev.pane)) end,
 on_result=function(ev)
  if not ev.ok then return afterglow.log(ev.error.message) end
  if ev.request_id=="scene" then result=ev.data; return afterglow.request("shaders",afterglow.shaders(ev.pane))
  elseif ev.request_id=="shaders" then result.shaders=ev.data.shaders; return afterglow.request("saved",afterglow.state_write(result,revision))
  elseif ev.request_id=="saved" then revision=ev.data.revision end
 end,
}
''')
    (root/'.bashrc').write_text("PS1='SCENE> '\n")
    settings=json.loads((repo/'data/examples/phosphor/config/phosphor.json').read_text())
    settings.update(json.loads((repo/'data/examples/phosphor/config/dark.json').read_text()))
    settings['Terminal'].update(shell='/bin/bash',env={'HOME':tmp},confirmClose=False,scrollback=20000)
    settings['Control']={'allowReadText':True};settings['QuickTerminal']={'shortcut':''}
    settings['Audio'].update(inputMethod='pulse',inputSource=sink+'.monitor')
    settings['Window'].update(width=1000,height=650)
    settings['Shaders']['text'].append('feedback-glow')
    settings['Shaders']['params']={'phosphor-field':{'quality':0}}
    path=config/'config.json';path.write_text(json.dumps(settings))
    tone=root/'tone.wav'
    with wave.open(str(tone),'wb') as wav:
        wav.setparams((1,2,22050,0,'NONE','not compressed'))
        block=b''.join(struct.pack('<h',int(9000*(math.sin(2*math.pi*90*i/22050)+.3*math.sin(2*math.pi*900*i/22050)))) for i in range(22050))
        for _ in range(60): wav.writeframes(block)
    with NestedDesktop(root) as desktop, (out/'application.log').open('w') as log:
        env=dict(desktop.env,QT_FORCE_STDERR_LOGGING='1',QT_LOGGING_TO_CONSOLE='1')
        app=subprocess.Popen([binary,'--title','V1 reactive scene acceptance'],env=env,stdout=log,stderr=subprocess.STDOUT)
        def ctl(*args):
            p=subprocess.run([binary,'ctl',*map(str,args)],env=env,capture_output=True,text=True,timeout=10)
            if p.returncode: raise RuntimeError(p.stderr)
            return p.stdout.strip()
        def panes():
            try:return json.loads(ctl('ls','--json'))
            except RuntimeError:return []
        state=config/'state/scripts/inspect.json'
        def inspect(pane):
            revision=json.loads(state.read_text())['revision'] if state.exists() else 0
            ctl('action','script:inspect.read','--pane',pane)
            wait_for(lambda: state.exists() and json.loads(state.read_text())['revision']>revision)
            return json.loads(state.read_text())['value']
        def until(pane,predicate):
            end=time.monotonic()+15
            while time.monotonic()<end:
                data=inspect(pane)
                if predicate(data):return data
                time.sleep(.5)
            raise AssertionError(data)
        def field(data):return next(s['values'] for s in data['shaders'] if s['shader']=='phosphor-field')
        try:
            first=wait_for(panes)[0]['id'];desktop.activate()
            wait_for(lambda:'SCENE>' in ctl('get-text','--pane',first,'--screen'))
            second=int(ctl('split','--pane',first,'--right'))
            time.sleep(.5)
            silent=until(first,lambda d:d['scene']['audio_available'])
            assert silent['scene']['overall']<.01,silent
            play=subprocess.Popen(['paplay','--device='+sink,str(tone)])
            audio=until(first,lambda d:d['scene']['overall']>.02)
            ctl('send-text','--pane',first,'seq 1 5000\\n')
            ctl('send-text','--pane',second,'seq 1 5000\\n')
            history=until(first,lambda d:d['scene']['history_rows']>9000)
            ctl('send-text','--pane',first,'sleep 3\\n')
            charged=until(first,lambda d:field(d)['commandHeat']>.9)
            assert charged['scene']['history_rows']>9000 and charged['scene']['overall']>.02
            cooled=until(first,lambda d:field(d)['commandHeat']<.05)
            for _ in range(4):
                ctl('focus','--pane',first);ctl('focus','--pane',second)
            ctl('action','script:contextual-scene.toggle','--pane',first)
            contextual=until(first,lambda d:abs(field(d)['intensity']-1.05)>.02)
            ctl('action','script:contextual-scene.toggle','--pane',first)
            reset=until(first,lambda d:abs(field(d)['intensity']-1.05)<.001)
            third=int(ctl('new-tab','--pane',first));time.sleep(.3)
            empty=inspect(third)
            assert empty['scene']['history_rows']<50,empty
            ctl('focus','--pane',first)
            assert inspect(first)['scene']['history_rows']>9000
            for action in ['resize_left','resize_right','toggle_pane_zoom','toggle_pane_zoom','toggle_fullscreen','toggle_fullscreen']:
                ctl('action',action,'--pane',first);time.sleep(.15)
            ctl('send-text','--pane',first,"printf 'READABLE PROMPT / V1 ACCEPTANCE\\n'\\n")
            time.sleep(.6);desktop.screenshot(out/'reactive.png')
            # Exercise live opacity/font changes through the normal config reload.
            settings['Window']['backgroundOpacity']=.75;settings['Font']['size']=15
            path.write_text(json.dumps(settings));time.sleep(.8)
            desktop.screenshot(out/'translucent-resized.png')
            play.terminate();play.wait();play=None
            silence=until(first,lambda d:d['scene']['overall']<.01)
            settings['Audio']['enabled']=False;path.write_text(json.dumps(settings));time.sleep(.8)
            unavailable=until(first,lambda d:not d['scene']['audio_available'])
            assert unavailable['scene']['overall']==0
            assert 'too many effects' not in (out/'application.log').read_text()
            evidence=dict(audio=audio['scene'],history=history['scene'],charged=field(charged),
                          contextual=field(contextual),reset=field(reset),silence=silence['scene'],unavailable=unavailable['scene'])
            (out/'results.json').write_text(json.dumps(evidence,indent=2)+'\n')
            print('PASS: real CAVA audio + command transitions + 10k current-tab history; focus/splits/zoom/fullscreen; contextual toggle/reset; transparency/font resize; silence/disabled capture')
        finally:
            app.terminate()
            try:app.wait(timeout=5)
            except subprocess.TimeoutExpired:app.kill();app.wait()
finally:
    if play:play.terminate();play.wait()
    subprocess.run(['pactl','unload-module',module],check=True)
