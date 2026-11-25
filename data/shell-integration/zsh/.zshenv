# SPDX-FileCopyrightText: 2026 Phosphor Works Team
# SPDX-License-Identifier: GPL-3.0-or-later
# Once: a second source would reset the switches and add the hooks again,
# also one from the user's .zshenv, which this file reads.
[[ ${__afterglow_loaded-} == 1 ]] && return
typeset -g __afterglow_loaded=1
# Afterglow set ZDOTDIR to this directory, and saved the user's. Sourced by
# hand from ~/.zshrc instead (the no-rc switch, or a shell that Afterglow
# did not start), zsh reads the user's files itself.
if [[ -n ${AFTERGLOW_ORIGINAL_ZDOTDIR_SET-} ]]; then
    if [[ $AFTERGLOW_ORIGINAL_ZDOTDIR_SET == 1 ]]; then
        export ZDOTDIR=$AFTERGLOW_ORIGINAL_ZDOTDIR
    else
        unset ZDOTDIR
    fi
    unset AFTERGLOW_ORIGINAL_ZDOTDIR AFTERGLOW_ORIGINAL_ZDOTDIR_SET
    typeset -g __afterglow_injected=1
fi
# The parts that Terminal.shellIntegration turns off, as Kitty's switches,
# and the prompt cursor. Read before any return, and not exported: the
# user's programs do not get them.
typeset -g __afterglow_features=" ${AFTERGLOW_SHELL_FEATURES-} "
typeset -g __afterglow_cursor=${AFTERGLOW_PROMPT_CURSOR:-5}
unset AFTERGLOW_SHELL_FEATURES AFTERGLOW_PROMPT_CURSOR
# Let zsh read the real profile, rc and login files in their normal order.
if (( ${__afterglow_injected-0} )); then
    unset __afterglow_injected
    [[ -r ${ZDOTDIR-$HOME}/.zshenv ]] && source "${ZDOTDIR-$HOME}/.zshenv"
fi
[[ -o interactive && ${AFTERGLOW_SHELL_INTEGRATION-} != 0 ]] || return
# Whether the system has afterglow's terminfo entry, without the directories
# of Afterglow, and without this ~/.terminfo: the command of sudo runs as
# another user.
function __afterglow_system_knows_term {
    emulate -L zsh
    if (( $+commands[tput] )); then
        HOME=/nonexistent TERMINFO_DIRS='' TERMINFO='' command tput -T afterglow longname >/dev/null 2>&1
        return
    fi
    local dir
    for dir in /etc/terminfo /lib/terminfo /usr/share/terminfo /usr/lib/terminfo; do
        [[ -e $dir/a/afterglow || -e $dir/61/afterglow ]] && return 0
    done
    return 1
}
# sudo and run0 drop TERMINFO_DIRS, where afterglow's own terminfo entry is:
# the command gets a TERM that every system knows, as Kitty's no-sudo
# switch covers. sudo keeps TERM from its own environment (sudoers
# env_reset), so its options stay as typed. A function of the user's
# .zshenv stays. Defined before .zshrc, so that an alias of sudo there still
# works; the function keyword keeps an alias of .zshenv from the parse.
typeset -gA __afterglow_wrappers=()
if [[ $__afterglow_features != *" no-sudo "* && ${TERM-} == afterglow ]] && ! __afterglow_system_knows_term; then
    if (( ! $+functions[sudo] )); then
        function sudo { TERM=xterm-256color command sudo "$@"; }
        __afterglow_wrappers[sudo]=$functions[sudo]
    fi
    if (( $+commands[run0] && ! $+functions[run0] )); then
        function run0 { TERM=xterm-256color command run0 --setenv=TERM "$@"; }
        __afterglow_wrappers[run0]=$functions[run0]
    fi
fi
unfunction __afterglow_system_knows_term
# The host before the title of an ssh login, as Kitty shows it.
typeset -g __afterglow_host=
[[ -n ${SSH_TTY-} ]] && __afterglow_host="${HOST%%.*}: "
# Set while the prompt carries afterglow's marks, not a framework's own.
typeset -gi __afterglow_marks=0
# Set when the main keymap is vi's: its modes set their own cursors.
typeset -gi __afterglow_vi=0
function __afterglow_preexec {
    emulate -L zsh
    (( __afterglow_marks )) && printf '\033]133;C\007'
    # The configured cursor for the command, and its line as the title.
    [[ $__afterglow_features == *" no-cursor "* ]] || (( __afterglow_vi )) || printf '\033[0 q'
    [[ $__afterglow_features == *" no-title "* ]] || printf '\033]2;%s%s\007' "$__afterglow_host" "${1//[[:cntrl:]]}"
}
# zsh runs the hooks that precmd_functions held when the prompt began. A hook
# after this one (a theme's, or one that a deferred loader added) sets PS1
# again: this one goes last for the next prompts, and this prompt gets its
# marks from a scheduled event, which runs after every hook.
function __afterglow_precmd {
    typeset -g __afterglow_status=$?
    emulate -L zsh
    if [[ ${precmd_functions[-1]} != __afterglow_precmd ]]; then
        precmd_functions=(${precmd_functions:#__afterglow_precmd} __afterglow_precmd)
        __afterglow_after_hooks
    else
        __afterglow_prompt
    fi
}
# Runs __afterglow_prompt after the rest of this prompt's hooks.
function __afterglow_after_hooks {
    if zmodload zsh/sched 2>/dev/null; then
        sched +0 __afterglow_prompt
    else
        __afterglow_prompt
    fi
}
function __afterglow_prompt {
    local result=$__afterglow_status
    emulate -L zsh
    local begin=$'%{\e]133;A;redraw=1\a%}' end=$'%{\e]133;B\a%}'
    # The prompt of a command that goes on (an open quote, a \ at the end):
    # the shell waits for the rest of it.
    local secondary=$'%{\e]133;A;k=s\a%}'
    # Every copy of the last prompt's marks: a venv or conda activate puts its
    # text before them.
    PS1=${PS1//"$begin"/}; PS1=${PS1//"$end"/}; PS2=${PS2//"$secondary"/}
    # A bar while the user edits the command, and the directory as the title.
    [[ $__afterglow_features == *" no-cursor "* ]] || (( __afterglow_vi )) || printf '\033[%s q' "$__afterglow_cursor"
    [[ $__afterglow_features == *" no-title "* ]] || print -Pn -- "\e]2;${__afterglow_host//\%/%%}%~\a"
    # Checked at each prompt: frameworks such as powerlevel10k set the prompt,
    # and their own OSC 133/633 marks, in precmd hooks that run before this one.
    if [[ $__afterglow_features == *" no-prompt-mark "* || $PS1 == *']133;'* || $PS1 == *']633;'* ]]; then
        __afterglow_marks=0
    else
        (( __afterglow_marks )) && printf '\033]133;D;%d\007' "$result"
        PS1=$begin$PS1$end
        PS2=$secondary$PS2
        __afterglow_marks=1
    fi
    [[ $__afterglow_features == *" no-cwd "* ]] && return $result
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
function __afterglow_setup {
    # The user's options (ksh_arrays) must not change how the hooks are read.
    emulate -L zsh
    # Defer until user prompt frameworks have loaded.
    precmd_functions=(${precmd_functions:#__afterglow_setup})
    # .zshrc, read after .zshenv, can turn the integration off: the wrappers
    # go with it, unless the user defined their own in their place.
    if [[ ${AFTERGLOW_SHELL_INTEGRATION-} == 0 ]]; then
        local name
        for name in ${(k)__afterglow_wrappers}; do
            [[ $functions[$name] == $__afterglow_wrappers[$name] ]] && unfunction $name
        done
        unset __afterglow_wrappers
        return
    fi
    unset __afterglow_wrappers
    [[ $(bindkey -lL main) == *' viins '* ]] && __afterglow_vi=1
    # ssh through `afterglow ssh`, unless the user already wraps it.
    if [[ -n ${AFTERGLOW_SSH_HELPER-} ]] && (( ! $+functions[ssh] && ! $+aliases[ssh] )); then
        function ssh {
            if [[ -x $AFTERGLOW_SSH_HELPER ]]; then "$AFTERGLOW_SSH_HELPER" ssh "$@"; else command ssh "$@"; fi
        }
    fi
    preexec_functions+=(__afterglow_preexec)
    precmd_functions+=(__afterglow_precmd)
    typeset -g __afterglow_status=0
    __afterglow_after_hooks
}
precmd_functions+=(__afterglow_setup)
