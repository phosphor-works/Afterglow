#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Reproducible Phosphor scene renders and NVIDIA per-process memory sampling.

Build afterglow_renderpng with AFTERGLOW_BUILD_TOOLS=ON. Timings include CPU
submission and synchronous GPU readback, not presentation or input latency.
"""
import argparse
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import time
import xml.etree.ElementTree as ET

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('binary', type=Path)
parser.add_argument('output', type=Path)
args = parser.parse_args()
repo = Path(__file__).resolve().parents[2]
out = args.output.resolve(); out.mkdir(parents=True, exist_ok=True)
config = out / 'config/afterglow/shaders'
shutil.copytree(repo / 'data/examples/phosphor/shaders', config, dirs_exist_ok=True)
shutil.copytree(repo / 'data/shaders/feedback-glow', config / 'feedback-glow', dirs_exist_ok=True)
source = out / 'input.ansi'
source.write_text('\x1b[38;2;100;228;242mPHOSPHOR / REACTIVE SCENE\x1b[0m\r\n\r\n' +
    ''.join(f'  [{i:02d}]  build / render / verify    \x1b[32mPASS\x1b[0m\r\n' for i in range(24)) +
    '\r\nuser@afterglow ~/Projects $ ')
env = dict(os.environ, XDG_CONFIG_HOME=str(out / 'config'))
results = []
full = ['--background-shader','phosphor-field','--background-shader','phosphor-lens',
        '--text-shader','phosphor-emission','--cursor-shader','phosphor-cursor','--pane-shader','phosphor-frame']
for pixels in ['1920x1080', '3840x2160']:
    for name, extra, frames in [
        ('plain',[],32),
        ('field-baseline-repeat',['--background-shader','phosphor-field','--param','phosphor-field:quality=1'],12),
        ('quiet',full+['--param','phosphor-field:quality=0','--param','phosphor-field:intensity=.75','--param','phosphor-field:audioStrength=.5','--param','phosphor-field:historyStrength=.5'],32),
        ('balanced',full+['--param','phosphor-field:quality=1'],32),
        ('cinematic',full+['--param','phosphor-field:quality=2','--param','phosphor-field:intensity=1.15','--param','phosphor-field:audioStrength=1.2'],32),
        ('balanced-feedback',full+['--param','phosphor-field:quality=1','--text-shader','feedback-glow'],32),
    ]:
        stem = pixels+'-'+name
        cmd = [str(args.binary.resolve()),'--pixels',pixels,'--cols','120','--rows','40',
               '--frames',str(frames),'--last-only','--timings','--time','10',
               '--audio-bands','.5,.3,.2','--scrollback-lines','5000'] + extra + [str(source),str(out/(stem+'.png'))]
        memory = []
        with (out/(stem+'.log')).open('w') as log:
            process = subprocess.Popen(cmd, env=env, stdout=log, stderr=subprocess.STDOUT)
            while process.poll() is None:
                try:
                    xml = subprocess.check_output(['nvidia-smi','-q','-x'],timeout=5)
                    for item in ET.fromstring(xml).findall('.//process_info'):
                        if item.findtext('pid') == str(process.pid):
                            value = item.findtext('used_memory','')
                            if value.endswith(' MiB'): memory.append(int(value.split()[0]))
                except (OSError, subprocess.SubprocessError, ET.ParseError): pass
                time.sleep(.1)
            if process.returncode: raise RuntimeError((out/(stem+'.log')).read_text())
        text = (out/(stem+'.log')).read_text()
        p50,p95 = re.search(r'p50 ([\d.]+) ms; p95 ([\d.]+) ms',text).groups()
        mean = re.search(r'render \+ readback ([\d.]+) ms/frame',text).group(1)
        result = dict(case=stem,frames_after_warmup=frames-1,p50_ms=float(p50),p95_ms=float(p95),mean_ms=float(mean),
                      gpu_peak_mib=max(memory) if memory else None,gpu_samples=len(memory),command=cmd)
        results.append(result); (out/'results.json').write_text(json.dumps(results,indent=2)+'\n')
        print(json.dumps({k:v for k,v in result.items() if k!='command'}),flush=True)
