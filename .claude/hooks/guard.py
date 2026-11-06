#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 fuddlesworth
# SPDX-License-Identifier: GPL-3.0-or-later
"""PreToolUse guard for the hard rules of this project (CLAUDE.md).

Claude Code passes the tool call as JSON on stdin. Exit code 2 blocks the call and shows stderr to Claude.

A Bash command is read the way the shell reads it: split into simple commands at ; & | newlines, parentheses and
backticks outside quotes, each one tokenized with its quotes removed, and the wrappers in front of a command (then,
do, !, env, xargs, timeout, nohup, sudo, strace...) skipped. So `then pkill x` doesn't get past a rule, and a rule's
word inside a quoted string (grep 'a|sudo') doesn't trip it. Here-document bodies (commit messages) are text, not
commands. The scripts of nested shells, eval and watch are read the same way, and so are the commands in another
command's arguments (find -exec, gdb --args, systemd-run).
"""

import json
import os
import re
import shlex
import subprocess
import sys

# Local state that never goes into git: another tool's workspace, Python caches, build trees
LOCAL_PATH = re.compile(r'(^|/)(\.Codex|__pycache__|build[^/]*|\.cache)(/|$)')

# Generated files: run the generator instead
GENERATED = {
    'docs/CONFIGURATION.md': 'python3 tools/generate-reference.py build/afterglow/afterglow',
    'libs/afterglow-vt/src/UnicodeTables.inc': 'python3 -I tools/gen_unicode_tables.py <ucd-dir> '
    'libs/afterglow-vt/src/UnicodeTables.inc',
}
BUILD_TREE = re.compile(r'^(build[^/]*|\.cache)/')

# The Phosphor libraries are built from this checkout, but they belong to PlasmaZones, which changes only through
# its own branch and pull request
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
            common = os.path.join(gitdir, f.readline().strip())
    except (OSError, UnicodeDecodeError):  # the main checkout's .git is a directory
        return repo
    common = os.path.realpath(common)
    # A separate git directory or a bare repository has no working tree next to it
    if os.path.basename(common) != '.git':
        return repo
    return os.path.dirname(common)


# The checkout next to the main checkout of this repository, where cmake/AfterglowPhosphor.cmake looks for it by
# default. This file is .claude/hooks/guard.py, so the repository is two directories up. From a worktree, the
# directory next to it is not that checkout.
REPOSITORY = main_checkout(os.path.realpath(os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', '..')))
PLASMAZONES = os.path.realpath(os.path.join(REPOSITORY, '..', 'PlasmaZones')) + '/'

SEPARATORS = ';&|()\n`'
# Words that run the command after them: skipped, with their options, to find the command itself
WRAPPERS = set(
    'then do else elif if while until ! { } time nohup env exec command builtin xargs timeout nice ionice stdbuf setsid '
    'unbuffer sudo doas dbus-run-session strace ltrace valgrind perf noglob nocorrect coproc'.split()
)
# The perf subcommands that run the command after them
PERF_COMMANDS = set('record stat trace ftrace c2c mem sched lock kmem kwork timechart kvm'.split())
# Shells whose -c runs its argument as a script
SHELLS = set('bash sh zsh dash ash ksh mksh yash fish'.split())
# Commands that start the command in their arguments with their own environment (find -exec, gdb --args). Any
# other (systemd-run, ssh, flatpak-spawn) may start it in an environment that the guard cannot see.
INHERITS = set('find gdb lldb rr flock chrt taskset catchsegv parallel'.split())
# gdb options that take a value, without their dashes: gdb takes -NAME and --NAME alike
GDB_VALUES = set(
    'p pid c core x command ex eval-command iex init-eval-command ix init-command d directory s symbols e exec se '
    'cd D data-directory tty b l i interpreter'.split()
)

MSG_LOCAL = (
    '.Codex/, __pycache__/, build*/ and .cache/ are local state and never go into git, and `git add -f` would '
    'bypass .gitignore. Stage only source files, by name.'
)
MSG_BROAD = 'Stage files by name (git add path...), not with -A, -u, ., *, :/ or commit -a.'
MSG_PUSH = 'Never force-push. Fix the branch with a new commit instead.'
MSG_PUSH_MAIN = ('Never push to main. Push a branch and open a pull request. The user merges it. '
                 'If this command makes the branch, name it: git push -u origin BRANCH.')
MSG_PUSH_UNKNOWN = ('This push names no branch, and the command runs git in a directory the guard cannot resolve. '
                    'Name the branch: git push -u origin BRANCH.')
# A directory the guard cannot know before the shell runs: $(...), $VAR, a backquote command, cd -, GIT_DIR
UNKNOWN = object()
# Wrapper options that take the next word as their value
WRAPPER_VALUES = {
    'env': {'-C', '--chdir', '-u', '--unset', '-S', '--split-string'},
    'dbus-run-session': {'--config-file', '--dbus-daemon'},
    'timeout': {'-s', '--signal', '-k', '--kill-after'},
    'nice': {'-n', '--adjustment'},
    'ionice': {'-c', '--class', '-n', '--classdata', '-p', '--pid'},
    'stdbuf': {'-i', '--input', '-o', '--output', '-e', '--error'},
    'xargs': {'-I', '--replace', '-d', '--delimiter', '-a', '--arg-file', '-E', '-n', '--max-args', '-P', '-s'},
    'exec': {'-a'},
    'time': {'-f', '--format', '-o', '--output'},
    'strace': {'-a', '-b', '-e', '-E', '-I', '-o', '-O', '-p', '-P', '-s', '-S', '-u', '-U', '-X', '--output',
               '--trace', '--signal', '--status', '--trace-path', '--string-limit', '--user', '--env', '--attach',
               '--columns', '--summary-sort-by', '--detach-on', '--interruptible', '--argv0'},
    'ltrace': {'-a', '-A', '-D', '-e', '-F', '-l', '-n', '-o', '-p', '-s', '-u', '-w', '-x', '--output', '--align',
               '--indent', '--library'},
    'perf': {'-o', '-e', '-p', '-t', '-c', '-F', '-C', '-G', '-r', '-x', '-I', '-u', '-m', '-j', '--output', '--event',
             '--pid', '--tid', '--cpu', '--repeat', '--call-graph', '--freq', '--count', '--mmap-pages', '--cgroup',
             '--uid', '--field-separator', '--interval-print'},
    'watch': {'-n', '--interval', '-q', '--equexit', '-s', '--shotsdir'},
}
MSG_PKILL = (
    'pkill and killall match by name, and the user runs Afterglow as their terminal: `pkill afterglow` closes the '
    "user's own windows, and possibly this session. Use `pgrep -a NAME`, then `kill PID` on a build/ process only."
)
MSG_PGREP_KILL = (
    'kill with pgrep -f matches the shell that runs the command, because its command line holds the pattern, and '
    "it can match the user's own Afterglow. List the processes with `pgrep -a NAME`. Then run `kill PID` with the "
    'numbers in a separate command.'
)
MSG_SUDO = 'Do not use sudo. The user installs system packages. Tell the user what you need and why.'
MSG_INSTALL = ('Never install the build (cmake --install, the install target). The user installs Afterglow. '
               'Only `cmake --install` into a temporary directory (--prefix /tmp/... or DESTDIR=/tmp/...) is allowed.')
MSG_CTL = (
    "`afterglow ctl`, `aglow run` and `afterglow run` drive the user's running Afterglow: this session runs inside "
    "one of its panes, and they use the pane's $AFTERGLOW_SOCKET, else the socket of the user's Wayland display. A "
    'private D-Bus does not change that. Give them the socket of a test session '
    '(AFTERGLOW_SOCKET=/path/to/test.sock), or remove the pane variables and point them at another display '
    '(env -u AFTERGLOW_SOCKET -u AFTERGLOW_PANE_ID WAYLAND_DISPLAY=...). Ask the user before you drive their '
    'instance.'
)


def strip_heredocs(command):
    """The command with the bodies of its here-documents left out."""
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
    lex.commenters = ''
    try:
        return list(lex)
    except ValueError:  # unbalanced quotes: split at whitespace and separators, quotes dropped
        return re.findall(r'[%s]+|[^\s%s]+' % (re.escape(SEPARATORS), re.escape(SEPARATORS)), command.replace('"', ''))


def without_substitutions(command):
    """(the command line with each $(...) and backquote command as one word, $SUBST; the bodies of those commands).
    Quotes are followed as the shell follows them: nothing is special inside single quotes or after a #."""
    out, bodies, i, quote, word_start = [], [], 0, '', True
    while i < len(command):
        ch = command[i]
        if quote == "'" or (not quote and ch == '#' and word_start):
            end = command.find("'", i) if quote else command.find('\n', i + 1)
            end = len(command) if end < 0 else end + (quote == "'")
            if quote:  # a comment is left out: it is not a command, and its apostrophes are not quotes
                out.append(command[i:end])
            i, quote, word_start = end, '', False
            continue
        if ch == '\\':
            if command[i + 1 : i + 2] != '\n':  # a backslash before a newline joins the lines
                out.append(command[i : i + 2])
            i, word_start = i + 2, False
            continue
        word_start = not quote and ch in ' \t\n;&|('
        if ch in '\'"' and quote in ('', ch) and not (ch == "'" and quote):
            quote = '' if quote else ch
        if command.startswith('$(', i):
            depth, j = 1, i + 2
            while j < len(command) and depth:
                depth += {'(': 1, ')': -1}.get(command[j], 0)
                j += 1
            if not depth:
                out.append('$SUBST')
                bodies.append(command[i + 2 : j - 1])
                i = j
                continue
        elif ch == '`':
            j = command.find('`', i + 1)
            if j >= 0:
                out.append('$SUBST')
                bodies.append(command[i + 1 : j])
                i = j + 1
                continue
        out.append(ch)
        i += 1
    return ''.join(out), bodies


def simple_commands(command):
    """(wrappers, command word, arguments) for every simple command in a command line, including the ones in a
    command substitution. A substitution inside single quotes or a comment is text, as in the shell."""
    command = strip_heredocs(command)
    lines, pending = [], [command]
    while pending:
        line, bodies = without_substitutions(pending.pop())
        lines.append(line)
        pending += bodies
    for line in lines:
        words = []
        for tok in tokens(line) + [';']:
            if tok and all(ch in SEPARATORS for ch in tok):
                if words:
                    yield split_wrappers(words)
                words = []
            else:
                words.append(tok)


def split_words(text):
    try:
        return shlex.split(text)
    except ValueError:
        return text.split()


def option_value(name, words, i):
    """(the option of wrapper NAME that takes a value, '' if none; the short letters before it; its value; the index
    after the option) for words[i], read as getopt reads it: --opt=value, or a short bundle such as -vu NAME or
    -uNAME, where the first letter that takes a value takes the rest of the word, or the next word."""
    option, values = words[i], WRAPPER_VALUES.get(name, ())
    following = words[i + 1] if i + 1 < len(words) else ''
    if not option.startswith('-'):  # timeout's duration, perf's subcommand
        return '', '', '', i + 1
    if option.startswith('--'):
        flag, eq, value = option.partition('=')
        # getopt_long takes a unique prefix of a long option (env --split-str)
        prefixed = {v for v in values if v.startswith(flag)}
        flag = prefixed.pop() if flag not in values and len(prefixed) == 1 else flag
        if flag not in values:
            return '', '', '', i + 1
        return (flag, '', value, i + 1) if eq else (flag, '', following, i + 2)
    for k in range(1, len(option)):
        if '-' + option[k] in values:
            rest = option[k + 1 :]
            return '-' + option[k], option[1:k], rest or following, i + 1 if rest else i + 2
    return '', option[1:], '', i + 1


def split_wrappers(words):
    wrappers, i, words = [], 0, list(words)
    while i < len(words):
        w = words[i]
        name = os.path.basename(w)
        if re.match(r'^[A-Za-z_][A-Za-z0-9_]*=', w):  # VAR=value before a command
            if w.startswith(('GIT_DIR=', 'GIT_WORK_TREE=')):
                wrappers.append('chdir:$')  # git then runs elsewhere
            wrappers.append('set:' + w)
            i += 1
        elif name in WRAPPERS:
            wrappers.append(name)
            i += 1
            if name == 'coproc' and words[i + 1 : i + 2] == ['{']:
                i += 1  # coproc NAME { ...; }: the word names the coprocess
            # the wrapper's options with their values, timeout's duration and perf's subcommand
            while i < len(words) and (words[i].startswith('-') or re.match(r'^[\d.]+[smhd]?$', words[i])
                                      or (name == 'perf' and words[i] in PERF_COMMANDS)):
                flag, before, value, after = option_value(name, words, i)
                if name == 'env':
                    if words[i] == '-' or 'i' in before or (
                            len(words[i]) >= len('--ignore-e') and '--ignore-environment'.startswith(words[i])):
                        wrappers.append('clear')
                    if flag in ('-u', '--unset'):
                        wrappers.append('unset:' + value)
                    elif flag in ('-C', '--chdir'):
                        wrappers.append('chdir:' + value)
                    elif flag in ('-S', '--split-string'):
                        # env splits the string into the words that follow it: options, VAR=value and the command
                        words[i:after] = split_words(value)
                        continue
                elif name == 'strace' and flag in ('-E', '--env'):
                    wrappers.append(('set:' if '=' in value else 'unset:') + value)
                i = after
        else:
            return wrappers, name, words[i + 1 :]
    return wrappers, '', []


def short_flags(arg):
    return arg[1:] if re.match(r'^-[A-Za-z]+$', arg) else ''


def git_subcommand(args):
    """The git subcommand and its arguments, after git's own options (-C dir, -c key=value...)."""
    i = 0
    while i < len(args) and args[i].startswith('-'):
        i += 1 + (args[i] in ('-C', '-c', '--git-dir', '--work-tree', '--namespace', '--config-env'))
    return (args[i], args[i + 1 :]) if i < len(args) else ('', [])


def resolve(base, target):
    """Where `cd TARGET` from BASE lands, or UNKNOWN when the shell decides (expansions, cd -)."""
    if base is UNKNOWN or '$' in target or '`' in target or target == '-':
        return UNKNOWN
    path = os.path.abspath(os.path.join(base, os.path.expanduser(target)))
    # A cd that fails leaves the shell where it was, which is kept; a directory that mkdir made earlier in the
    # command belongs to the repository of its nearest existing parent.
    while path != '/' and not os.path.isdir(path):
        path = os.path.dirname(path)
    return path


def git_directory(args, cwd):
    """The directory git runs in: @p cwd, then its own -C options (they stack, like cd)."""
    directory, i = cwd, 0
    while i < len(args) and args[i].startswith('-'):
        if args[i] == '-C' and i + 1 < len(args):
            directory = resolve(directory, args[i + 1])
        elif args[i].startswith(('--git-dir', '--work-tree')):
            directory = UNKNOWN
        i += 1 + (args[i] in ('-C', '-c', '--git-dir', '--work-tree', '--namespace', '--config-env'))
    return directory


def check_git(args, places):
    sub, rest = git_subcommand(args)
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
        for a in rest:
            if a.startswith(('--force', '--mirror')) or 'f' in short_flags(a) or a.startswith('+'):
                return MSG_PUSH
        return pushes_main(rest, [git_directory(args, place) for place in places])
    return None


def current_branch(directory):
    try:
        out = subprocess.run(['git', 'symbolic-ref', '--short', '-q', 'HEAD'], capture_output=True, text=True,
                             timeout=5, cwd=directory)
        return out.stdout.strip()
    except (OSError, subprocess.SubprocessError):
        return ''


def pushes_main(rest, directories):
    """The refusal for `git push ARGS`, if it can update main: a refspec whose destination is main, --all, or no
    refspec (push.default then pushes the current branch) while main is checked out in any directory the command
    may run git in. A directory the guard cannot resolve counts as main for HEAD and the current branch."""

    def current_is_main():
        if any(d is UNKNOWN for d in directories):
            return MSG_PUSH_UNKNOWN
        return MSG_PUSH_MAIN if any(current_branch(d) == 'main' for d in directories) else None

    words, k = [], 0
    while k < len(rest):
        a = rest[k]
        k += 1
        if a == '--all' or a == '--branches':
            return MSG_PUSH_MAIN
        if a in ('-o', '--push-option', '--repo', '--receive-pack', '--exec'):
            k += 1
        elif not a.startswith('-'):
            words.append(a)
    refspecs = words[1:]
    if not refspecs:
        return current_is_main()
    for spec in refspecs:
        dst = spec.split(':', 1)[1] if ':' in spec else spec
        if dst in ('HEAD', '@') or '$' in dst:  # $(git branch --show-current) is the current branch
            if message := current_is_main():
                return message
        elif dst.removeprefix('refs/').removeprefix('heads/') == 'main':
            return MSG_PUSH_MAIN
    return None


def under_tmp(path):
    """An absolute path, written out, under /tmp. A relative path, ~ or $VAR is resolved by the shell, in a place
    the guard does not know."""
    return path.startswith('/') and os.path.realpath(path).startswith('/tmp/')


def temp_destination(command, args):
    """An install into a temporary directory (--prefix or DESTDIR under /tmp) is allowed: it never touches the system."""
    prefix = None
    for k, a in enumerate(args):
        if a == '--prefix' and k + 1 < len(args):
            prefix = args[k + 1]
        elif a.startswith('--prefix='):
            prefix = a.split('=', 1)[1]
    if prefix is not None:
        return under_tmp(prefix)
    match = re.search(r'(?:^|\s)DESTDIR=(\S+)', command)
    return bool(match) and under_tmp(match.group(1).strip('\'"'))


def is_install(word, args, command=''):
    if word == 'cmake':
        if '--install' in args:
            return not temp_destination(command, args)
        targets = [args[k + 1] for k, a in enumerate(args[:-1]) if a in ('--target', '-t')]
        return 'install' in targets or any(a.startswith('--target=install') for a in args)
    return word in ('ninja', 'make') and 'install' in args


def ctl_socket_name(display, runtime):
    """The file name of a display's main socket in RUNTIME, as AfterglowProto::sharedSocketPath names it: a
    WAYLAND_DISPLAY under the runtime directory loses that directory, then one leading '/', and each byte of the
    rest that is not a letter, digit, '.', '_' or '-' becomes '_', so a character of two UTF-8 bytes becomes two.
    Without the first step the two spellings of one display name two sockets here and one socket there."""
    name = os.fsencode(display or '')
    # Socket.cpp's runtimeDirectory: a relative XDG_RUNTIME_DIR counts as none.
    if runtime and runtime.startswith('/'):
        prefix = os.fsencode(runtime)
        prefix = prefix if prefix.endswith(b'/') else prefix + b'/'
        name = name[len(prefix):] if name.startswith(prefix) else name
    name = name[1:] if name.startswith(b'/') else name
    return 'afterglow-%s.sock' % (re.sub(rb'[^A-Za-z0-9._-]', b'_', name).decode('ascii') or 'wayland-0')


def isolated_ctl(wrappers):
    """Whether one afterglow ctl, aglow run or afterglow run command reaches a socket other than the user's. Its
    VAR=value and env -u words apply in order over this environment, and each picks its socket as
    AfterglowProto::findSocket does: AFTERGLOW_SOCKET, else the main socket of WAYLAND_DISPLAY in XDG_RUNTIME_DIR.
    Only literal values count: an expansion, a brace, a glob or ~ can name the user's own socket. After
    'inherit:?' the command runs in an environment the guard cannot see, so each of those variables is unknown until
    a later word sets or unsets it."""
    env = dict(os.environ)
    for w in wrappers:
        if w == 'clear':
            env.clear()
        elif w == 'inherit:?':
            env = dict.fromkeys(('AFTERGLOW_SOCKET', 'AFTERGLOW_PANE_ID', 'WAYLAND_DISPLAY', 'XDG_RUNTIME_DIR'),
                                UNKNOWN)
        elif w.startswith('set:'):
            name, value = w[len('set:'):].split('=', 1)
            if any(mark in value for mark in '$`{*?[~'):
                return False
            env[name] = value
        elif w.startswith('unset:'):
            env.pop(w[len('unset:'):], None)

    def known(name):
        if env.get(name) is UNKNOWN:
            raise LookupError(name)
        return env.get(name)

    try:
        # The shell expands ~ in an assignment, and Qt puts a relative socket
        # name under the temporary directory: only an absolute path is known here.
        for name in ('AFTERGLOW_SOCKET', 'XDG_RUNTIME_DIR'):
            if env.get(name) and env[name] is not UNKNOWN and not env[name].startswith('/'):
                return False
        if known('AFTERGLOW_SOCKET'):
            target = env['AFTERGLOW_SOCKET']
        elif known('AFTERGLOW_PANE_ID'):
            return True  # ctl refuses a pane ID without its socket
        elif known('XDG_RUNTIME_DIR'):
            target = os.path.join(env['XDG_RUNTIME_DIR'],
                                  ctl_socket_name(known('WAYLAND_DISPLAY'), env['XDG_RUNTIME_DIR']))
        else:
            return True  # ctl stops: no socket to find
    except LookupError:
        return False
    target = os.path.realpath(target)
    runtime = os.environ.get('XDG_RUNTIME_DIR')
    if os.environ.get('AFTERGLOW_SOCKET') and target == os.path.realpath(os.environ['AFTERGLOW_SOCKET']):
        return False
    if runtime and os.path.dirname(target) == os.path.realpath(runtime):
        # The main socket of the user's display, or a separate process's on it.
        own = ctl_socket_name(os.environ.get('WAYLAND_DISPLAY'), runtime)[: -len('.sock')]
        if re.fullmatch(re.escape(own) + r'(-\d+)?\.sock', os.path.basename(target)):
            return False
    return True


def drives_instance(args, program='afterglow'):
    """Whether PROGRAM ARGS connects to a running Afterglow: afterglow ctl, afterglow run and aglow run do, also
    run --complete. Their help alone, as Ctl.cpp and Run.cpp read it, prints the usage and connects to nothing."""
    if args[:1] not in ((['ctl'], ['run']) if program == 'afterglow' else (['run'],)):
        return False
    return args[1:2] not in (['-h'], ['--help'])


def argument_commands(wrappers, word, args):
    """(wrappers, command word, arguments) for each command that may run from the arguments of WORD: find -exec,
    gdb --args, systemd-run, a debugger or a tracer the guard does not list. Only afterglow, a wrapper, a shell, gdb
    or VAR=value starts one, so that an argument such as -name stays an argument."""
    outer = wrappers if word in INHERITS else wrappers + ['inherit:?']
    k = 0
    while k < len(args):
        name = os.path.basename(args[k])
        if (name in WRAPPERS or name in SHELLS or name in ('afterglow', 'aglow', 'eval', 'watch', 'gdb')
                or '=' in args[k]):
            inner, name, rest = split_wrappers(args[k:])
            yield outer + inner, name, rest
            # Go on after the command word: its env and VAR=value words belong to it
            k = len(args) - len(rest)
        else:
            k += 1


def gdb_inferior(args):
    """(program, its arguments, the gdb commands, whether a command file runs) for gdb ARGS. The program is the
    word after --args, else the first operand."""
    program, arguments, commands, files, k = None, [], [], False, 0
    while k < len(args):
        a = args[k]
        if a in ('--args', '-args'):
            program, arguments = (args[k + 1], args[k + 2 :]) if k + 1 < len(args) else (None, [])
            break
        name, eq, value = a.lstrip('-').partition('=')
        if a.startswith('-') and name in GDB_VALUES:
            if not eq:
                k += 1
                value = args[k] if k < len(args) else ''
            if name in ('ex', 'eval-command', 'iex', 'init-eval-command'):
                commands.append(value)
            elif name in ('x', 'command', 'ix', 'init-command'):
                files = True
        elif not a.startswith('-') and program is None:
            program = a
        k += 1
    return program, arguments, commands, files


def gdb_drives(wrappers, args):
    """Whether gdb ARGS runs an afterglow ctl, afterglow run or aglow run that reaches the user's socket. A gdb
    command (-ex 'run ctl list', set args) gives the program its arguments, and set environment changes its
    environment. A command file can do either."""
    program, arguments, commands, files = gdb_inferior(args)
    if program is None or os.path.basename(program) not in ('afterglow', 'aglow'):
        return False
    extra = []
    for words in map(split_words, commands):
        if words[:1] in (['r'], ['run'], ['start'], ['starti']) and len(words) > 1:
            arguments = words[1:]
        elif words[:2] == ['set', 'args']:
            arguments = words[2:]
        elif words[:1] in (['set'], ['unset']) and words[1:2] and words[1].startswith('env'):
            extra = ['inherit:?']
    if files:
        arguments, extra = ['run'], ['inherit:?']
    return drives_instance(arguments, os.path.basename(program)) and not isolated_ctl(wrappers + extra)


def drives(wrappers, word, args):
    if word in ('afterglow', 'aglow'):
        return drives_instance(args, word) and not isolated_ctl(wrappers)
    return word == 'gdb' and gdb_drives(wrappers, args)


def shell_scripts(word, args):
    """The scripts that a nested shell, eval, watch or a gdb shell command runs. Every word after a shell's -c is
    checked: bash takes options after -c, and fish takes several -c and -C scripts."""
    if word in SHELLS:
        for k, a in enumerate(args):
            if a in ('-c', '--command', '-C', '--init-command') or 'c' in short_flags(a) or 'C' in short_flags(a):
                return args[k + 1 :]
            if a.startswith(('--command=', '--init-command=')):
                return [a.split('=', 1)[1]] + args[k + 1 :]
        return []
    if word == 'eval' and args:
        return [' '.join(args)]
    if word == 'watch':  # watch runs its words with sh -c
        k = 0
        while k < len(args) and args[k].startswith('-'):
            k = option_value('watch', args, k)[3]
        return [' '.join(args[k:])] if k < len(args) else []
    if word == 'gdb':
        return [m.group(1) for c in gdb_inferior(args)[2] if (m := re.match(r'\s*(?:shell\b|!)(.*)', c, re.S))]
    return []


def check_bash(command, cwd=None):
    cmds = list(simple_commands(command))
    # Every directory a later git may run in: the start, each cd, pushd and env -C target, and UNKNOWN after a
    # GIT_DIR or GIT_WORK_TREE. A cd in a subshell or a failed cd leaves the start in place, so all of them are
    # kept, not only the last. A nested shell starts from all of them.
    places = list(cwd) if isinstance(cwd, list) else [cwd or os.getcwd()]
    for wrappers, word, args in cmds:
        targets = [w[len('chdir:'):] for w in wrappers if w.startswith('chdir:')]
        if word in ('cd', 'pushd'):
            operands = [a for a in args if not a.startswith('-') or a == '-']
            targets += operands[:1] or ['~']
        if word == 'export' and any(a.startswith(('GIT_DIR=', 'GIT_WORK_TREE=')) for a in args):
            targets.append('$')
        places += [resolve(place, target) for target in targets for place in list(places)]
        places = list(dict.fromkeys(places))
        if len(places) > 16:  # many cds: the guard stops guessing
            places = places[:16] + [UNKNOWN]
        # After this command's own env -C or GIT_DIR, which the nested shell inherits
        commands = [(wrappers, word, args)] + list(argument_commands(wrappers, word, args))
        for script in (script for _, name, rest in commands for script in shell_scripts(name, rest)):
            if message := check_bash(script, list(places)):
                return message
        if word in ('pkill', 'killall'):
            return MSG_PKILL
        if word in ('sudo', 'doas') or 'sudo' in wrappers or 'doas' in wrappers:
            return MSG_SUDO
        if is_install(word, args, command):
            return MSG_INSTALL
        if any(drives(*c) for c in commands):
            return MSG_CTL
        if word == 'git':
            msg = check_git(args, places)
            if msg:
                return msg
    # kill $(pgrep -f ...), pgrep -f ... | xargs kill, a loop over pgrep -f
    pgrep_f = any(w == 'pgrep' and any(a == '--full' or 'f' in short_flags(a) for a in args) for _, w, args in cmds)
    if pgrep_f and any(w == 'kill' for _, w, _ in cmds):
        return MSG_PGREP_KILL
    return None


def check_edit(path):
    real = os.path.realpath(path)
    if real.startswith(PLASMAZONES):
        return (
            f'{path} is in PlasmaZones. Afterglow builds the Phosphor libraries from that checkout, but PlasmaZones '
            'changes only through its own branch and pull request. Tell the user what Phosphor needs and why.'
        )
    project = os.path.realpath(os.environ.get('CLAUDE_PROJECT_DIR', os.getcwd())) + '/'
    if not real.startswith(project):
        return None
    rel = real[len(project) :]
    if rel in GENERATED:
        return f'{rel} is generated. Change its source, then run: {GENERATED[rel]}'
    if BUILD_TREE.match(rel):
        return f'{rel} is in a build tree. Change the source or CMake code that makes it.'
    return None


def main():
    call = json.load(sys.stdin)
    tool = call.get('tool_name', '')
    params = call.get('tool_input', {})
    message = None
    if tool == 'Bash':
        message = check_bash(params.get('command', ''), call.get('cwd'))
    elif tool in ('Edit', 'Write', 'MultiEdit', 'NotebookEdit'):
        message = check_edit(params.get('file_path') or params.get('notebook_path') or '')
    if message:
        print(message, file=sys.stderr)
        sys.exit(2)


if __name__ == '__main__':
    main()
