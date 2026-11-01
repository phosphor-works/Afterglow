#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 fuddlesworth
# SPDX-License-Identifier: GPL-3.0-or-later
"""Generate the settings reference from the executable's runtime schema.
Usage: python3 tools/generate-reference.py build/afterglow/afterglow
"""
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
if len(sys.argv) != 2:
    raise SystemExit('Usage: python3 tools/generate-reference.py build/afterglow/afterglow')
repo = Path(__file__).resolve().parents[1]
reference = json.loads(subprocess.check_output([str(Path(sys.argv[1]).resolve()), '--config-reference'],
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
         'Numeric values are clamped to the listed ranges. An integer key drops the fraction of a value, and uses its default for a value outside the 32-bit range. Invalid enum choices use the default. Where an empty string has a special meaning, the description says so. Omitted values use defaults.', '']
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
# Replace the reference in one step, so that a failed run leaves the old one.
target = repo / 'docs/CONFIGURATION.md'
with tempfile.NamedTemporaryFile('w', encoding='utf-8', dir=target.parent, prefix='.CONFIGURATION.', delete=False) as out:
    out.write('\n'.join(lines) + '\n')
os.chmod(out.name, 0o644)
os.replace(out.name, target)
