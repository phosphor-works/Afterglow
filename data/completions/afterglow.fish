# SPDX-FileCopyrightText: 2026 fuddlesworth
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Fish completion for afterglow, for Fish 4 or later. Script IDs and their
# commands come from the running Afterglow, through the aglow that afterglow
# runs (aglow run --complete).
#
# One function prints the candidates for the word at the cursor, so that the
# position of a word decides what it is, as it does for afterglow.

# The keybind actions that ctl action runs (afterglow/src/config/Keybinds.cpp).
set -g __afterglow_actions swap_left swap_right swap_up swap_down rotate_panes_clockwise \
    rotate_panes_counterclockwise pane_overview use_package_shaders move_pane_to_tab move_pane_to_window \
    detach_tab save_project switch_project end_script_key_mode cancel_overlay split_right split_down split_left \
    split_up rename_pane close_pane new_window new_tab close_tab rename_tab move_tab_left move_tab_right \
    goto_tab_1 goto_tab_2 goto_tab_3 goto_tab_4 goto_tab_5 goto_tab_6 goto_tab_7 goto_tab_8 goto_tab_9 \
    close_window toggle_fullscreen next_tab previous_tab show_hints show_last_output copy_mode copy paste \
    select_all clear_screen reset notify_on_finish focus_left focus_right focus_up focus_down equalize_splits \
    toggle_pane_zoom resize_left resize_right resize_up resize_down font_bigger font_smaller font_reset \
    scroll_page_up scroll_page_down scroll_top scroll_bottom jump_prompt_up jump_prompt_down search \
    command_palette reload_config open_config show_urls_open show_urls_copy unicode_input

# Prints the arguments, one on each line, and nothing when there are none.
function __afterglow_print
    string join \n -- $argv
end

# Prints the aglow that afterglow runs: the one in the directory of its
# executable, with symbolic links resolved. aglow starts faster than
# afterglow, which loads Qt. An afterglow with no path (a function of that
# name) gets the aglow on PATH.
function __afterglow_aglow
    set -l exe (commandline -xpc)[1]
    string match -q -- '*/*' $exe; or set exe (command -s -- $exe)
    if not set -q exe[1]
        echo aglow
        return
    end
    set exe (path resolve -- $exe)
    printf '%s/aglow\n' (path dirname -- $exe)
end

function __afterglow_window
    set -l cur (commandline -ct)
    set -l n (count $argv)
    set -l i 1
    while test $i -le $n
        switch $argv[$i]
            case -e --execute
                # The rest is the program and its arguments. Not
                # __fish_complete_subcommand: it would read the word after an
                # --option=value as that option's value.
                set -l rest (string escape -- $argv[(math $i + 1)..])
                complete -C (string join ' ' -- $rest "$cur")
                return
            case -d --working-directory --title --config -o --override --class
                if test $i -eq $n
                    # The cursor is at the option's value.
                    switch $argv[$i]
                        case -d --working-directory
                            __fish_complete_directories "$cur"
                        case --config
                            __fish_complete_path "$cur"
                    end
                    return
                end
                set i (math $i + 1)
            case --config-reference -h --help --help-all -v --version
                # Each of these is the whole command line.
                return
        end
        set i (math $i + 1)
    end
    if test $n -eq 0; and not string match -q -- '-*' "$cur"
        __afterglow_print ctl\t'Drive a running Afterglow' run\t'Run a command of a script' \
            ssh\t'ssh with the remote side prepared' \
            transfer\t'Copy files to and from the computer of the terminal' \
            dnd\t'Drag files and data out of the terminal, and drop them in' \
            icat\t'Show images in the terminal' \
            clipboard\t'Read and write the clipboard, with any MIME type'
        return
    end
    set -l offer
    if test $n -eq 0
        set -a offer --config-reference\t'Print the configuration reference as JSON' \
            -h\t'Show the help' --help\t'Show the help' --help-all\t'Show the help, with the options of Qt' \
            -v\t'Show the version' --version\t'Show the version'
    end
    # The options without their values, for --title=x.
    set -l given (string replace -r -- '=.*' '' $argv)
    # --quick-terminal takes no -e, -d, --title or --hold.
    set -l quick (contains -- --quick-terminal $given; and echo 1)
    set -l window (string match -r -- '^(-d|--working-directory|--title|--hold)$' $given)
    if test -z "$quick"
        contains -- -d $given; or contains -- --working-directory $given
        or set -a offer -d\t'Start in the directory' --working-directory\t'Start in the directory'
        set -a offer -e\t'Run the rest of the command line instead of the shell' \
            --execute\t'Run the rest of the command line instead of the shell'
        contains -- --title $given; or set -a offer --title\t'Give the window a title'
        contains -- --hold $given; or set -a offer --hold\t'Keep the window open after the program exits'
    end
    test -z "$quick" -a -z "$window"; and set -a offer --quick-terminal\t'Show or hide the quick terminal'
    contains -- --config $given; or set -a offer --config\t'Read settings from the file'
    set -a offer -o\t'Override a setting (Group.key=value)' --override\t'Override a setting (Group.key=value)'
    contains -- --class $given; or set -a offer --class\t'Wayland app id for window rules'
    __afterglow_print $offer
end

function __afterglow_ctl
    set -l cur (commandline -ct)
    if not set -q argv[1]
        if string match -q -- '-*' "$cur"
            __afterglow_print -h\t'Show the usage' --help\t'Show the usage'
        else
            __afterglow_print ls\t'List panes' send-text\t'Type text into the pane' get-text\t'Print pane text' \
                split\t'Split the pane' move-pane\t'Move the pane beside another' \
                detach-tab\t"Move the pane's tab to a new window" new-tab\t'Open a tab' new-window\t'Open a window' \
                focus\t'Focus the pane' close\t'Close the pane' set-title\t"Set the pane's title" \
                signal\t'Set a shader signal' notify\t'Show a desktop notification' \
                scripts\t'Show script diagnostics' open-directory\t'Open a directory' \
                projects\t'List saved projects' save-project\t'Save the window layout' \
                open-project\t'Open a saved project' action\t'Run a keybind action or start a script command' \
                watch\t'Print events' overlay\t'Run a command over the pane and print its result' \
                screenshot\t'Save the pane, its tab or its window as PNG'
        end
        return
    end
    set -l command $argv[1]
    set -e argv[1]
    set -l n (count $argv)
    set -l positional 0
    set -l i 1
    while test $i -le $n
        switch $argv[$i]
            case --
                # A command to run follows, or text, or nothing that ctl takes.
                if contains -- $command split new-tab new-window overlay
                    set -l rest (string escape -- $argv[(math $i + 1)..])
                    complete -C (string join ' ' -- $rest "$cur")
                end
                return
            case --pane --cwd --layout --capture --title -w --width -h --height --position
                # The options after --layout are those of overlay. Elsewhere they are no option of ctl.
                if test $command = overlay; or not contains -- $argv[$i] --capture --title -w --width -h --height --position
                    if test $i -eq $n
                        # The cursor is at the option's value.
                        test $argv[$i] = --cwd; and __fish_complete_directories "$cur"
                        test $argv[$i] = --capture; and __afterglow_print stdout file none
                        test $argv[$i] = --position
                        and __afterglow_print center top bottom left right top-left top-right bottom-left bottom-right
                        return
                    end
                    set i (math $i + 1)
                else if contains -- $argv[$i] -w -h
                    # Words outside overlay, as -x.png is.
                    set positional (math $positional + 1)
                end
            case '-'
                # A lone - is a word for every command: the standard output of
                # screenshot, text for send-text.
                set positional (math $positional + 1)
            case '--*'
            case '-*'
                # -x.png is a word too, except where overlay takes -n, -w and -h.
                test $command != overlay; and set positional (math $positional + 1)
            case '*'
                set positional (math $positional + 1)
        end
        set i (math $i + 1)
    end
    if not string match -q -- '-*' "$cur"
        switch $command:$positional
            case open-directory:0
                __fish_complete_directories "$cur"
                return
            case screenshot:0
                __fish_complete_path "$cur"
                return
            case move-pane:1
                __afterglow_print left right up down new-tab new-window
                return
            case signal:0
                __afterglow_print 0 1 2 3 4 5 6 7
                return
            case action:0
                __afterglow_print $__afterglow_actions
                # The word at the cursor is text, not a pattern: compared, not matched.
                if string match -q -- 'script:*' "$cur"
                    or test "$cur" = "$(string sub -l (string length -- "$cur") -- script:)"
                    # Without that aglow, fish would print "Unknown command" on the line.
                    set -l aglow (__afterglow_aglow)
                    command -q -- $aglow; or return
                    # One connection: ID<TAB>COMMAND<TAB>SUMMARY lines.
                    for entry in ($aglow run --complete --all 2>/dev/null)
                        set -l fields (string split -m 2 \t -- $entry)
                        test -n "$fields[1]" -a -n "$fields[2]"; or continue
                        # printf, not string replace: a '$' in the id would be a group in the replacement.
                        printf 'script:%s.%s\t%s\n' $fields[1] $fields[2] "$fields[3]"
                    end
                end
                return
        end
    end
    set -l offer
    if not contains -- $command ls watch scripts projects open-directory open-project new-window
        contains -- --pane $argv; or set -a offer --pane\t'The pane'
    end
    switch $command
        case ls
            contains -- --json $argv; or set -a offer --json\t'Print JSON'
        case send-text
            # --stdin takes no text, and the text takes no --stdin.
            if not contains -- --stdin $argv
                contains -- --raw $argv; or set -a offer --raw\t'Send the text as it is'
                test $positional -eq 0; and not contains -- --raw $argv
                and set -a offer --stdin\t'Send standard input'
            end
        case get-text
            string match -q -r -- '^--(screen|scrollback|selection|last-output)$' $argv
            or set -a offer --screen\t'The screen' --scrollback\t'The scrollback' --selection\t'The selection' \
                --last-output\t'The output of the last command'
        case screenshot
            string match -q -r -- '^--(tab|window)$' $argv
            or set -a offer --tab\t'The panes of its tab' --window\t'The whole window'
        case split
            string match -q -r -- '^--(left|right|up|down)$' $argv
            or set -a offer --left --right --up --down
        case new-tab new-window
            contains -- --cwd $argv; or set -a offer --cwd\t'Working directory'
        case overlay
            contains -- --capture $argv; or set -a offer --capture\t'Where the result comes from'
            contains -- --cwd $argv; or set -a offer --cwd\t'Working directory'
            contains -- --title $argv; or set -a offer --title\t"The pane's title while the command runs"
            contains -- -n $argv; or contains -- --no-stdin $argv
            or set -a offer -n\t'Keep standard input from the command' \
                --no-stdin\t'Keep standard input from the command'
            contains -- -w $argv; or contains -- --width $argv
            or set -a offer -w\t'A popup this wide, in cells or N%' --width\t'A popup this wide, in cells or N%'
            contains -- -h $argv; or contains -- --height $argv
            or set -a offer -h\t'A popup this tall, in cells or N%' --height\t'A popup this tall, in cells or N%'
            contains -- --position $argv; or set -a offer --position\t'Where the popup sits'
            contains -- --border $argv; or contains -- --no-border $argv
            or set -a offer --border\t'Draw a border' --no-border\t'Draw no border'
        case open-directory
            contains -- --layout $argv; or set -a offer --layout\t'A saved layout'
        case save-project
            contains -- --overwrite $argv; or set -a offer --overwrite\t'Replace a project with the name'
        case open-project
            contains -- --new $argv; or set -a offer --new\t'Launch the layout again'
    end
    __afterglow_print $offer
end

function __afterglow_complete
    set -l words (commandline -xpc)[2..]
    switch "$words[1]"
        case ssh
            # The rest is ssh's command line.
            __fish_complete_subcommand
        case ctl
            __afterglow_ctl $words[2..]
        case run transfer dnd icat clipboard
            # These run aglow, which completes the same arguments. Fish loads
            # the completion of aglow only when aglow is on PATH, and afterglow
            # runs the one next to itself: load it here. aglow.fish can load
            # twice, and loads its completions once. The line names that
            # aglow, whose run --complete lists the script commands.
            if not functions -q __aglow_command
                for dir in $fish_complete_path
                    if test -f $dir/aglow.fish
                        source $dir/aglow.fish
                        break
                    end
                end
            end
            if functions -q __aglow_command
                set -l aglow (__afterglow_aglow)
                complete -C "$(string join ' ' -- (string escape -- $aglow $words)) $(commandline -ct)"
            else
                # No completion of aglow: its arguments are files, most likely.
                __fish_complete_path (commandline -ct)
            end
        case '*'
            __afterglow_window $words
    end
end

complete -c afterglow -f -a '(__afterglow_complete)'
