#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 fuddlesworth
# SPDX-License-Identifier: GPL-3.0-or-later
# Opt-in integration check: requires a running Wayland desktop, Bash and D-Bus.
import argparse,json,os,re,shutil,subprocess,sys,tempfile,time
from pathlib import Path
parser = argparse.ArgumentParser(description="Live Wayland package/navigator smoke test; opens temporary windows on a private D-Bus session.")
parser.add_argument("binary", type=Path)
parser.add_argument("--private-bus", action="store_true", help=argparse.SUPPRESS)
options = parser.parse_args()
binary = str(options.binary.resolve())
if not options.private_bus:
    sys.exit(subprocess.call(["dbus-run-session", "--", sys.executable, __file__, binary, "--private-bus"]))
with tempfile.TemporaryDirectory(prefix='afterglow-l3-') as tmp:
    root=Path(tmp);(root/'.bashrc').write_text("PS1='T3> '\n")
    conf=root/'afterglow';(conf/'scripts').mkdir(parents=True)
    package=conf/'scripts/project-navigator'
    source=Path(__file__).resolve().parents[2]/'data/examples/packages/project-navigator'
    shutil.copytree(source,package)
    project=root/'repository';project.mkdir()
    (package/'config.json').write_text(json.dumps({'roots':[str(project)],'fd':False,'zoxide':False}))
    state=conf/'state/scripts';state.mkdir(parents=True)
    # Exercise the shipped schema-1 -> schema-2 migration before actions run.
    (state/'project-navigator.json').write_text(json.dumps({'format':1,'version':1,'revision':1,'value':{'favorites':[str(project)],'recent':[str(project)]}}))
    (conf/'config.json').write_text(json.dumps({'Terminal':{'shell':'/bin/bash','env':{'HOME':tmp},'confirmClose':False},'Control':{'allowReadText':True},'QuickTerminal':{'shortcut':''},'Audio':{'enabled':False},'Animation':{'enabled':False}}))
    env=dict(os.environ,XDG_CONFIG_HOME=tmp,QT_FORCE_STDERR_LOGGING='1',QT_LOGGING_TO_CONSOLE='1',QT_LOGGING_RULES='afterglow.scripts.info=true')
    lp=root/'application.log'
    with lp.open('w') as log:
        app=subprocess.Popen([binary,'--title','Afterglow T3 transfer probe'],env=env,stdout=log,stderr=subprocess.STDOUT)
        def ctl(*args):return subprocess.check_output([binary,'ctl',*map(str,args)],env=env,stderr=subprocess.PIPE,text=True,timeout=5).strip()
        def panes():return json.loads(ctl('ls','--json'))
        def wait(fn):
            end=time.monotonic()+10
            while time.monotonic()<end:
                try:
                    out=fn()
                    if out:return out
                except (subprocess.CalledProcessError,json.JSONDecodeError):pass
                if app.poll()!=None:raise AssertionError(lp.read_text()[-3000:])
                time.sleep(.05)
            raise AssertionError('Timeout: '+lp.read_text()[-2000:])
        def screen(p):return ctl('get-text','--pane',p,'--screen')
        def identity(p):
            ctl('send-text','--pane',p,"printf 'PID_%s_END\\n' $$\\n")
            return wait(lambda:re.findall(r'PID_(\d+)_END',screen(p)))[-1]
        try:
            first=wait(panes)[0]['id'];wait(lambda:'T3>' in screen(first))
            def diagnostics(): return json.loads(ctl('scripts'))
            def saved(): return json.loads((state/'project-navigator.json').read_text())
            assert diagnostics()[0]['status']=='active',diagnostics()
            assert saved()['version']==2 and saved()['revision']==2,saved()
            # Actual Luau action -> native launch -> result -> durable history.
            ctl('action','script:project-navigator.previous','--pane',first)
            wait(lambda:len(panes())==2)
            wait(lambda:saved()['revision']==3)
            launched=next(p for p in panes() if p['id']!=first)
            newpane=launched['id'];wait(lambda:'T3>' in screen(newpane))
            pid=identity(newpane)
            reused=json.loads(ctl('open-directory',project))
            assert reused['reused'] and reused['pane_id']==newpane and len(panes())==2
            assert identity(newpane)==pid
            ctl('action','script:project-navigator.favorite','--pane',newpane)
            wait(lambda:saved()['revision']==4)
            assert str(project) not in saved()['value']['favorites']
            second=int(ctl('split','--pane',newpane,'--right'))
            wait(lambda:'T3>' in screen(second))
            ctl('save-project','two-pane','--pane',newpane)
            other=root/'other';other.mkdir()
            template=json.loads(ctl('open-directory',other,'--layout','two-pane'))
            assert len(template['pane_ids'])==2,template
            for pane in template['pane_ids'].values():
                wait(lambda:next(p['cwd'] for p in panes() if p['id']==pane)==str(other))
            before=len(panes())
            result=subprocess.run([binary,'ctl','open-directory',str(root/'missing')],env=env,capture_output=True)
            assert result.returncode and len(panes())==before
            # Atomic module edits keep the last good package and then recover.
            module=package/'history.luau';original=module.read_text()
            module.write_text('invalid syntax !')
            wait(lambda:diagnostics()[0]['status']=='last-good')
            assert diagnostics()[0]['error']
            module.write_text(original)
            wait(lambda:diagnostics()[0]['status']=='active')
            app.terminate();app.wait(timeout=5)
            app=subprocess.Popen([binary,'--title','Afterglow L3 restored probe'],env=env,stdout=log,stderr=subprocess.STDOUT)
            wait(panes)
            assert diagnostics()[0]['status']=='active',diagnostics()
            assert saved()['revision']==4 and saved()['version']==2
            assert saved()['value']['recent'][0]['value']==str(project)
            print('PASS: installed package, schema migration, Luau previous-project action, launch/reuse preserves PID, persistent favorites, template directories, invalid directory isolation, module hot reload/diagnostics, restart history')
        except Exception:
            print(lp.read_text()[-5000:], file=sys.stderr)
            raise
        finally:
            app.terminate()
            try:app.wait(timeout=5)
            except subprocess.TimeoutExpired:app.kill();app.wait()
