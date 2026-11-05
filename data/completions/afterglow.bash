# SPDX-FileCopyrightText: 2026 fuddlesworth
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Bash completion for afterglow. Script package IDs and their commands come
# from the running afterglow (afterglow run --complete).

_afterglow_ctl_commands="ls send-text get-text split move-pane detach-tab new-tab new-window focus close set-title
signal notify scripts open-directory projects save-project open-project action watch"

_afterglow()
{
    local cur prev
    cur=${COMP_WORDS[COMP_CWORD]}
    prev=${COMP_WORDS[COMP_CWORD-1]}

    case ${COMP_WORDS[1]} in
    ssh)
        # The rest is ssh's command line.
        if declare -F _comp_command_offset >/dev/null; then
            _comp_command_offset 1
        elif declare -F _command_offset >/dev/null; then
            _command_offset 1
        fi
        return
        ;;
    ctl)
        if ((COMP_CWORD == 2)); then
            mapfile -t COMPREPLY < <(compgen -W "$_afterglow_ctl_commands" -- "$cur")
            return
        fi
        case $prev in
        --pane | --layout) return ;;
        --cwd)
            mapfile -t COMPREPLY < <(compgen -d -- "$cur")
            return
            ;;
        esac
        case ${COMP_WORDS[2]} in
        get-text) mapfile -t COMPREPLY < <(compgen -W "--pane --screen --scrollback --selection --last-output" -- "$cur") ;;
        split) mapfile -t COMPREPLY < <(compgen -W "--pane --left --right --up --down" -- "$cur") ;;
        new-tab) mapfile -t COMPREPLY < <(compgen -W "--pane --cwd" -- "$cur") ;;
        new-window) mapfile -t COMPREPLY < <(compgen -W "--cwd" -- "$cur") ;;
        send-text) mapfile -t COMPREPLY < <(compgen -W "--pane --raw --stdin" -- "$cur") ;;
        ls) mapfile -t COMPREPLY < <(compgen -W "--json" -- "$cur") ;;
        open-directory)
            if [[ $cur == -* ]]; then
                mapfile -t COMPREPLY < <(compgen -W "--layout" -- "$cur")
            else
                mapfile -t COMPREPLY < <(compgen -d -- "$cur")
            fi
            ;;
        save-project) mapfile -t COMPREPLY < <(compgen -W "--pane --overwrite" -- "$cur") ;;
        open-project) mapfile -t COMPREPLY < <(compgen -W "--new" -- "$cur") ;;
        watch | scripts | projects) ;;
        *) mapfile -t COMPREPLY < <(compgen -W "--pane" -- "$cur") ;;
        esac
        return
        ;;
    run)
        if [[ $prev == --pane ]]; then
            return
        fi
        # The words after "run" up to here, without its options.
        local i id="" command=""
        for ((i = 2; i < COMP_CWORD; i++)); do
            case ${COMP_WORDS[i]} in
            -n | --no-stdin) ;;
            --pane) ((i++)) ;;
            *)
                if [[ -z $id ]]; then
                    id=${COMP_WORDS[i]}
                else
                    command=${COMP_WORDS[i]}
                    break
                fi
                ;;
            esac
        done
        if [[ -n $command ]]; then
            # The command's own arguments: files, most likely.
            mapfile -t COMPREPLY < <(compgen -f -- "$cur")
        elif [[ -n $id ]]; then
            mapfile -t COMPREPLY < <(compgen -W "--help $(afterglow run --complete "$id" 2>/dev/null | cut -f1)" -- "$cur")
        elif [[ $cur == -* ]]; then
            mapfile -t COMPREPLY < <(compgen -W "-n --no-stdin --pane --help" -- "$cur")
        else
            mapfile -t COMPREPLY < <(compgen -W "$(afterglow run --complete 2>/dev/null)" -- "$cur")
        fi
        return
        ;;
    esac

    # The options of a window, and of -e the program and its arguments.
    local i
    for ((i = 1; i < COMP_CWORD; i++)); do
        if [[ ${COMP_WORDS[i]} == -e || ${COMP_WORDS[i]} == --execute ]]; then
            if declare -F _comp_command_offset >/dev/null; then
                _comp_command_offset $((i + 1))
            elif declare -F _command_offset >/dev/null; then
                _command_offset $((i + 1))
            fi
            return
        fi
    done
    case $prev in
    -d | --working-directory)
        mapfile -t COMPREPLY < <(compgen -d -- "$cur")
        return
        ;;
    --config)
        mapfile -t COMPREPLY < <(compgen -f -- "$cur")
        return
        ;;
    --title | -o | --override | --class) return ;;
    esac
    if ((COMP_CWORD == 1)) && [[ $cur != -* ]]; then
        mapfile -t COMPREPLY < <(compgen -W "ctl run ssh" -- "$cur")
        return
    fi
    mapfile -t COMPREPLY < <(compgen -W "-d --working-directory -e --execute --title --hold --quick-terminal --config
-o --override --class --config-reference -h --help -v --version" -- "$cur")
}

complete -F _afterglow afterglow
