# SPDX-FileCopyrightText: 2026 fuddlesworth
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Bash completion for afterglow. Script IDs and their commands come from the
# running afterglow (afterglow run --complete). It works best with the
# bash-completion package, which keeps a word whole at = and : and completes
# the commands of -e, ssh and ctl ... -- CMD.

_afterglow_ctl_commands="ls send-text get-text split move-pane detach-tab new-tab new-window focus close set-title
signal notify scripts open-directory projects save-project open-project action watch"

# Completes the command line from words[$1] on as a command of its own.
_afterglow_offset()
{
    if declare -F _comp_command_offset >/dev/null; then
        _comp_command_offset "$1"
    elif declare -F _command_offset >/dev/null; then
        # The older helper counts COMP_WORDS, which splits words at = and :.
        local i=0 j word
        for ((j = 0; j < $1; j++)); do
            word=${words[j]}
            while [[ $word && i -lt ${#COMP_WORDS[@]} && $word == *"${COMP_WORDS[i]}"* ]]; do
                word=${word#*"${COMP_WORDS[i++]}"}
            done
        done
        _command_offset "$i"
    fi
}

# File names, or with -d directory names.
_afterglow_files()
{
    if declare -F _comp_compgen_filedir >/dev/null; then
        _comp_compgen_filedir "$@"
    elif declare -F _filedir >/dev/null; then
        _filedir "$@"
    else
        compopt -o filenames 2>/dev/null
        mapfile -t COMPREPLY < <(compgen "${1:--f}" -- "$cur")
    fi
}

# The words of the arguments that start with $cur. The words come from
# afterglow and are matched as text: compgen -W would expand them.
_afterglow_matching()
{
    local word
    for word; do
        [[ $word == "$cur"* ]] && COMPREPLY+=("$word")
    done
}

# The options in $@ that are not on the command line yet. An argument
# "a|b|c" is a group of options that exclude each other.
_afterglow_unused()
{
    local group option given j
    for group; do
        given=""
        for option in ${group//|/ }; do
            for ((j = start; j < cword; j++)); do
                [[ ${words[j]} == "$option" ]] && given=1
            done
        done
        [[ $given ]] || _afterglow_matching ${group//|/ }
    done
}

_afterglow_ctl()
{
    local command=${words[2]} start=3 i j positional=0 text=""
    if ((cword == 2)); then
        if [[ $cur == -* ]]; then
            _afterglow_matching -h --help
        else
            # shellcheck disable=SC2086 # the list splits into its words
            _afterglow_matching $_afterglow_ctl_commands
        fi
        return
    fi
    for ((i = start; i < cword; i++)); do
        case ${words[i]} in
        --)
            case $command in
            split | new-tab | new-window) _afterglow_offset $((i + 1)) ;;
            esac
            # Text follows, or the command takes nothing after it.
            return
            ;;
        --pane | --cwd | --layout)
            ((i++))
            ;;
        -*) ;;
        *)
            ((positional++))
            text=1
            ;;
        esac
    done
    case $prev in
    --pane | --layout) return ;;
    --cwd)
        _afterglow_files -d
        return
        ;;
    esac
    local pane=--pane
    case $command in
    ls | watch | scripts | projects | open-directory | open-project | new-window) pane="" ;;
    esac
    if [[ $cur != -* ]]; then
        case $command:$positional in
        open-directory:0)
            _afterglow_files -d
            return
            ;;
        move-pane:1)
            _afterglow_matching left right up down new-tab new-window
            return
            ;;
        action:0)
            if [[ script: == "$cur"* || $cur == script:* ]]; then
                local id name
                while IFS= read -r id; do
                    while IFS=$'\t' read -r name _; do
                        _afterglow_matching "script:$id.$name"
                    done < <("$self" run --complete "$id" 2>/dev/null)
                done < <("$self" run --complete 2>/dev/null)
                if declare -F _comp_ltrim_colon_completions >/dev/null; then
                    _comp_ltrim_colon_completions "$cur"
                elif declare -F __ltrim_colon_completions >/dev/null; then
                    __ltrim_colon_completions "$cur"
                fi
            fi
            return
            ;;
        esac
    fi
    case $command in
    ls) _afterglow_unused --json ;;
    send-text)
        # --stdin takes no text, and the text takes no --stdin.
        if _afterglow_given --stdin; then
            _afterglow_unused $pane
        elif [[ $text ]] || _afterglow_given --raw; then
            _afterglow_unused $pane --raw
        else
            _afterglow_unused $pane --raw --stdin
        fi
        ;;
    get-text) _afterglow_unused $pane "--screen|--scrollback|--selection|--last-output" ;;
    split) _afterglow_unused $pane "--left|--right|--up|--down" ;;
    new-tab | new-window) _afterglow_unused $pane --cwd ;;
    open-directory) _afterglow_unused --layout ;;
    save-project) _afterglow_unused $pane --overwrite ;;
    open-project) _afterglow_unused --new ;;
    watch | scripts | projects) ;;
    *) _afterglow_unused $pane ;;
    esac
}

# Whether the option $1 is on the command line before the cursor.
_afterglow_given()
{
    local j
    for ((j = start; j < cword; j++)); do
        [[ ${words[j]} == "$1" ]] && return 0
    done
    return 1
}

_afterglow_run()
{
    # The words after "run" up to here: options, then ID, then COMMAND.
    local i id="" command="" options=1 start=2
    for ((i = start; i < cword; i++)); do
        local word=${words[i]}
        if [[ $options && -z $id ]]; then
            case $word in
            -n | --no-stdin) continue ;;
            --pane)
                ((i++))
                ((i == cword)) && return
                continue
                ;;
            --)
                options=""
                continue
                ;;
            # -h and --help only alone, and any other option is unknown.
            -*) return ;;
            esac
        fi
        if [[ -z $id ]]; then
            id=$word
        else
            command=$word
            break
        fi
    done
    if [[ -n $command ]]; then
        # The command's own arguments: files, most likely.
        [[ $command == -h || $command == --help ]] || _afterglow_files
    elif [[ -n $id ]]; then
        local name
        _afterglow_matching --help
        while IFS=$'\t' read -r name _; do
            _afterglow_matching "$name"
        done < <("$self" run --complete "$id" 2>/dev/null)
    elif [[ $options && $cur == -* ]]; then
        # --help only alone: after an option it would name a script.
        ((cword == start)) && _afterglow_matching -h --help
        _afterglow_unused "-n|--no-stdin" --pane
    else
        local script
        while IFS= read -r script; do
            _afterglow_matching "$script"
        done < <("$self" run --complete 2>/dev/null)
    fi
}

_afterglow()
{
    local cur prev words cword
    if declare -F _comp_initialize >/dev/null; then
        _comp_initialize -n =: -- "$@" || return
    elif declare -F _init_completion >/dev/null; then
        _init_completion -n =: || return
    else
        words=("${COMP_WORDS[@]}")
        cword=$COMP_CWORD
        cur=${words[cword]}
        prev=${words[cword - 1]}
    fi
    COMPREPLY=()
    # The afterglow that is being completed, which need not be the one on PATH. The word is as typed, quotes
    # included: bash-completion 2.12 and later remove them, without running a command substitution.
    local self=${words[0]} REPLY
    declare -F _comp_dequote >/dev/null && _comp_dequote "$self" && self=${REPLY[0]}
    [[ $self == "~/"* ]] && self=$HOME/${self#"~/"}

    if ((cword > 1)); then
        case ${words[1]} in
        ssh)
            # The rest is ssh's command line.
            _afterglow_offset 1
            return
            ;;
        ctl)
            _afterglow_ctl
            return
            ;;
        run)
            _afterglow_run
            return
            ;;
        transfer | dnd | icat | clipboard)
            # These run aglow, whose completion reads its command at the same position.
            if ! declare -F _aglow >/dev/null; then
                if declare -F _comp_load >/dev/null; then
                    _comp_load -- aglow
                elif declare -F _completion_loader >/dev/null; then
                    _completion_loader aglow
                fi
            fi
            declare -F _aglow >/dev/null && _aglow
            return
            ;;
        esac
    fi

    # The options of a window, and of -e the program and its arguments.
    local i
    for ((i = 1; i < cword; i++)); do
        case ${words[i]} in
        -e | --execute)
            _afterglow_offset $((i + 1))
            return
            ;;
        -d | --working-directory | --title | --config | -o | --override | --class) ((i++)) ;;
        # Each of these is the whole command line.
        --config-reference | -h | --help | -v | --version) return ;;
        esac
    done
    case $prev in
    -d | --working-directory)
        _afterglow_files -d
        return
        ;;
    --config)
        _afterglow_files
        return
        ;;
    --title | -o | --override | --class) return ;;
    esac
    if ((cword == 1)) && [[ $cur != -* ]]; then
        _afterglow_matching ctl run ssh transfer dnd icat clipboard
        return
    fi
    local start=1
    if ((cword == 1)); then
        _afterglow_matching --config-reference -h --help -v --version
    fi
    _afterglow_unused "-d|--working-directory" "-e|--execute" --title --hold --quick-terminal --config --class
    # Repeatable.
    _afterglow_matching -o --override
}

complete -F _afterglow afterglow
