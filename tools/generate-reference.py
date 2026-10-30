#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Generate the settings reference from the executable's runtime schema.
Usage: python3 tools/generate-reference.py build/afterglow/afterglow
"""
import json
from pathlib import Path
import subprocess
import sys
repo = Path(__file__).resolve().parents[1]
reference = json.loads(subprocess.check_output([str(Path(sys.argv[1]).resolve()), '--config-reference'], text=True))
def code(value):
    return '`' + json.dumps(value, ensure_ascii=False).replace('|', '\\|').replace('`', '&#96;') + '`'
lines = ['<!-- SPDX-FileCopyrightText: 2026 fuddlesworth -->', '<!-- SPDX-License-Identifier: GPL-3.0-or-later -->', '', '# Configuration reference', '',
         'Generated from the runtime Phosphor schema by `tools/generate-reference.py`. Group and key names are case-sensitive. See the [user guide](README.md) for loading, examples and live reload.', '',
         'Numeric values are clamped to the listed ranges. Invalid enum choices fall back to the default. Empty strings retain the meaning in each description. Omitted values use defaults.', '']
for group, keys in reference['groups'].items():
    lines += ['## ' + group, '', '| Key | Default | Range or choices | Meaning |', '| --- | --- | --- | --- |']
    for name, entry in keys.items():
        default = entry['default']
        constraint = ', '.join(code(x) for x in entry.get('choices', []))
        if 'minimum' in entry: constraint = f"{entry['minimum']}–{entry['maximum']}"
        description = entry['description'].replace('|', '\\|').replace('\n', ' ')
        if group == 'Keybinds' and name == 'bindings':
            shown = '[default bindings](#default-bindings)'
        else: shown = code(default)
        lines.append(f'| `{name}` | {shown} | {constraint} | {description} |')
    lines += ['']
lines += ['## Default bindings', '', '| Action | Keys |', '| --- | --- |']
for name, value in reference['groups']['Keybinds']['bindings']['default'].items():
    lines.append(f'| `{name}` | {code(value)} |')
(repo / 'docs/CONFIGURATION.md').write_text('\n'.join(lines) + '\n')
