#!/bin/sh
# SPDX-FileCopyrightText: 2026 fuddlesworth
# SPDX-License-Identifier: LGPL-2.1-or-later
#
# Runs esctest2 (https://github.com/ThomasDickey/esctest2) against afterglow's
# terminal core through afterglow_headless, and compares the failures with
# esctest-expected-failures.txt. Exits non-zero on unexpected failures.
#
#   tools/conformance/esctest.sh BUILD_DIR ESCTEST2_DIR [esctest args...]
#
# Build the runner with -DAFTERGLOW_BUILD_TOOLS=ON. The suite plays xterm 383+
# (checksums and reverse wrap); a run takes about a minute.
set -eu
if [ $# -lt 2 ]; then
    echo "usage: $0 BUILD_DIR ESCTEST2_DIR [esctest args...]" >&2
    exit 2
fi
build=$1
suite=$2
shift 2
here=$(cd "$(dirname "$0")" && pwd)
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT

"$build/tools/conformance/afterglow_headless" -- python3 "$suite/esctest/esctest.py" \
    --expected-terminal xterm --xterm-checksum 400 --xterm-reverse-wrap 383 \
    --timeout 0.1 --no-print-logs --logfile "$work/esctest.log" "$@" >/dev/null 2>&1 || true

grep '^\*\*\* .* tests passed' "$work/esctest.log" || { echo "esctest produced no summary" >&2; exit 1; }
sed -n 's/^\*\*\* TEST [A-Za-z0-9_]*\.\(test_[A-Za-z0-9_]*\) FAILED:.*/\1/p' "$work/esctest.log" | sort -u > "$work/failed"
grep -v '^#' "$here/esctest-expected-failures.txt" | grep . | sort -u > "$work/expected"

unexpected=$(comm -23 "$work/failed" "$work/expected")
fixed=$(comm -13 "$work/failed" "$work/expected")
if [ -n "$fixed" ]; then
    echo "Now passing (remove from esctest-expected-failures.txt):"
    echo "$fixed" | sed 's/^/  /'
fi
if [ -n "$unexpected" ]; then
    echo "Unexpected failures:"
    echo "$unexpected" | sed 's/^/  /'
    exit 1
fi
echo "No unexpected failures."
