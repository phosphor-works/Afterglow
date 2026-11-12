# SPDX-FileCopyrightText: 2026 fuddlesworth
# SPDX-License-Identifier: LGPL-2.1-or-later
# Fish completion for aglow, for Fish 4 or later: its commands, their options and the values of the options.
# Script IDs and their commands come from the running Afterglow (aglow run --complete).
# afterglow run, transfer, dnd, icat and clipboard use it too (afterglow.fish).

# afterglow.fish sources this file, and fish can load it again when aglow is on PATH: the completions of the second
# load replace those of the first, or each would run, and list the scripts, once more.
complete -e -c aglow

# The options of each command that take a value, as the next word or after
# "=", from the declarations below.
set -g __aglow_values_transfer -d --direction -m --mode --compress -p --permissions-bypass
set -g __aglow_values_dnd --drag --drop --drop-dest --copy-mode --confirm-drop-timeout --drop-anywhere \
    --drag-thumbnail --drag-thumbnail-size --exit-on
set -g __aglow_values_icat --align --place --fit --background --mirror --transfer-mode --detection-timeout \
    --use-window-size --stdin -z --z-index -l --loop --passthrough --image-id --engine
set -g __aglow_values_clipboard -m --mime -a --alias --password --human-name --detection-timeout

# Whether the command of the line is $argv[1], or with no argument whether
# the line has no command yet. The command is the first word after aglow
# only: a file of the same name later on the line is not one. After -h or
# --help aglow shows the help and reads no other word, so the command
# completes nothing more. The word after an option that takes a value is
# that value, -h too, but not a word that starts with "--", which aglow
# never takes as a value.
function __aglow_command
    set -l words (commandline -xpc)
    if not set -q argv[1]
        test (count $words) -eq 1
        return
    end
    test (count $words) -ge 2; and test "$words[2]" = $argv[1]; or return
    set -l values __aglow_values_$argv[1]
    set -l value 0
    for word in $words[3..]
        if test $value -eq 1
            set value 0
            string match -q -- '--*' $word; or continue
        end
        switch $word
            case --
                return 0
            case -h --help
                return 1
            case '--*=*'
            case '--*'
                if contains -- $word $$values
                    set value 1
                end
            case '-*'
                # Flags together (-cp): a short option that takes a value
                # takes the rest of the word, or the next word at its end.
                set -l letters (string split '' -- (string sub -s 2 -- $word))
                for k in (seq (count $letters))
                    if contains -- -$letters[$k] $$values
                        if test $k -eq (count $letters)
                            set value 1
                        end
                        break
                    end
                end
        end
    end
    # The status of the last test above is no answer.
    return 0
end

# Whether the word at the cursor is a file operand. A word that starts with -
# is an option, and fish completes the part after = of a flag (--hold=) as a
# word of its own: a flag takes only a yes or no value there, which the
# completion does not offer, and no file. After "--" every word is a file.
function __aglow_operand
    contains -- -- (commandline -xpc); and return
    not string match -q -- '-*' (commandline -ct)
end

# The password of -p: a file only when the word starts with ., / or ~, as aglow
# reads it. Any other word is the password itself.
function __aglow_password
    set -l cur (commandline -ct)
    string match -q -r -- '^[./~]' "$cur"; and __fish_complete_path "$cur"
end

# The events of --exit-on, separated by commas: the ones given stay, and each
# is offered once.
function __aglow_events
    set -l cur (commandline -ct | string replace -r -- '^--exit-on=' '')
    set -l given (string split , -- $cur)
    set -e given[-1]
    set -l prefix (string join , -- $given)
    test -n "$prefix"; and set prefix "$prefix,"
    for event in drag-finish drop-finish esc-key
        contains -- $event $given; or printf '%s%s\n' $prefix $event
    end
end

# aglow run: options, then the script ID, then its command and the
# command's own arguments. The aglow of the line lists the script commands:
# for afterglow run, afterglow.fish names the aglow that afterglow runs.
function __aglow_run
    set -l words (commandline -xpc)
    set -l program $words[1]
    set -l cur (commandline -ct)
    set -l id
    set -l command
    set -l options 1
    set -l n (count $words)
    set -l i 3
    while test $i -le $n
        set -l word $words[$i]
        if test -n "$options"; and not set -q id[1]
            switch $word
                case -n --no-stdin
                    set i (math $i + 1)
                    continue
                case --pane
                    # At the cursor: the pane number.
                    test $i -eq $n; and return
                    set i (math $i + 2)
                    continue
                case --
                    set options
                    set i (math $i + 1)
                    continue
                case '-*'
                    # -h and --help only alone, and any other option is unknown.
                    return
            end
        end
        if not set -q id[1]
            set id $word
        else
            set command $word
            break
        end
        set i (math $i + 1)
    end
    if set -q command[1]
        # The command's own arguments: files, most likely.
        contains -- $command -h --help; or __fish_complete_path "$cur"
    else if set -q id[1]
        printf '%s\n' --help\t"List the script's commands"
        # Without that aglow, fish would print "Unknown command" on the line.
        command -q -- $program; and $program run --complete $id 2>/dev/null
    else if test -n "$options"; and string match -q -- '-*' "$cur"
        # --help is offered only first, as for the other commands.
        test $n -eq 2; and printf '%s\n' -h\t'Show the usage' --help\t'Show the usage'
        contains -- -n $words; or contains -- --no-stdin $words
        or printf '%s\n' -n\t'Do not pass standard input to the command' \
            --no-stdin\t'Do not pass standard input to the command'
        contains -- --pane $words; or printf '%s\n' --pane\t'The pane that the command acts on'
    else if command -q -- $program
        $program run --complete 2>/dev/null | string replace -r '$' \tScript
    end
end

# Whether the command of the line is run.
function __aglow_is_run
    set -l words (commandline -xpc)
    test (count $words) -ge 2; and test "$words[2]" = run
end

complete -c aglow -f
complete -c aglow -n '__aglow_command' -a transfer -d 'Copy files to and from the computer of the terminal'
complete -c aglow -n '__aglow_command' -a dnd -d 'Drag files and data out of the terminal, and drop them in'
complete -c aglow -n '__aglow_command' -a icat -d 'Show images in the terminal'
complete -c aglow -n '__aglow_command' -a clipboard -d 'Read and write the clipboard, with any MIME type'
complete -c aglow -n '__aglow_command' -a run -d 'Run a command of a script in a running Afterglow'
complete -c aglow -n '__aglow_command' -a help -d 'Show the help, or the help of a command'
complete -c aglow -n '__aglow_command' -a version -d 'Show the version'
complete -c aglow -n '__aglow_command' -s v -l version -d 'Show the version'
complete -c aglow -n '__aglow_command' -s h -l help -d 'Show the help'
complete -c aglow -n '__aglow_command help; and test (count (commandline -xpc)) -eq 2' -a 'transfer dnd icat clipboard run'
complete -c aglow -n __aglow_is_run -a '(__aglow_run)'
for command in transfer dnd icat clipboard
    complete -c aglow -n "__aglow_command $command" -s h -l help -d 'Show the help'
end

set -l t '__aglow_command transfer'
complete -c aglow -n "$t; and __aglow_operand" -F
complete -c aglow -n $t -s d -l direction -x -a 'upload download send receive' -d 'Send or receive files'
complete -c aglow -n $t -s m -l mode -x -a 'normal mirror' -d 'How to read the arguments'
complete -c aglow -n $t -l compress -x -a 'auto never always' -d 'Compress the data'
complete -c aglow -n $t -s p -l permissions-bypass -x -a '(__aglow_password)' -d 'Password that skips the prompt'
complete -c aglow -n $t -s c -l confirm-paths -d 'Confirm where each file goes'
complete -c aglow -n $t -s x -l transmit-deltas -d 'Send only the changes'

set -l d '__aglow_command dnd'
complete -c aglow -n "$d; and __aglow_operand" -F
complete -c aglow -n $d -l drag -x -d 'Offer a file as a MIME type (MIME:PATH)'
complete -c aglow -n $d -l drop -x -d 'Write a dropped MIME type to a file (MIME:PATH)'
complete -c aglow -n $d -l drop-dest -x -a '(__fish_complete_directories)' -d 'Directory for dropped files'
complete -c aglow -n $d -l copy-mode -x -a 'auto independent' -d 'How local files are copied'
complete -c aglow -n $d -l confirm-drop-overwrite -d 'Ask before replacing files'
complete -c aglow -n $d -l confirm-drop-timeout -x -d 'Seconds that the question waits'
complete -c aglow -n $d -l drop-anywhere -x -a 'disallowed copy move' -d 'Accept drops anywhere'
complete -c aglow -n $d -l drag-thumbnail -r -d 'Image under the pointer'
complete -c aglow -n $d -l drag-thumbnail-size -x -d 'Largest side of the drag image'
complete -c aglow -n $d -l exit-on -x -a '(__aglow_events)' -d 'Events to exit on'

set -l i '__aglow_command icat'
complete -c aglow -n "$i; and __aglow_operand" -F
complete -c aglow -n $i -l align -x -a 'center left right' -d 'Horizontal alignment'
complete -c aglow -n $i -l place -x -d 'Where to show the image (WIDTHxHEIGHT@LEFTxTOP)'
complete -c aglow -n $i -l scale-up -d 'Scale small images up'
complete -c aglow -n $i -l fit -x -a 'width height both none' -d 'How to fit the screen'
complete -c aglow -n $i -l background -x -d 'Background color'
complete -c aglow -n $i -l mirror -x -a 'none horizontal vertical both' -d 'Mirror the image'
complete -c aglow -n $i -l clear -d 'Remove the images on the screen'
complete -c aglow -n $i -l clear-all -d 'Remove every image'
complete -c aglow -n $i -l transfer-mode -x -a 'detect file stream memory' -d 'How images reach the terminal'
complete -c aglow -n $i -l detect-support -d 'Ask whether the terminal shows images'
complete -c aglow -n $i -l detection-timeout -x -d 'Seconds to wait'
complete -c aglow -n $i -l use-window-size -x -d 'Window size (COLUMNS,ROWS,WIDTH,HEIGHT)'
complete -c aglow -n $i -l print-window-size -d 'Print the window size in pixels'
complete -c aglow -n $i -l stdin -x -a 'detect yes no' -d 'Read standard input'
complete -c aglow -n $i -s z -l z-index -x -d 'Z-index of the image'
complete -c aglow -n $i -s l -l loop -x -d 'Times an animation plays'
complete -c aglow -n $i -l hold -d 'Wait for a key press'
complete -c aglow -n $i -l unicode-placeholder -d 'Show images through placeholder characters'
complete -c aglow -n $i -l passthrough -x -a 'detect tmux none' -d 'Pass through tmux'
complete -c aglow -n $i -l image-id -x -d 'Graphics protocol id'
complete -c aglow -n $i -l engine -x -a 'auto builtin' -d 'Engine that decodes images'
complete -c aglow -n $i -l silent -d 'Not used, for compatibility'
complete -c aglow -n $i -s n -l no-trailing-newline -d 'Leave the cursor after the image'

set -l c '__aglow_command clipboard'
complete -c aglow -n "$c; and __aglow_operand" -F
complete -c aglow -n $c -s g -l get-clipboard -d 'Write the clipboard out'
complete -c aglow -n $c -s p -l use-primary -d 'Use the primary selection'
complete -c aglow -n $c -s m -l mime -x -d 'MIME type of a file'
complete -c aglow -n $c -s a -l alias -x -d 'Two MIME types for the same data'
complete -c aglow -n $c -l wait-for-completion -d 'Wait until the terminal has the data'
complete -c aglow -n $c -l password -x -d 'Password for the clipboard'
complete -c aglow -n $c -l human-name -x -d 'Name shown with the permission prompt'
complete -c aglow -n $c -l detection-timeout -x -d 'Seconds to wait'
