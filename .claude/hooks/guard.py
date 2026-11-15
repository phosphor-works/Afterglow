#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 fuddlesworth
# SPDX-License-Identifier: GPL-3.0-or-later
"""PreToolUse guard for the hard rules of this project (CLAUDE.md).

Claude Code passes the tool call as JSON on stdin. Exit code 2 blocks the call and shows stderr to Claude.

The guard catches the plain form of a mistake that a session makes in good faith: `pkill afterglow`, `sudo`,
`git add -A`, `git push origin main`, `afterglow ctl` against the user's terminal, an edit of a generated file. It
is not a security boundary. A shell is too expressive for a list of command shapes to be complete, and a program
that wants to get past the guard can write a script and run it. So the guard reads a command line simply, and a
rule covers the spelling that a session types, not every spelling that the shell accepts. Do not add rules for
unusual spellings (commands hidden in parallel, find -exec, nested quoting, encoded delimiters): that work never
ends and makes the guard slow and fragile. Add a rule only for a mistake that a session made, or would make, in
its plain form.

How a Bash command is read: the bodies of here-documents (commit messages) are text. The rest is split into simple
commands at ; & | newlines, parentheses and backquotes outside quotes, and each one is tokenized with its quotes
removed. The commands in $(...) and backquotes are read too, and so is the script of `bash -c`, `sh -c` and
`zsh -c`. The words in front of a command (VAR=value, env, timeout, nice, nohup, sudo, systemd-run...) are skipped,
and the variables that they set are kept for the rules that look at the environment.
"""

import fnmatch
import json
import os
import pwd
import re
import shlex
import subprocess
import sys

# Local state that never goes into git: another tool's workspace, Python caches, build trees (build/, build-asan/)
# and .cache. A file whose name starts with "build" is a source file (build-inspector.luau).
LOCAL_PATH = re.compile(r'(^|/)(\.Codex|__pycache__|\.cache)(/|$)|(^|/)build[^/]*/|(^|/)build[^/.]*$')

# Generated files: run the generator instead
GENERATED = {
    'docs/CONFIGURATION.md': 'python3 tools/generate-reference.py build/afterglow/afterglow',
    'libs/afterglow-vt/src/UnicodeTables.inc': 'python3 -I tools/gen_unicode_tables.py <ucd-dir> '
    'libs/afterglow-vt/src/UnicodeTables.inc',
}
BUILD_TREE = re.compile(r'^(build[^/]*|\.cache)/')


def main_checkout(repo):
    """The main working tree of the repository at REPO. A linked worktree (git worktree add, .claude/worktrees/...)
    has a .git file that names its git directory, whose commondir file names the main .git directory. Read without
    running git, so that every tool call stays cheap. Anything unexpected keeps REPO."""
    try:
        with open(os.path.join(repo, '.git')) as f:
            line = f.readline().strip()
        if not line.startswith('gitdir:'):
            return repo
        gitdir = os.path.join(repo, line[len('gitdir:') :].strip())
        with open(os.path.join(gitdir, 'commondir')) as f:
            common = os.path.realpath(os.path.join(gitdir, f.readline().strip()))
    except (OSError, UnicodeDecodeError):  # the main checkout's .git is a directory
        return repo
    return os.path.dirname(common) if os.path.basename(common) == '.git' else repo


# The Phosphor libraries are built from the checkout next to the main checkout of this repository, but they belong
# to PlasmaZones, which changes only through its own branch and pull request. This file is .claude/hooks/guard.py,
# so the repository is two directories up.
REPOSITORY = main_checkout(os.path.realpath(os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', '..')))
PLASMAZONES = os.path.realpath(os.path.join(REPOSITORY, '..', 'PlasmaZones')) + '/'

SEPARATORS = ';&|()\n`'
# Words that run the command after them: skipped, with their options, to find the command itself
WRAPPERS = set(
    'then do else elif if while until ! { } time nohup env exec command builtin xargs timeout nice ionice stdbuf '
    'setsid unbuffer systemd-run dbus-run-session sudo doas pkexec run0 valgrind heaptrack strace ltrace catchsegv'
    .split()
)
# Wrapper options that take the next word as their value
WRAPPER_VALUES = {
    'env': {'-C', '--chdir', '-u', '--unset', '-S', '--split-string'},
    'timeout': {'-s', '--signal', '-k', '--kill-after'},
    'nice': {'-n', '--adjustment'},
    'ionice': {'-c', '--class', '-n', '--classdata'},
    'stdbuf': {'-i', '--input', '-o', '--output', '-e', '--error'},
    'xargs': {'-I', '--replace', '-d', '--delimiter', '-a', '--arg-file', '-E', '-n', '--max-args', '-P', '-s'},
    'time': {'-f', '--format', '-o', '--output'},
    'systemd-run': {'-p', '--property', '-E', '--setenv', '-u', '--unit', '--description', '--slice', '-M',
                    '--machine', '--uid', '--gid', '--nice', '--working-directory'},
    'dbus-run-session': {'--config-file', '--dbus-daemon'},
    'strace': {'-o', '-e', '-p', '-s', '-u', '-E', '--output', '--trace', '--string-limit', '--user', '--env'},
    'ltrace': {'-o', '-e', '-p', '-s', '-u', '--output'},
}
# Debuggers that start the program after these words: gdb --args PROGRAM, lldb -- PROGRAM, rr record PROGRAM
DEBUGGER_STARTS = {'gdb': '--args', 'lldb': '--', 'rr': 'record'}
SHELLS = {'bash', 'sh', 'zsh', 'dash'}
KILL_BY_NAME = {'pkill', 'killall', 'killall5', 'skill'}
PRIVILEGED = {'sudo', 'sudoedit', 'doas', 'su', 'pkexec', 'run0'}
INSTALL_TARGET = re.compile(r'^install(/(strip|local|fast))?$')

MSG_LOCAL = (
    '.Codex/, __pycache__/, build*/ and .cache/ are local state and never go into git, and `git add -f` would '
    'bypass .gitignore. Stage only source files, by name.'
)
MSG_BROAD = 'Stage files by name (git add path...), not with -A, -u, ., *, :/ or commit -a.'
MSG_PUSH = 'Never force-push. Fix the branch with a new commit instead.'
MSG_PUSH_MAIN = ('Never push to main. Push a branch and open a pull request. The user merges it. '
                 'If this command makes the branch, name it: git push -u origin BRANCH.')
MSG_PKILL = (
    'pkill and killall match by name, and the user runs Afterglow as their terminal: `pkill afterglow` closes the '
    "user's own windows, and possibly this session. Use `pgrep -a NAME`, then `kill PID` on a build/ process only."
)
MSG_PGREP_KILL = (
    "kill with the PIDs that pgrep or pidof found can match the user's own Afterglow or mux daemon, and pgrep -f "
    'also matches the shell that runs the command, because its command line holds the pattern. List the processes '
    'with `pgrep -a NAME`. Then run `kill PID` with the numbers in a separate command.'
)
MSG_SUDO = ('Do not use sudo, su, doas, pkexec or run0. The user installs system packages. Tell the user what you '
            'need and why.')
MSG_INSTALL = ('Never install the build (cmake --install, the install target, cmake_install.cmake). The user '
               'installs Afterglow. Only an install into a temporary directory (--prefix /tmp/... or '
               'DESTDIR=/tmp/... on the command itself) is allowed.')
MSG_CTL = (
    "`afterglow ctl`, `aglow run` and `afterglow run` drive the user's running Afterglow: this session runs inside "
    "one of its panes, and they use the pane's $AFTERGLOW_SOCKET, else the socket of the user's Wayland display. "
    'Give them the socket of a test session, with the pane ID cleared '
    '(AFTERGLOW_SOCKET=/path/to/test.sock AFTERGLOW_PANE_ID=), or remove the pane variables and point them at '
    'another display (env -u AFTERGLOW_SOCKET -u AFTERGLOW_PANE_ID WAYLAND_DISPLAY=...). Ask the user before you '
    'drive their instance.'
)
MSG_MUX = (
    "`afterglow mux`, `afterglow ctl --session`, `aglow-mux` (`afterglow-mux` in an older build) and "
    "`afterglow --mux-attach` reach the user's mux daemon, which holds the user's terminal sessions. Its sockets "
    'are in $XDG_RUNTIME_DIR/aglow-mux and $XDG_STATE_HOME/aglow-mux (else ~/.local/state/aglow-mux), so '
    'WAYLAND_DISPLAY does not isolate it. Name a private runtime directory and a private state directory under /tmp '
    'on the command (XDG_RUNTIME_DIR=/tmp/... XDG_STATE_HOME=/tmp/...), and kill the sessions that you made with the '
    'same variables. Never stop, kill, restart or mask aglow-mux.service, and never remove the files in the two '
    "socket directories of the user: that ends or strands the user's sessions. `afterglow mux` alone and its "
    '--help connect to nothing.'
)
MSG_LAUNCH = (
    "This starts Afterglow on the user's own Wayland display: it opens a window on the user's desktop, or hands the "
    "window to the user's running Afterglow through the socket of that display. Test in a nested KWin with its own "
    'display (the ag-verify-live skill), or name another display on the command: WAYLAND_DISPLAY=NAME or '
    'XDG_RUNTIME_DIR=/tmp/... before the program. --help, --version and --config-reference open no window.'
)
MSG_FAILED = ('The guard could not check this call ({}), so it blocks it. Write the command in a simpler form, or '
              'ask the user.')


def plasmazones_message(path):
    return (f'{path} is in PlasmaZones. Afterglow builds the Phosphor libraries from that checkout, but PlasmaZones '
            'changes only through its own branch and pull request. Tell the user what Phosphor needs and why.')


# Reading a command line

def strip_heredocs(command):
    """The command with the bodies of its here-documents left out: they are text (commit messages, PR bodies)."""
    out, pos = [], 0
    for m in re.finditer(r'<<-?[ \t]*([\'"]?)([A-Za-z_][A-Za-z0-9_]*)\1', command):
        if m.start() < pos:
            continue
        body = command.find('\n', m.end())
        if body < 0:
            break
        end = re.compile(r'^[ \t]*%s[ \t]*$' % re.escape(m.group(2)), re.M).search(command, body + 1)
        if not end:
            break
        out.append(command[pos : body + 1])
        pos = end.end()
    out.append(command[pos:])
    return ''.join(out)


def tokens(command):
    lex = shlex.shlex(command, posix=True, punctuation_chars=SEPARATORS)
    lex.whitespace = ' \t\r'
    lex.whitespace_split = True
    try:
        return list(lex)
    except ValueError:  # unbalanced quotes: split at whitespace and separators, quotes dropped
        return re.findall(r'[%s]+|[^\s%s]+' % (re.escape(SEPARATORS), re.escape(SEPARATORS)),
                          command.replace('"', '').replace("'", ''))


def split_wrappers(words):
    """(environment changes, wrappers, command word, arguments) of one simple command. An environment change is
    ('set', NAME, VALUE), ('unset', NAME) or ('clear',), from VAR=value words, env and systemd-run -E."""
    env, wrappers, i = [], [], 0
    while i < len(words):
        w = words[i]
        name = os.path.basename(w)
        if m := re.match(r'^([A-Za-z_][A-Za-z0-9_]*)=(.*)$', w, re.S):
            env.append(('set', m.group(1), m.group(2)))
            i += 1
        elif name in WRAPPERS:
            wrappers.append(name)
            values = WRAPPER_VALUES.get(name, set())
            i += 1
            while i < len(words):
                a = words[i]
                if name == 'env' and (a in ('-i', '--ignore-environment') or a == '-'):
                    env.append(('clear',))
                elif name == 'env' and a in ('-u', '--unset') and i + 1 < len(words):
                    env.append(('unset', words[i + 1]))
                    i += 1
                elif name == 'env' and a.startswith('--unset='):
                    env.append(('unset', a.split('=', 1)[1]))
                elif name == 'systemd-run' and a in ('-E', '--setenv') and i + 1 < len(words):
                    n, _, v = words[i + 1].partition('=')
                    env.append(('set', n, v))
                    i += 1
                elif a in values:
                    i += 1
                elif not (a.startswith('-') and a != '-') and not (name == 'timeout' and re.match(r'^[\d.]+[smhd]?$', a)):
                    break
                i += 1
        else:
            return env, wrappers, name, words[i + 1 :]
    return env, wrappers, '', []


def substitutions(command):
    """The commands in the $(...) and backquotes of COMMAND that the shell runs: not the ones in single quotes
    (sed 's/`x`/y/', grep 'a `b`'), which are text."""
    found, i, double = [], 0, False
    while i < len(command):
        ch = command[i]
        if ch == '\\':
            i += 2
            continue
        if ch == "'" and not double:
            end = command.find("'", i + 1)
            i = len(command) if end < 0 else end + 1
            continue
        if ch == '"':
            double = not double
        elif ch == '`':
            end = command.find('`', i + 1)
            if end < 0:
                break
            found.append(command[i + 1 : end])
            i = end
        elif command.startswith('$(', i):
            depth, j = 1, i + 2
            while j < len(command) and depth:
                depth += {'(': 1, ')': -1}.get(command[j], 0)
                j += 1
            found.append(command[i + 2 : j - 1])
            i = j - 1
        i += 1
    return found


def simple_commands(command, depth=0):
    """(environment changes, wrappers, command word, arguments) for every simple command in a command line, with
    the commands in $(...), backquotes and the script of a shell's -c."""
    command = strip_heredocs(command)
    lines = [command] + substitutions(command)
    for line in lines:
        words = []
        for tok in tokens(line) + [';']:
            if tok and all(ch in SEPARATORS for ch in tok):
                if words:
                    cmd = split_wrappers(words)
                    yield cmd
                    _, _, word, args = cmd
                    if word in SHELLS and depth < 3 and '-c' in args[:-1]:
                        yield from simple_commands(args[args.index('-c') + 1], depth + 1)
                words = []
            else:
                words.append(tok)


def environment(changes):
    """The environment of a command: this session's, with CHANGES applied in order. A value that the shell expands
    is None: the guard cannot know it."""
    env = dict(os.environ)
    for change in changes:
        if change[0] == 'clear':
            env.clear()
        elif change[0] == 'unset':
            env.pop(change[1], None)
        else:
            env[change[1]] = None if any(mark in change[2] for mark in '$`*?[~') else change[2]
    return env


def short_flags(arg):
    return arg[1:] if re.match(r'^-[A-Za-z]+$', arg) else ''


def under_tmp(path):
    return bool(path) and os.path.realpath(path).startswith(('/tmp/', '/var/tmp/'))


# git

def git_subcommand(args):
    """The git subcommand, its arguments, and the -C directories, after git's own options (-C dir, -c key=value...)."""
    i, dirs = 0, []
    while i < len(args) and args[i].startswith('-'):
        if args[i] == '-C' and i + 1 < len(args):
            dirs.append(args[i + 1])
        i += 1 + (args[i] in ('-C', '-c', '--git-dir', '--work-tree', '--namespace', '--config-env'))
    return (args[i], args[i + 1 :], dirs) if i < len(args) else ('', [], dirs)


def current_branch(directory):
    """The branch checked out in DIRECTORY, or None when git cannot tell."""
    try:
        out = subprocess.run(['git', 'symbolic-ref', '--short', '-q', 'HEAD'], capture_output=True, text=True,
                             timeout=5, cwd=directory)
    except (OSError, subprocess.SubprocessError):
        return None
    return out.stdout.strip() if out.returncode == 0 else None


def check_git(args, cwd):
    sub, rest, dirs = git_subcommand(args)
    directory = cwd or os.getcwd()
    for d in dirs:
        directory = os.path.join(directory, os.path.expanduser(d))
    if sub in ('add', 'stage', 'commit', 'push', 'rm', 'mv', 'reset', 'checkout', 'switch', 'rebase', 'merge',
               'cherry-pick', 'revert', 'tag', 'restore', 'stash', 'apply', 'am'):
        if (os.path.realpath(directory) + '/').startswith(PLASMAZONES):
            return plasmazones_message(directory)
    if sub in ('add', 'stage'):
        paths, broad, force = [], False, False
        for k, a in enumerate(rest):
            if a == '--':
                paths += rest[k + 1 :]
                break
            if a.startswith('-'):
                force |= a == '--force' or 'f' in short_flags(a)
                broad |= a in ('--all', '--update', '--no-ignore-removal') or a.startswith('--pathspec-from-file')
                broad |= 'A' in short_flags(a) or 'u' in short_flags(a)
            else:
                paths.append(a)
        if force or any(LOCAL_PATH.search(p) for p in paths):
            return MSG_LOCAL
        if broad or any(p in ('.', './', '..', '*', ':', ':/') or p.startswith((':/', ':(')) for p in paths):
            return MSG_BROAD
    elif sub == 'commit':
        k = 0
        while k < len(rest) and rest[k] != '--':
            a = rest[k]
            k += 1
            if a == '--all':
                return MSG_BROAD
            for n, ch in enumerate(short_flags(a)):
                if ch == 'a':
                    return MSG_BROAD
                if ch in 'mFCct':  # takes a value: the rest of the word, or the next word
                    k += n == len(a) - 2
                    break
    elif sub == 'push':
        words, k = [], 0
        while k < len(rest):
            a = rest[k]
            k += 1
            if a.startswith(('--force', '--mirror')) or 'f' in short_flags(a) or a.startswith('+'):
                return MSG_PUSH
            if a in ('--all', '--branches'):
                return MSG_PUSH_MAIN
            if a in ('-o', '--push-option', '--repo', '--receive-pack', '--exec'):
                k += 1
            elif not a.startswith('-'):
                words.append(a)
        refspecs = words[1:]
        for spec in refspecs:
            dst = spec.split(':', 1)[1] if ':' in spec else spec
            if dst.removeprefix('refs/').removeprefix('heads/') == 'main':
                return MSG_PUSH_MAIN
        # No refspec, or HEAD: the current branch
        if (not refspecs or any(s.split(':')[-1] in ('HEAD', '@') for s in refspecs)) \
                and current_branch(directory) == 'main':
            return MSG_PUSH_MAIN
    return None


# Installs

def is_install(env, word, args):
    """Whether WORD ARGS installs the build outside a temporary directory: cmake --install, an install target of
    cmake --build, ninja or make, or cmake -P cmake_install.cmake."""
    temporary = under_tmp(env.get('DESTDIR'))
    if word == 'cmake':
        prefix = None
        for k, a in enumerate(args):
            if a == '--prefix' and k + 1 < len(args):
                prefix = args[k + 1]
            elif a.startswith('--prefix='):
                prefix = a.split('=', 1)[1]
            elif m := re.match(r'^-DCMAKE_INSTALL_PREFIX(:\w+)?=(.*)$', a):
                prefix = m.group(2)
        if '--install' in args:
            return not (temporary or under_tmp(prefix))
        if '-P' in args and any(os.path.basename(a) == 'cmake_install.cmake' for a in args):
            return not (temporary or under_tmp(prefix))
        targets = [args[k + 1] for k, a in enumerate(args[:-1]) if a in ('--target', '-t')]
        targets += [a.split('=', 1)[1] for a in args if a.startswith('--target=')]
        return '--build' in args and any(INSTALL_TARGET.match(t) for t in targets) and not temporary
    if word in ('ninja', 'make', 'gmake'):
        temporary |= any(a.startswith('DESTDIR=') and under_tmp(a[len('DESTDIR='):]) for a in args)
        return any(INSTALL_TARGET.match(a) for a in args) and not temporary
    return False


# The user's Afterglow and mux daemon

def display_socket_name(display, runtime):
    """The file name of a display's main socket in RUNTIME, as AfterglowProto::sharedSocketPath names it."""
    name = display or ''
    if runtime and name.startswith(runtime.rstrip('/') + '/'):
        name = name[len(runtime.rstrip('/')) + 1 :]
    name = name.lstrip('/')
    return 'afterglow-%s.sock' % (re.sub(r'[^A-Za-z0-9._-]', '_', name) or 'wayland-0')


def isolated_client(env):
    """Whether afterglow ctl, aglow run or afterglow run reaches a socket other than the user's. Each picks
    AFTERGLOW_SOCKET, else the main socket of WAYLAND_DISPLAY in XDG_RUNTIME_DIR. Only literal values count."""
    if any(env.get(n, '') is None for n in ('AFTERGLOW_SOCKET', 'AFTERGLOW_PANE_ID', 'XDG_RUNTIME_DIR',
                                            'WAYLAND_DISPLAY')):
        return False
    socket = env.get('AFTERGLOW_SOCKET')
    if socket:
        if not socket.startswith('/'):
            return False
        target = os.path.realpath(socket)
    elif env.get('AFTERGLOW_PANE_ID'):
        return True  # the client refuses a pane ID without its socket
    elif not env.get('XDG_RUNTIME_DIR'):
        return True  # no socket to find
    else:
        runtime = env['XDG_RUNTIME_DIR']
        target = os.path.realpath(os.path.join(runtime, display_socket_name(env.get('WAYLAND_DISPLAY'), runtime)))
    if os.environ.get('AFTERGLOW_SOCKET') and target == os.path.realpath(os.environ['AFTERGLOW_SOCKET']):
        return False
    runtime = os.environ.get('XDG_RUNTIME_DIR')
    if runtime and os.path.dirname(target) == os.path.realpath(runtime):
        # The main socket of the user's display, or the socket of a separate process on it
        own = display_socket_name(os.environ.get('WAYLAND_DISPLAY'), runtime)[: -len('.sock')]
        return not re.fullmatch(re.escape(own) + r'(-\d+)?\.sock', os.path.basename(target))
    return True


def isolated_launch(env):
    """Whether a launch of afterglow opens on a display other than the user's: another WAYLAND_DISPLAY or another
    XDG_RUNTIME_DIR, given literally."""
    runtime, display = env.get('XDG_RUNTIME_DIR', ''), env.get('WAYLAND_DISPLAY', '')
    if runtime is None or display is None:
        return False
    return (os.path.realpath(runtime or '/-') != os.path.realpath(os.environ.get('XDG_RUNTIME_DIR', '') or '/-')
            or (display or 'wayland-0') != (os.environ.get('WAYLAND_DISPLAY') or 'wayland-0'))


# The names of the daemon: aglow-mux, and afterglow-mux of an older build, which the user may still run. Both name
# its executable, its systemd unit and its socket directories.
MUX_DAEMONS = ('aglow-mux', 'afterglow-mux')


def user_bases():
    """The directories that hold the socket directories of the user's own mux daemon."""
    bases = {'/run/user/%d' % os.getuid()}
    if os.environ.get('XDG_RUNTIME_DIR', '').startswith('/'):
        bases.add(os.environ['XDG_RUNTIME_DIR'])
    if os.environ.get('XDG_STATE_HOME', '').startswith('/'):
        bases.add(os.environ['XDG_STATE_HOME'])
    try:
        bases.add(os.path.join(pwd.getpwuid(os.getuid()).pw_dir, '.local', 'state'))
    except KeyError:
        pass
    return {os.path.realpath(b) for b in bases}


def mux_directories():
    return {os.path.join(base, name) for base in user_bases() for name in MUX_DAEMONS}


def isolated_mux(env):
    """Whether a mux command reaches a daemon other than the user's: a literal XDG_RUNTIME_DIR and a state
    directory (XDG_STATE_HOME, else HOME/.local/state) that are not the user's."""
    runtime = env.get('XDG_RUNTIME_DIR')
    if not runtime or not runtime.startswith('/'):
        return False
    users = user_bases()
    if os.path.realpath(runtime) in users:
        return False
    state = env.get('XDG_STATE_HOME')
    if not state:
        home = env.get('HOME')
        if not home:
            return home is not None  # no HOME: no state socket
        state = os.path.join(home, '.local', 'state')
    return state.startswith('/') and os.path.realpath(state) not in users


def reaches_mux(word, args):
    """Whether afterglow ARGS or a daemon of MUX_DAEMONS reaches a mux daemon: afterglow mux (but not its help),
    afterglow ctl --session, afterglow --mux-attach, and the daemon itself (but not its --help or --version)."""
    if word == 'afterglow':
        if args[:2] == ['ctl', '--session']:
            return True
        if any(a == '--mux-attach' or a.startswith('--mux-attach=') for a in args):
            return True
        if args[:1] != ['mux'] or len(args) == 1 or args[1] in ('-h', '--help', 'help'):
            return False
        return args[1:] not in (['server', '--help'], ['server', '-h'])
    return word in MUX_DAEMONS and args[:1] not in (['--help'], ['-h'], ['--version'])


def drives_instance(word, args):
    """Whether afterglow ctl, afterglow run or aglow run connects to a running Afterglow. Their help does not."""
    if word == 'afterglow' and args[:1] == ['ctl']:
        return args[1:2] not in ([], ['-h'], ['--help'], ['--session'])
    if word in ('afterglow', 'aglow') and args[:1] == ['run']:
        return not any(a in ('-h', '--help') for a in args[1:2])
    return False


# afterglow's options that take a value, and the ones that end the process before a window (main.cpp)
LAUNCH_VALUES = {'-d', '--working-directory', '--title', '--config', '-o', '--override', '--class', '--mux-attach'}
NO_WINDOW = {'-h', '--help', '-?', '--help-all', '-v', '--version', '--config-reference'}
# The words after afterglow that run something else than the terminal
NO_TERMINAL = {'ctl', 'ssh', 'mux', 'run', 'transfer', 'dnd', 'icat', 'clipboard'}


def launches_window(args):
    if args[:1] and args[0] in NO_TERMINAL:
        return False
    k = 0
    while k < len(args):
        a = args[k]
        if a == '--' or not a.startswith('-'):
            break
        if a.split('=', 1)[0] in NO_WINDOW:
            return False
        k += 1 + (a in LAUNCH_VALUES)
    return True


SYSTEMCTL_ENDS = {'stop', 'kill', 'restart', 'try-restart', 'reload-or-restart', 'try-reload-or-restart',
                  'condrestart', 'force-reload', 'disable', 'mask', 'clean', 'freeze', 'reset-failed'}
MUX_UNITS = tuple(f'{name}.service' for name in MUX_DAEMONS) + ('app.slice',)


def stops_mux_unit(args):
    """Whether systemctl ARGS stops, restarts or masks the unit of the user's mux daemon, or app.slice, which
    holds it."""
    words = [a for a in args if not a.startswith('-')]
    if not SYSTEMCTL_ENDS & set(words):
        return False
    for unit in words:
        if '.' not in unit and not any(c in unit for c in '*?['):
            unit += '.service'
        if any(fnmatch.fnmatchcase(name, unit) for name in MUX_UNITS):
            return True
    return False


REMOVES = {'rm', 'unlink', 'rmdir', 'mv', 'shred', 'trash', 'trash-put'}


def removes_mux_files(word, args, cwd):
    """Whether a removal or a move names one of the user's mux socket directories, a file in one, or a directory
    above one."""
    if word not in REMOVES:
        return False
    protected = mux_directories()
    for a in args:
        if a.startswith('-'):
            continue
        if '$' in a or '`' in a:
            if any(name in a for name in MUX_DAEMONS):
                return True
            continue
        path = os.path.realpath(os.path.join(cwd or os.getcwd(), os.path.expanduser(a)))
        if any(path == d or path.startswith(d + '/') or d.startswith(path.rstrip('/') + '/') for d in protected):
            return True
    return False


# The checks

def check_bash(command, cwd=None):
    cmds = list(simple_commands(command))
    words, kills_found = set(), False
    for changes, wrappers, word, args in cmds:
        # The program that a debugger starts is checked as a command of its own
        if (start := DEBUGGER_STARTS.get(word)) and start in args:
            rest = args[args.index(start) + 1 :]
            if word == 'rr':
                rest = [a for a in rest if not a.startswith('-')][:1] + rest[1:]
            if rest:
                inner_changes, inner_wrappers, inner_word, inner_args = split_wrappers(rest)
                cmds.append((changes + inner_changes, wrappers + [word] + inner_wrappers, inner_word, inner_args))
        words.add(word)
        # kill with PIDs that the shell fills in, or that xargs reads: not kill with numbers
        kills_found |= word == 'kill' and ('xargs' in wrappers or any('$' in a or '`' in a for a in args))
        env = environment(changes)
        if word in KILL_BY_NAME:
            return MSG_PKILL
        if word in PRIVILEGED or PRIVILEGED & set(wrappers):
            return MSG_SUDO
        if is_install(env, word, args):
            return MSG_INSTALL
        if word == 'git' and (message := check_git(args, cwd)):
            return message
        if reaches_mux(word, args) and not isolated_mux(env):
            return MSG_MUX
        if word == 'systemctl' and stops_mux_unit(args):
            return MSG_MUX
        if removes_mux_files(word, args, cwd):
            return MSG_MUX
        if drives_instance(word, args) and not isolated_client(env):
            return MSG_CTL
        if word == 'afterglow' and launches_window(args) and not reaches_mux(word, args) \
                and not isolated_launch(env):
            return MSG_LAUNCH
    # kill $(pgrep ...), pgrep ... | xargs kill, a loop over pidof
    if words & {'pgrep', 'pidof'} and kills_found:
        return MSG_PGREP_KILL
    return None


def check_edit(path):
    real = os.path.realpath(path)
    if real.startswith(PLASMAZONES):
        return plasmazones_message(path)
    project = os.path.realpath(os.environ.get('CLAUDE_PROJECT_DIR', os.getcwd())) + '/'
    if not real.startswith(project):
        return None
    rel = real[len(project) :]
    if rel in GENERATED:
        return f'{rel} is generated. Change its source, then run: {GENERATED[rel]}'
    if BUILD_TREE.match(rel):
        return f'{rel} is in a build tree. Change the source or CMake code that makes it.'
    return None


def check(call):
    """The message that blocks the hook input CALL, or None. A malformed input raises ValueError."""
    if not isinstance(call, dict):
        raise ValueError('the hook input is not an object')
    tool = call.get('tool_name', '')
    params = call.get('tool_input', {})
    cwd = call.get('cwd')
    if not isinstance(tool, str) or not isinstance(params, dict) or not (cwd is None or isinstance(cwd, str)):
        raise ValueError('the hook input has a field of the wrong type')
    if tool == 'Bash':
        command = params.get('command', '')
        if not isinstance(command, str):
            raise ValueError('the command is not a string')
        return check_bash(command, cwd)
    if tool in ('Edit', 'Write', 'MultiEdit', 'NotebookEdit'):
        path = params.get('file_path') or params.get('notebook_path') or ''
        if not isinstance(path, str):
            raise ValueError('the path is not a string')
        return check_edit(path)
    return None


def main():
    # A hook that fails with another status than 2 lets the call run: any failure of a check blocks it instead
    try:
        message = check(json.load(sys.stdin))
    except Exception as error:
        print(MSG_FAILED.format(f'{type(error).__name__}: {error}'[:200]), file=sys.stderr)
        sys.exit(2)
    if message:
        print(message, file=sys.stderr)
        sys.exit(2)


if __name__ == '__main__':
    main()
