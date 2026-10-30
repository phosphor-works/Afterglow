#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 fuddlesworth
# SPDX-License-Identifier: GPL-3.0-or-later
# Opt-in integration check: requires a running Wayland desktop, Bash and D-Bus.
import argparse,json,os,re,subprocess,sys,tempfile,time
from pathlib import Path
parser = argparse.ArgumentParser(description="Live Wayland project/transfer smoke test; opens temporary windows on a private D-Bus session.")
parser.add_argument("binary", type=Path)
parser.add_argument("--private-bus", action="store_true", help=argparse.SUPPRESS)
options = parser.parse_args()
binary = str(options.binary.resolve())
if not options.private_bus:
    sys.exit(subprocess.call(["dbus-run-session", "--", sys.executable, __file__, binary, "--private-bus"]))
with tempfile.TemporaryDirectory(prefix='afterglow-t3-') as tmp:
    root=Path(tmp);(root/'.bashrc').write_text("PS1='T3> '\n")
    conf=root/'afterglow';(conf/'scripts').mkdir(parents=True)
    (conf/'scripts/probe.luau').write_text('return {on_pane_moved=function(ev) return afterglow.log("MOVED|"..ev.pane.id.."|"..ev.pane.tab_id.."|"..ev.pane.window) end}')
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
            second=int(ctl('split','--pane',first,'--right'));wait(lambda:'T3>' in screen(second))
            firstpid,secondpid=identity(first),identity(second)
            ctl('action','swap_left','--pane',second)
            ctl('move-pane','0','new-tab','--pane',second)
            wait(lambda:len({p['tab'] for p in panes()})==2)
            ctl('move-pane',first,'right','--pane',second)
            wait(lambda:len({p['tab'] for p in panes()})==1)
            spare=int(ctl('new-tab','--pane',first))
            ctl('detach-tab','--pane',first)
            wait(lambda:len({p['window'] for p in panes()})==2)
            ctl('move-pane',spare,'right','--pane',second)
            ctl('move-pane',second,'left','--pane',first)
            wait(lambda:len({p['window'] for p in panes()})==1)
            ctl('close','--pane',spare)
            wait(lambda:len(panes())==2)
            assert identity(first)==firstpid and identity(second)==secondpid
            ctl('action','resize_left','--pane',second)
            ctl('set-title','--pane',second,'build shell')
            third=int(ctl('new-tab','--pane',first,'--cwd','/tmp'));wait(lambda:'T3>' in screen(third))
            original={first,second,third}
            oldpid=identity(first)
            saved=json.loads(ctl('save-project','dev','--pane',first))
            path=conf/'projects/dev.json'
            layout=json.loads(path.read_text())
            assert len(layout['tabs'])==2 and layout['tabs'][0]['tree']['ratio']!=.5,layout
            same=json.loads(ctl('open-project','dev'));assert same['reused'] and len(panes())==3
            failed=subprocess.run([binary,'ctl','save-project','dev','--pane',str(first)],env=env,capture_output=True,text=True)
            assert failed.returncode!=0
            fresh=json.loads(ctl('open-project','dev','--new'))
            assert not fresh['reused'] and len(fresh['pane_ids'])==3 and len(fresh['tab_ids'])==2,fresh
            restored=set(fresh['pane_ids'].values());assert not (original & restored)
            newfirst=fresh['pane_ids'][str(first)]
            assert identity(newfirst)!=oldpid
            ctl('save-project','copy','--pane',newfirst)
            copy=json.loads((conf/'projects/copy.json').read_text())
            assert copy['tabs'][0]['tree']['ratio']==layout['tabs'][0]['tree']['ratio']
            assert copy['tabs'][0]['tree']['second']['pane']['title']=='build shell'
            assert copy['tabs'][1]['tree']['pane']['cwd']=='/tmp'
            def active_position(doc):return next(i for i,t in enumerate(doc['tabs']) if t['id']==doc['active_tab'])
            assert active_position(copy)==active_position(layout)
            # Invalid files must create no objects or launch commands.
            before=len(panes())
            (conf/'projects/bad.json').write_text('{bad')
            bad=subprocess.run([binary,'ctl','open-project','bad'],env=env,capture_output=True,text=True)
            assert bad.returncode!=0 and len(panes())==before
            recovery={'version':1,'active_tab':'main','tabs':[{'id':'main','title':'Recovery','active_pane':'shell','tree':{'direction':'right','ratio':.6,'first':{'pane':{'id':'shell','cwd':'/this-directory-does-not-exist-afterglow','title':'Fallback','command':['/missing-command-afterglow']}},'second':{'pane':{'id':'command','cwd':'/tmp','title':'Status','command':['/bin/sh','-c','printf PROJECT_COMMAND_OK; exit 7']}}}}]}
            (conf/'projects/recovery.json').write_text(json.dumps(recovery))
            result=json.loads(ctl('open-project','recovery'))
            assert len(result['warnings'])==2,result
            wait(lambda:'T3>' in screen(result['pane_ids']['shell']))
            wait(lambda:'PROJECT_COMMAND_OK' in screen(result['pane_ids']['command']))
            wait(lambda:'exited with status 7' in screen(result['pane_ids']['command']))
            assert len(panes())==before+2
            # A saved project remains usable across complete application restart.
            app.terminate();app.wait(timeout=5)
            app=subprocess.Popen([binary,'--title','Afterglow T3 restored probe'],env=env,stdout=log,stderr=subprocess.STDOUT)
            wait(panes)
            restarted=json.loads(ctl('open-project','dev'))
            assert not restarted['reused'] and len(restarted['pane_ids'])==3
            wait(lambda:'T3>' in screen(restarted['pane_ids'][str(first)]))
            assert identity(restarted['pane_ids'][str(first)])!=oldpid
            print('PASS: live transfer/swap/detach preserves PTYs; save, duplicate protection, running-project reuse, fresh identities/processes, nested ratios/titles/directories/focus, invalid-file isolation, missing directory/command recovery, held failure output, restart restoration')
        except Exception:
            print(lp.read_text()[-5000:], file=sys.stderr)
            raise
        finally:
            app.terminate()
            try:app.wait(timeout=5)
            except subprocess.TimeoutExpired:app.kill();app.wait()
