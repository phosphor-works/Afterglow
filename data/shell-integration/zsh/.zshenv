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
# The parts that Terminal.shellIntegration turns off, as Kitty's switches.
typeset -g __afterglow_features=" ${AFTERGLOW_SHELL_FEATURES-} "
unset AFTERGLOW_SHELL_FEATURES
# sudo and run0 drop TERMINFO_DIRS, where afterglow's own terminfo entry is:
# the command gets a TERM that every system knows, as Kitty's no-sudo
# switch covers. Defined before .zshrc, so that an alias of sudo there
# still works.
if [[ $__afterglow_features != *" no-sudo "* && ${TERM-} == afterglow ]] \
    && ! TERMINFO_DIRS='' TERMINFO='' command tput -T afterglow longname >/dev/null 2>&1; then
    sudo() {
        # VAR=value must come after sudo's options; -e and -v take none.
        emulate -L zsh
        local -a args=("$@")
        integer i skip=0
        for ((i = 1; i <= $#args; i++)); do
            if (( skip )); then skip=0; continue; fi
            case $args[i] in
                (--) (( i++ )); break ;;
                (--edit|-e*|-[^-]*e*|--validate|-v*|-[^-]*v*) command sudo "$@"; return ;;
                (--user|--group|--host|--chdir|--chroot|--role|--type|--command-timeout|--auth-type|--login-class|--prompt|--close-from|--other-user) skip=1 ;;
                (--*) ;;
                (-*[aCcDghpRrtTuU]) skip=1 ;;
                (-*|*=*) ;;
                (*) break ;;
            esac
        done
        command sudo "${(@)args[1,i-1]}" TERM=xterm-256color "${(@)args[i,-1]}"
    }
    (( $+commands[run0] )) && run0() { TERM=xterm-256color command run0 --setenv=TERM "$@"; }
fi
# The host before the title of an ssh login, as Kitty shows it.
typeset -g __afterglow_host=
[[ -n ${SSH_TTY-} ]] && __afterglow_host="${HOST%%.*}: "
# Set while the prompt carries afterglow's marks, not a framework's own.
typeset -gi __afterglow_marks=0
__afterglow_preexec() {
    emulate -L zsh
    (( __afterglow_marks )) && printf '\033]133;C\007'
    # The configured cursor for the command, and its line as the title.
    [[ $__afterglow_features == *" no-cursor "* ]] || printf '\033[0 q'
    [[ $__afterglow_features == *" no-title "* ]] || printf '\033]2;%s%s\007' "$__afterglow_host" "${1//[[:cntrl:]]}"
}
__afterglow_precmd() {
    local result=$?
    emulate -L zsh
    local begin=$'%{\e]133;A;redraw=1\a%}' end=$'%{\e]133;B\a%}'
    PS1=${PS1#"$begin"}; PS1=${PS1%"$end"}
    # A bar while the user edits the command, and the directory as the title.
    [[ $__afterglow_features == *" no-cursor "* ]] || printf '\033[%s q' "${AFTERGLOW_PROMPT_CURSOR:-5}"
    [[ $__afterglow_features == *" no-title "* ]] || print -Pn -- "\e]2;${__afterglow_host//\%/%%}%~\a"
    # Checked at each prompt: frameworks such as powerlevel10k set the prompt,
    # and their own OSC 133/633 marks, in precmd hooks that run before this one.
    if [[ $__afterglow_features == *" no-prompt-mark "* || $PS1 == *']133;'* || $PS1 == *']633;'* ]]; then
        __afterglow_marks=0
    else
        (( __afterglow_marks )) && printf '\033]133;D;%d\007' "$result"
        PS1=$begin$PS1$end
        __afterglow_marks=1
    fi
    local LC_ALL=C path_text=$PWD encoded='' char hex
    integer i
    for ((i=1; i<=${#path_text}; i++)); do
        char=${path_text[i]}
        case $char in
            [a-zA-Z0-9/._~-]) encoded+=$char ;;
            *) printf -v hex '%%%02X' "'$char"; encoded+=$hex ;;
        esac
    done
    [[ $__afterglow_features == *" no-cwd "* ]] || printf '\033]7;file://%s\007' "$encoded"
    return $result
}
__afterglow_setup() {
    # Defer until user prompt frameworks have loaded.
    precmd_functions=(${precmd_functions:#__afterglow_setup})
    # .zshrc, read after .zshenv, can turn the integration off.
    [[ ${AFTERGLOW_SHELL_INTEGRATION-} == 0 ]] && return
    # ssh through `afterglow ssh`, unless the user already wraps it.
    if [[ -n ${AFTERGLOW_SSH_HELPER-} ]] && (( ! $+functions[ssh] && ! $+aliases[ssh] )); then
        ssh() {
            if [[ -x $AFTERGLOW_SSH_HELPER ]]; then "$AFTERGLOW_SSH_HELPER" ssh "$@"; else command ssh "$@"; fi
        }
    fi
    preexec_functions+=(__afterglow_preexec)
    precmd_functions+=(__afterglow_precmd)
    __afterglow_precmd
}
precmd_functions+=(__afterglow_setup)
