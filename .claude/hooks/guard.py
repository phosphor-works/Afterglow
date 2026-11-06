#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 fuddlesworth
# SPDX-License-Identifier: GPL-3.0-or-later
"""PreToolUse guard for the hard rules of this project (CLAUDE.md).

Claude Code passes the tool call as JSON on stdin. Exit code 2 blocks the call and shows stderr to Claude.

A Bash command is read the way the shell reads it: split into simple commands at ; & | newlines, parentheses and
backticks outside quotes, each one tokenized with its quotes removed, and the wrappers in front of a command (then,
do, !, env, xargs, timeout, nohup, sudo, strace...) skipped. So `then pkill x` doesn't get past a rule, and a rule's
word inside a quoted string (grep 'a|sudo') doesn't trip it. Here-document bodies (commit messages) are text, not
commands, unless a shell reads them as its script. The $(...) and backquote commands in a body whose delimiter is
not quoted (<<EOF, not <<'EOF') are commands, because the shell runs them to expand the body. The scripts of nested
shells, eval, watch, script -c, tmux, ssh and a shell that reads standard input are read the same way, and so are the
commands in another command's arguments (find -exec, gdb --args, systemd-run).
"""

import json
import os
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
    'then do else elif if while until ! { } time nohup env exec command builtin xargs timeout nice ionice stdbuf '
    'setsid unbuffer sudo doas pkexec run0 busybox toybox dbus-run-session strace ltrace valgrind perf noglob '
    'nocorrect coproc'.split()
)
# The perf subcommands that run the command after them
PERF_COMMANDS = set('record stat trace ftrace c2c mem sched lock kmem kwork timechart kvm'.split())
# Shells whose -c runs its argument as a script
SHELLS = set('bash sh zsh dash ash ksh mksh yash fish'.split())
# Commands that start the command in their arguments with their own environment (find -exec, gdb --args). Any
# other (systemd-run, ssh, flatpak-spawn) may start it in an environment that the guard cannot see.
INHERITS = set('find gdb lldb rr flock chrt taskset catchsegv parallel'.split())
# Commands that never run their arguments: a word of theirs is text (echo aglow run, grep -r afterglow ctl)
TEXT_COMMANDS = set(
    'echo printf print grep egrep fgrep rg ugrep cat head tail less more wc ls stat file which whereis type whence '
    'man cut sort uniq tr diff cmp comm test [ [[ printenv basename dirname realpath readlink pgrep pidof ps strings '
    'nm ldd readelf objdump jq'.split()
)
# The commands in LAUNCHERS start one command from their arguments: (the operands before it, whether it gets this
# environment). Their options that take a value are in WRAPPER_VALUES. A systemd-run --scope runs in this
# environment, and a service in the one of the user's service manager.
LAUNCHERS = {
    'systemd-run': (0, False),
    'flatpak-spawn': (0, False),
    'flock': (1, True),
    'chrt': (1, True),
    'taskset': (1, True),
    'catchsegv': (0, True),
}
# The commands that kill processes by name
KILL_BY_NAME = set('pkill killall killall5 skill'.split())
# The commands that run a command as another user
PRIVILEGED = set('sudo sudoedit doas su pkexec run0'.split())
# gdb options that take a value, without their dashes: gdb takes -NAME and --NAME alike
GDB_VALUES = set(
    'p pid c core x command ex eval-command iex init-eval-command ix init-command d directory s symbols e exec se '
    'cd D data-directory tty b l i interpreter'.split()
)
# ssh options that take a value
SSH_VALUES = set('BbcDEeFIiJLlmOoPpQRSWw')
# util-linux script options that take a value, besides -c
SCRIPT_VALUES = set('EIOBTm')
# git subcommands that change neither the working tree nor a branch (fetch updates only the remote branches)
GIT_READS = set(
    'status log diff show rev-parse ls-files ls-tree grep blame describe shortlog cat-file rev-list merge-base '
    'name-rev for-each-ref show-ref whatchanged check-ignore check-attr var help version fetch ls-remote'.split()
)
# The install targets of the CMake generators (Ninja, Makefiles)
INSTALL_TARGET = re.compile(r'^install(/(strip|local|fast))?$')

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
    'pkexec': {'-u', '--user'},
    'run0': {'-u', '--user', '-g', '--group', '-D', '--chdir', '--setenv', '--unit', '--property', '--description',
             '--slice', '--nice', '--machine', '--background'},
    'systemd-run': {'-p', '--property', '-E', '--setenv', '-u', '--unit', '--description', '--slice', '-M',
                    '--machine', '-H', '--host', '--uid', '--gid', '--nice', '-s', '--service-type',
                    '--working-directory', '--path-property', '--socket-property', '--timer-property', '--on-active',
                    '--on-boot', '--on-startup', '--on-unit-active', '--on-unit-inactive', '--on-calendar',
                    '--on-timezone-change', '--on-clock-change', '--expand-environment', '--json'},
    'flock': {'-w', '--timeout', '--wait', '-E', '--conflict-exit-code'},
    'chrt': {'-T', '--sched-runtime', '-P', '--sched-period', '-D', '--sched-deadline'},
}
MSG_PKILL = (
    'pkill and killall match by name, and the user runs Afterglow as their terminal: `pkill afterglow` closes the '
    "user's own windows, and possibly this session. Use `pgrep -a NAME`, then `kill PID` on a build/ process only."
)
MSG_PGREP_KILL = (
    'kill with the PIDs that pgrep or pidof found can match the user\'s own Afterglow, and pgrep -f also matches the '
    'shell that runs the command, because its command line holds the pattern. List the processes with '
    '`pgrep -a NAME`. Then run `kill PID` with the numbers in a separate command.'
)
MSG_SUDO = ('Do not use sudo, su, doas, pkexec or run0. The user installs system packages. Tell the user what you '
            'need and why.')
MSG_INSTALL = ('Never install the build (cmake --install, the install target, cmake_install.cmake). The user '
               'installs Afterglow. Only an install into a temporary directory (--prefix /tmp/... or '
               'DESTDIR=/tmp/... on the command itself) is allowed.')
MSG_CTL = (
    "`afterglow ctl`, `aglow run` and `afterglow run` drive the user's running Afterglow: this session runs inside "
    "one of its panes, and they use the pane's $AFTERGLOW_SOCKET, else the socket of the user's Wayland display. A "
    'private D-Bus does not change that. Give them the socket of a test session, with the pane ID cleared '
    '(AFTERGLOW_SOCKET=/path/to/test.sock AFTERGLOW_PANE_ID=), or remove the pane variables and point them at '
    'another display '
    '(env -u AFTERGLOW_SOCKET -u AFTERGLOW_PANE_ID WAYLAND_DISPLAY=...). Ask the user before you drive their '
    'instance.'
)
MSG_LAUNCH = (
    "This starts Afterglow on the user's own Wayland display: it opens a window on the user's desktop, or hands the "
    "window to the user's running Afterglow through the socket of that display. Test in a nested KWin with its own "
    'display (the ag-verify-live skill), or name another display on the command: WAYLAND_DISPLAY=NAME or '
    'XDG_RUNTIME_DIR=/tmp/... before the program. --help, --version and --config-reference open no window.'
)
MSG_HEREDOC = ("The command is in a $(...) or backquotes in a here-document whose delimiter is not quoted, so the "
               "shell runs it. Quote the delimiter (<<'EOF') to keep the body as text.")


def plasmazones_message(path):
    return (f'{path} is in PlasmaZones. Afterglow builds the Phosphor libraries from that checkout, but PlasmaZones '
            'changes only through its own branch and pull request. Tell the user what Phosphor needs and why.')


HEREDOC = re.compile(r'<<(-?)[ \t]*((?:\'[^\']*\'|"[^"]*"|\\.|[^\s;&|<>()\'"\\])+)')


def unquote(word):
    """WORD with its quotes and backslashes removed, as the delimiter of a here-document is."""
    return re.sub(r'\'([^\']*)\'|"([^"]*)"|\\(.)', lambda m: next(g for g in m.groups() if g is not None), word)


DOUBLE_QUOTED = re.compile(r'"(?:[^"\\]|\\.)*"', re.S)
# The rest of a backquote substitution after its opening backquote: \` is a backquote inside it
BACKQUOTED = re.compile(r'(?:[^`\\]|\\.)*`', re.S)


def substitution_end(text, i):
    """The index after the ) that closes a $( whose text starts at I, or None when none does. Inside, the text is a
    command: quotes and backslashes hide a parenthesis, and a nested ( opens one more."""
    depth = 1
    while i < len(text):
        ch = text[i]
        if ch == '\\':
            i += 2
            continue
        if ch == "'":
            end = text.find("'", i + 1)
            if end < 0:
                return None
            i = end
        elif ch == '"':
            m = DOUBLE_QUOTED.match(text, i)
            if not m:
                return None
            i = m.end() - 1
        elif ch == '(':
            depth += 1
        elif ch == ')':
            depth -= 1
            if not depth:
                return i + 1
        i += 1
    return None


def body_substitutions(body):
    """The commands of the $(...) and backquote substitutions in the body of a here-document whose delimiter is not
    quoted. The shell expands them when the command runs. Quotes are text in such a body, and a backslash hides only
    $, ` and \\. A substitution that does not end is an error in the shell, which then runs nothing."""
    commands, i = [], 0
    while i < len(body):
        ch = body[i]
        if ch == '\\':
            i += 2
            continue
        if body.startswith('$(', i):
            end = substitution_end(body, i + 2)
            if end is None:
                break
            commands.append(body[i + 2 : end - 1])
            i = end
            continue
        if ch == '`':
            m = BACKQUOTED.match(body, i + 1)
            if not m:
                break
            commands.append(m.group(0)[:-1].replace('\\`', '`'))
            i = m.end()
            continue
        i += 1
    return commands


def split_heredocs(command):
    """(the command with the bodies of its here-documents left out, the bodies, the commands that the shell runs to
    expand them). A << inside quotes or a comment, and a here-string (<<<), starts none. The quotes start again
    inside $(...) and backquotes, as in the commit message of git commit -m "$(cat <<'EOF' ... EOF)". The bodies
    follow the line of their operators, in order. A body whose delimiter has no quote or backslash expands its
    $(...) and backquotes, so their commands run: git commit -F - <<EOF with `afterglow ctl list` in the message
    runs that command. A body with a quoted delimiter (<<'EOF', <<"EOF", <<\\EOF) is text only."""
    out, bodies, commands, pending, stack = [], [], [], [], ['']
    i, start, word_start, n = 0, 0, True, len(command)
    while i < n:
        ch, context = command[i], stack[-1]
        if ch == '\\':
            i, word_start = i + 2, False
            continue
        if context == '"':
            if ch == '"':
                stack.pop()
            elif command.startswith('$(', i):
                stack.append(')')
                i += 1
            elif ch == '`':
                stack.append('`')
            i += 1
            continue
        if ch == "'":
            end = command.find("'", i + 1)
            i, word_start = (n if end < 0 else end + 1), False
            continue
        if ch == '#' and word_start:
            end = command.find('\n', i)
            i = n if end < 0 else end
            continue
        if ch == '"':
            stack.append('"')
        elif command.startswith('$(', i) or ch == '(':
            stack.append(')')
            i += ch == '$'
        elif ch == ')' and context == ')':
            stack.pop()
        elif ch == '`':
            stack.pop() if context == '`' else stack.append('`')
        elif command.startswith('<<<', i):
            i += 2
        elif (m := HEREDOC.match(command, i)) is not None:
            pending.append((unquote(m.group(2)), any(q in m.group(2) for q in '\'"\\')))
            i, word_start = m.end(), False
            continue
        elif ch == '\n' and pending:
            pos = i + 1
            for delimiter, quoted in pending:
                end = re.compile(r'^[ \t]*%s[ \t]*$' % re.escape(delimiter), re.M).search(command, pos)
                if not end:  # no end: the rest stays in the command, where every rule reads it
                    break
                out.append(command[start:pos])
                bodies.append(command[pos:end.start()])
                if not quoted:
                    commands += body_substitutions(bodies[-1])
                start = pos = end.end()
            pending, i, word_start = [], pos, True
            continue
        word_start = ch in ' \t\n;&|()<>'
        i += 1
    out.append(command[start:])
    return ''.join(out), bodies, commands


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
    """(wrappers, command word, arguments) for every simple command in a command line whose here-documents are left
    out, including the ones in a command substitution. A substitution inside single quotes or a comment is text, as
    in the shell."""
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
                elif name == 'command' and ('v' in before or 'V' in before):
                    # command -v NAME looks NAME up and runs nothing
                    return wrappers[:-1], 'command', words[i:]
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


def git_reads(args):
    """Whether git ARGS only reads: a subcommand of GIT_READS, or a listing (git branch -a, stash list...)."""
    sub, rest = git_subcommand(args)
    if sub in GIT_READS:
        return True
    if sub in ('branch', 'tag'):
        return all(a in ('-a', '--all', '-r', '--remotes', '-v', '-vv', '--verbose', '-l', '--list',
                         '--show-current', '--no-color') for a in rest)
    first = rest[:1]
    if sub == 'remote':
        return first in ([], ['-v'], ['--verbose'], ['show'], ['get-url'])
    if sub in ('stash', 'worktree'):
        return first == ['list'] or (sub == 'stash' and first == ['show'])
    if sub == 'config':
        return any(a in ('--get', '--get-all', '--get-regexp', '-l', '--list', 'get', 'list') for a in rest)
    return False


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


def in_plasmazones(path, places):
    """Whether PATH, read from any of PLACES, is in the PlasmaZones checkout. A path that the shell expands is not
    known here."""
    if not path or '$' in path or '`' in path:
        return False
    path = os.path.expanduser(path)
    bases = ['/'] if path.startswith('/') else [p for p in places if p is not UNKNOWN]
    return any((os.path.realpath(os.path.join(base, path)) + '/').startswith(PLASMAZONES) for base in bases)


def operands(args):
    """The words of ARGS that are not options, and the targets of their redirections."""
    words, targets, k, ended = [], [], 0, False
    while k < len(args):
        a = args[k]
        redirect = re.match(r'^(\d*|&)(>>?\|?|<>?)(.*)$', a)
        if redirect and not ended:
            target = redirect.group(3) or (args[k + 1] if k + 1 < len(args) else '')
            (targets if '>' in redirect.group(2) or '<>' == redirect.group(2) else []).append(target)
            k += 1 if redirect.group(3) else 2
            continue
        if a == '--' and not ended:
            ended = True
        elif ended or not a.startswith('-') or a == '-':
            words.append(a)
        k += 1
    return words, targets


def edits_plasmazones(word, args, places):
    """The path in PlasmaZones that WORD ARGS writes, or None: a redirection, sed -i, a file tool, or a git command
    in that checkout that is not a read."""
    words, targets = operands(args)
    paths = list(targets)
    if word in ('tee', 'touch', 'rm', 'rmdir', 'mkdir', 'truncate', 'chmod', 'chown', 'chgrp', 'shred', 'mv',
                'unlink', 'patch'):
        paths += words
    elif word in ('cp', 'ln', 'rsync', 'install'):
        paths += words[-1:]
        paths += [a.split('=', 1)[1] for a in args if a.startswith('--target-directory=')]
        paths += [args[k + 1] for k, a in enumerate(args[:-1]) if a == '-t']
    elif word in ('sed', 'perl') and any(a.startswith(('-i', '--in-place')) or 'i' in short_flags(a) for a in args):
        paths += words
    for path in paths:
        if in_plasmazones(path, places):
            return path
    if word == 'patch' and in_plasmazones('.', places):
        return PLASMAZONES.rstrip('/')
    if word == 'git' and not git_reads(args):
        for place in places:
            directory = git_directory(args, place)
            if directory is not UNKNOWN and in_plasmazones(directory, ['/']):
                return directory
    return None


def under_tmp(path):
    """An absolute path, written out, under /tmp. A relative path, ~ or $VAR is resolved by the shell, in a place
    the guard does not know."""
    return isinstance(path, str) and path.startswith('/') and os.path.realpath(path).startswith('/tmp/')


def is_install(wrappers, word, args):
    """Whether WORD ARGS installs the build outside a temporary directory: cmake --install, an install target of
    cmake --build, ninja or make, or cmake -P cmake_install.cmake. Only the DESTDIR of this command counts: its
    VAR=value or env words, the session's environment, and make's DESTDIR= argument."""
    temporary = under_tmp(environment(wrappers).get('DESTDIR'))
    if word == 'cmake':
        if '--install' in args:
            prefix = None
            for k, a in enumerate(args):
                if a == '--prefix' and k + 1 < len(args):
                    prefix = args[k + 1]
                elif a.startswith('--prefix='):
                    prefix = a.split('=', 1)[1]
            return not (temporary or under_tmp(prefix))
        if '-P' in args:
            script = args[args.index('-P') + 1] if args.index('-P') + 1 < len(args) else ''
            if os.path.basename(script) != 'cmake_install.cmake':
                return False
            prefix = next((m.group(2) for a in args if (m := re.match(r'^-DCMAKE_INSTALL_PREFIX(:\w+)?=(.*)$', a))),
                          None)
            return not (temporary or under_tmp(prefix))
        targets, k = [], 0
        while k < len(args):
            a = args[k]
            if a == '--':  # the arguments of the build tool
                targets += args[k + 1 :]
                break
            if a in ('--target', '-t'):
                k += 1
                while k < len(args) and not args[k].startswith('-'):
                    targets.append(args[k])
                    k += 1
                continue
            if a.startswith('--target='):
                targets.append(a.split('=', 1)[1])
            k += 1
        return '--build' in args and any(INSTALL_TARGET.match(t) for t in targets) and not temporary
    if word in ('ninja', 'make', 'gmake', 'samu'):
        temporary |= any(a.startswith('DESTDIR=') and under_tmp(a[len('DESTDIR='):]) for a in args)
        return any(INSTALL_TARGET.match(a) for a in args) and not temporary
    return False


def display_socket_name(display, runtime):
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


# The variables that decide where a client or a launch of Afterglow connects
CLIENT_VARIABLES = ('AFTERGLOW_SOCKET', 'AFTERGLOW_PANE_ID', 'WAYLAND_DISPLAY', 'XDG_RUNTIME_DIR')


def environment(wrappers):
    """The environment of a command: its VAR=value and env -u words apply in order over this environment. A value
    that the shell expands (an expansion, a brace, a glob or ~) is UNKNOWN. After 'inherit:?' the command runs in an
    environment the guard cannot see, so each of CLIENT_VARIABLES is UNKNOWN until a later word sets or unsets it."""
    env = dict(os.environ)
    for w in wrappers:
        if w == 'clear':
            env.clear()
        elif w == 'inherit:?':
            env = dict.fromkeys(CLIENT_VARIABLES, UNKNOWN)
        elif w.startswith('set:'):
            name, value = w[len('set:'):].split('=', 1)
            env[name] = UNKNOWN if any(mark in value for mark in '$`{*?[~') else value
        elif w.startswith('unset:'):
            env.pop(w[len('unset:'):], None)
    return env


def isolated_client(wrappers):
    """Whether one afterglow ctl, aglow run or afterglow run command reaches a socket other than the user's. Each
    picks its socket as AfterglowProto::findSocket does: AFTERGLOW_SOCKET, else the main socket of WAYLAND_DISPLAY
    in XDG_RUNTIME_DIR. Only literal values count: an expansion can name the user's own socket."""
    env = environment(wrappers)

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
            return True  # findSocket refuses a pane ID without its socket
        elif known('XDG_RUNTIME_DIR'):
            target = os.path.join(env['XDG_RUNTIME_DIR'],
                                  display_socket_name(known('WAYLAND_DISPLAY'), env['XDG_RUNTIME_DIR']))
        else:
            return True  # findSocket finds no socket
    except LookupError:
        return False
    target = os.path.realpath(target)
    runtime = os.environ.get('XDG_RUNTIME_DIR')
    if os.environ.get('AFTERGLOW_SOCKET') and target == os.path.realpath(os.environ['AFTERGLOW_SOCKET']):
        return False
    if runtime and os.path.dirname(target) == os.path.realpath(runtime):
        # The main socket of the user's display, or a separate process's on it.
        own = display_socket_name(os.environ.get('WAYLAND_DISPLAY'), runtime)[: -len('.sock')]
        if re.fullmatch(re.escape(own) + r'(-\d+)?\.sock', os.path.basename(target)):
            return False
    return True


def wayland_socket(runtime, display):
    """The compositor socket that a Wayland client connects to: WAYLAND_DISPLAY ("wayland-0" when it is empty) in
    XDG_RUNTIME_DIR, or WAYLAND_DISPLAY itself when it is absolute. None without an absolute XDG_RUNTIME_DIR."""
    display = display or 'wayland-0'
    if display.startswith('/'):
        return os.path.realpath(display)
    return os.path.realpath(os.path.join(runtime, display)) if runtime and runtime.startswith('/') else None


def isolated_launch(wrappers):
    """Whether a launch of afterglow reaches a display other than the user's. Its window goes to the compositor of
    WAYLAND_DISPLAY in XDG_RUNTIME_DIR, and the launch hands it over to the main Afterglow on the socket of the same
    display there (main.cpp). Only literal values count, as in isolated_client."""
    env = environment(wrappers)
    runtime, display = env.get('XDG_RUNTIME_DIR'), env.get('WAYLAND_DISPLAY')
    if runtime is UNKNOWN or display is UNKNOWN or (runtime and not runtime.startswith('/')):
        return False
    target = wayland_socket(runtime, display)
    return target is None or target != wayland_socket(os.environ.get('XDG_RUNTIME_DIR'),
                                                      os.environ.get('WAYLAND_DISPLAY'))


def drives_instance(args, program='afterglow', appended=False):
    """Whether PROGRAM ARGS connects to a running Afterglow: afterglow ctl, afterglow run and aglow run do, also
    run --complete. Their help, as afterglow/src/control/Ctl.cpp and libs/afterglow-proto/src/Run.cpp read it,
    prints the usage and connects to nothing: ctl's as its first word, and run's among the options before the
    script. So does afterglow ctl with no command, unless APPENDED: xargs and parallel add the words of their input
    after ARGS (echo list | xargs afterglow ctl), and those words come after any help option."""
    if args[:1] not in ((['ctl'], ['run']) if program == 'afterglow' else (['run'],)):
        return False
    if args[:1] == ['ctl']:
        return args[1:2] not in (['-h'], ['--help']) and (len(args) > 1 or appended)
    k = 1
    while k < len(args) and args[k].startswith('-') and args[k] != '--':
        if args[k] in ('-h', '--help'):
            return False
        k += 2 if args[k] == '--pane' else 1
    return True


# afterglow's options (main.cpp): the ones that take a value, and the ones that end the process before a window,
# with the help and the version of QCommandLineParser
LAUNCH_VALUES = {'-d', '--working-directory', '--title', '--config', '-o', '--override', '--class'}
NO_WINDOW = {'-h', '--help', '-?', '--help-all', '-v', '--version', '--config-reference'}
# The words after afterglow that run something else than the terminal: ctl, ssh and the aglow commands
# (Aglow::Commands in afterglow/src/shell/Aglow.h)
NO_TERMINAL = {'ctl', 'ssh', 'run', 'transfer', 'dnd', 'icat', 'clipboard'}


def launches_window(args):
    """Whether afterglow ARGS starts the terminal, as main.cpp and QCommandLineParser read the arguments: options
    up to the first positional argument or "--", short options together (-hv), and a value as the rest of the word
    (-dDIR, --title=T) or the next word."""
    if args[:1] and args[0] in NO_TERMINAL:
        return False
    k = 0
    while k < len(args):
        a = args[k]
        if a == '--' or a == '-' or not a.startswith('-'):
            break
        name = a.split('=', 1)[0]
        if name in NO_WINDOW:
            return False
        if a.startswith('--'):
            k += 1 + (name in LAUNCH_VALUES and '=' not in a)
            continue
        for j, letter in enumerate(a[1:]):
            if '-' + letter in NO_WINDOW:
                return False
            if '-' + letter in LAUNCH_VALUES:
                k += j + 2 == len(a)  # the value is the next word
                break
        k += 1
    return True


def argument_commands(wrappers, word, args):
    """(wrappers, command word, arguments) for each command that may run from the arguments of WORD: find -exec,
    gdb --args, systemd-run, a debugger or a tracer the guard does not list. Only afterglow, aglow, a wrapper, a
    shell, eval, watch, gdb or VAR=value starts one, so that an argument such as -name stays an argument. A command
    of TEXT_COMMANDS runs none, and find only after -exec, -execdir, -ok or -okdir."""
    if word in TEXT_COMMANDS:
        return
    if word == 'find':
        start = next((k for k, a in enumerate(args) if a in ('-exec', '-execdir', '-ok', '-okdir')), len(args))
        args = args[start + 1 :]
    outer = wrappers if word in INHERITS else wrappers + ['inherit:?']
    if word == 'parallel':  # parallel adds the words of its input, or of its ::: lists, after the command
        outer = outer + ['append']
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


def launched_commands(wrappers, word, args):
    """(wrappers, command word, arguments) of the command that WORD starts from its arguments: the one after the
    options and operands of a command in LAUNCHERS, with the environment it gets, or each command of find -exec."""
    if word == 'find':
        yield from argument_commands(wrappers, word, args)
        return
    if word not in LAUNCHERS:
        return
    before, inherits = LAUNCHERS[word]
    sets, k = [], 0
    while k < len(args) and args[k].startswith('-') and args[k] != '-':
        if args[k] == '--':
            k += 1
            break
        a = args[k]
        flag, letters, value, after = option_value(word, args, k)
        if word == 'systemd-run' and a == '--scope':
            inherits = True
        elif word == 'systemd-run' and flag in ('-E', '--setenv'):
            # -E NAME passes systemd-run's own value
            sets.append('set:' + value if '=' in value else
                        ('set:%s=%s' % (value, os.environ[value]) if value in os.environ else 'unset:' + value))
        elif word == 'flatpak-spawn' and a.startswith('--env='):
            sets.append('set:' + a[len('--env='):])
        elif word == 'flatpak-spawn' and a == '--clear-env':
            sets.insert(0, 'clear')
        elif word == 'taskset' and ('p' in letters or a == '--pid'):
            return  # taskset -p changes a running process
        k = after
    k += before
    if k < len(args):
        inner, name, rest = split_wrappers(args[k:])
        yield (wrappers if inherits else wrappers + ['inherit:?']) + sets + inner, name, rest


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


def gdb_program(args):
    """(the program of gdb ARGS without its directory, its arguments, the wrappers that its environment adds, whether
    gdb runs it, whether a command file runs). A gdb command (-ex 'run ctl list', set args) gives the program its
    arguments, and set environment changes its environment. A command file can do any of these."""
    program, arguments, commands, files = gdb_inferior(args)
    extra, runs = [], files
    for words in map(split_words, commands):
        if words[:1] in (['r'], ['run'], ['start'], ['starti']):
            runs = True
            if len(words) > 1:
                arguments = words[1:]
        elif words[:2] == ['set', 'args']:
            arguments = words[2:]
        elif words[:1] in (['set'], ['unset']) and words[1:2] and words[1].startswith('env'):
            extra = ['inherit:?']
    if files:
        extra = ['inherit:?']
    return (os.path.basename(program) if program else None), arguments, extra, runs, files


def gdb_drives(wrappers, args):
    """Whether gdb ARGS runs an afterglow ctl, afterglow run or aglow run that reaches the user's socket."""
    program, arguments, extra, _, files = gdb_program(args)
    if program not in ('afterglow', 'aglow'):
        return False
    return (drives_instance(['run'] if files else arguments, program, appends(wrappers))
            and not isolated_client(wrappers + extra))


def gdb_launches(wrappers, args):
    """Whether gdb ARGS runs afterglow as the terminal on the user's display. Reading a core (gdb afterglow core)
    runs nothing."""
    program, arguments, extra, runs, files = gdb_program(args)
    return (program == 'afterglow' and runs and launches_window([] if files else arguments)
            and not isolated_launch(wrappers + extra))


def appends(wrappers):
    """Whether xargs or parallel starts the command, and so adds words of its input after its arguments."""
    return 'xargs' in wrappers or 'append' in wrappers


def drives(wrappers, word, args):
    if word in ('afterglow', 'aglow'):
        return drives_instance(args, word, appends(wrappers)) and not isolated_client(wrappers)
    if '$' in word or '`' in word:  # the shell finds the program ($(which afterglow) ctl): it can be either
        return drives_instance(args, appended=appends(wrappers)) and not isolated_client(wrappers)
    return word == 'gdb' and gdb_drives(wrappers, args)


def opens_window(wrappers, word, args):
    return word == 'afterglow' and launches_window(args) and not isolated_launch(wrappers)


def ssh_command(args):
    """The command that ssh ARGS runs on the other computer, or None: the words after the destination."""
    k = 0
    while k < len(args):
        a = args[k]
        if a == '--':
            k += 1
            break
        if not a.startswith('-') or a == '-':
            break
        letter = next((ch for ch in a[1:] if ch in SSH_VALUES), None)
        k += 2 if letter and a.endswith(letter) else 1
    return ' '.join(args[k + 1 :]) if k + 1 < len(args) else None


def shell_scripts(word, args):
    """The scripts that a nested shell, eval, watch, script -c, tmux, ssh or a gdb shell command runs. Every word
    after a shell's -c is checked: bash takes options after -c, and fish takes several -c and -C scripts. Each word
    of tmux can be a shell command (new-session, send-keys, run-shell). flock -c runs its command with sh -c."""
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
    if word == 'script':  # script -c CMD, as -qc CMD or --command CMD
        for k, a in enumerate(args):
            if a.startswith('--command='):
                return [a.split('=', 1)[1]]
            if a == '--command' or (a.startswith('-') and not a.startswith('--')):
                letters = a[1:] if a != '--command' else 'c'
                for j, letter in enumerate(letters):
                    if letter == 'c':
                        rest = letters[j + 1 :]
                        return [rest] if rest else args[k + 1 : k + 2]
                    if letter in SCRIPT_VALUES:
                        break
        return []
    if word == 'flock':
        return [args[k + 1] for k, a in enumerate(args[:-1]) if a in ('-c', '--command')]
    if word == 'tmux':
        return list(args)
    if word == 'ssh' or (word == 'afterglow' and args[:1] == ['ssh']):
        remote = ssh_command(args if word == 'ssh' else args[1:])
        return [remote] if remote else []
    if word == 'gdb':
        return [m.group(1) for c in gdb_inferior(args)[2] if (m := re.match(r'\s*(?:shell\b|!)(.*)', c, re.S))]
    return []


def reads_stdin(word, args):
    """Whether shell WORD ARGS reads its script from standard input: no -c script and no script file (bash, bash -s,
    sh -)."""
    if word not in SHELLS or shell_scripts(word, args):
        return False
    k = 0
    while k < len(args):
        a = args[k]
        if redirect := re.match(r'^\d*(<<<|<<-?|<>?|>>?|&>>?|[<>]&)', a):  # a redirection and its target
            k += 1 if len(a) > redirect.end() else 2
            continue
        if a in ('-', '--') or 's' in short_flags(a):
            return True
        if a in ('-o', '+o', '-O', '+O', '--rcfile', '--init-file'):
            k += 2
            continue
        if not a.startswith(('-', '+')):
            return False
        k += 1
    return True


def stdin_scripts(cmds, bodies, places):
    """The scripts that a shell reads from standard input in this command line, when one does: the bodies of the
    here-documents, the here-strings, a file after <, and the words of echo and printf, which a pipe can feed it."""
    shells = [(word, args) for _, word, args in cmds if reads_stdin(word, args)]
    if not shells:
        return []
    scripts = list(bodies)
    for word, args in shells:
        for k, a in enumerate(args):
            if a in ('<<<', '<') and k + 1 < len(args):
                target = args[k + 1]
            elif a.startswith('<<<') or (a.startswith('<') and not a.startswith(('<<', '<>', '<&'))):
                target = a.lstrip('<')
            else:
                continue
            if a.startswith('<<<'):
                scripts.append(target)
                continue
            for place in places:
                path = '' if place is UNKNOWN or '$' in target else os.path.join(place, os.path.expanduser(target))
                # A regular file only: a FIFO or a terminal (/dev/tty) would block the guard, or take its input
                if path and os.path.isfile(path):
                    try:
                        with open(path, errors='replace') as f:
                            scripts.append(f.read(1 << 20))
                        break
                    except OSError:
                        pass
    for _, word, args in cmds:
        if word in ('echo', 'printf'):
            scripts.append(' '.join(args).replace('\\n', '\n'))
    return scripts


def check_bash(command, cwd=None):
    text, bodies, expansions = split_heredocs(command)
    cmds = list(simple_commands(text))
    # Every directory a later git may run in: the start, each cd, pushd and env -C target, and UNKNOWN after a
    # GIT_DIR or GIT_WORK_TREE. A cd in a subshell or a failed cd leaves the start in place, so all of them are
    # kept, not only the last. A nested shell starts from all of them.
    places = list(cwd) if isinstance(cwd, list) else [cwd or os.getcwd()]
    for wrappers, word, args in cmds:
        targets = [w[len('chdir:'):] for w in wrappers if w.startswith('chdir:')]
        if word in ('cd', 'pushd'):
            operands_ = [a for a in args if not a.startswith('-') or a == '-']
            targets += operands_[:1] or ['~']
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
        if word in KILL_BY_NAME:
            return MSG_PKILL
        if word in PRIVILEGED or PRIVILEGED & set(wrappers):
            return MSG_SUDO
        if is_install(wrappers, word, args):
            return MSG_INSTALL
        if any(drives(*c) for c in commands):
            return MSG_CTL
        launches = [(wrappers, word, args)] + list(launched_commands(wrappers, word, args))
        if any(opens_window(*c) for c in launches) or (word == 'gdb' and gdb_launches(wrappers, args)):
            return MSG_LAUNCH
        if path := edits_plasmazones(word, args, places):
            return plasmazones_message(path)
        if word == 'git':
            msg = check_git(args, places)
            if msg:
                return msg
    # Here-document commands run where the shell is when it reaches them, after any cd before them: check
    # them against every directory of the line, not only the start.
    for script in expansions:
        if message := check_bash(script, list(places)):
            return message + ' ' + MSG_HEREDOC
    for script in stdin_scripts(cmds, bodies, places):
        if message := check_bash(script, list(places)):
            return message
    # kill $(pgrep ...), pgrep ... | xargs kill, a loop over pgrep -f: kill with PIDs that a name matched
    finders = {w for _, w, a in cmds if w in ('pgrep', 'pidof', 'ps')}
    pgrep_f = any(w == 'pgrep' and any(a == '--full' or 'f' in short_flags(a) for a in args) for _, w, args in cmds)
    for wrappers, word, args in cmds:
        # parallel kill takes its PIDs from its input, as xargs kill does
        if word == 'parallel' and 'kill' in (os.path.basename(a) for a in args):
            word, wrappers = 'kill', wrappers + ['append']
        if word != 'kill':
            continue
        found = appends(wrappers) and finders or any('$' in a for a in args) and finders - {'ps'}
        if pgrep_f or found:
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
