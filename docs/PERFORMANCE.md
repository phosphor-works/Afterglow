<!-- SPDX-FileCopyrightText: 2026 Phosphor Works Team -->
<!-- SPDX-License-Identifier: GPL-3.0-or-later -->

# Native terminal performance

This page gives the performance measurements of Afterglow and their test conditions, for developers who change the terminal core or the renderer.
The native benchmark measures the application in an isolated KWin desktop.
The headless VT benchmark measures the terminal core without a window.
See [testing](TESTING.md#native-performance-measurements) for the commands of the native benchmark.
See the [Phosphor example](../examples/phosphor/README.md#performance) for the shader scene measurements.

## Test conditions

The native benchmark tables use this machine and these settings.

| Condition | Value |
| --- | --- |
| CPU | Intel Core i7-9700K, 8 cores |
| GPU | NVIDIA GeForce RTX 2080, driver 615.78.08 |
| System | CachyOS 7.3.0-4, Qt 6.12.0, nested KWin 6.8.1 |
| Build type | `RelWithDebInfo` |
| Viewport | Exact 1920×1080 and 3840×2160 windows, scale 1 |
| Font | Monaspace Neon Var, 11 points, grayscale antialiasing, light hinting |
| Appearance | Basic controls, generic platform theme, opaque background, no shaders or audio |
| Power | 225 W GPU limit, normal power management, shared desktop GPU, unlocked clocks |

The native measurements come from two data sets.

| Data set | Runs | Workloads | Files |
| --- | --- | --- | --- |
| Native | 72 case runs: three repeats of each workload at each resolution | All workloads except resize | [Summary and ranges](../data/benchmarks/native-2026-10-30.summary.json), [compressed raw samples](../data/benchmarks/native-2026-10-30.raw.json.gz) |
| Resize | Three repeats of the resize workload at each resolution | Resize | [Summary and ranges](../data/benchmarks/resize-optimized-2026-10-30.summary.json), [compressed raw samples](../data/benchmarks/resize-optimized-2026-10-30.raw.json.gz) |

Timed cases request 15 seconds. CPU rates use actual elapsed time.
Input cases use 100 acknowledgments per run. Script cases use 10,000 callbacks per run.
Tables show medians of per-run statistics. Maximum columns contain the largest observed sample.
Percentiles are not pooled across runs.

The headless VT benchmark tables give their own conditions.

## CPU and frame service

CPU percentages use 100% for one full core.

| Workload | 1080p CPU (%) | 4K CPU (%) | 1080p frame p95 (ms) | 4K frame p95 (ms) |
| --- | ---: | ---: | ---: | ---: |
| Idle | 0.09 | 0.08 | — | — |
| Idle with blink | 0.31 | 0.52 | 0.96 | 1.98 |
| Scroll, 100,000 rows | 6.79 | 12.79 | 1.29 | 2.71 |
| Resize, 100,000 rows | 40.32 | 46.09 | 22.85 | 24.58 |

Static idle submitted no frames in all six runs.
Scroll frame-interval p95 is 33.56 ms at 1080p and 33.60 ms at 4K.
The resize rows come from the resize data set.

## Terminal GPU timing and allocation

GPU elapsed time is asynchronous. Allocation is the largest external process sample across all repeats.

| Workload | 1080p GPU p95 (ms) | 4K GPU p95 (ms) | 1080p allocation (MiB) | 4K allocation (MiB) |
| --- | ---: | ---: | ---: | ---: |
| Idle with blink | 0.11 | 0.33 | 71.00 | 137.00 |
| Scroll | 0.13 | 0.40 | 81.00 | 169.00 |
| Resize | 0.21 | 0.74 | 91.00 | 201.00 |

The driver supplied process activity for only some external samples.
The table below pools available SM activity readings across three repeats.
Coverage is the number of numerical readings divided by all external samples.
These medians exclude missing readings. They are not time-weighted utilization.
Vendor readings can repeat between polls.

| Workload | 1080p coverage | 1080p SM median / max (%) | 4K coverage | 4K SM median / max (%) |
| --- | ---: | ---: | ---: | ---: |
| Scroll | 117 / 177 | 21 / 41 | 79 / 177 | 25 / 37 |
| Resize | 10 / 178 | 16 / 33 | 4 / 180 | 21 / 21 |
| Input | 13 / 121 | 17 / 29 | 1 / 123 | 20 / 20 |
| Input with output | 85 / 123 | 20 / 41 | 52 / 123 | 17 / 39 |

Idle and idle with blink supplied no numerical SM activity readings.
Zero submitted frames does not establish zero GPU utilization.
The GPU values of the resize workload come from the native data set, not from the resize data set.
In the build of the native data set, each width change reflowed all history rows.

## History memory

| Retained rows | 1080p RSS / PSS (MiB) | 4K RSS / PSS (MiB) | 1080p RSS growth (MiB) | 4K RSS growth (MiB) |
| ---: | ---: | ---: | ---: | ---: |
| 0 | 150.89 / 71.70 | 155.74 / 76.57 | 1.25 | 2.39 |
| 100,000 | 236.04 / 148.94 | 243.34 / 156.11 | 86.19 | 89.57 |
| 1,000,000 | 802.78 / 715.59 | 847.72 / 760.58 | 651.36 | 695.86 |

The growth is the change from the same process before it fills the history.
It includes parser storage, viewport content and font-cache changes.

## Input response

All 1,200 acknowledgment requests completed. These times end at terminal frame submission.

| Viewport | Output | p50 (ms) | p95 (ms) | p99 (ms) | Maximum (ms) | Achieved output (MiB/s) |
| --- | --- | ---: | ---: | ---: | ---: | ---: |
| 1080p | Acknowledgments only | 1.52 | 5.52 | 6.34 | 7.20 | — |
| 1080p | 4 MiB/s target | 33.93 | 50.55 | 50.68 | 66.87 | 4.00 |
| 4K | Acknowledgments only | 2.31 | 4.31 | 5.24 | 6.34 | — |
| 4K | 4 MiB/s target | 13.75 | 18.64 | 30.76 | 50.33 | 4.00 |

The observed maximum is not a worst-case latency bound.
Compositor scheduling, other applications and power management can change tail latency.

## Script callback duration

All 180,000 callbacks completed without handler errors.
The maximum column covers both viewport sizes. The duration does not include effect application.

| Synthetic panes | 1080p p50 / p95 (µs) | 4K p50 / p95 (µs) | Maximum (µs) |
| ---: | ---: | ---: | ---: |
| 1 | 5.12 / 9.04 | 5.13 / 9.30 | 84.68 |
| 8 | 9.31 / 14.25 | 9.27 / 14.14 | 146.91 |
| 32 | 21.48 / 30.27 | 21.66 / 30.31 | 225.91 |

## Resize

The resize data set uses 100,000 history rows in each process. Each text line has 75 ASCII characters.

| Metric | 1080p | 4K |
| --- | ---: | ---: |
| Frame service p95 (ms) | 22.85 | 24.58 |
| CPU (% of one core) | 40.32 | 46.09 |
| RSS after resize (MiB) | 268.43 | 276.30 |
| Resize requests per second | 19.80 | 19.73 |

The resize path moves compact history rows when a complete line still fits.
It keeps their cell allocations, styles and graphemes.
Rows that enter the active screen expand to the new screen width.
Wrapped lines and lines wider than the new width still use full reflow.
The resize path still visits each history row.
Long wrapped text can cost more than these short lines. See [reflow of long wrapped lines](#reflow-of-long-wrapped-lines).
These results do not establish a frame-time bound for all history content.

## Reflow of long wrapped lines

This measurement uses the headless VT benchmark.
It measures the `Terminal::resize` call only. It does not measure rendering or frame time.
Build with `-DAFTERGLOW_BUILD_TOOLS=ON`.
Run `afterglow_vtbench --reflow [RUNS] [CASE]` from the build's `tools/bench` directory.
`RUNS` is an integer from 1 to 1000, and the default is 5. Other values print the usage.
`CASE` is a case-sensitive part of a case name, such as `long rich`.

Each run fills a 200×50 terminal until it holds 100,000 history rows.
The scrollback limit is 100,000 rows.
The run then resizes to 180×45, 160×50, 140×45, 160×50, 180×45 and 200×50 four times.
These are not the native benchmark's steps. Each resize changes the size by 20 columns and 5 rows.
The benchmark times each resize in process CPU time. Other load changes CPU time less than elapsed time.
A run gives the median and 95th percentile of its 24 resizes.

| Case | Case name | History lines |
| --- | --- | --- |
| Short ASCII | `short ascii 20-120` | 20 to 120 ASCII characters. Each line fits on one row at every tested width |
| Long ASCII | `long ascii 300-3000` | 300 to 3,000 ASCII characters |
| Long rich | `long rich 300-3000` | 300 to 3,000 cells with about 4% wide characters, 3% combining marks, color and bold changes and 1% hyperlinks |
| Mixed rich | `mixed rich 50% long` | Half short ASCII lines and half long rich lines |

The generated text is the same in each run.
The long cases fill 100,000 rows at 200 columns.
Narrower widths need more rows, so the scrollback limit removes the oldest lines.
After the first pass, the long cases hold about 71,000 rows at 200 columns.

| Condition | Value |
| --- | --- |
| Machine | Shared 8-core machine, load average between 11 and 23 |
| Build type | `RelWithDebInfo` |
| Runs | Five. The tables show the medians of the five runs |

| Case | Median (ms) | p95 (ms) |
| --- | ---: | ---: |
| Short ASCII | 9.69 | 12.03 |
| Long ASCII | 59.94 | 65.66 |
| Long rich | 140.12 | 168.89 |
| Mixed rich | 134.24 | 150.98 |

| Case | Process peak RSS, one run (MiB) |
| --- | ---: |
| Long ASCII | 185 |
| Long rich | 305 |

Absolute values change with load.
Reflow rewraps one logical line at a time, directly from the old rows into the new rows.
It does not copy a line into a temporary buffer first.
A line that fits on one row gets no temporary line record.
It copies runs of narrow cells in one operation.
Grapheme entries move between rows without new allocations.
Reflow frees the old rows of a line when the line is complete. This lowers peak memory.
Reflow compacts again only the rows that can store blank cells after their content.

Reflow rebuilds every row of a wrapped line. Each new row needs one allocation.
Cost increases with the number of history cells.
Grapheme clusters add cost, because reflow visits each stored cluster.
Long rich history at this size takes more than 100 ms of CPU for each resize.
This is longer than one frame interval.
No native benchmark data set uses long wrapped lines, so these results do not give frame service times.

## Worst-case terminal input

Run `afterglow_vtbench --stress [CASE]` from the build's `tools/bench` directory.
`CASE` is a case-sensitive part of a case name, such as `cursors`.
Each case feeds its setup before the clock starts.
Then it feeds one input many times, in 64 KiB chunks, as the PTY reader does.
The time is process CPU time, the best of three runs.
After each run, the case checks the state that its input must leave, for example a cursor in every cell.
A case that fails this check prints an error and the command exits with status 1.

| Case name | Terminal | Setup | Repeated input |
| --- | --- | --- | --- |
| `region lf, no regions` | 80×24 | Margins over rows 1 to 23, cursor on row 23 | One 51-byte line that ends with a line feed at the bottom margin |
| `region lf, 1024 regions` | 80×24 | 400 shell commands with OSC 133 marks, which keep 1,024 regions in the history, then the same margins | The same line |
| `cursors 80x24, one group repeated` | 80×24 | None | One extra-cursor sequence with 4,095 full-screen groups |
| `cursors 80x24, distinct rects` | 80×24 | None | One extra-cursor sequence with 812 different overlapping rectangles |

An 80×24 screen has 1,920 cells, fewer than the limit of 4,096 extra cursors.
As a result, no sequence stops at the limit.

The measurement used a `RelWithDebInfo` build on a shared 8-core machine with a load average of about 16.

| Case name | Unit | Time per unit (µs) | Throughput (MB/s) |
| --- | --- | ---: | ---: |
| `region lf, no regions` | Line feed | 0.40 | 128.5 |
| `region lf, 1024 regions` | Line feed | 0.41 | 125.9 |
| `cursors 80x24, one group repeated` | Sequence | 235 | 34.8 |
| `cursors 80x24, distinct rects` | Sequence | 1,599 | 6.4 |

The terminal keeps an upper bound on the rows of the retained OSC 133 regions.
When every region is above the scroll region, a scroll of that region does not examine them.
An extra-cursor sequence visits each cell of each different rectangle once.
It also compares each existing cursor with each rectangle.
As a result, the cost increases with the number of rectangles multiplied by their area.

## Output throughput

Run `afterglow_vtbench` without arguments from the build's `tools/bench` directory.
Each case feeds 64 MiB of generated output to a 200×60 terminal, in 64 KiB chunks.
The scrollback cases keep 10,000 lines.
The measurement used a `RelWithDebInfo` build on the Intel Core i7-9700K. The values are the best of three runs.

| Case name | Output | Throughput (MB/s) |
| --- | --- | ---: |
| `ascii` | Lines of 20 to 119 random printable ASCII characters | 146 |
| `ascii, no scrollback` | The same lines, with no scrollback | 219 |
| `sgr colours` | Words of six letters, each in a 256-color SGR color | 156 |
| `unicode` | Japanese, Chinese and Korean words, emoji, a ZWJ family sequence and accented Latin words | 63 |

## Comparison with other terminals

[DOOM-fire-zig](https://github.com/const-void/DOOM-fire-zig) draws a full-screen fire with 256-color SGR sequences and the `▀` character.
Each frame is one write of about 200 KiB to the terminal.
The program counts the frames that it writes in each second.
As a result, the result shows how fast a terminal reads and parses output.
It does not show how many frames the terminal shows on the display.

| Condition | Value |
| --- | --- |
| Machine | Intel Core i7-9700K and NVIDIA GeForce RTX 2080, load average below 3 |
| Display | Each terminal full screen in its own nested KWin at 1920×1080, with its default settings |
| Font | DejaVu Sans Mono, 11 points, in every terminal |
| Runs | 15 seconds each. The terminals ran in turn, three rounds |
| Program | Commit `eb0631b`, built with Zig 0.17 in `ReleaseFast` mode |

For Zig 0.17, a port uses C library calls for the terminal I/O. The port also stops the program after 15 seconds.
The fonts and the window size give each terminal a different grid.
As a result, the cells per second column compares the terminals best.
The three runs of each terminal are within 1% of each other.

| Terminal | Grid | Frames per second (three runs) | Median | Cells per second |
| --- | --- | --- | ---: | ---: |
| Alacritty 0.17.0 | 212×112 | 626, 626, 628 | 626 | 14.9 M |
| Afterglow | 210×118 | 595, 596, 596 | 596 | 14.8 M |
| foot 1.28.0 | 212×112 | 533, 536, 537 | 536 | 12.7 M |
| kitty 0.49.2 | 212×112 | 510, 510, 512 | 510 | 12.1 M |
| Ghostty 1.3.1 | 211×118 | 287, 288, 289 | 288 | 7.2 M |
| Konsole 26.08.2 | 208×109 | 186, 187, 188 | 187 | 4.2 M |

## Workloads

These are the workloads of the native benchmark.

| Case | `--cases` value | Work |
| --- | --- | --- |
| Idle | `idle` | 75-character viewport rows, no output, static cursor |
| Idle with blink | `idle-blink` | 75-character viewport rows, no output, blinking cursor |
| Scroll | `scroll` | Repeated scrollbar traversal through 100,000 history rows, with a request every 16 ms |
| Resize | `resize` | Width changes in 16-pixel steps across a 160-pixel range, and height changes in 8-pixel steps across 80 pixels, with a request every 50 ms |
| History | `history` | Retain exactly 0, 100,000 or 1,000,000 rows, then measure idle memory |
| Input | `input` | Send ASCII key events through Qt and the PTY and wait for a visible acknowledgment |
| Input with output | `input-load` | Repeat the input case while the child targets 4 MiB/s of colored output |
| Scripts | `scripts` | Run the Phosphor scene's command-finished callback 10,000 times with 1, 8 or 32 pane snapshots |

Resize uses 100,000 history rows and enables normal text reflow.
History population feeds the terminal parser directly, before measurement.
Each history line has 75 ASCII characters and a carriage return/newline pair.
The input cases use a real PTY child.
They send one acknowledgment request every 100 ms, with at most one outstanding request.
The child reserves the first screen row for acknowledgments.
Its partial scrolling region does not add output to scrollback.
The recorded output rate covers the interval between the first and last requests.

The script case loads the Phosphor scene's callback through ScriptHost.
All pane snapshots belong to the same tab and have no active command.
The harness discards returned effects to isolate callback execution.
It does not measure effect application, real pane creation or a complete plugin workload.

## Metrics

| Metric | Meaning |
| --- | --- |
| CPU percent | Application process CPU time divided by elapsed time. 100% means one full CPU core |
| Frame service | Elapsed time inside a Qt update or expose event that submits a terminal frame |
| Frame interval | Time between the starts of consecutive submitted frame events |
| GPU elapsed | Asynchronous terminal command-buffer elapsed time reported by Qt |
| RSS | Resident process memory, including shared pages |
| PSS | Resident memory with shared pages divided between their users |
| Peak RSS | Largest resident allocation since process startup |
| Input response | Synthetic Qt key dispatch until a submitted terminal snapshot contains the child acknowledgment |
| Callback duration | ScriptHost's measured callback invocation time |

Frame service includes scheduling waits inside the event handler.
It is not GPU-only execution time or physical display latency.
Qt GPU timestamps can lag several frames, repeat, or return zero when unavailable.
Do not sum these samples to calculate GPU utilization.
See [Qt command-buffer timing](https://doc.qt.io/qt-6/qrhicommandbuffer.html#lastCompletedGpuTime).
The terminal GPU metric excludes separate Qt Quick chrome commands.

An empty timing distribution has a count of zero and no percentile fields.
This can mean that Afterglow submitted no frames, or that the timing source was not available.
NVIDIA process utilization fields remain null when the driver does not supply them.
GPU allocation samples describe the application process, not the complete desktop.
CPU measurements exclude the child workload and nested compositor.
Application CPU time includes the bookkeeping of the benchmark.

## Limits

These workloads use no shaders, no audio, one visible terminal pane and scale 1.
The scripts case changes only its synthetic state snapshot size.
The tests do not measure physical keyboard latency, presentation latency, mixed-scale monitors or thermal stability.
The scroll case traverses the scrollbar position. It does not reproduce smooth wheel input.
The history case uses short ASCII rows. Images, styles and complex text change memory requirements.
The input case does not represent every full-screen application or output pattern.
The nested compositor shares the host GPU and normal power management.
Its additional presentation stage can affect frame pacing.
These timings do not establish latency with a direct connection to the host compositor.
The headless VT benchmark ran on a shared, loaded machine. Its absolute values change with load.
Short repeated measurements do not establish a cross-terminal performance comparison.
