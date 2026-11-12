#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 fuddlesworth
# SPDX-License-Identifier: GPL-3.0-or-later
"""Generate the settings reference from the executable's runtime schema.
Usage: python3 tools/generate-reference.py [--check] build/afterglow/afterglow
With --check, write nothing: print a diff and exit 1 when docs/CONFIGURATION.md
is not what the executable generates.
"""
import difflib
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
USAGE = 'Usage: python3 tools/generate-reference.py [--check] build/afterglow/afterglow'
args = sys.argv[1:]
check = bool(args) and args[0] == '--check'
if check: args = args[1:]
if len(args) != 1 or args[0].startswith('-'):
    raise SystemExit(USAGE)
repo = Path(__file__).resolve().parents[1]
reference = json.loads(subprocess.check_output([str(Path(args[0]).resolve()), '--config-reference'],
                                               text=True, encoding='utf-8', timeout=30))
def code(value):
    text = json.dumps(value, ensure_ascii=False).replace('|', '\\|')
    # Character references are literal inside a code span, so a backtick needs
    # a longer fence, padded so that an edge backtick is not part of it.
    return f'`` {text} ``' if '`' in text else f'`{text}`'
def range_of(entry):
    low, high = entry.get('minimum'), entry.get('maximum')
    if low is not None and high is not None: return f'{low}–{high}'
    if low is not None: return f'{low} or more'
    if high is not None: return f'{high} or less'
    return ', '.join(code(x) for x in entry.get('choices', []))
lines = ['<!-- SPDX-FileCopyrightText: 2026 fuddlesworth -->', '<!-- SPDX-License-Identifier: GPL-3.0-or-later -->', '', '# Configuration reference', '',
         '`tools/generate-reference.py` generates this document from the runtime Phosphor schema. Group and key names are case-sensitive. See [appearance](APPEARANCE.md) for configuration loading and examples.', '',
         'Afterglow clamps a number to the listed range. The description of Font.size gives 0 or less a meaning of its own. An integer key drops the fraction of a number, so 6.7 is 6. It uses its default for a value outside the 32-bit range and for text that is not a whole number. Invalid enum choices use the default. Where an empty string has a special meaning, the description says so. Omitted values use defaults.', '',
         'A color in `Colors` has one of the forms of [color values](APPEARANCE.md#color-values), such as `#1e1e2e`, `red` or `oklch(0.7 0.1 200)`.', '']
for group, keys in reference['groups'].items():
    lines += ['## ' + group, '', '| Key | Default | Range or choices | Meaning |', '| --- | --- | --- | --- |']
    for name, entry in keys.items():
        default = entry['default']
        constraint = range_of(entry)
        description = entry['description'].replace('|', '\\|').replace('\n', ' ')
        if group == 'Keybinds' and name == 'bindings':
            shown = '[default bindings](#default-bindings)'
        else: shown = code(default)
        lines.append(f'| `{name}` | {shown} | {constraint} | {description} |')
    lines += ['']
lines += ['## Default bindings', '', '| Action | Keys |', '| --- | --- |']
for name, value in reference['groups']['Keybinds']['bindings']['default'].items():
    lines.append(f'| `{name}` | {code(value)} |')
text = '\n'.join(lines) + '\n'
target = repo / 'docs/CONFIGURATION.md'
if check:
    current = target.read_text(encoding='utf-8') if target.exists() else ''
    if current == text: sys.exit(0)
    sys.stdout.writelines(difflib.unified_diff(current.splitlines(keepends=True), text.splitlines(keepends=True),
                                               'docs/CONFIGURATION.md', 'generated'))
    print('docs/CONFIGURATION.md is out of date. Run: python3 tools/generate-reference.py build/afterglow/afterglow')
    sys.exit(1)
# Replace the reference in one step, so that a failed run leaves the old one.
with tempfile.NamedTemporaryFile('w', encoding='utf-8', dir=target.parent, prefix='.CONFIGURATION.', delete=False) as out:
    out.write(text)
os.chmod(out.name, 0o644)
os.replace(out.name, target)
