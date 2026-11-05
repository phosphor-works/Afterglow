# SPDX-FileCopyrightText: 2026 fuddlesworth
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Fish completion for afterglow. Script package IDs and their commands come
# from the running afterglow (afterglow run --complete).

set -l subcommands ctl run ssh
set -l ctl_commands ls send-text get-text split move-pane detach-tab new-tab new-window focus close set-title signal notify \
    scripts open-directory projects save-project open-project action watch

# The words of `afterglow run` before the cursor, without its options.
function __afterglow_run_words
    set -l words (commandline -xpc)[3..]
    set -l out
    set -l skip 0
    for word in $words
        if test $skip -eq 1
            set skip 0
            continue
        end
        switch $word
            case -n --no-stdin
            case --pane
                set skip 1
            case '*'
                set -a out $word
        end
    end
    printf '%s\n' $out
end

function __afterglow_run_needs_id
    __fish_seen_subcommand_from run; or return 1
    test (count (__afterglow_run_words)) -eq 0
end

function __afterglow_run_needs_command
    __fish_seen_subcommand_from run; or return 1
    test (count (__afterglow_run_words)) -eq 1
end

function __afterglow_run_commands
    afterglow run --complete (__afterglow_run_words)[1] 2>/dev/null
end

# The window's options, before a subcommand.
complete -c afterglow -n "not __fish_seen_subcommand_from $subcommands" -f -a ctl -d 'Drive a running afterglow'
complete -c afterglow -n "not __fish_seen_subcommand_from $subcommands" -f -a run -d 'Run a command of a script package'
complete -c afterglow -n "not __fish_seen_subcommand_from $subcommands" -f -a ssh -d 'ssh with the remote side prepared'
complete -c afterglow -n "not __fish_seen_subcommand_from $subcommands" -s d -l working-directory -x -a '(__fish_complete_directories)' -d 'Start in the directory'
complete -c afterglow -n "not __fish_seen_subcommand_from $subcommands" -s e -l execute -d 'Run the rest of the command line instead of the shell'
complete -c afterglow -n "not __fish_seen_subcommand_from $subcommands" -l title -x -d 'Give the window a title'
complete -c afterglow -n "not __fish_seen_subcommand_from $subcommands" -l hold -d 'Keep the window open after the program exits'
complete -c afterglow -n "not __fish_seen_subcommand_from $subcommands" -l quick-terminal -d 'Show or hide the quick terminal'
complete -c afterglow -n "not __fish_seen_subcommand_from $subcommands" -l config -r -F -d 'Read settings from the file'
complete -c afterglow -n "not __fish_seen_subcommand_from $subcommands" -s o -l override -x -d 'Override a setting (Group.key=value)'
complete -c afterglow -n "not __fish_seen_subcommand_from $subcommands" -l class -x -d 'Wayland app id for window rules'
complete -c afterglow -n "not __fish_seen_subcommand_from $subcommands" -l config-reference -d 'Print the configuration reference as JSON'
complete -c afterglow -n "not __fish_seen_subcommand_from $subcommands" -s h -l help -d 'Show the help'
complete -c afterglow -n "not __fish_seen_subcommand_from $subcommands" -s v -l version -d 'Show the version'

# afterglow ctl
complete -c afterglow -n "__fish_seen_subcommand_from ctl; and not __fish_seen_subcommand_from $ctl_commands" -f -a "$ctl_commands"
complete -c afterglow -n "__fish_seen_subcommand_from ctl; and __fish_seen_subcommand_from $ctl_commands" -l pane -x -d 'The pane'
complete -c afterglow -n "__fish_seen_subcommand_from ctl; and __fish_seen_subcommand_from get-text" -l screen -l scrollback -l selection -l last-output
complete -c afterglow -n "__fish_seen_subcommand_from ctl; and __fish_seen_subcommand_from split" -l left -l right -l up -l down
complete -c afterglow -n "__fish_seen_subcommand_from ctl; and __fish_seen_subcommand_from new-tab new-window" -l cwd -x -a '(__fish_complete_directories)'
complete -c afterglow -n "__fish_seen_subcommand_from ctl; and __fish_seen_subcommand_from send-text" -l raw -l stdin
complete -c afterglow -n "__fish_seen_subcommand_from ctl; and __fish_seen_subcommand_from ls" -l json
complete -c afterglow -n "__fish_seen_subcommand_from ctl; and __fish_seen_subcommand_from open-directory" -l layout -x
complete -c afterglow -n "__fish_seen_subcommand_from ctl; and __fish_seen_subcommand_from save-project" -l overwrite
complete -c afterglow -n "__fish_seen_subcommand_from ctl; and __fish_seen_subcommand_from open-project" -l new

# afterglow run
complete -c afterglow -n __afterglow_run_needs_id -f -a '(afterglow run --complete 2>/dev/null)' -d 'Script package'
complete -c afterglow -n __afterglow_run_needs_id -s n -l no-stdin -d 'Do not pass standard input to the command'
complete -c afterglow -n __afterglow_run_needs_id -l pane -x -d 'The pane that the command acts on'
complete -c afterglow -n __afterglow_run_needs_command -f -a '(__afterglow_run_commands)'
complete -c afterglow -n __afterglow_run_needs_command -f -l help -d "List the package's commands"

# afterglow ssh: the rest is ssh's command line.
complete -c afterglow -n '__fish_seen_subcommand_from ssh' -f -a '(__fish_complete_subcommand)'
