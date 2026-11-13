#!/bin/sh
# SPDX-FileCopyrightText: 2026 fuddlesworth
# SPDX-License-Identifier: GPL-3.0-or-later
#
# A fake ssh for the tests of remote commands. It writes each run's words to
# FAKE_SSH_LOG, separated by 0x1f, one run on each line, and acts as the
# FAKE_* variables say: the probe's report, the exit status of -O check
# and -O forward, an -O forward that ends only once the file
# FAKE_FORWARD_GATE is there, and a session that waits for the file
# FAKE_RELEASE. With FAKE_FORWARD_LINK, -O forward -R REMOTE:LOCAL links
# REMOTE to LOCAL, as the forward makes REMOTE reach LOCAL, and -O cancel
# removes the link. With FAKE_SESSION, the session runs that shell
# command.
# ssh -G reports what real ssh does without a ControlPath, or with the
# user's ControlPath FAKE_CONTROL_PATH, for the destination it is given,
# with RequestTTY force (FAKE_TTY), as -tt would: the helper runs without
# a terminal here. The helper's second ssh -G reads the user's files
# through -F /dev/stdin, and its own Host * block last: the fake takes
# that block's values, with %C as "fakehash", unless FAKE_USER sets
# ControlPath none or ControlMaster no ("path-none", "master-no"), or
# FAKE_CONTROL_PATH is set. A session whose test failed before it
# released it ends when its parent has gone, or after 3000 waits of 20 ms:
# it never outlives the test for long.
{ for a in "$@"; do printf '%s\037' "$a"; done; printf '\n'; } >> "$FAKE_SSH_LOG"
op=; prev=; last=
for a in "$@"; do
    [ "$prev" = "-O" ] && op=$a
    prev=$a; last=$a
done
for a in "$@"; do
    case "$a" in -G)
        printf 'host %s\nhostname %s.example\nuser me\nport 22\ncontrolpersist no\nrequesttty %s\n' "$last" "$last" "${FAKE_TTY:-force}"
        [ -n "${FAKE_CONTROL_PATH:-}" ] && printf 'controlpath %s\n' "$FAKE_CONTROL_PATH"
        case " $* " in
            *" /dev/stdin "*)
                own=$(sed -n 's/^ *ControlPath \(.*\)$/\1/p; s/^ *ControlMaster \(.*\)$/master \1/p')
                case "${FAKE_USER:-}${FAKE_CONTROL_PATH:-}" in
                    path-none|/*) ;;
                    *) printf 'controlpath %s\n' "$(printf '%s\n' "$own" | sed -n '1s/%C/fakehash/p')";;
                esac
                case "${FAKE_USER:-}" in
                    master-no) printf 'controlmaster false\n';;
                    *) printf 'controlmaster %s\n' "$(printf '%s\n' "$own" | sed -n 's/^master //p')";;
                esac;;
            *) printf 'controlmaster false\n';;
        esac
        exit 0;;
    esac
done
case "$op" in
    check) exit "${FAKE_CHECK:-0}";;
    forward)
        if [ -n "${FAKE_FORWARD_LINK:-}" ]; then
            for a in "$@"; do [ "$link" = -R ] && ln -s "${a#*:}" "${a%%:*}"; link=$a; done
        fi
        n=0
        while [ -n "${FAKE_FORWARD_GATE:-}" ] && [ ! -e "$FAKE_FORWARD_GATE" ]; do
            n=$((n + 1)); [ "$n" -gt 3000 ] && exit 8
            sleep 0.02
        done
        exit "${FAKE_FORWARD:-0}";;
    cancel)
        if [ -n "${FAKE_FORWARD_LINK:-}" ]; then
            for a in "$@"; do [ "$link" = -R ] && rm -f "${a%%:*}"; link=$a; done
        fi
        exit 0;;
    ?*) exit 0;;
esac
case "$last" in
    "sh -c "*)
        printf 'runtime=/run/user/1000\nhome=/home/me\n'
        case "${FAKE_AGLOW:-same}" in
            same) printf 'aglow=aglow 0.0.0 (protocol %s)\ndir=%s\n' "$FAKE_PROTOCOL" "${FAKE_DIR:-/run/user/1000/afterglow}";;
            other) printf 'aglow=aglow 9.9.9 (protocol 99)\ndir=%s\n' "${FAKE_DIR:-/run/user/1000/afterglow}";;
            another) printf 'aglow=aglow 9.9.8 (protocol 98)\ndir=%s\n' "${FAKE_DIR:-/run/user/1000/afterglow}";;
        esac
        exit "${FAKE_PROBE_STATUS:-0}";;
esac
[ -n "${FAKE_SESSION:-}" ] && exec /bin/sh -c "$FAKE_SESSION"
: > "$FAKE_RELEASE.started"
parent=$PPID; n=0
while [ ! -e "$FAKE_RELEASE" ]; do
    kill -0 "$parent" 2>/dev/null || exit 9
    n=$((n + 1)); [ "$n" -gt 3000 ] && exit 8
    sleep 0.02
done
exit 7
