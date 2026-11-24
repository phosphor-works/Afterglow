---
name: ag-debug
description: "Find why an Afterglow test or program crashes or hangs: gdb in batch mode, watchpoints for memory that something overwrote, cores with coredumpctl, and gdb in a pane of the afterglow MCP server when the questions span several turns. Use when: crash, segfault, SIGSEGV, SIGABRT, hang, deadlock, core dump, gdb, backtrace, a test that fails in some runs only."
---

<!--
SPDX-FileCopyrightText: 2026 fuddlesworth
SPDX-License-Identifier: GPL-3.0-or-later
-->

# Debug a crash or a hang

A fix explains why the bug happens (CLAUDE.md, "Find the mechanism"). The backtrace of a crash shows where the
program fell, which is often not where the bad value came from. Find the write that made the value.

Debug only the processes that you started: a test binary, a program from `build/`, or a test's own Afterglow.
Never attach to the user's `afterglow` or `aglow-mux`. A program that opens a window runs only in a nested KWin
(`ag-verify-live`).

## Batch mode first

One `gdb -batch` call answers most questions. It took 3 tool calls and 20 seconds in a measurement, where gdb in
a pane took 26 calls and 3 minutes for the same answer.

A crash in one test:

```sh
gdb -q -batch -ex run -ex bt -ex 'info locals' --args build/afterglow/tests/afterglow_config_tests \
    --gtest_filter='Config.TheTestThatCrashes'
```

`ctest --test-dir build -R NAME -V` prints the command line of a test, with its environment.

A value that something overwrote: stop before the write, watch the memory, and let the program run. The
watchpoint stops at the write, with the stack that made it.

```sh
gdb -q -batch -ex 'break functionBeforeTheWrite' -ex run -ex 'watch -l object.field' -ex continue -ex bt \
    --args PROGRAM ARGS
```

`watch -l` watches the address, so it outlives the scope of the expression. `ptype /o TYPE` gives the offsets of
the fields: an overflow from one field reaches the next.

A hang: start the program in the background, find its PID with `pgrep -a NAME`, and take the stacks of all its
threads:

```sh
gdb -q -batch -p PID -ex 'thread apply all bt'
```

## A crash in some runs only

Do not rerun until it passes. Read the core of a run that failed, then write a test that makes the crash happen
every time (CLAUDE.md, the native suite lesson).

```sh
coredumpctl list --since today
coredumpctl debug PID --debugger-arguments="-q -batch -ex 'thread apply all bt'"
```

A memory error that does not crash every time: build `build-debug` with ASan and UBSan (`ag-build`) and run the
test there. ASan names the write and the allocation.

## gdb in a pane of the agent session

The `afterglow` MCP server (`.mcp.json`) keeps a program running between turns. Use a pane for gdb only when the
questions come one after the other over several turns: a breakpoint that leads to the next one, or a state that
you want to look at again after you read the code.

1. Open a pane in the repository: `open_pane` with `where` `tab` and `cwd` the repository root.
2. Start gdb with `send` and `text` `gdb -q --args PROGRAM ARGS` and a line end.
3. Wait for its prompt: `wait` with `until` `text` and `pattern` `^\(gdb\) $`. After a send, the wait reads only
   what gdb writes after your input, so the old prompt does not match.
4. Send each command the same way, wait for the prompt, and `read` the screen when the wait's output is too short.
5. End with `quit` and `close_pane`.

A pane runs in the cgroup of the user's `aglow-mux.service`. A program that can fill the memory runs in
`systemd-run --user --scope -p MemoryMax=6G -p MemorySwapMax=0` there too.
