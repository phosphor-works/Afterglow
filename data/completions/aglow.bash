# SPDX-FileCopyrightText: 2026 fuddlesworth
# SPDX-License-Identifier: LGPL-2.1-or-later
# Bash completion for aglow: its commands, their options and the values of the options.

_aglow()
{
    local cur prev words cword
    _init_completion 2>/dev/null || {
        cur=${COMP_WORDS[COMP_CWORD]}
        prev=${COMP_WORDS[COMP_CWORD-1]}
        words=("${COMP_WORDS[@]}")
        cword=$COMP_CWORD
    }
    local commands="transfer dnd icat clipboard"
    if (( cword == 1 )); then
        COMPREPLY=($(compgen -W "$commands --help --version" -- "$cur"))
        return
    fi
    local command=${words[1]} options="" values=""
    case $command in
        transfer)
            options="--direction -d --mode -m --compress --permissions-bypass -p --confirm-paths -c --transmit-deltas -x --help"
            case $prev in
                --direction|-d) values="upload download send receive" ;;
                --mode|-m) values="normal mirror" ;;
                --compress) values="auto never always" ;;
                --permissions-bypass|-p) COMPREPLY=($(compgen -f -- "$cur")); return ;;
            esac ;;
        dnd)
            options="--drag --drop --drop-dest --copy-mode --confirm-drop-overwrite --drop-anywhere --drag-thumbnail --drag-thumbnail-size --exit-on --help"
            case $prev in
                --copy-mode) values="auto independent" ;;
                --drop-anywhere) values="disallowed copy move" ;;
                --exit-on) values="drag-finish drop-finish esc-key" ;;
                --drop-dest) COMPREPLY=($(compgen -d -- "$cur")); return ;;
                --drag-thumbnail) COMPREPLY=($(compgen -f -- "$cur")); return ;;
                --drag|--drop|--drag-thumbnail-size) return ;;
            esac ;;
        icat)
            options="--align --place --scale-up --fit --background --mirror --clear --clear-all --transfer-mode --detect-support --detection-timeout --use-window-size --print-window-size --stdin --z-index -z --loop -l --hold --unicode-placeholder --passthrough --image-id --no-trailing-newline -n --help"
            case $prev in
                --align) values="center left right" ;;
                --fit) values="width height both none" ;;
                --mirror) values="none horizontal vertical both" ;;
                --transfer-mode) values="detect file stream memory" ;;
                --stdin) values="detect yes no" ;;
                --passthrough) values="detect tmux none" ;;
                --place|--background|--detection-timeout|--use-window-size|--z-index|-z|--loop|-l|--image-id) return ;;
            esac ;;
        clipboard)
            options="--get-clipboard -g --use-primary -p --mime -m --alias -a --wait-for-completion --password --human-name --detection-timeout --help"
            case $prev in
                --mime|-m|--alias|-a|--password|--human-name|--detection-timeout) return ;;
            esac ;;
        *) return ;;
    esac
    if [[ -n $values ]]; then
        COMPREPLY=($(compgen -W "$values" -- "$cur"))
    elif [[ $cur == -* ]]; then
        COMPREPLY=($(compgen -W "$options" -- "$cur"))
    else
        COMPREPLY=($(compgen -f -- "$cur"))
        compopt -o filenames 2>/dev/null
    fi
}
complete -F _aglow aglow
