# SPDX-FileCopyrightText: 2026 fuddlesworth
# SPDX-License-Identifier: LGPL-2.1-or-later
# Bash completion for aglow: its commands, their options and the values of the options.
# Script IDs and their commands come from the running Afterglow (aglow run --complete).
# afterglow run, transfer, dnd, icat and clipboard use it too (afterglow.bash).

# The options of a command, one entry each: the names, separated by "|",
# then what the value is. No value: a flag. "-": text to type. "@files",
# "@dirs": a path. "@password": a path only when it starts with ., / or ~,
# as aglow reads it (else it is the password itself). "@events": a list
# separated by commas. Other words: the choices. A "*" before the names: the
# option can be given more than once.
_aglow_table()
{
    case $1 in
    transfer)
        table=("-d|--direction upload download send receive" "-m|--mode normal mirror"
            "--compress auto never always" "-p|--permissions-bypass @password" "-c|--confirm-paths"
            "-x|--transmit-deltas") ;;
    dnd)
        table=("*--drag -" "*--drop -" "--drop-dest @dirs" "--copy-mode auto independent"
            "--confirm-drop-overwrite" "--confirm-drop-timeout -" "--drop-anywhere disallowed copy move"
            "--drag-thumbnail @files" "--drag-thumbnail-size -" "--exit-on @events") ;;
    icat)
        table=("--align center left right" "--place -" "--scale-up" "--fit width height both none" "--background -"
            "--mirror none horizontal vertical both" "--clear" "--clear-all"
            "--transfer-mode detect file stream memory" "--detect-support" "--detection-timeout -"
            "--use-window-size -" "--print-window-size" "--stdin detect yes no" "-z|--z-index -" "-l|--loop -"
            "--hold" "--unicode-placeholder" "--passthrough detect tmux none" "--image-id -"
            "--engine auto builtin" "--silent" "-n|--no-trailing-newline") ;;
    clipboard)
        table=("-g|--get-clipboard" "-p|--use-primary" "*-m|--mime -" "*-a|--alias -" "--wait-for-completion"
            "--password -" "--human-name -" "--detection-timeout -") ;;
    *) return 1 ;;
    esac
    table+=("-h|--help")
}

# The entry of the table that names the option $1, in $entry.
_aglow_entry()
{
    local names
    for entry in "${table[@]}"; do
        names=${entry%% *}
        names=${names#\*}
        [[ "|$names|" == *"|$1|"* ]] && return 0
    done
    entry=""
    return 1
}

# The words of the arguments that start with $cur, with $prefix before each.
# The words are matched as text: compgen -W would expand them.
_aglow_matching()
{
    local word
    for word; do
        [[ $word == "$cur"* ]] && COMPREPLY+=("$prefix$word")
    done
}

# File names, or with -d directory names, quoted as the shell needs them.
_aglow_files()
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

# Completes the value of the option of table entry $1 at $cur.
_aglow_value()
{
    local value=${1#* }
    [[ $value == "$1" ]] && return
    case $value in
    -) ;;
    @files) _aglow_files ;;
    @dirs) _aglow_files -d ;;
    @password) [[ $cur == [./~]* ]] && _aglow_files ;;
    @events)
        # The events given so far stay, and each is offered once.
        local event
        [[ $cur == *,* ]] && prefix=${cur%,*},
        local listed=",$prefix"
        cur=${cur##*,}
        for event in drag-finish drop-finish esc-key; do
            [[ $listed == *",$event,"* ]] || _aglow_matching "$event"
        done
        compopt -o nospace 2>/dev/null
        ;;
    # shellcheck disable=SC2086 # the choices split into their words
    *) _aglow_matching $value ;;
    esac
}

# The word $1 as the shell runs it, in $REPLY: bash-completion 2.12 and later
# remove its quotes and expand it, without running a command substitution.
# Without them, the quotes and backslashes go: the names that aglow lists
# have none.
_aglow_dequote()
{
    if ! declare -F _comp_dequote >/dev/null || ! _comp_dequote "$1"; then
        REPLY=${1//[\'\"\\]/}
        [[ $REPLY == "~/"* ]] && REPLY=$HOME/${REPLY#"~/"}
    fi
}

# The aglow whose run --complete lists the script commands, in $program: the
# aglow that is being completed, or for afterglow run the aglow that afterglow
# runs, which afterglow.bash names in $aglow. The word is as typed, quotes
# included.
_aglow_program()
{
    local REPLY
    _aglow_dequote "${words[0]}"
    program=${REPLY[0]}
    [[ ${program##*/} == aglow ]] && return 0
    program=${aglow-}
    [[ $program && -x $program ]]
}

# aglow run: options, then the script ID, then its command and the command's
# own arguments.
_aglow_run()
{
    local i word id="" command="" options=1 program
    for ((i = 2; i < cword; i++)); do
        word=${words[i]}
        if [[ $options && -z $id ]]; then
            case $word in
            -n | --no-stdin) continue ;;
            --pane)
                # At the cursor: the pane number.
                ((++i == cword)) && return
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
            # As typed: run --complete takes the ID itself.
            local REPLY
            _aglow_dequote "$word"
            id=${REPLY[0]}
        else
            command=$word
            break
        fi
    done
    if [[ $command ]]; then
        # The command's own arguments: files, most likely.
        [[ $command == -h || $command == --help ]] || _aglow_files
        return
    fi
    if [[ $options && -z $id && $cur == -* ]]; then
        # --help is offered only first, as for the other commands. The other
        # options once each.
        ((cword == 2)) && _aglow_matching -h --help
        local given=" ${words[*]:2:cword-2} "
        [[ $given == *" -n "* || $given == *" --no-stdin "* ]] || _aglow_matching -n --no-stdin
        [[ $given == *" --pane "* ]] || _aglow_matching --pane
        return
    fi
    # --help needs no aglow. The names from Afterglow are matched as text:
    # compgen -W would expand them.
    [[ $id ]] && _aglow_matching --help
    _aglow_program || return
    if [[ $id ]]; then
        local name
        while IFS=$'\t' read -r name _; do
            _aglow_matching "$name"
        done < <("$program" run --complete "$id" 2>/dev/null)
    else
        local script
        while IFS= read -r script; do
            _aglow_matching "$script"
        done < <("$program" run --complete 2>/dev/null)
    fi
}

_aglow()
{
    # comp_args, was_split and split are what the initializations below set besides.
    local cur prev words cword comp_args was_split split
    if declare -F _comp_initialize >/dev/null; then
        _comp_initialize -s -- "$@" || return
    elif declare -F _init_completion >/dev/null; then
        _init_completion -s || return
    else
        words=("${COMP_WORDS[@]}")
        cword=$COMP_CWORD
        cur=${words[cword]}
    fi
    COMPREPLY=()
    local prefix=""
    if ((cword == 1)); then
        if [[ $cur == -* ]]; then
            _aglow_matching -h --help -v --version
        else
            _aglow_matching transfer dnd icat clipboard run help version
        fi
        return
    fi
    local -a table
    case ${words[1]} in
    help)
        ((cword == 2)) && _aglow_matching transfer dnd icat clipboard run
        return
        ;;
    run)
        _aglow_run
        return
        ;;
    esac
    _aglow_table "${words[1]}" || return

    # The words before the cursor: the options given, whether "--" ended them,
    # whether -h or --help asked for the help, and the option whose value
    # comes next. Without bash-completion, readline splits --option=value into
    # three words, with "=" in the middle. The word after an option is its
    # value, -h too, but not a word that starts with "--": aglow takes that
    # word as an option, and the value as left out.
    local i j word entry given="|" expect="" ended="" helped=""
    for ((i = 2; i < cword; i++)); do
        word=${words[i]}
        if [[ $ended ]]; then
            continue
        elif [[ $expect ]]; then
            [[ $word == = ]] && continue
            expect=""
            [[ $word == --* && ${words[i - 1]} != = ]] || continue
        fi
        case $word in
        --) ended=1 ;;
        -h | --help) helped=1 ;;
        --*)
            _aglow_entry "${word%%=*}" || continue
            given+="${entry%% *}|"
            # A flag takes only a yes or no value after "=" (--hold=no), and
            # the completion offers none: what comes after the "=" completes
            # as the flag's value, to nothing.
            if [[ $word != *=* ]] && [[ $entry == *" "* || ${words[i + 1]} == = ]]; then
                expect=$entry
            fi
            ;;
        -?*)
            # Flags together (-cx), and an option whose value is the rest or the next word.
            for ((j = 1; j < ${#word}; j++)); do
                _aglow_entry "-${word:j:1}" || break
                given+="${entry%% *}|"
                if [[ $entry == *" "* ]]; then
                    ((j + 1 == ${#word})) && expect=$entry
                    break
                fi
            done
            ;;
        esac
    done

    # aglow shows the help and reads no other word.
    [[ $helped ]] && return
    if [[ $ended ]]; then
        _aglow_files
        return
    fi
    word=${words[cword]}
    if [[ $word == --*=* ]]; then
        # bash-completion split it: $cur is the value.
        _aglow_entry "${word%%=*}" && _aglow_value "$entry"
        return
    elif [[ $expect ]] && [[ $cur != --* || ${words[cword - 1]} == = ]]; then
        [[ $cur == = ]] && cur=""
        _aglow_value "$expect"
        return
    elif [[ $cur == -[!-]?* ]] && _aglow_entry "${cur:0:2}" && [[ $entry == *" "* ]]; then
        # A value right after its short option, as -dupload.
        prefix=${cur:0:2}
        cur=${cur:2}
        case ${entry#* } in
        @* | -) ;;
        *) _aglow_value "$entry" ;;
        esac
        return
    fi
    if [[ $cur == -* ]]; then
        local names option
        for entry in "${table[@]}"; do
            names=${entry%% *}
            # --help only alone, and an option that is given once is not offered again.
            [[ $names == "-h|--help" && $given != "|" ]] && continue
            if [[ $names != \** ]]; then
                [[ $given == *"|$names|"* ]] && continue
            fi
            names=${names#\*}
            for option in ${names//|/ }; do
                _aglow_matching "$option"
            done
        done
        return
    fi
    _aglow_files
}
complete -F _aglow aglow
