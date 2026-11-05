#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 fuddlesworth
# SPDX-License-Identifier: GPL-3.0-or-later
"""Checks the bash, fish and zsh completions in data/completions against a stub afterglow.

The stub answers `afterglow run --complete` and logs its calls. No real afterglow runs, and the test fails when a
completion calls the stub for anything else. Bash runs twice: with the bash-completion package when it is installed,
and without it. A shell that is not installed is skipped. With no shell at all, the test exits 77, which CTest reports
as skipped. The completions of aglow are checked on their own and through afterglow, against the option tables in
tools/aglow, and the keybind actions of ctl action against afterglow/src/config/Keybinds.cpp.
"""
import os
import pty
import select
from pathlib import Path
import re
import shlex
import shutil
import subprocess
import sys
import tempfile
import time

ROOT = Path(__file__).resolve().parents[2]
COMPLETIONS = ROOT / 'data/completions'
CTL = ROOT / 'afterglow/src/control/Ctl.cpp'
KEYBINDS = ROOT / 'afterglow/src/config/Keybinds.cpp'
AGLOW = ROOT / 'tools/aglow'
AGLOW_COMMANDS = {'transfer': 'Transfer.cpp', 'dnd': 'Dnd.cpp', 'icat': 'Icat.cpp', 'clipboard': 'Clipboard.cpp'}
BASH_COMPLETION = [Path(p) for p in ('/usr/share/bash-completion/bash_completion',
                                     '/usr/local/share/bash-completion/bash_completion',
                                     '/etc/bash_completion')]
# What readline splits words at, besides blanks. Quotes are not used in the cases.
WORDBREAKS = '"\'><=;|&(:'

# STUB_IDS and STUB_NAMES hold one name on each line.
STUB = r'''#!/bin/sh
echo "$0 $*" >> "$STUB_LOG"
if [ "$1" = run ] && [ "$2" = --complete ]; then
    case $# in
    2) printf '%s\n' "$STUB_IDS" ;;
    3) if [ "$3" = nav ]; then
           printf 'open\tOpen a directory\nlist\tList the projects\n'
       elif [ -n "$STUB_NAMES" ]; then
           printf '%s\n' "$STUB_NAMES"
       fi ;;
    *) exit 2 ;;
    esac
    exit 0
fi
exit 1
'''

# Zsh completes only in its line editor. The probe types each line with a TAB into an interactive zsh on a pseudo
# terminal, and compadd logs what it adds. TAB runs a widget that completes, clears the line and then shows a
# marker, so the next line is typed only after the completion has ended. Typeahead would change what zsh does.
# \x52 keeps the echo of the typed setup from matching its marker.
ZSH_SETUP = (
    "fpath=({functions} {fpath} $fpath); autoload -U compinit; compinit -u -D; LISTMAX=100000; PROMPT=''\n"
    "zstyle ':completion:*' completer _complete; zstyle ':completion:*' menu no\n"
    'compadd() {{ builtin compadd -O __hits "$@"; print -rl -- $__hits >> "$__log"; builtin compadd "$@" }}\n'
    'integer __n=0; __out={out}\n'
    '__probe() {{ __log=$__out/$(( ++__n )); : > "$__log"; zle expand-or-complete; BUFFER=; zle -M "DONE-$__n-" }}\n'
    "zle -N __probe; bindkey '^I' __probe; print -r -- $'\\x52EADY'\n"
)
ZSH_LINE_SECONDS = 10
OTHER_ID = 'other-id'

failures = []


def check(condition, message):
    # Not assert: python -O would skip the checks. Printed at once, so that a run that times out still shows them.
    if not condition:
        failures.append(message)
        print('FAIL:', message, flush=True)


def string_list(source, name):
    """The QStringLiteral items of `NAME = {...}` in a C++ source."""
    match = re.search(r'\b%s\s*=\s*\{(.*?)\};' % re.escape(name), source, re.S)
    if not match:
        raise SystemExit(f'FAIL: no {name} list in {CTL}')
    return re.findall(r'QStringLiteral\("([^"]+)"\)', match.group(1))


def keybind_actions():
    """The config names of the keybind actions in Keybinds.cpp."""
    match = re.search(r'const ActionInfo Actions\[\] = \{(.*?)\n\};', KEYBINDS.read_text(), re.S)
    if not match:
        raise SystemExit(f'FAIL: no Actions table in {KEYBINDS}')
    return re.findall(r'\{Action::\w+, "([a-z0-9_]+)"', match.group(1))


def aglow_options():
    """COMMAND -> the option names in the option table of tools/aglow/COMMAND.cpp, with -h and --help."""
    options = {}
    for command, name in AGLOW_COMMANDS.items():
        names = re.findall(r'^\s*\{ "(-[^"]+)", Cli::Option::Kind::', (AGLOW / name).read_text(), re.M)
        if not names:
            raise SystemExit(f'FAIL: no options in {AGLOW / name}')
        options[command] = {n for entry in names for n in entry.split()} | {'-h', '--help'}
    return options


def bash_words(line):
    """COMP_WORDS and COMP_CWORD for a line with the cursor at its end, split the way readline splits it."""
    words = re.findall(r'[^\s%s]+|[%s]+' % (re.escape(WORDBREAKS), re.escape(WORDBREAKS)), line)
    if not words or line[-1].isspace():
        words.append('')
    return words, len(words) - 1


class ZshSession:
    def __init__(self, cwd, env):
        master, slave = pty.openpty()
        self.master = master
        self.process = subprocess.Popen(['zsh', '-f', '-i'], stdin=slave, stdout=slave, stderr=slave, cwd=cwd,
                                        env=dict(env, TERM='dumb'), start_new_session=True)
        os.close(slave)
        self.output = ''

    def send(self, text, marker):
        """Types TEXT, then reads until the shell prints MARKER."""
        os.write(self.master, text.encode())
        deadline = time.monotonic() + ZSH_LINE_SECONDS
        while marker not in self.output:
            left = deadline - time.monotonic()
            if left <= 0 or not select.select([self.master], [], [], left)[0]:
                return False
            try:
                self.output += os.read(self.master, 65536).decode(errors='replace')
            except OSError:  # the shell has gone
                return False
        self.output = self.output[self.output.index(marker) + len(marker):]
        return True

    def close(self):
        self.process.kill()
        self.process.wait()
        os.close(self.master)


class Shells:
    def __init__(self, tmp):
        self.tmp = tmp
        self.bin = tmp / 'bin'
        self.other = tmp / 'other'
        self.work = tmp / 'work'
        self.fish_dir = tmp / 'fish'
        self.log = tmp / 'calls.log'
        # aglow is on PATH only for the lines of aglow itself: afterglow runs the aglow next to it, and its
        # completion must not need one on PATH.
        self.aglow_bin = tmp / 'aglow-bin'
        self.zsh_functions = tmp / 'zsh-functions'
        for d in (self.bin, self.other, self.work / 'adir', self.fish_dir, self.aglow_bin, self.zsh_functions):
            d.mkdir(parents=True)
        (self.work / 'afile').write_text('')
        # The afterglow outside PATH gives an id that the one on PATH never gives.
        for d, stub in ((self.bin, STUB), (self.other, STUB.replace('$STUB_IDS', OTHER_ID))):
            (d / 'afterglow').write_text(stub)
            (d / 'afterglow').chmod(0o755)
        # Fish loads the completion of a command only when the command is on PATH.
        for d, name in ((self.aglow_bin, 'aglow'), (self.bin, 'ssh')):
            (d / name).write_text('#!/bin/sh\nexit 1\n')
            (d / name).chmod(0o755)
        shutil.copy(COMPLETIONS / 'afterglow.fish', self.fish_dir / 'afterglow.fish')
        shutil.copy(COMPLETIONS / 'aglow.fish', self.fish_dir / 'aglow.fish')
        # Where bash-completion loads the completion of aglow from, as it does for an installed one.
        bash_dir = tmp / '.local/share/bash-completion/completions'
        bash_dir.mkdir(parents=True)
        shutil.copy(COMPLETIONS / 'aglow.bash', bash_dir / 'aglow')
        # A completion of ssh in each shell that offers one word, so that afterglow ssh shows that it delegates.
        (bash_dir / 'ssh').write_text('_stub_ssh() { COMPREPLY=(ssh-marker); }\ncomplete -F _stub_ssh ssh\n')
        (self.fish_dir / 'ssh.fish').write_text('complete -c ssh -f -a ssh-marker\n')
        (self.zsh_functions / '_ssh').write_text('#compdef ssh\ncompadd ssh-marker\n')
        self.env = dict(os.environ, PATH=f'{self.bin}:{os.environ.get("PATH", "/usr/bin:/bin")}',
                        STUB_LOG=str(self.log), STUB_IDS='nav\nscene', HOME=str(tmp),
                        XDG_CONFIG_HOME=str(tmp / '.config'), XDG_DATA_HOME=str(tmp / '.local/share'),
                        XDG_CACHE_HOME=str(tmp / '.cache'), XDG_STATE_HOME=str(tmp / '.local/state'))
        for name in ('AFTERGLOW_SOCKET', 'AFTERGLOW_PANE_ID', 'BASH_ENV', 'ENV', 'BASH_COMPLETION_USER_DIR',
                     'BASH_COMPLETION_USER_FILE', 'BASH_COMPLETION_COMPAT_DIR'):
            self.env.pop(name, None)
        self.aglow_env = dict(self.env, PATH=f'{self.aglow_bin}:{self.env["PATH"]}')

    def run(self, argv, env=None):
        result = subprocess.run(argv, cwd=self.work, env=env or self.env, capture_output=True, text=True, timeout=30)
        # compopt works only inside a completion that readline started, which a probe is not.
        errors = [e for e in result.stderr.splitlines() if 'compopt: not currently executing completion' not in e]
        check(result.returncode == 0 and not errors, f'{argv[0]} failed ({result.returncode}): {errors[:3]}')
        return result.stdout

    def bash(self, line, helpers, env=None, words=None):
        """WORDS, when given, are COMP_WORDS for a line that bash_words cannot split, such as one with quotes."""
        words = words or bash_words(line)[0]
        cword = len(words) - 1
        script = '''
            # Without bash-completion nothing loads a completion on demand: source it, as a user must.
            if [[ -n $HELPERS ]]; then source "$HELPERS"; else source "$AGLOW"; fi
            source "$COMPLETION"
            COMP_CWORD=$1; shift
            COMP_WORDS=("$@")
            COMP_LINE=$LINE
            COMP_POINT=${#LINE}
            # The function of the command: _afterglow, or _aglow, which bash-completion would load for aglow.
            name=${COMP_WORDS[0]##*/}
            quote="'"
            name=${name//$quote/}
            [[ $name == aglow ]] && ! declare -F _aglow >/dev/null && source "$AGLOW"
            "_$name" "${COMP_WORDS[0]}" "${COMP_WORDS[COMP_CWORD]}" "${COMP_WORDS[COMP_CWORD-1]}"
            printf '%s\\n' "${COMPREPLY[@]}"
        '''
        env = dict(env or self.env, HELPERS=str(helpers or ''), COMPLETION=str(COMPLETIONS / 'afterglow.bash'),
                   AGLOW=str(COMPLETIONS / 'aglow.bash'), LINE=line)
        out = self.run(['bash', '--norc', '--noprofile', '-c', script, 'bash', str(cword), *words], env)
        return {w.rstrip('/') for w in out.splitlines() if w}

    def zsh(self, lines, env=None):
        """The candidates of each line. A line that does not finish in time fails, and the next starts a new zsh."""
        out = self.tmp / 'zsh'
        shutil.rmtree(out, ignore_errors=True)
        out.mkdir()
        env = env or self.env
        results, session, sessions = [], None, 0
        for line in lines:
            if session is None:
                sessions += 1
                logs = out / str(sessions)
                logs.mkdir()
                session = ZshSession(self.work, env)
                count = 0
                setup = ZSH_SETUP.format(functions=shlex.quote(str(self.zsh_functions)),
                                         fpath=shlex.quote(str(COMPLETIONS)), out=shlex.quote(str(logs)))
                if not session.send(setup, 'READY'):
                    check(False, f'zsh did not start: {session.output[-400:]!r}')
                    session.close()
                    # The lines that ran keep their results.
                    return results + [set() for _ in lines[len(results):]]
            count += 1
            if not session.send(f'{line}\t', f'DONE-{count}-'):
                check(False, f'zsh: {line!r} did not finish: {session.output[-200:]!r}')
                session.close()
                session = None
            log = logs / str(count)
            results.append({w.rstrip('/') for w in log.read_text().splitlines() if w} if log.exists() else set())
        if session:
            session.close()
        return results

    def calls(self):
        """The calls of the stubs that are not afterglow run --complete [ID]."""
        if not self.log.exists():
            return []
        return [c for c in self.log.read_text().splitlines() if not re.fullmatch(r'\S+ run --complete( .*)?', c)]

    def fish(self, line, env=None):
        out = self.run(['fish', '--no-config', '-c',
                        'set -p fish_complete_path $argv[1]; complete -C $argv[2]', str(self.fish_dir), line], env)
        return {w.split('\t', 1)[0].rstrip('/') for w in out.splitlines() if w}


def has(*words):
    return lambda got: set(words) <= got


def lacks(*words):
    return lambda got: not set(words) & got


def last_part(separator, expected):
    """The result, each candidate cut to its part after the last SEPARATOR, is EXPECTED.
    A shell gives a value after "=" or "," with or without what comes before it."""
    return lambda got: {g.rsplit(separator, 1)[-1] for g in got} == expected


FILES = {'adir', 'afile'}


def command_cases(program, options):
    """(line, expected) pairs for the commands of aglow, run as PROGRAM (aglow, or afterglow, which runs aglow)."""
    for command, names in options.items():
        yield f'{program} {command} -', names
    yield f'{program} transfer --direction ', {'upload', 'download', 'send', 'receive'}
    yield f'{program} transfer --direction=', last_part('=', {'upload', 'download', 'send', 'receive'})
    yield f'{program} transfer --mode n', {'normal'}
    # Flags together, then an option whose value is the next word.
    yield f'{program} transfer -cd ', {'upload', 'download', 'send', 'receive'}
    # A password is a file only when it starts with ., / or ~.
    yield f'{program} transfer -p ', set()
    yield f'{program} transfer -p ./', last_part('/', FILES)
    yield f'{program} transfer ', FILES
    yield f'{program} dnd --exit-on ', {'drag-finish', 'drop-finish', 'esc-key'}
    yield f'{program} dnd --exit-on drag-finish,', last_part(',', {'drop-finish', 'esc-key'})
    yield f'{program} dnd --confirm-drop-t', {'--confirm-drop-timeout'}
    yield f'{program} dnd --drop-dest ', {'adir'}
    yield f'{program} icat --no-trailing', {'--no-trailing-newline'}
    yield f'{program} icat --engine ', {'auto', 'builtin'}
    # A file named like a command is a file: its options are not those of transfer.
    yield f'{program} icat transfer --dir', set()
    yield f'{program} clipboard -', has('--help')
    # After -h or --help aglow reads no other word: no file, no option.
    yield f'{program} icat -h ', set()
    yield f'{program} transfer --help ', set()
    yield f'{program} dnd -h -', set()
    # A flag takes only a yes or no value after its "=", and the completion offers none, and no file.
    yield f'{program} transfer --confirm-paths=', set()
    yield f'{program} icat --hold=a', set()
    # A flag right before the cursor takes no word: the next word is a file or an option.
    yield f'{program} transfer --confirm-paths ', FILES
    yield f'{program} icat --hold -', has('--align', '--fit')
    # An option that takes a file still gets one after its "=".
    yield f'{program} dnd --drag-thumbnail=a', last_part('=', FILES)
    # After "--" every word is a file, -h too.
    yield f'{program} transfer -- ', FILES
    yield f'{program} transfer -- -h ', FILES


def bash_cases(program):
    """Cases for bash only. Zsh leaves out a given option and stops at "--" through _arguments, not through
    aglow's own code. Fish offers a given option again, as fish completions do."""
    # An option that is given is not offered again, except one that repeats.
    yield f'{program} transfer -c --con', set()
    yield f'{program} clipboard --mime text/plain --mi', {'--mime'}
    # "--" ends the options.
    yield f'{program} transfer -- -', set()


def value_cases(program):
    """Cases for bash and fish. The _arguments of zsh takes a -h in the place of a value as the help."""
    # The value of an option is no option, -h too.
    yield f'{program} dnd --drop-dest -h ', FILES
    yield f'{program} transfer -cp -h ', FILES
    yield f'{program} transfer -p -h ', FILES


def aglow_cases(options):
    yield 'aglow ', {'transfer', 'dnd', 'icat', 'clipboard', 'help', 'version'}
    yield 'aglow -', {'-h', '--help', '-v', '--version'}
    yield 'aglow help ', {'transfer', 'dnd', 'icat', 'clipboard'}
    yield from command_cases('aglow', options)


def cases(ctl_commands, paneless, actions, options):
    """(line, expected) pairs. A set is the exact result, a callable checks the result."""
    files = FILES
    yield 'afterglow ', {'ctl', 'run', 'ssh', 'transfer', 'dnd', 'icat', 'clipboard'}
    # The word at the cursor is the subcommand itself.
    yield 'afterglow ss', {'ssh'}
    # A subcommand only comes first.
    yield 'afterglow --hold ', lambda got: '--title' in got and not {'ctl', '--hold'} & got
    yield 'afterglow -', has('--config-reference', '--help', '--help-all', '--execute', '--override')
    # Options that are the whole command line come only first.
    yield 'afterglow --hold -', lacks('--config-reference', '--help', '--help-all', '--version')
    # --quick-terminal takes no -e, -d, --title or --hold.
    yield 'afterglow --quick-terminal -', lambda got: '--config' in got and not {'-e', '-d', '--title', '--hold'} & got
    yield 'afterglow --title x -', lambda got: '--hold' in got and '--quick-terminal' not in got
    yield 'afterglow -o A.b=1 -', has('-o', '--override')
    yield 'afterglow --config-reference ', set()
    yield 'afterglow -d ', {'adir'}
    yield 'afterglow --config ', files
    yield 'afterglow run ', {'nav', 'scene'}
    yield 'afterglow run -', {'-h', '--help', '-n', '--no-stdin', '--pane'}
    # --help only alone, and an option once.
    yield 'afterglow run -n -', {'--pane'}
    yield 'afterglow run -n ', {'nav', 'scene'}
    yield 'afterglow run --pane ', set()
    yield 'afterglow run --pane 3 ', {'nav', 'scene'}
    # "--" ends the options and is not the script.
    yield 'afterglow run -- ', {'nav', 'scene'}
    yield 'afterglow run -- nav ', {'--help', 'open', 'list'}
    yield 'afterglow run nav ', {'--help', 'open', 'list'}
    yield 'afterglow run nav open ', files
    yield 'afterglow run nav open --pane ', files
    yield 'afterglow run nav --help ', set()
    yield 'afterglow ctl ', set(ctl_commands)
    yield 'afterglow ctl -', {'-h', '--help'}
    yield 'afterglow ctl ls ', {'--json'}
    yield 'afterglow ctl ls --json ', set()
    for command in ctl_commands:
        if command in paneless:
            yield f'afterglow ctl {command} -', lacks('--pane')
        else:
            yield f'afterglow ctl {command} -', has('--pane')
    yield 'afterglow ctl focus ', {'--pane'}
    yield 'afterglow ctl focus --pane ', set()
    yield 'afterglow ctl get-text --screen ', {'--pane'}
    yield 'afterglow ctl split --left -', {'--pane'}
    yield 'afterglow ctl send-text -', {'--pane', '--raw', '--stdin'}
    yield 'afterglow ctl send-text --stdin -', {'--pane'}
    yield 'afterglow ctl send-text --raw -', {'--pane'}
    yield 'afterglow ctl send-text hello -', {'--pane', '--raw'}
    yield 'afterglow ctl send-text -- -', set()
    yield 'afterglow ctl move-pane 2 ', {'left', 'right', 'up', 'down', 'new-tab', 'new-window'}
    yield 'afterglow ctl new-window --cwd ', {'adir'}
    yield 'afterglow ctl new-tab --cwd adir ', {'--pane'}
    yield 'afterglow ctl open-directory ', {'adir'}
    yield 'afterglow ctl open-directory adir ', {'--layout'}
    yield 'afterglow ctl action ', set(actions) | {'script:nav.open', 'script:nav.list'}
    yield 'afterglow ctl action split_', {'split_right', 'split_down', 'split_left', 'split_up'}
    yield 'afterglow ctl signal ', {str(n) for n in range(8)}
    yield 'afterglow ctl watch ', set()
    # The commands of aglow complete as they do for aglow.
    yield 'afterglow tr', {'transfer'}
    yield from command_cases('afterglow', options)


# The command lines of -e, ssh and ctl ... -- come from the shell's own completion, with the word expected there.
FULL_CASES = (('afterglow -e ech', 'echo'), ('afterglow --title x -e ech', 'echo'),
              ('afterglow --title=x -e ech', 'echo'), ('afterglow -d adir -e ech', 'echo'),
              ('afterglow -o A.b=1 -e ech', 'echo'), ('afterglow ctl split -- ech', 'echo'),
              ('afterglow ctl new-window --cwd adir -- ech', 'echo'), ('afterglow -e ssh --help ', 'ssh-marker'),
              ('afterglow ssh ', 'ssh-marker'))


def check_pairs(name, complete_all, pairs, full=()):
    """complete_all takes a list of lines and gives their candidates."""
    pairs = list(pairs)
    results = complete_all([line for line, _ in pairs] + [line for line, _ in full])
    for (line, expected), got in zip(pairs, results):
        if callable(expected):
            check(expected(got), f'{name}: {line!r} gave {sorted(got)}')
        else:
            check(got == expected, f'{name}: {line!r} gave {sorted(got)}, expected {sorted(expected)}')
    for (line, word), got in zip(full, results[len(pairs):]):
        check(word in got, f'{name}: {line!r} gave {sorted(got)}, expected {word}')


def check_shell(name, complete_all, complete_aglow, sources, full=True, extra=()):
    """The afterglow lines through complete_all, the aglow lines through complete_aglow."""
    check_pairs(name, complete_all, [*cases(*sources), *extra], FULL_CASES if full else ())
    check_pairs(name, complete_aglow, aglow_cases(sources[3]))


def each(complete):
    return lambda lines: [complete(line) for line in lines]


def main():
    source = CTL.read_text()
    sources = (string_list(source, 'Commands'), set(string_list(source, 'Paneless')), keybind_actions(),
               aglow_options())
    with tempfile.TemporaryDirectory(prefix='afterglow-completion-') as tmp:
        shells = Shells(Path(tmp))
        tested = []
        helpers = next((p for p in BASH_COMPLETION if p.exists()), None)
        values = [*value_cases('afterglow'), *value_cases('aglow')]
        bash_extra = [*bash_cases('afterglow'), *bash_cases('aglow'), *values]
        if shutil.which('bash'):
            for completion in ('afterglow.bash', 'aglow.bash'):
                shells.run(['bash', '-n', str(COMPLETIONS / completion)])
            if helpers:
                complete = each(lambda line: shells.bash(line, helpers))
                check_shell('bash', complete, complete, sources, extra=bash_extra)
                # The word at the cursor keeps its colon.
                got = shells.bash('afterglow ctl action script:nav.o', helpers)
                check(got == {'nav.open'}, f'bash: action script:nav.o gave {sorted(got)}')
                tested.append('bash with bash-completion')
            else:
                print('SKIP: bash-completion is not installed')
            complete = each(lambda line: shells.bash(line, None))
            check_shell('bash without bash-completion', complete, complete, sources, full=False, extra=bash_extra)
            tested.append('bash')
        else:
            print('SKIP: bash is not installed')
        if shutil.which('fish'):
            for completion in ('afterglow.fish', 'aglow.fish'):
                shells.run(['fish', '--no-config', '--no-execute', str(COMPLETIONS / completion)])
            check_shell('fish', each(shells.fish), each(lambda line: shells.fish(line, shells.aglow_env)), sources,
                        extra=values)
            check(shells.fish('afterglow ctl action script:nav.o') == {'script:nav.open'}, 'fish: action script:nav.o')
            tested.append('fish')
        else:
            print('SKIP: fish is not installed')
        if shutil.which('zsh'):
            for completion in ('_afterglow', '_aglow'):
                shells.run(['zsh', '-n', str(COMPLETIONS / completion)])
            check_shell('zsh', shells.zsh, shells.zsh, sources)
            got = shells.zsh(['afterglow ctl action script:nav.o'])[0]
            check(got == {'script:nav.open'}, f'zsh: action script:nav.o gave {sorted(got)}')
            # Zsh finds the completion by the command's last part without quotes, and words[1] keeps them.
            for line in (f'{shells.other}/afterglow run ', f"{shells.other}/'afterglow' run "):
                got = shells.zsh([line])[0]
                check(got == {OTHER_ID}, f'zsh: {line!r} gave {sorted(got)}')
            got = shells.zsh(["'afterglow' run "])[0]
            check(got == {'nav', 'scene'}, f"zsh: 'afterglow' run gave {sorted(got)}")
            tested.append('zsh')
        else:
            print('SKIP: zsh is not installed')
        if not tested:
            print('SKIP: no shell to test')
            return 77

        # The afterglow that the line names, not the one on PATH.
        line = f'{shells.other}/afterglow run '
        if shutil.which('bash'):
            got = shells.bash(line, None)
            check(got == {OTHER_ID}, f'bash: {line!r} gave {sorted(got)}')
            if helpers:
                # Readline keeps the quotes in the word, and bash-completion removes them.
                quoted = [f"'{shells.other}/afterglow'", 'run', '']
                got = shells.bash(' '.join(quoted), helpers, words=quoted)
                check(got == {OTHER_ID}, f'bash: {quoted} gave {sorted(got)}')
        if shutil.which('fish'):
            for line in (line, f"'{shells.other}/afterglow' run "):
                got = shells.fish(line)
                check(got == {OTHER_ID}, f'fish: {line!r} gave {sorted(got)}')

        # Names from the socket are text, never shell code.
        # Each form is a command substitution in some shell: $() and `` in all three, () in fish.
        names = '$(touch expanded)\n`touch expanded`\n(touch expanded)'
        evil = dict(shells.env, STUB_IDS=names, STUB_NAMES=names)
        lines = ['afterglow run ', 'afterglow run x ', 'afterglow ctl action ']
        if shutil.which('bash'):
            for line in lines:
                shells.bash(line, None, evil)
                if helpers:
                    shells.bash(line, helpers, evil)
        if shutil.which('fish'):
            for line in lines:
                shells.fish(line, evil)
        if shutil.which('zsh'):
            shells.zsh(lines, evil)
        check(not (shells.work / 'expanded').exists(), 'a name from afterglow run --complete was expanded')
        calls = shells.calls()
        check(not calls, f'a completion called afterglow for something else than run --complete: {calls[:3]}')

    print(f'{"FAIL" if failures else "PASS"}: {", ".join(tested)}')
    return 1 if failures else 0


if __name__ == '__main__':
    sys.exit(main())
