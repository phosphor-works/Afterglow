# SPDX-FileCopyrightText: 2026 fuddlesworth
# SPDX-License-Identifier: GPL-3.0-or-later
if [[ ${AFTERGLOW_ORIGINAL_ZDOTDIR_SET-} == 1 ]]; then
    export ZDOTDIR=$AFTERGLOW_ORIGINAL_ZDOTDIR
else
    unset ZDOTDIR
fi
unset AFTERGLOW_ORIGINAL_ZDOTDIR AFTERGLOW_ORIGINAL_ZDOTDIR_SET
# Let zsh read the real profile, rc and login files in their normal order.
[[ -r ${ZDOTDIR-$HOME}/.zshenv ]] && source "${ZDOTDIR-$HOME}/.zshenv"
[[ -o interactive && ${AFTERGLOW_SHELL_INTEGRATION-} != 0 ]] || return
__afterglow_preexec() {
    emulate -L zsh
    printf '\033]133;C\007'
}
__afterglow_precmd() {
    local result=$?
    emulate -L zsh
    printf '\033]133;D;%d\007' "$result"
    local begin=$'%{\e]133;A;redraw=1\a%}' end=$'%{\e]133;B\a%}'
    PS1=${PS1#"$begin"}; PS1=${PS1%"$end"}
    PS1=$begin$PS1$end
    local LC_ALL=C path_text=$PWD encoded='' char hex
    integer i
    for ((i=1; i<=${#path_text}; i++)); do
        char=${path_text[i]}
        case $char in
            [a-zA-Z0-9/._~-]) encoded+=$char ;;
            *) printf -v hex '%%%02X' "'$char"; encoded+=$hex ;;
        esac
    done
    printf '\033]7;file://%s\007' "$encoded"
    return $result
}
__afterglow_setup() {
    # Defer until user prompt frameworks have loaded.
    precmd_functions=(${precmd_functions:#__afterglow_setup})
    [[ $PS1 == *']133;'* || $PS1 == *']633;'* ]] && return
    preexec_functions+=(__afterglow_preexec)
    precmd_functions+=(__afterglow_precmd)
    __afterglow_precmd
}
precmd_functions+=(__afterglow_setup)
