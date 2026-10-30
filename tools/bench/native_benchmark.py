#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 fuddlesworth
# SPDX-License-Identifier: GPL-3.0-or-later
"""Measure native terminal workloads in an isolated KWin desktop.

Results describe frame service, asynchronous terminal GPU timestamps, retained
memory, synthetic key-to-submitted-snapshot response and isolated Luau callbacks.
They do not measure physical keyboard-to-display latency.
"""
import argparse
from datetime import datetime, timezone
import hashlib
import json
import os
from pathlib import Path
import platform
import shutil
import signal
import subprocess
import sys
import tempfile
import time

REPO = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(REPO / 'afterglow/tests'))
from nested_desktop import NestedDesktop


def command(args, cwd=None):
    try:
        return subprocess.check_output(args, cwd=cwd, text=True, stderr=subprocess.STDOUT, timeout=10).strip()
    except (OSError, subprocess.SubprocessError) as error:
        return f'unavailable: {error}'


def write_json(path, value):
    temporary = path.with_suffix(path.suffix + '.tmp')
    temporary.write_text(json.dumps(value, indent=2) + '\n')
    temporary.replace(path)


def gpu_sample(pid):
    samples = []
    if not shutil.which('nvidia-smi'):
        return samples
    text = command(['nvidia-smi', 'pmon', '-s', 'um', '-c', '1'])
    for line in text.splitlines():
        parts = line.split()
        if len(parts) >= 12 and parts[1] == str(pid):
            def number(i):
                return float(parts[i]) if parts[i] != '-' else None
            samples.append({'gpu': int(parts[0]), 'sm_percent': number(3),
                            'memory_activity_percent': number(4), 'framebuffer_mib': number(9)})
    return samples


def run_case(binary, output, config_root, case, width, height, repeat, duration, samples):
    mode, history, panes = case
    name = f'{width}x{height}-{mode}-h{history}-p{panes}-r{repeat}'
    result_path = output / (name + '.json')
    config = config_root / name
    config.mkdir()
    env = dict(os.environ, XDG_CONFIG_HOME=str(config), QT_QUICK_CONTROLS_STYLE='Basic',
               QT_QPA_PLATFORMTHEME='generic', QT_LOGGING_RULES='*.debug=false',
               QT_SCALE_FACTOR='1', QT_SCREEN_SCALE_FACTORS='', QT_AUTO_SCREEN_SCALE_FACTOR='0')
    cmd = [str(binary), '--case', mode, '--output', str(result_path), '--config', str(config / 'config.json'),
           '--repo', str(REPO), '--child', str(REPO / 'tools/bench/pty_workload.py'),
           '--width', str(width), '--height', str(height), '--duration', str(duration),
           '--history', str(history), '--panes', str(panes), '--samples', str(samples)]
    gpu = []
    start = time.monotonic()
    with (output / (name + '.log')).open('w') as log:
        process = subprocess.Popen(['dbus-run-session', '--config-file', str(config_root / 'bus.conf'), '--'] + cmd,
                                   env=env, stdout=log, stderr=subprocess.STDOUT, start_new_session=True)
        try:
            while process.poll() is None:
                if time.monotonic() - start > 240:
                    raise TimeoutError(name)
                try:
                    phase = json.loads(Path(str(result_path) + '.phase.json').read_text())
                except (FileNotFoundError, json.JSONDecodeError):
                    phase = {}
                if phase.get('phase') == 'measure':
                    gpu.extend(gpu_sample(phase['pid']))
                time.sleep(.2)
            if process.returncode:
                raise RuntimeError(f'{name} failed ({process.returncode}); see {output / (name + ".log")}')
        finally:
            if process.poll() is None:
                os.killpg(process.pid, signal.SIGTERM)
                try:
                    process.wait(timeout=10)
                except subprocess.TimeoutExpired:
                    os.killpg(process.pid, signal.SIGKILL)
                    process.wait()
    result = json.loads(result_path.read_text())
    result['repeat'] = repeat
    result['gpu_process_samples'] = gpu
    child = Path(str(result_path) + '.child.json')
    if child.exists():
        result['pty_output'] = json.loads(child.read_text())
    result['command'] = cmd
    write_json(result_path, result)
    print(json.dumps({'case': name, 'cpu_percent': result['cpu_percent_one_core'],
                      'frames': result['frame_service']['count'],
                      'frame_p95_ms': result['frame_service'].get('p95_ms'),
                      'rss_mib': result['memory_after'].get('Rss_kib', 0)/1024}), flush=True)
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('binary', type=Path)
    parser.add_argument('output', type=Path)
    parser.add_argument('--repeats', type=int, default=3)
    parser.add_argument('--duration', type=int, default=15)
    parser.add_argument('--samples', type=int, default=100)
    parser.add_argument('--sizes', nargs='+', default=['1920x1080', '3840x2160'])
    parser.add_argument('--cases', nargs='+', choices=['idle','idle-blink','scroll','resize','history','input','input-load','scripts'])
    args = parser.parse_args()
    if not 1 <= args.repeats <= 10 or not 1 <= args.duration <= 120 or not 1 <= args.samples <= 1000:
        parser.error('Repeats, duration or samples are outside supported limits')
    sizes = []
    for value in args.sizes:
        try:
            w, h = map(int, value.split('x'))
            if not 640 <= w <= 7680 or not 480 <= h <= 4320:
                raise ValueError()
            sizes.append((w,h))
        except ValueError:
            parser.error('Sizes must be WIDTHxHEIGHT within 640x480 and 7680x4320')
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=True)
    if any(output.glob('*-r*.json')):
        parser.error('Output contains earlier runs; select an empty directory')
    binary = args.binary.resolve(strict=True)
    metadata = {
        'started_utc': datetime.now(timezone.utc).isoformat(), 'platform': platform.platform(),
        'commit': command(['git','rev-parse','HEAD'],REPO),
        'source_status': command(['git','status','--short','--untracked-files=no'],REPO),
        'tool_sources': {str(path.relative_to(REPO)): hashlib.sha256(path.read_bytes()).hexdigest()
                         for path in sorted((REPO/'tools/bench').glob('*')) if path.is_file()},
        'source_diff_sha256': hashlib.sha256(subprocess.check_output(['git','diff','HEAD'],cwd=REPO)).hexdigest(),
        'binary_sha256': hashlib.sha256(binary.read_bytes()).hexdigest(),
        'cpu': command(['lscpu']), 'gpu': command(['nvidia-smi','--query-gpu=name,driver_version,power.limit,clocks.gr,clocks.mem','--format=csv']),
        'kernel': platform.release(), 'compositor': command(['kwin_wayland','--version']),
        'font': command(['fc-match','Monaspace Neon Var']),
        'build_type': next((line.split('=',1)[1] for line in (binary.parents[1]/'CMakeCache.txt').read_text().splitlines()
                           if line.startswith('CMAKE_BUILD_TYPE:')), 'unknown'),
        'repeats':args.repeats, 'duration_s':args.duration, 'input_samples':args.samples,
        'style':'Basic', 'platform_theme':'generic', 'shaders':[], 'opacity':1, 'scale':1,
        'note':'Nested KWin on the active desktop; ordinary power management and shared GPU. No clock locking.',
    }
    write_json(output/'environment.json',metadata)
    cases = [('idle',0,1),('idle-blink',0,1),('scroll',100000,1),('resize',100000,1),
             ('history',0,1),('history',100000,1),('history',1000000,1),
             ('input',0,1),('input-load',0,1),('scripts',0,1),('scripts',0,8),('scripts',0,32)]
    if args.cases:
        cases = [case for case in cases if case[0] in args.cases]
    results = []
    with tempfile.TemporaryDirectory(prefix='afterglow-benchmark-') as temp:
        for width,height in sizes:
            desktop_root=Path(temp)/f'desktop-{width}-{height}'
            desktop_root.mkdir()
            # No activation directories: unrelated portal services must not join a workload.
            (desktop_root / 'bus.conf').write_text(
                '<busconfig><type>session</type><listen>unix:tmpdir=/tmp</listen><auth>EXTERNAL</auth>'
                '<policy context="default"><allow send_destination="*"/><allow receive_sender="*"/>'
                '<allow own="*"/></policy></busconfig>')
            with NestedDesktop(desktop_root,width=width,height=height) as desktop:
                # Set only this runner's environment; children use its private Wayland socket.
                previous = os.environ.copy()
                os.environ.update(desktop.env)
                try:
                    for repeat in range(1,args.repeats+1):
                        for case in cases:
                            results.append(run_case(binary,output,desktop_root,case,width,height,repeat,args.duration,args.samples))
                            write_json(output/'results.json',results)
                finally:
                    os.environ.clear()
                    os.environ.update(previous)
    metadata['finished_utc'] = datetime.now(timezone.utc).isoformat()
    write_json(output/'environment.json',metadata)


if __name__ == '__main__':
    main()
