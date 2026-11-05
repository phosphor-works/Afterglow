# SPDX-FileCopyrightText: 2026 fuddlesworth
# SPDX-License-Identifier: LGPL-2.1-or-later
# Fish completion for aglow: its commands, their options and the values of the options.

set -l commands transfer dnd icat clipboard
complete -c aglow -f
complete -c aglow -n "not __fish_seen_subcommand_from $commands" -a transfer -d 'Copy files to and from the computer of the terminal'
complete -c aglow -n "not __fish_seen_subcommand_from $commands" -a dnd -d 'Drag files and data out of the terminal, and drop them in'
complete -c aglow -n "not __fish_seen_subcommand_from $commands" -a icat -d 'Show images in the terminal'
complete -c aglow -n "not __fish_seen_subcommand_from $commands" -a clipboard -d 'Read and write the clipboard, with any MIME type'
complete -c aglow -n "not __fish_seen_subcommand_from $commands" -l version -s v -d 'Show the version'
complete -c aglow -n "not __fish_seen_subcommand_from $commands" -l help -s h -d 'Show the help'
complete -c aglow -n "__fish_seen_subcommand_from $commands" -l help -d 'Show the help'

set -l t "__fish_seen_subcommand_from transfer"
complete -c aglow -n $t -F
complete -c aglow -n $t -s d -l direction -x -a 'upload download send receive' -d 'Send or receive files'
complete -c aglow -n $t -s m -l mode -x -a 'normal mirror' -d 'How to read the arguments'
complete -c aglow -n $t -l compress -x -a 'auto never always' -d 'Compress the data'
complete -c aglow -n $t -s p -l permissions-bypass -r -d 'Password that skips the prompt'
complete -c aglow -n $t -s c -l confirm-paths -d 'Confirm where each file goes'
complete -c aglow -n $t -s x -l transmit-deltas -d 'Send only the changes'

set -l d "__fish_seen_subcommand_from dnd"
complete -c aglow -n $d -F
complete -c aglow -n $d -l drag -x -d 'Offer a file as a MIME type (MIME:PATH)'
complete -c aglow -n $d -l drop -x -d 'Write a dropped MIME type to a file (MIME:PATH)'
complete -c aglow -n $d -l drop-dest -x -a '(__fish_complete_directories)' -d 'Directory for dropped files'
complete -c aglow -n $d -l copy-mode -x -a 'auto independent' -d 'How local files are copied'
complete -c aglow -n $d -l confirm-drop-overwrite -d 'Ask before replacing files'
complete -c aglow -n $d -l drop-anywhere -x -a 'disallowed copy move' -d 'Accept drops anywhere'
complete -c aglow -n $d -l drag-thumbnail -r -d 'Image under the pointer'
complete -c aglow -n $d -l drag-thumbnail-size -x -d 'Largest side of the drag image'
complete -c aglow -n $d -l exit-on -x -a 'drag-finish drop-finish esc-key' -d 'Events to exit on'

set -l i "__fish_seen_subcommand_from icat"
complete -c aglow -n $i -F
complete -c aglow -n $i -l align -x -a 'center left right' -d 'Horizontal alignment'
complete -c aglow -n $i -l place -x -d 'Where to show the image (WIDTHxHEIGHT@LEFTxTOP)'
complete -c aglow -n $i -l scale-up -d 'Scale small images up'
complete -c aglow -n $i -l fit -x -a 'width height both none' -d 'How to fit the screen'
complete -c aglow -n $i -l background -x -d 'Background colour'
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
complete -c aglow -n $i -s n -l no-trailing-newline -d 'Leave the cursor after the image'

set -l c "__fish_seen_subcommand_from clipboard"
complete -c aglow -n $c -F
complete -c aglow -n $c -s g -l get-clipboard -d 'Write the clipboard out'
complete -c aglow -n $c -s p -l use-primary -d 'Use the primary selection'
complete -c aglow -n $c -s m -l mime -x -d 'MIME type of a file'
complete -c aglow -n $c -s a -l alias -x -d 'Two MIME types for the same data'
complete -c aglow -n $c -l wait-for-completion -d 'Wait until the terminal has the data'
complete -c aglow -n $c -l password -x -d 'Password for the clipboard'
complete -c aglow -n $c -l human-name -x -d 'Name shown with the permission prompt'
complete -c aglow -n $c -l detection-timeout -x -d 'Seconds to wait'
