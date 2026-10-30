# SPDX-FileCopyrightText: 2026 fuddlesworth
# SPDX-License-Identifier: GPL-3.0-or-later
# A clean demo shell; no startup hooks or persistent shell history.
unset HISTFILE
PS1='\[\e]133;A\a\]\[\e[36m\]❯\[\e[0m\] \[\e]133;B\a\]'
_phosphor_running=0
_phosphor_preexec() {
    if [[ $_phosphor_running == 0 && $BASH_COMMAND != _phosphor_prompt* ]]; then
        _phosphor_running=1
        printf '\e]133;C\a'
    fi
}
_phosphor_prompt() {
    local status=$?
    if [[ $_phosphor_running == 1 ]]; then printf '\e]133;D;%d\a' "$status"; fi
    _phosphor_running=0
}
PROMPT_COMMAND=_phosphor_prompt
trap '_phosphor_preexec' DEBUG
phosphor() { "$PHOSPHOR_DEMO_ROOT/bin/phosphor-afterglow" "$@"; }
printf '\n  \e[36mP H O S P H O R\e[0m\n  light, with a memory\n\n'
printf '  phosphor show          play the showcase\n  phosphor scene light   switch the material\n  phosphor scene dark    return to the dark scene\n\n'
printf '  Ctrl+Alt+1 / 2 / 3 / 4: fracture / recall / impulse / charge\n  Split and resize panes to explore the shared 3D scene.\n\n'

# Autoplay only the first pane. Splits continue to open quiet demo shells.
if [[ ${PHOSPHOR_AUTOPLAY:-0} == 1 ]] && (set -o noclobber; : > "$PHOSPHOR_DEMO_CONFIG.started") 2>/dev/null; then
    phosphor show
fi
