#!/bin/sh
# SPDX-FileCopyrightText: 2026 fuddlesworth
# SPDX-License-Identifier: LGPL-2.1-or-later
#
# Runs ucs-detect (https://github.com/jquast/ucs-detect) against afterglow's
# terminal core through afterglow_headless, once with grapheme clustering
# (mode 2027) on and once with it off, and compares the errors with
# ucs-detect-expected.txt. Exits non-zero when a result is worse than the file.
#
#   tools/conformance/ucs-detect.sh BUILD_DIR UCS_DETECT [RESULTS_DIR]
#
# UCS_DETECT is the ucs-detect program, for example the one in a virtual
# environment: VENV/bin/ucs-detect. RESULTS_DIR keeps the JSON results of the
# two runs. Build the runner with -DAFTERGLOW_BUILD_TOOLS=ON. ucs-detect sets
# mode 2027 itself around its tables of characters, so the mode changes only
# the language results. A run takes a few seconds.
set -eu
if [ $# -lt 2 ]; then
    echo "usage: $0 BUILD_DIR UCS_DETECT [RESULTS_DIR]" >&2
    exit 2
fi
headless=$1/tools/conformance/afterglow_headless
ucs=$2
here=$(cd "$(dirname "$0")" && pwd)
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT
results=${3:-$work}
mkdir -p "$results" "$work/home" "$work/run"

# The mode is set before ucs-detect starts. The environment is cleared, so the
# results do not depend on the terminal that runs this script.
cat >"$work/with2027.sh" <<'SH'
#!/bin/sh
mode=$1
shift
printf '\033[?2027%s' "$mode"
exec "$@"
SH
chmod +x "$work/with2027.sh"

for mode in on off; do
    flag=h
    [ "$mode" = off ] && flag=l
    if ! env -i PATH=/usr/bin:/bin HOME="$work/home" XDG_RUNTIME_DIR="$work/run" LANG=C.UTF-8 TERM=xterm-256color \
        "$headless" --cols 120 --rows 40 -- "$work/with2027.sh" "$flag" "$ucs" \
        --set-software-name afterglow --set-software-version dev --no-final-summary \
        --save-json "$results/$mode.json" >"$work/$mode.log" 2>&1; then
        tail -20 "$work/$mode.log" >&2
        echo "ucs-detect did not complete with mode 2027 $mode" >&2
        exit 1
    fi
done

python3 -I - "$here/ucs-detect-expected.txt" "$results/on.json" "$results/off.json" <<'PY'
import json
import sys

# The expected errors: "MODE | KEY | ERRORS | reason". A key that is not in
# the file expects no error.
expected = {}
for number, line in enumerate(open(sys.argv[1], encoding="utf-8"), 1):
    line = line.strip()
    if not line or line.startswith("#"):
        continue
    fields = [f.strip() for f in line.split("|")]
    if len(fields) != 4 or fields[0] not in ("on", "off") or not fields[2].isdigit() or not fields[3]:
        sys.exit(f"ucs-detect-expected.txt:{number}: expected MODE | KEY | ERRORS | reason")
    expected[(fields[0], fields[1])] = int(fields[2])


def results(path):
    data = json.load(open(path, encoding="utf-8"))
    tests = data["test_results"]
    out = {}
    for category, versions in tests.items():
        if not versions:
            continue
        if category == "language_results":
            for name, r in versions.items():
                out["language " + name] = (r.get("n_errors", 0), r.get("n_total", 0))
            continue
        for r in versions.values():
            out[category.removesuffix("_results")] = (r["n_errors"], r["n_total"])
    return out


worse = better = 0
for mode, path in (("on", sys.argv[2]), ("off", sys.argv[3])):
    got = results(path)
    if not got:
        sys.exit(f"ucs-detect with mode 2027 {mode} measured nothing")
    for key, (errors, total) in sorted(got.items()):
        allowed = expected.get((mode, key), 0)
        if errors > allowed:
            worse += 1
            print(f"worse: mode 2027 {mode}, {key}: {errors} of {total} wrong, expected {allowed}")
        elif errors < allowed:
            better += 1
            print(f"better: mode 2027 {mode}, {key}: {errors} of {total} wrong, expected {allowed}")
    for (m, key), allowed in expected.items():
        if m == mode and key not in got:
            print(f"not measured: mode 2027 {mode}, {key} (expected {allowed} errors)")
print(f"ucs-detect: {worse} worse than expected, {better} better")
if better:
    print("Lower the numbers of the better results in ucs-detect-expected.txt.")
sys.exit(1 if worse else 0)
PY
