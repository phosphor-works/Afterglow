#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 fuddlesworth
# SPDX-License-Identifier: GPL-3.0-or-later
"""PreToolUse guard for the hard rules of this project (CLAUDE.md).

Claude Code passes the tool call as JSON on stdin. Exit code 2 blocks the call and shows stderr to Claude.

A Bash command is read the way the shell reads it: split into simple commands at ; & | newlines, parentheses and
backticks outside quotes, each one tokenized with its quotes removed, and the wrappers in front of a command (then,
do, !, env, xargs, timeout, nohup, sudo...) skipped. So `then pkill x` doesn't get past a rule, and a rule's word
inside a quoted string (grep 'a|sudo') doesn't trip it. Here-document bodies (commit messages) are text, not commands.
"""

import json
import os
import re
import shlex
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
PLASMAZONES = os.path.realpath(os.path.expanduser('~/Projects/PlasmaZones')) + '/'

SEPARATORS = ';&|()\n`'
# Words that run the command after them: skipped, with their options, to find the command itself
WRAPPERS = set(
    'then do else elif if while until ! { } time nohup env exec command builtin xargs timeout nice ionice stdbuf setsid '
    'unbuffer sudo doas'.split()
)

MSG_LOCAL = (
    '.Codex/, __pycache__/, build*/ and .cache/ are local state and never go into git, and `git add -f` would '
    'bypass .gitignore. Stage only source files, by name.'
)
MSG_BROAD = 'Stage files by name (git add path...), not with -A, -u, ., *, :/ or commit -a.'
MSG_PUSH = 'Never force-push. Fix the branch with a new commit instead.'
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
    "`afterglow ctl` drives the user's running Afterglow, and this session runs inside one of its panes "
    '($AFTERGLOW_PANE_ID), so it acts on the user\'s own terminal. Run it only inside the private bus of a test '
    'session (dbus-run-session, or DBUS_SESSION_BUS_ADDRESS=...). Ask the user before you drive their instance.'
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
    try:
        return list(lex)
    except ValueError:  # unbalanced quotes: split at whitespace and separators, quotes dropped
        return re.findall(r'[%s]+|[^\s%s]+' % (re.escape(SEPARATORS), re.escape(SEPARATORS)), command.replace('"', ''))


def simple_commands(command):
    """(wrappers, command word, arguments) for every simple command in a command line, including the ones in a
    command substitution inside double quotes."""
    command = strip_heredocs(command)
    lines = [command] + re.findall(r'\$\(([^()]*)\)', command) + re.findall(r'`([^`]*)`', command)
    for line in lines:
        words = []
        for tok in tokens(line) + [';']:
            if tok and all(ch in SEPARATORS for ch in tok):
                if words:
                    yield split_wrappers(words)
                words = []
            else:
                words.append(tok)


def split_wrappers(words):
    wrappers, i = [], 0
    while i < len(words):
        w = words[i]
        name = os.path.basename(w)
        if re.match(r'^[A-Za-z_][A-Za-z0-9_]*=', w):  # VAR=value before a command
            i += 1
        elif name in WRAPPERS:
            wrappers.append(name)
            i += 1
            # the wrapper's options, and timeout's duration
            while i < len(words) and (words[i].startswith('-') or re.match(r'^[\d.]+[smhd]?$', words[i])):
                i += 1
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


def check_git(args):
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
    return None


def temp_destination(command, args):
    """An install into a temporary directory (--prefix or DESTDIR under /tmp) is allowed: it never touches the system."""
    prefix = None
    for k, a in enumerate(args):
        if a == '--prefix' and k + 1 < len(args):
            prefix = args[k + 1]
        elif a.startswith('--prefix='):
            prefix = a.split('=', 1)[1]
    if prefix is not None:
        return os.path.realpath(prefix).startswith('/tmp/')
    match = re.search(r'(?:^|\s)DESTDIR=(\S+)', command)
    return bool(match) and os.path.realpath(match.group(1).strip('\'"')).startswith('/tmp/')


def is_install(word, args, command=''):
    if word == 'cmake':
        if '--install' in args:
            return not temp_destination(command, args)
        targets = [args[k + 1] for k, a in enumerate(args[:-1]) if a in ('--target', '-t')]
        return 'install' in targets or any(a.startswith('--target=install') for a in args)
    return word in ('ninja', 'make') and 'install' in args


def private_bus(command, cmds):
    """Whether the command line gives afterglow ctl a bus other than the user's session bus."""
    return any(w == 'dbus-run-session' for _, w, _ in cmds) or 'DBUS_SESSION_BUS_ADDRESS=' in command


def check_bash(command):
    cmds = list(simple_commands(command))
    for wrappers, word, args in cmds:
        if word in ('pkill', 'killall'):
            return MSG_PKILL
        if word in ('sudo', 'doas') or 'sudo' in wrappers or 'doas' in wrappers:
            return MSG_SUDO
        if is_install(word, args, command):
            return MSG_INSTALL
        if word == 'afterglow' and args[:1] == ['ctl'] and not private_bus(command, cmds):
            return MSG_CTL
        if word == 'git':
            msg = check_git(args)
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
        message = check_bash(params.get('command', ''))
    elif tool in ('Edit', 'Write', 'MultiEdit', 'NotebookEdit'):
        message = check_edit(params.get('file_path') or params.get('notebook_path') or '')
    if message:
        print(message, file=sys.stderr)
        sys.exit(2)


if __name__ == '__main__':
    main()
