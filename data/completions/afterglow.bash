# SPDX-FileCopyrightText: 2026 fuddlesworth
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Bash completion for afterglow. Script IDs and their commands come from the
# running Afterglow, through the aglow that afterglow runs (aglow run
# --complete). It works best with the bash-completion package, which
# completes the commands of -e, ssh, ctl ... -- CMD and mux new -- CMD, and
# the hosts of mux --ssh. afterglow run,
# transfer, dnd, icat and clipboard complete as aglow does: bash-completion
# loads the completion of aglow when it is first needed. Without
# bash-completion, source aglow.bash as well.

_afterglow_ctl_commands="ls send-text paste get-text split move-pane detach-tab new-tab new-window focus close set-title
set-tab-title resize send-key scroll signal notify scripts open-directory projects save-project open-project action
watch overlay screenshot colors set-colors set-font-size set-opacity set-user-var signal-program window"
# Key names that ctl send-key takes, of many more.
_afterglow_keys="enter escape tab backspace space up down left right home end page_up page_down insert delete f1 f2 f3
f4 f5 f6 f7 f8 f9 f10 f11 f12 ctrl+c ctrl+d ctrl+l ctrl+z"
# The keybind actions that ctl action runs (afterglow/src/config/Keybinds.cpp).
_afterglow_actions="swap_left swap_right swap_up swap_down rotate_panes_clockwise rotate_panes_counterclockwise
pane_overview use_package_shaders move_pane_to_tab move_pane_to_window detach_tab save_project switch_project
end_script_key_mode cancel_overlay split_right split_down split_left split_up rename_pane close_pane new_window
new_tab close_tab rename_tab move_tab_left move_tab_right goto_tab_1 goto_tab_2 goto_tab_3 goto_tab_4 goto_tab_5
goto_tab_6 goto_tab_7 goto_tab_8 goto_tab_9 close_window toggle_fullscreen next_tab previous_tab show_hints
show_last_output show_first_output_on_screen show_last_visited_output copy_mode copy paste select_all clear_screen reset notify_on_finish focus_left focus_right focus_up
focus_down equalize_splits toggle_pane_zoom resize_left resize_right resize_up resize_down font_bigger font_smaller
font_reset scroll_page_up scroll_page_down scroll_top scroll_bottom jump_prompt_up jump_prompt_down search
command_palette reload_config open_config show_urls_open show_urls_copy unicode_input"

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

# The aglow that afterglow runs, in $aglow: the one in the directory of its
# executable, with symbolic links resolved. aglow starts faster than
# afterglow, which loads Qt. The completion of aglow uses it for afterglow
# run.
_afterglow_aglow()
{
    aglow=$self
    [[ $aglow == */* ]] || aglow=$(type -P -- "$aglow")
    [[ $aglow ]] && aglow=$(readlink -f -- "$aglow") && [[ $aglow ]] || return 1
    aglow=${aglow%/*}/aglow
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
                # --title=x gives --title as well.
                [[ ${words[j]%%=*} == "$option" ]] && given=1
            done
        done
        [[ $given ]] || _afterglow_matching ${group//|/ }
    done
}

_afterglow_ctl()
{
    # --session NAME comes before the command. afterglow mux ls lists the
    # names, but a completion runs no program but aglow run --complete, so
    # none is offered. With the cursor on the word --session itself, that
    # word is the one being completed.
    # In a pane of a session, the commands act on it as with --session.
    local first=2 session=${AFTERGLOW_SESSION:+1}
    if [[ ${words[2]} == --session ]] && ((cword > 2)); then
        ((cword == 3)) && return
        first=4 session=1
    fi
    local command=${words[first]} start=$((first + 1)) i j positional=0 text=""
    if ((cword == first)); then
        if [[ $cur == -* ]]; then
            # The usage only comes first: after --session NAME a command follows.
            ((first == 2)) && _afterglow_matching -h --help --session
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
            split | new-tab | new-window | overlay) _afterglow_offset $((i + 1)) ;;
            esac
            # Text follows, or the command takes nothing after it.
            return
            ;;
        --pane | --cwd | --layout | --direction | --cells | --pages | --prompt | --env | --var | --stdin-text | --size)
            ((i++))
            ;;
        --capture | --title | -w | --width | -h | --height | --position)
            # The options of overlay. Elsewhere they are no option of ctl,
            # and -w and -h are words, as -x.png is.
            if [[ $command == overlay ]]; then
                ((i++))
            elif [[ ${words[i]} == -[wh] ]]; then
                ((positional++))
                text=1
            fi
            ;;
        # A lone - is a word for every command: the standard output of
        # screenshot, text for send-text.
        - | [!-]* | '')
            ((positional++))
            text=1
            ;;
        -[!-]*)
            # -x.png is a word too, except where overlay takes -n, -w and -h.
            if [[ $command != overlay ]]; then
                ((positional++))
                text=1
            fi
            ;;
        -*) ;;
        esac
    done
    case $prev in
    --pane | --layout | --cells | --pages | --prompt | --env | --var | --size) return ;;
    --stdin-text)
        _afterglow_matching screen scrollback selection last-output first-output-on-screen last-visited-output
        return
        ;;
    --direction)
        _afterglow_matching left right up down
        return
        ;;
    --cwd)
        _afterglow_files -d
        return
        ;;
    esac
    if [[ $command == overlay ]]; then
        case $prev in
        --title | -w | --width | -h | --height) return ;;
        --capture)
            _afterglow_matching stdout file none
            return
            ;;
        --position)
            _afterglow_matching center top bottom left right top-left top-right bottom-left bottom-right
            return
            ;;
        esac
    fi
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
        screenshot:0)
            _afterglow_files
            return
            ;;
        move-pane:1)
            _afterglow_matching left right up down new-tab new-window
            return
            ;;
        signal:0)
            _afterglow_matching 0 1 2 3 4 5 6 7
            return
            ;;
        signal-program:0)
            _afterglow_matching INT TERM HUP KILL QUIT USR1 USR2 STOP CONT TSTP WINCH
            return
            ;;
        send-key:*)
            # shellcheck disable=SC2086 # the list splits into its words
            _afterglow_matching $_afterglow_keys
            return
            ;;
        action:0)
            # shellcheck disable=SC2086 # the list splits into its words
            _afterglow_matching $_afterglow_actions
            local aglow
            if [[ script: == "$cur"* || $cur == script:* ]] && _afterglow_aglow && [[ -x $aglow ]]; then
                # One connection: ID<TAB>COMMAND<TAB>SUMMARY lines.
                local id name
                while IFS=$'\t' read -r id name _; do
                    [[ $id && $name ]] && _afterglow_matching "script:$id.$name"
                done < <("$aglow" run --complete --all 2>/dev/null)
                # Readline replaces only the part of the word after its last colon.
                if declare -F _comp_ltrim_colon_completions >/dev/null; then
                    _comp_ltrim_colon_completions "$cur"
                elif declare -F __ltrim_colon_completions >/dev/null; then
                    __ltrim_colon_completions "$cur"
                elif [[ $cur == *:* && $COMP_WORDBREAKS == *:* ]]; then
                    local colon=${cur%"${cur##*:}"}
                    COMPREPLY=("${COMPREPLY[@]#"$colon"}")
                fi
            fi
            return
            ;;
        esac
    fi
    case $command in
    ls) _afterglow_unused --json ;;
    send-text | paste)
        # --stdin takes no text, and the text takes no --stdin.
        if _afterglow_given --stdin; then
            _afterglow_unused $pane
        elif [[ $text ]] || _afterglow_given --raw; then
            _afterglow_unused $pane --raw
        else
            _afterglow_unused $pane --raw --stdin
        fi
        ;;
    get-text)
        _afterglow_unused $pane "--screen|--scrollback|--selection|--last-output|--first-output-on-screen|--last-visited-output" \
            --ansi --cursor --wrap-markers
        ;;
    screenshot) _afterglow_unused $pane "--tab|--window" ;;
    split)
        # --env and --var can come again. A pane of a mux session takes
        # only --env: its daemon starts the program.
        if [[ $session ]]; then
            _afterglow_unused $pane "--left|--right|--up|--down"
            _afterglow_matching --env
        else
            _afterglow_unused $pane "--left|--right|--up|--down" --hold --no-focus --stdin-text --stdin-ansi
            _afterglow_matching --env --var
        fi
        ;;
    focus) _afterglow_unused $pane --direction ;;
    colors) _afterglow_unused $pane --json ;;
    # --all sets every pane: no --pane with it.
    set-colors) _afterglow_unused "${pane:+$pane|}--all" --reset ;;
    window) _afterglow_unused $pane "--fullscreen|--maximized|--normal" --size ;;
    resize) _afterglow_unused $pane --direction --cells ;;
    scroll)
        # LINES or one of the options.
        if ((positional)); then
            _afterglow_unused $pane
        else
            _afterglow_unused $pane "--pages|--prompt|--top|--bottom"
        fi
        ;;
    new-tab)
        if [[ $session ]]; then
            _afterglow_unused $pane --cwd
            _afterglow_matching --env
        else
            _afterglow_unused $pane --cwd --hold --no-focus --stdin-text --stdin-ansi
            _afterglow_matching --env --var
        fi
        ;;
    new-window)
        _afterglow_unused --cwd --hold
        _afterglow_matching --env --var
        ;;
    overlay) _afterglow_unused $pane --capture --cwd --title "-n|--no-stdin" "-w|--width" "-h|--height" --position "--border|--no-border" \
        --stdin-text --stdin-ansi ;;
    open-directory) _afterglow_unused --layout ;;
    save-project) _afterglow_unused $pane --overwrite ;;
    open-project) _afterglow_unused --new "--into-session|--no-session" ;;
    watch | scripts | projects) ;;
    *) _afterglow_unused $pane ;;
    esac
}

# The ssh destinations that bash-completion knows, when it is loaded.
_afterglow_hosts()
{
    if declare -F _comp_compgen_known_hosts >/dev/null; then
        _comp_compgen_known_hosts -- "$cur"
    elif declare -F _known_hosts_real >/dev/null; then
        _known_hosts_real -- "$cur"
    fi
}

# afterglow mux COMMAND ...: the commands, their options, the hosts of
# --ssh, the directory of new --cwd, and after new -- the program and its
# arguments.
_afterglow_mux()
{
    if ((cword == 2)); then
        if [[ $cur == -* ]]; then
            _afterglow_matching -h --help
        else
            _afterglow_matching server new attach detach ls kill
        fi
        return
    fi
    local i start=3
    case ${words[2]} in
    new)
        for ((i = 3; i < cword; i++)); do
            case ${words[i]} in
            --)
                _afterglow_offset $((i + 1))
                return
                ;;
            --name | --cwd | --ssh) ((i++)) ;;
            esac
        done
        case $prev in
        --ssh)
            _afterglow_hosts
            return
            ;;
        --cwd)
            # With --ssh, also after the cursor, it is a directory of that host.
            for ((i = 3; i < ${#words[@]}; i++)); do
                [[ ${words[i]} == -- ]] && break
                [[ ${words[i]} == --ssh ]] && return
            done
            _afterglow_files -d
            return
            ;;
        --name) return ;;
        esac
        _afterglow_unused --name --cwd --detach --ssh
        ;;
    kill | attach | detach | ls)
        if [[ $prev == --ssh ]]; then
            _afterglow_hosts
            return
        fi
        if [[ $cur == -* ]]; then
            case ${words[2]} in
            kill) _afterglow_unused --force --ssh ;;
            attach) _afterglow_unused --no-takeover --ssh ;;
            *) _afterglow_unused --ssh ;;
            esac
        fi
        ;;
    esac
}

# Whether the option $1 is on the command line before the cursor.
_afterglow_given()
{
    local j
    for ((j = start; j < cword; j++)); do
        [[ ${words[j]%%=*} == "$1" ]] && return 0
    done
    return 1
}

_afterglow()
{
    # comp_args and split are what the initializations below set besides.
    local cur prev words cword comp_args split
    if declare -F _comp_initialize >/dev/null; then
        _comp_initialize -n =: -- "$@" || return
    elif declare -F _init_completion >/dev/null; then
        _init_completion -n =: || return
    else
        # COMP_WORDS splits a word at = and :, as readline does. The words of
        # the line before the cursor, split at blanks only, keep script:ID.CMD
        # and --option=value whole, as bash-completion does.
        local line=${COMP_LINE:0:COMP_POINT}
        read -ra words <<<"$line"
        [[ ! $line || $line == *[[:blank:]] ]] && words+=("")
        cword=$((${#words[@]} - 1))
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
        mux)
            _afterglow_mux
            return
            ;;
        run | transfer | dnd | icat | clipboard)
            # These run aglow, whose completion reads its command at the same
            # position. Its completion is found by the path of that aglow too,
            # as share/bash-completion/completions/aglow beside the bin
            # directory of an afterglow outside PATH. $aglow names that aglow
            # to the completion of aglow, for run --complete.
            local aglow
            _afterglow_aglow || aglow=""
            if ! declare -F _aglow >/dev/null; then
                if declare -F _comp_load >/dev/null; then
                    _comp_load -- "${aglow:-aglow}"
                elif declare -F _completion_loader >/dev/null; then
                    _completion_loader "${aglow:-aglow}"
                fi
            fi
            if declare -F _aglow >/dev/null; then
                _aglow
            else
                # No completion of aglow: its arguments are files, most likely.
                _afterglow_files
            fi
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
        --config-reference | -h | --help | --help-all | -v | --version) return ;;
        esac
    done
    # --option=value: the value is at the cursor. Readline replaces only the
    # part after the "=".
    if [[ $cur == --*=* ]]; then
        prev=${cur%%=*}
        cur=${cur#*=}
    fi
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
        _afterglow_matching ctl mux run ssh transfer dnd icat clipboard
        return
    fi
    local start=1
    if ((cword == 1)); then
        _afterglow_matching --config-reference -h --help --help-all -v --version
    fi
    # --quick-terminal takes no -e, -d, --title or --hold.
    if _afterglow_given --quick-terminal; then
        _afterglow_unused --config --class
    elif _afterglow_given -d || _afterglow_given --working-directory || _afterglow_given --title \
        || _afterglow_given --hold; then
        _afterglow_unused "-d|--working-directory" "-e|--execute" --title --hold --config --class
    else
        _afterglow_unused "-d|--working-directory" "-e|--execute" --title --hold --quick-terminal --config --class
    fi
    # Repeatable.
    _afterglow_matching -o --override
}

complete -F _afterglow afterglow
