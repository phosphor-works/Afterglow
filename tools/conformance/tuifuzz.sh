#!/bin/sh
# SPDX-FileCopyrightText: 2026 fuddlesworth
# SPDX-License-Identifier: LGPL-2.1-or-later
#
# Runs afterglow_tuifuzz on each full-screen program that is installed, with
# the same seed and number of steps. Exits non-zero when a check failed for
# any program. A program that is not installed is skipped.
#
#   tools/conformance/tuifuzz.sh BUILD_DIR [STEPS [SEED [RECORD_DIR]]]
#
# Build the tool with -DAFTERGLOW_BUILD_TOOLS=ON. It needs bwrap. Each
# program gets a home of its own with a sample text file. The programs run
# in their restricted modes where they have one (vim -Z, nano --restricted,
# LESSSECURE): the sandbox of the tool keeps the rest inside.
set -eu
if [ $# -lt 1 ]; then
    echo "usage: $0 BUILD_DIR [STEPS [SEED [RECORD_DIR]]]" >&2
    exit 2
fi
fuzz=$1/tools/conformance/afterglow_tuifuzz
steps=${2:-500}
seed=${3:-1}
record=${4:-}
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT

# Lines of each width class: wide, combining, emoji, tabs, long lines.
i=0
while [ $i -lt 300 ]; do
    printf '%4d plain é 中文字 😀 e\314\201 →\tafter a tab %*s\n' $i $((i % 90)) end
    i=$((i + 1))
done >"$work/sample.txt"
printf 'set -g mouse on\nset -g default-command /bin/sh\n' >"$work/tmux.conf"

status=0
# run NAME [tool options] -- command...
run() {
    name=$1
    shift
    if ! command -v "$name" >/dev/null 2>&1; then
        echo "$name: not installed, skipped"
        return
    fi
    home=$work/$name
    mkdir -p "$home"
    cp "$work/sample.txt" "$work/tmux.conf" "$home/"
    if [ -n "$record" ]; then
        set -- --record "$record/$name" "$@"
    fi
    if ! "$fuzz" --seed "$seed" --steps "$steps" --home "$home" "$@"; then
        status=1
    fi
}

run vim -- vim -u NONE -U NONE -N -n -Z -i NONE sample.txt
run nvim -- nvim --clean -n -i NONE sample.txt
run less --env LESSSECURE=1 -- less sample.txt
run nano -- nano --restricted --ignorercfile sample.txt
run tmux --env LESSSECURE=1 -- tmux -f tmux.conf new-session less sample.txt
run btop -- btop
run top -- top
exit $status
