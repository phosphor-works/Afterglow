#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 fuddlesworth
# SPDX-License-Identifier: GPL-3.0-or-later
"""Checks the bash, fish and zsh completions in data/completions against a stub afterglow and stub aglows.

A stub aglow answers `aglow run --complete` and logs its calls. The stub afterglow only logs its calls. No real
afterglow or aglow runs. The test fails when a completion calls afterglow, or calls aglow for anything else: afterglow
run completes through the aglow that afterglow runs, the one next to its executable. Bash runs twice: with the
bash-completion package when it is installed, and without it. A shell that is not installed is skipped. With no shell
at all, the test exits 77, which CTest reports as skipped. The completions of aglow are checked on their own and
through afterglow, against the option tables in tools/aglow (the names, and which options take a value), the options
of aglow run against libs/afterglow-proto/src/Run.cpp, and the keybind actions of ctl action against
afterglow/src/config/Keybinds.cpp.
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
RUN = ROOT / 'libs/afterglow-proto/src/Run.cpp'
AGLOW_COMMANDS = {'transfer': 'Transfer.cpp', 'dnd': 'Dnd.cpp', 'icat': 'Icat.cpp', 'clipboard': 'Clipboard.cpp'}
BASH_COMPLETION = [Path(p) for p in ('/usr/share/bash-completion/bash_completion',
                                     '/usr/local/share/bash-completion/bash_completion',
                                     '/etc/bash_completion')]
# What readline splits words at, besides blanks. Quotes are not used in the cases.
WORDBREAKS = '"\'><=;|&(:'

OTHER_ID = 'other-id'
# The stub aglow. STUB_IDS and STUB_NAMES hold one name on each line. run --complete --all prints ID<TAB>COMMAND<TAB>
# SUMMARY lines from the same names.
STUB = r'''#!/bin/sh
echo "$0 $*" >> "$STUB_LOG"
# The COMMAND<TAB>SUMMARY lines of script $1.
names() {
    case $1 in
    nav) printf 'open\tOpen a directory\nlist\tList the projects\n' ;;
    @OTHER@) printf 'other\tThe other command\n' ;;
    *) if [ -n "$STUB_NAMES" ]; then printf '%s\n' "$STUB_NAMES"; fi ;;
    esac
}
if [ "$1" = run ] && [ "$2" = --complete ]; then
    case $# in
    2) printf '%s\n' "$STUB_IDS" ;;
    3) if [ "$3" = --all ]; then
           printf '%s\n' "$STUB_IDS" | while IFS= read -r id; do
               names "$id" | while IFS= read -r line; do printf '%s\t%s\n' "$id" "$line"; done
           done
       else
           names "$3"
       fi ;;
    *) exit 2 ;;
    esac
    exit 0
fi
exit 1
'''.replace('@OTHER@', OTHER_ID)
# The stub afterglow: a completion never runs it.
AFTERGLOW_STUB = '#!/bin/sh\necho "$0 $*" >> "$STUB_LOG"\nexit 1\n'

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


def ctl_options(source, command):
    """The options that the COMMAND branch of runCtl in Ctl.cpp takes, besides --pane."""
    match = re.search(r'command == QLatin1String\("%s"\)\) \{\n(.*?)\n    \}' % re.escape(command), source, re.S)
    if not match:
        raise SystemExit(f'FAIL: no {command} branch in {CTL}')
    return set(re.findall(r'take(?:Value|Flag)\(QStringLiteral\("(-[^"]+)"\)\)', match.group(1)))


def keybind_actions():
    """The config names of the keybind actions in Keybinds.cpp."""
    match = re.search(r'const ActionInfo Actions\[\] = \{(.*?)\n\};', KEYBINDS.read_text(), re.S)
    if not match:
        raise SystemExit(f'FAIL: no Actions table in {KEYBINDS}')
    return re.findall(r'\{Action::\w+, "([a-z0-9_]+)"', match.group(1))


def aglow_option_table(command):
    """(names, kind) of each entry in the option table of tools/aglow/COMMAND.cpp."""
    path = AGLOW / AGLOW_COMMANDS[command]
    entries = re.findall(r'^\s*\{ "(-[^"]+)", Cli::Option::Kind::(\w+)', path.read_text(), re.M)
    if not entries:
        raise SystemExit(f'FAIL: no options in {path}')
    return entries


def aglow_options():
    """COMMAND -> the option names in the option table of tools/aglow/COMMAND.cpp, with -h and --help."""
    return {command: {n for names, _ in aglow_option_table(command) for n in names.split()} | {'-h', '--help'}
            for command in AGLOW_COMMANDS}


def aglow_value_options():
    """COMMAND -> the options in its table that take a value: all but the flags."""
    return {command: {n for names, kind in aglow_option_table(command) if kind != 'Flag' for n in names.split()}
            for command in AGLOW_COMMANDS}


def run_options():
    """The options of aglow run, as start() in Run.cpp reads them. "--" ends them, and --complete is for the shells."""
    match = re.search(r'^int start\(Client& client.*?^\}', RUN.read_text(), re.M | re.S)
    if not match:
        raise SystemExit(f'FAIL: no start() in {RUN}')
    return set(re.findall(r'\bargs?(?:\[0\])? == "(-[^"]*)"', match.group(0))) - {'--', '--complete'}


def completion_value_options():
    """SHELL -> COMMAND -> the options that the completions of aglow read a value after: the entries of _aglow_table
    with a value in aglow.bash, the specs that end with + or = in _aglow, and in aglow.fish both the
    __aglow_values_COMMAND lists and the options declared with -x or -r."""
    found = {'bash': {}, 'zsh': {}, 'fish lists': {}, 'fish declarations': {}}
    bash = (COMPLETIONS / 'aglow.bash').read_text()
    zsh = (COMPLETIONS / '_aglow').read_text()
    fish = (COMPLETIONS / 'aglow.fish').read_text()
    for command in AGLOW_COMMANDS:
        block = re.search(r'^    %s\)\n(.*?);;' % command, bash, re.M | re.S)
        entries = re.findall(r'"\*?([^ "]+)( [^"]*)?"', block.group(1)) if block else []
        found['bash'][command] = {n for names, value in entries if value for n in names.split('|')}
        block = re.search(r'^        %s\)\n(.*?)&& ret=0 ;;' % command, zsh, re.M | re.S)
        specs = set()
        for line in (block.group(1) if block else '').splitlines():
            head = re.sub(r"^\s*'\([^)]*\)'", '', line).split('[', 1)[0]
            specs |= {name for name, mark in re.findall(r'(--?[A-Za-z][\w-]*)([+=]?)', head) if mark}
        found['zsh'][command] = specs
        listed = re.search(r'^set -g __aglow_values_%s (.*?)(?<!\\)\n' % command, fish, re.M | re.S)
        found['fish lists'][command] = set(listed.group(1).replace('\\\n', ' ').split()) if listed else set()
        variable = {'transfer': 't', 'dnd': 'd', 'icat': 'i', 'clipboard': 'c'}[command]
        declared = set()
        for line in fish.splitlines():
            if not line.startswith(f'complete -c aglow -n ${variable} '):
                continue
            words = shlex.split(line)
            if '-x' in words or '-r' in words:
                declared |= {('-' if flag == '-s' else '--') + words[k + 1]
                             for k, flag in enumerate(words[:-1]) if flag in ('-s', '-l')}
        found['fish declarations'][command] = declared
    return found


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
        # The afterglow on PATH is a link to app/afterglow, as a link in ~/.local/bin to a build is. afterglow runs
        # app/aglow, the aglow next to its executable, so its completion must resolve the link.
        self.app = tmp / 'app'
        self.other = tmp / 'other'
        self.work = tmp / 'work'
        self.fish_dir = tmp / 'fish'
        self.log = tmp / 'calls.log'
        # An afterglow with no aglow next to it: its completion offers no script and runs nothing.
        self.lonely = tmp / 'lonely'
        # An afterglow and its aglow outside PATH, in a directory with a space in its name.
        self.spaced = tmp / 'with space'
        # An installed prefix outside PATH, with the completion of aglow in its share directory only.
        self.prefix = tmp / 'prefix'
        # aglow is on PATH for the lines of aglow itself, and for the checks that read the calls of a line and that
        # names stay text. The other afterglow lines run without it: afterglow runs the aglow next to it, and its
        # completion must not need one on PATH.
        self.aglow_bin = tmp / 'aglow-bin'
        self.zsh_functions = tmp / 'zsh-functions'
        # The runtime directory of no display: a stub never reaches the socket of the user's Afterglow, and neither
        # would a real aglow run by mistake.
        self.runtime = tmp / 'runtime'
        for d in (self.bin, self.app, self.other, self.work / 'adir', self.fish_dir, self.aglow_bin,
                  self.zsh_functions, self.lonely, self.spaced, self.prefix / 'bin',
                  self.prefix / 'share/bash-completion/completions', tmp / 'no-data', self.runtime):
            d.mkdir(parents=True)
        self.runtime.chmod(0o700)
        (self.work / 'afile').write_text('')
        # The aglow next to the afterglow outside PATH gives an id that the others never give.
        other = STUB.replace('$STUB_IDS', OTHER_ID)
        for path, stub in ((self.app / 'afterglow', AFTERGLOW_STUB), (self.app / 'aglow', STUB),
                           (self.other / 'afterglow', AFTERGLOW_STUB), (self.other / 'aglow', other),
                           (self.lonely / 'afterglow', AFTERGLOW_STUB), (self.spaced / 'afterglow', AFTERGLOW_STUB),
                           (self.spaced / 'aglow', other), (self.prefix / 'bin/afterglow', AFTERGLOW_STUB),
                           (self.prefix / 'bin/aglow', other),
                           (self.aglow_bin / 'aglow', STUB), (self.bin / 'ssh', '#!/bin/sh\nexit 1\n')):
            path.write_text(stub)
            path.chmod(0o755)
        shutil.copy(COMPLETIONS / 'aglow.bash', self.prefix / 'share/bash-completion/completions/aglow')
        (self.bin / 'afterglow').symlink_to(self.app / 'afterglow')
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
                        XDG_CACHE_HOME=str(tmp / '.cache'), XDG_STATE_HOME=str(tmp / '.local/state'),
                        XDG_RUNTIME_DIR=str(self.runtime))
        for name in ('AFTERGLOW_SOCKET', 'AFTERGLOW_PANE_ID', 'WAYLAND_DISPLAY', 'BASH_ENV', 'ENV',
                     'BASH_COMPLETION_USER_DIR', 'BASH_COMPLETION_USER_FILE', 'BASH_COMPLETION_COMPAT_DIR'):
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

    def logged(self):
        return self.log.read_text().splitlines() if self.log.exists() else []

    def calls(self):
        """The calls of the stubs that are not aglow run --complete [ID|--all]: every call of afterglow is one."""
        return [c for c in self.logged() if not re.fullmatch(r'.*/aglow run --complete( .*)?', c)]

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
    # The word after an option that takes a value is that value, -h too, but not a word that starts with "--":
    # aglow takes that word as an option, and the value as left out.
    yield f'{program} dnd --drop-dest -h ', FILES
    yield f'{program} transfer -cp -h ', FILES
    yield f'{program} transfer -p -h ', FILES
    yield f'{program} transfer -d --mode ', {'normal', 'mirror'}
    yield f'{program} dnd --drop-dest --help ', set()
    yield f'{program} transfer -d --', has('--mode', '--confirm-paths')


def bash_cases(program):
    """Cases for bash only. Zsh leaves out a given option and stops at "--" through _arguments, not through
    aglow's own code. Fish offers a given option again, as fish completions do."""
    # An option that is given is not offered again, except one that repeats.
    yield f'{program} transfer -c --con', set()
    yield f'{program} clipboard --mime text/plain --mi', {'--mime'}
    # "--" ends the options.
    yield f'{program} transfer -- -', set()


def run_cases(program, options):
    """(line, expected) pairs for run, as PROGRAM (aglow, or afterglow, which runs aglow). OPTIONS are those of
    Run.cpp."""
    yield f'{program} run ', {'nav', 'scene'}
    yield f'{program} run -', options
    # --help is offered only first, and an option once.
    yield f'{program} run -n -', {'--pane'}
    yield f'{program} run -n ', {'nav', 'scene'}
    yield f'{program} run --pane ', set()
    yield f'{program} run --pane 3 ', {'nav', 'scene'}
    # aglow takes an option again, and the last --pane counts.
    yield f'{program} run --pane 3 --pane 4 ', {'nav', 'scene'}
    yield f'{program} run --pane 3 --pane ', set()
    yield f'{program} run -n --pane 3 -n ', {'nav', 'scene'}
    # "--" ends the options and is not the script.
    yield f'{program} run -- ', {'nav', 'scene'}
    yield f'{program} run -- nav ', {'--help', 'open', 'list'}
    yield f'{program} run nav ', {'--help', 'open', 'list'}
    yield f'{program} run nav open ', FILES
    yield f'{program} run nav open --pane ', FILES
    yield f'{program} run nav --help ', set()


def aglow_cases(options, run):
    yield 'aglow ', {'transfer', 'dnd', 'icat', 'clipboard', 'run', 'help', 'version'}
    yield 'aglow -', {'-h', '--help', '-v', '--version'}
    yield 'aglow help ', {'transfer', 'dnd', 'icat', 'clipboard', 'run'}
    yield from run_cases('aglow', run)
    yield from command_cases('aglow', options)


def cases(ctl_commands, paneless, actions, options, run, overlay):
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
    yield from run_cases('afterglow', run)
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
    # Bash replaces only the part after the colon.
    yield 'afterglow ctl action script:', last_part(':', {'nav.open', 'nav.list'})
    yield 'afterglow ctl signal ', {str(n) for n in range(8)}
    yield 'afterglow ctl watch ', set()
    yield 'afterglow ctl overlay -', has('--pane', *overlay)
    yield 'afterglow ctl overlay --capture ', {'stdout', 'file', 'none'}
    yield 'afterglow ctl overlay --cwd ', {'adir'}
    yield 'afterglow ctl overlay --capture file -', lacks('--capture')
    # -n and --no-stdin are one option. A title is any text: nothing completes it, and it is no positional word.
    yield 'afterglow ctl overlay -n -', lambda got: '--capture' in got and not {'-n', '--no-stdin'} & got
    yield 'afterglow ctl overlay --no-stdin -', lacks('-n', '--no-stdin')
    yield 'afterglow ctl overlay --title ', set()
    yield 'afterglow ctl overlay --title x -', lambda got: {'--pane', '-n', '--capture'} <= got and '--title' not in got
    # The commands of aglow complete as they do for aglow.
    yield 'afterglow tr', {'transfer'}
    yield from command_cases('afterglow', options)


# The command lines of -e, ssh and ctl ... -- come from the shell's own completion, with the word expected there.
FULL_CASES = (('afterglow -e ech', 'echo'), ('afterglow --title x -e ech', 'echo'),
              ('afterglow --title=x -e ech', 'echo'), ('afterglow -d adir -e ech', 'echo'),
              ('afterglow -o A.b=1 -e ech', 'echo'), ('afterglow ctl split -- ech', 'echo'),
              ('afterglow ctl new-window --cwd adir -- ech', 'echo'),
              ('afterglow ctl overlay --capture none -- ech', 'echo'), ('afterglow -e ssh --help ', 'ssh-marker'),
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
    check_pairs(name, complete_aglow, aglow_cases(sources[3], sources[4]))


def each(complete):
    return lambda lines: [complete(line) for line in lines]


def check_value_options():
    """The completions read a value after the options of tools/aglow that take one, and after no other."""
    expected = aglow_value_options()
    for shell, commands in completion_value_options().items():
        for command, got in commands.items():
            check(got == expected[command], f'{shell}: the options of aglow {command} that take a value are '
                                            f'{sorted(got)}, expected {sorted(expected[command])}')


def main():
    source = CTL.read_text()
    sources = (string_list(source, 'Commands'), set(string_list(source, 'Paneless')), keybind_actions(),
               aglow_options(), run_options(), ctl_options(source, 'overlay'))
    check_value_options()
    with tempfile.TemporaryDirectory(prefix='afterglow-completion-') as tmp:
        shells = Shells(Path(tmp))
        tested = []
        helpers = next((p for p in BASH_COMPLETION if p.exists()), None)
        has_bash, has_fish, has_zsh = (bool(shutil.which(shell)) for shell in ('bash', 'fish', 'zsh'))
        bash_extra = [*bash_cases('afterglow'), *bash_cases('aglow')]
        if has_bash:
            for completion in ('afterglow.bash', 'aglow.bash'):
                shells.run(['bash', '-n', str(COMPLETIONS / completion)])
            if helpers:
                check_shell('bash', each(lambda line: shells.bash(line, helpers)),
                            each(lambda line: shells.bash(line, helpers, shells.aglow_env)), sources, extra=bash_extra)
                tested.append('bash with bash-completion')
            else:
                print('SKIP: bash-completion is not installed')
            check_shell('bash without bash-completion', each(lambda line: shells.bash(line, None)),
                        each(lambda line: shells.bash(line, None, shells.aglow_env)), sources, full=False,
                        extra=bash_extra)
            # The word at the cursor keeps its colon, with bash-completion or without it: readline splits the word
            # there, and the completion reads the line.
            for name, with_helpers in (('bash', helpers), ('bash without bash-completion', None)):
                if name == 'bash' and not helpers:
                    continue
                got = shells.bash('afterglow ctl action script:nav.o', with_helpers)
                check(got == {'nav.open'}, f'{name}: action script:nav.o gave {sorted(got)}')
                # Readline keeps the quotes in the word: run --complete gets the ID without them.
                quoted = ['aglow', 'run', "'nav'", '']
                got = shells.bash(' '.join(quoted), with_helpers, shells.aglow_env, words=quoted)
                check(got == {'--help', 'open', 'list'}, f'{name}: {quoted} gave {sorted(got)}')
            tested.append('bash')
        else:
            print('SKIP: bash is not installed')
        if has_fish:
            for completion in ('afterglow.fish', 'aglow.fish'):
                shells.run(['fish', '--no-config', '--no-execute', str(COMPLETIONS / completion)])
            check_shell('fish', each(shells.fish), each(lambda line: shells.fish(line, shells.aglow_env)), sources)
            check(shells.fish('afterglow ctl action script:nav.o') == {'script:nav.open'}, 'fish: action script:nav.o')
            got = shells.fish("aglow run 'nav' ", shells.aglow_env)
            check(got == {'--help', 'open', 'list'}, f"fish: aglow run 'nav' gave {sorted(got)}")
            tested.append('fish')
        else:
            print('SKIP: fish is not installed')
        if has_zsh:
            for completion in ('_afterglow', '_aglow'):
                shells.run(['zsh', '-n', str(COMPLETIONS / completion)])
            check_shell('zsh', shells.zsh, lambda lines: shells.zsh(lines, shells.aglow_env), sources)
            got = shells.zsh(['afterglow ctl action script:nav.o'])[0]
            check(got == {'script:nav.open'}, f'zsh: action script:nav.o gave {sorted(got)}')
            # Zsh finds the completion by the command's last part without quotes, and words[1] keeps them.
            for line in (f'{shells.other}/afterglow run ', f"{shells.other}/'afterglow' run ",
                         f'{shells.other}/aglow run ', f"{shells.other}/'aglow' run "):
                got = shells.zsh([line])[0]
                check(got == {OTHER_ID}, f'zsh: {line!r} gave {sorted(got)}')
            got = shells.zsh(["'afterglow' run "])[0]
            check(got == {'nav', 'scene'}, f"zsh: 'afterglow' run gave {sorted(got)}")
            got = shells.zsh(["aglow run 'nav' "], shells.aglow_env)[0]
            check(got == {'--help', 'open', 'list'}, f"zsh: aglow run 'nav' gave {sorted(got)}")
            # ~ and $NAME at the start of the command word, as the shell runs it.
            for line in ('~/other/afterglow run ', '$HOME/other/afterglow run ', '${HOME}/other/aglow run '):
                got = shells.zsh([line])[0]
                check(got == {OTHER_ID}, f'zsh: {line!r} gave {sorted(got)}')
            tested.append('zsh')
        else:
            print('SKIP: zsh is not installed')
        if not tested:
            print('SKIP: no shell to test')
            return 77

        # The afterglow or aglow that the line names, not the one on PATH: for afterglow, the aglow next to it. The
        # directory with a space in its name is quoted.
        other_action = {'script:other-id.other'}
        for directory in (shells.other, shells.spaced):
            for program in ('afterglow', 'aglow'):
                path = f'{directory}/{program}'
                word = shlex.quote(path)
                lines = [(f'{word} run ', {OTHER_ID})]
                if program == 'afterglow':
                    lines.append((f'{word} ctl action script:o', other_action))
                # Fish loads the completion of aglow only when aglow is on PATH.
                env = shells.aglow_env if program == 'aglow' else None
                for line, expected in lines:
                    # Bash replaces only the part after the colon.
                    bash_expected = last_part(':', {e.rsplit(':', 1)[-1] for e in expected})
                    if has_bash and word == path:
                        got = shells.bash(line, None, env)
                        check(bash_expected(got), f'bash: {line!r} gave {sorted(got)}')
                    if has_bash and helpers:
                        # Readline keeps the quotes in the word, and bash-completion removes them.
                        quoted = [word, *bash_words(line[len(word) + 1:])[0]]
                        got = shells.bash(line, helpers, env, words=quoted)
                        check(bash_expected(got), f'bash: {quoted} gave {sorted(got)}')
                    if has_fish:
                        got = shells.fish(line, env)
                        check(got == expected, f'fish: {line!r} gave {sorted(got)}')
                    if has_zsh:
                        # Zsh finds no completion for a command word in quotes: a backslash keeps the space.
                        line = path.replace(' ', '\\ ') + line[len(word):]
                        got = shells.zsh([line], env)[0]
                        check(got == expected, f'zsh: {line!r} gave {sorted(got)}')

        # An afterglow with no aglow next to it: no script, and no error from the shell.
        actions = set(sources[2])
        for line, expected in ((f'{shells.lonely}/afterglow run ', set()),
                               (f'{shells.lonely}/afterglow run nav ', {'--help'}),
                               (f'{shells.lonely}/afterglow ctl action ', actions)):
            if has_bash:
                for with_helpers in {helpers, None}:
                    got = shells.bash(line, with_helpers)
                    check(got == expected, f'bash: {line!r} gave {sorted(got)}')
            if has_fish:
                got = shells.fish(line)
                check(got == expected, f'fish: {line!r} gave {sorted(got)}')
            if has_zsh:
                got = shells.zsh([line])[0]
                check(got == expected, f'zsh: {line!r} gave {sorted(got)}')

        # bash-completion finds the completion of aglow by the path of the aglow that an afterglow outside PATH
        # runs: in the share directory beside its bin directory, as an install puts it.
        if has_bash and helpers:
            env = dict(shells.env, XDG_DATA_HOME=str(Path(tmp) / 'no-data'), XDG_DATA_DIRS=str(Path(tmp) / 'no-data'))
            for line, expected in ((f'{shells.prefix}/bin/afterglow transfer --mode n', {'normal'}),
                                   (f'{shells.prefix}/bin/afterglow run ', {OTHER_ID})):
                got = shells.bash(line, helpers, env)
                check(got == expected, f'bash: {line!r} gave {sorted(got)}')

        # Names from the socket are text, never shell code.
        # Each form is a command substitution in some shell: $() and `` in all three, () in fish.
        names = '$(touch expanded)\n`touch expanded`\n(touch expanded)'
        evil = dict(shells.aglow_env, STUB_IDS=names, STUB_NAMES=names)
        lines = ['afterglow run ', 'afterglow run x ', 'afterglow ctl action ', 'aglow run ', 'aglow run x ']
        if has_bash:
            for line in lines:
                shells.bash(line, None, evil)
                if helpers:
                    shells.bash(line, helpers, evil)
        if has_fish:
            for line in lines:
                shells.fish(line, evil)
        if has_zsh:
            shells.zsh(lines, evil)
        check(not (shells.work / 'expanded').exists(), 'a name from aglow run --complete was expanded')
        calls = shells.calls()
        check(not calls, f'a completion called afterglow, or aglow for something else than run --complete: {calls[:3]}')

        # One Tab asks Afterglow once, and only for a word that can be a script: each call connects. aglow is on
        # PATH, so that fish loads its completion a second time.
        completers = []
        if has_bash:
            completers += [(f'bash{"" if h else " without bash-completion"}', lambda line, h=h: shells.bash(
                line, h, shells.aglow_env)) for h in {helpers, None}]
        if has_fish:
            completers.append(('fish', lambda line: shells.fish(line, shells.aglow_env)))
        if has_zsh:
            completers.append(('zsh', lambda line: shells.zsh([line], shells.aglow_env)))
        for name, complete in completers:
            for line, expected in (('afterglow run nav ', ['run --complete nav']),
                                   ('afterglow ctl action ', ['run --complete --all']),
                                   ('afterglow ctl action split_', [])):
                shells.log.unlink(missing_ok=True)
                complete(line)
                got = [c.split('/aglow ', 1)[-1] for c in shells.logged()]
                check(got == expected, f'{name}: {line!r} called {got}, expected {expected}')

    print(f'{"FAIL" if failures else "PASS"}: {", ".join(tested)}')
    return 1 if failures else 0

if __name__ == '__main__':
    sys.exit(main())
