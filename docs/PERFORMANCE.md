<!-- SPDX-FileCopyrightText: 2026 fuddlesworth -->
<!-- SPDX-License-Identifier: GPL-3.0-or-later -->

# Native terminal performance

The native benchmark measures the application in an isolated KWin desktop.
See [testing](TESTING.md#native-performance-measurements) for commands.
See [Phosphor scene performance](../data/examples/phosphor/PERFORMANCE.md) for separate shader measurements.

## Original baseline

Measurement date: 30 October 2026.
Source commit: `2aa71e7`. Build type: `RelWithDebInfo`.
The recorded set contains 72 case runs: three repeats of each workload at each resolution.
Timed cases request 15 seconds. CPU rates use actual elapsed time.
Input cases use 100 acknowledgements per run. Script cases use 10,000 callbacks per run.
Tables show medians of per-run statistics. Maximum columns contain the largest observed sample.
Percentiles are not pooled across runs.
See the [summary and ranges](../data/benchmarks/native-2026-10-30.summary.json) and [compressed raw samples](../data/benchmarks/native-2026-10-30.raw.json.gz).

| Condition | Value |
| --- | --- |
| CPU | Intel Core i7-9700K, 8 cores |
| GPU | NVIDIA GeForce RTX 2080, driver 615.78.08 |
| System | CachyOS 7.3.0-4, Qt 6.12.0, nested KWin 6.8.1 |
| Viewport | Exact 1920×1080 and 3840×2160 windows, scale 1 |
| Font | Monaspace Neon Var, 11 points, grayscale antialiasing, light hinting |
| Appearance | Basic controls, generic platform theme, opaque background, no shaders or audio |
| Power | 225 W GPU limit, normal power management, shared desktop GPU, unlocked clocks |

### CPU and frame service

CPU percentages use 100% for one full core.

| Workload | 1080p CPU (%) | 4K CPU (%) | 1080p frame p95 (ms) | 4K frame p95 (ms) |
| --- | ---: | ---: | ---: | ---: |
| Idle | 0.09 | 0.08 | — | — |
| Idle with blink | 0.31 | 0.52 | 0.96 | 1.98 |
| Scroll, 100,000 rows | 6.79 | 12.79 | 1.29 | 2.71 |
| Resize, 100,000 rows | 91.54 | 94.09 | 237.06 | 341.84 |

Static idle submitted no frames in all six runs.
Scroll frame-interval p95 is 33.56 ms at 1080p and 33.60 ms at 4K.
Resize frame-interval p95 is 237.07 ms at 1080p and 341.86 ms at 4K.
After the resize workload, median RSS is 640.76 MiB at 1080p and 959.75 MiB at 4K.
In the `2aa71e7` build, resizing was a visible performance limit.
In that build, width changes ran [history reflow](../libs/afterglow-vt/src/Screen.cpp), which rebuilt stored rows.
The high CPU use and shorter GPU times suggested that CPU-side resize work was the main cost.
The next two sections measure the later resize and reflow work.
The 50 ms request timer cannot maintain its requested rate during this workload.

### Terminal GPU timing and allocation

GPU elapsed time is asynchronous. Allocation is the largest external process sample across all repeats.

| Workload | 1080p GPU p95 (ms) | 4K GPU p95 (ms) | 1080p allocation (MiB) | 4K allocation (MiB) |
| --- | ---: | ---: | ---: | ---: |
| Idle with blink | 0.11 | 0.33 | 71.00 | 137.00 |
| Scroll | 0.13 | 0.40 | 81.00 | 169.00 |
| Resize | 0.21 | 0.74 | 91.00 | 201.00 |

The driver supplied process activity for only some external samples.
The table pools available SM activity readings across three repeats.
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

### History memory

| Retained rows | 1080p RSS / PSS (MiB) | 4K RSS / PSS (MiB) | 1080p RSS growth (MiB) | 4K RSS growth (MiB) |
| ---: | ---: | ---: | ---: | ---: |
| 0 | 150.89 / 71.70 | 155.74 / 76.57 | 1.25 | 2.39 |
| 100,000 | 236.04 / 148.94 | 243.34 / 156.11 | 86.19 | 89.57 |
| 1,000,000 | 802.78 / 715.59 | 847.72 / 760.58 | 651.36 | 695.86 |

Growth is measured from the same process before history population.
It includes parser storage, viewport content and font-cache changes.

### Input response

All 1,200 acknowledgement requests completed. These times end at terminal frame submission.

| Viewport | Output | p50 (ms) | p95 (ms) | p99 (ms) | Maximum (ms) | Achieved output (MiB/s) |
| --- | --- | ---: | ---: | ---: | ---: | ---: |
| 1080p | Acknowledgements only | 1.52 | 5.52 | 6.34 | 7.20 | — |
| 1080p | 4 MiB/s target | 33.93 | 50.55 | 50.68 | 66.87 | 4.00 |
| 4K | Acknowledgements only | 2.31 | 4.31 | 5.24 | 6.34 | — |
| 4K | 4 MiB/s target | 13.75 | 18.64 | 30.76 | 50.33 | 4.00 |

The observed maximum is not a worst-case latency bound.
Compositor scheduling, other applications and power management can change tail latency.

### Script callback duration

All 180,000 callbacks completed without handler errors.
The maximum column covers both viewport sizes. Effect application is excluded.

| Synthetic panes | 1080p p50 / p95 (µs) | 4K p50 / p95 (µs) | Maximum (µs) |
| ---: | ---: | ---: | ---: |
| 1 | 5.12 / 9.04 | 5.13 / 9.30 | 84.68 |
| 8 | 9.31 / 14.25 | 9.27 / 14.14 | 146.91 |
| 32 | 21.48 / 30.27 | 21.66 / 30.31 | 225.91 |

## Resize after row reuse

Source commit: `f1fcaee`. Measurement date: 30 October 2026.
This set repeats the same resize workload three times at each resolution.
The hardware, build type, font and appearance settings are the same as the original baseline.
Each process contains 100,000 history rows. Each text line has 75 ASCII characters.
Tables show medians of per-run statistics.
See the [summary and ranges](../data/benchmarks/resize-optimized-2026-10-30.summary.json) and [compressed raw samples](../data/benchmarks/resize-optimized-2026-10-30.raw.json.gz).

| Metric | 1080p before | 1080p after | 4K before | 4K after |
| --- | ---: | ---: | ---: | ---: |
| Frame service p95 (ms) | 237.06 | 22.85 | 341.84 | 24.58 |
| CPU (% of one core) | 91.54 | 40.32 | 94.09 | 46.09 |
| RSS after resize (MiB) | 640.76 | 268.43 | 959.75 | 276.30 |
| Resize requests per second | 4.21 | 19.80 | 2.99 | 19.73 |

The resize path moves compact history rows when a complete line still fits.
It keeps their cell allocations, styles and graphemes.
Rows that enter the active screen expand to the new screen width.
Wrapped lines and lines wider than the new width still use full reflow.

This keeps repeated cell reconstruction out of this workload.
It does not remove the traversal of history rows.
Long wrapped text can still cost more than these short lines.
See [reflow of long wrapped lines](#reflow-of-long-wrapped-lines).
These results do not establish a frame-time bound for all history content.
The nested-compositor and timing limits below still apply.

## Reflow of long wrapped lines

Before: `3396c14`. After: `03e8bb9`. Measurement date: 31 October 2026. Build type: `RelWithDebInfo`.
This set uses the headless VT benchmark, not the native benchmark.
It measures the `Terminal::resize` call only. It does not measure rendering or frame time.
Build with `-DAFTERGLOW_BUILD_TOOLS=ON`.
Run `afterglow_vtbench --reflow [RUNS] [CASE]` from the build's `tools/bench` directory.
`RUNS` is an integer from 1 to 1000, and the default is 5. Other values print the usage.
`CASE` is a case-sensitive part of a case name, such as `long rich`.

Each run fills a 200×50 terminal until it holds 100,000 history rows.
The scrollback limit is 100,000 rows.
The run then resizes to 180×45, 160×50, 140×45, 160×50, 180×45 and 200×50 four times.
These are not the native benchmark's steps. Each resize changes the size by 20 columns and 5 rows.
Each resize is timed in process CPU time, which is less sensitive to other load than elapsed time.
A run gives the median and 95th percentile of its 24 resizes.
Tables show the medians of five runs.
The before and after runs use the same benchmark binary with the old and new library.
The runs alternate between the two libraries.

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

| Case | Median before (ms) | Median after (ms) | p95 before (ms) | p95 after (ms) |
| --- | ---: | ---: | ---: | ---: |
| Short ASCII | 18.70 | 9.69 | 22.73 | 12.03 |
| Long ASCII | 171.92 | 59.94 | 220.29 | 65.66 |
| Long rich | 348.29 | 140.12 | 461.94 | 168.89 |
| Mixed rich | 326.42 | 134.24 | 397.84 | 150.98 |

| Process peak RSS, one run (MiB) | Before | After |
| --- | ---: | ---: |
| Long ASCII | 555 | 185 |
| Long rich | 763 | 305 |

In the after build, reflow rewraps one logical line at a time, directly from the old rows into the new rows.
It does not copy a line into a temporary buffer first.
A line that fits on one row gets no temporary line record.
It copies runs of narrow cells in one operation.
Grapheme entries move between rows without new allocations.
The old rows of a line are freed when the line is complete, which lowers peak memory.
Only rows that can store blank cells after their content are compacted again.

These numbers come from a shared 8-core machine with a load average between 11 and 23.
Absolute values change with load. Compare the before and after columns of one table only.
Reflow rebuilds every row of a wrapped line. Each new row needs one allocation.
Cost increases with the number of history cells.
Grapheme clusters add cost, because reflow visits each stored cluster.
Long rich history at this size takes more than 100 ms of CPU for each resize.
This is longer than one frame interval.
The native benchmark was not repeated, so these results do not give frame service times.

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
Thus no sequence stops at the limit.

Measurement: 1 November 2026, `RelWithDebInfo`, a shared 8-core machine with a load average of about 16.
`fd1124a` examined every retained OSC 133 region on each scroll of a region on the main screen.
`e564a2f` keeps an upper bound on the rows of the regions.
When every region is above the scroll region, the terminal does not examine them.

| Case name | Unit | `fd1124a` (µs) | `e564a2f` (µs) | `e564a2f` throughput (MB/s) |
| --- | --- | ---: | ---: | ---: |
| `region lf, no regions` | Line feed | 0.40 | 0.40 | 128.5 |
| `region lf, 1024 regions` | Line feed | 2.29 | 0.41 | 125.9 |
| `cursors 80x24, one group repeated` | Sequence | 233 | 235 | 34.8 |
| `cursors 80x24, distinct rects` | Sequence | 1,637 | 1,599 | 6.4 |

An extra-cursor sequence visits each cell of each different rectangle once.
It also compares each existing cursor with each rectangle.
Thus the cost increases with the number of rectangles multiplied by their area.

## Output throughput

### Terminal core

Run `afterglow_vtbench` without arguments from the build's `tools/bench` directory.
Each case feeds 64 MiB of generated output to a 200×60 terminal, in 64 KiB chunks.
The scrollback cases keep 10,000 lines.

| Case name | Output |
| --- | --- |
| `ascii` | Lines of 20 to 119 random printable ASCII characters |
| `ascii, no scrollback` | The same lines, with no scrollback |
| `sgr colours` | Words of six letters, each in a 256-color SGR color |
| `unicode` | Japanese, Chinese and Korean words, emoji, a ZWJ family sequence and accented Latin words |

Measurement: 1 November 2026, `RelWithDebInfo`, Intel Core i7-9700K.
`46c2cb7` is before the throughput work. Its values are the best of two runs.
`ded14ad` is after the work. Its values are the best of three runs.

| Case name | `46c2cb7` (MB/s) | `ded14ad` (MB/s) | Change |
| --- | ---: | ---: | ---: |
| `ascii` | 113 | 146 | +29% |
| `ascii, no scrollback` | 215 | 219 | +2% |
| `sgr colours` | 117 | 156 | +33% |
| `unicode` | 45 | 63 | +40% |

The changes between the two commits:

- The parser reads a CSI sequence of parameters and a final byte in one loop.
- An unused style stays interned for a time, so the next pen that uses it does not intern it again.
- An open-addressing table finds interned styles without a heap allocation.
- The terminal keeps the properties of 256 recently printed characters.
- Narrow and wide characters go into plain cells without the general cell write.
- A line that scrolls into the history gives its buffer to the new bottom line.
- A history line that has only the default style is dropped without a read of its cells.

### Comparison with other terminals

[DOOM-fire-zig](https://github.com/const-void/DOOM-fire-zig) draws a full-screen fire with 256-color SGR sequences and the `▀` character.
Each frame is one write of about 200 KiB to the terminal.
The program counts the frames that it writes in each second.
Thus the result shows how fast a terminal reads and parses output.
It does not show how many frames the terminal shows on the display.

Each terminal ran full screen in its own nested KWin at 1920×1080, with its default settings.
Every terminal used DejaVu Sans Mono at 11 points.
Each run took 15 seconds. The terminals ran in turn, three rounds.
The program was commit `eb0631b`, built with Zig 0.17 in `ReleaseFast` mode.
For Zig 0.17, a port uses C library calls for the terminal I/O. The port also stops the program after 15 seconds.

Measurement: 1 November 2026, Afterglow `ded14ad`, Intel Core i7-9700K and NVIDIA GeForce RTX 2080.
Another program used about two of the eight cores during all runs, with a load average of 6 to 8.
The fonts and the window size give each terminal a different grid.
Thus the cells per second column compares the terminals best.

| Terminal | Grid | Frames per second (three runs) | Median | Cells per second |
| --- | --- | --- | ---: | ---: |
| Alacritty 0.17.0 | 212×112 | 493, 495, 569 | 495 | 11.8 M |
| Afterglow `ded14ad` | 210×118 | 449, 453, 575 | 453 | 11.2 M |
| kitty 0.49.2 | 212×112 | 451, 456, 465 | 456 | 10.8 M |
| foot 1.28.0 | 212×112 | 338, 340, 413 | 340 | 8.1 M |
| Ghostty 1.3.1 | 211×118 | 222, 223, 305 | 223 | 5.5 M |
| Konsole 26.08.2 | 208×109 | 147, 150, 165 | 150 | 3.4 M |

The first three terminals changed places between rounds.
Earlier that day, before the other program started, Afterglow `fa8bbf5` gave 582, 591 and 593 frames per second.
Alacritty gave 589, 591 and 591 frames per second in other runs before the other program started.
That is 14.6 M and 14.0 M cells per second.

## Workloads

| Case | `--cases` value | Work |
| --- | --- | --- |
| Idle | `idle` | 75-character viewport rows, no output, static cursor |
| Idle with blink | `idle-blink` | 75-character viewport rows, no output, blinking cursor |
| Scroll | `scroll` | Repeated scrollbar traversal through 100,000 history rows, with a request every 16 ms |
| Resize | `resize` | Width changes in 16-pixel steps across a 160-pixel range, and height changes in 8-pixel steps across 80 pixels, with a request every 50 ms |
| History | `history` | Retain exactly 0, 100,000 or 1,000,000 rows, then measure idle memory |
| Input | `input` | Send ASCII key events through Qt and the PTY and wait for a visible acknowledgement |
| Input with output | `input-load` | Repeat the input case while the child targets 4 MiB/s of colored output |
| Scripts | `scripts` | Run the Phosphor scene's command-finished callback 10,000 times with 1, 8 or 32 pane snapshots |

Resize uses 100,000 history rows and enables normal text reflow.
History population feeds the terminal parser directly, before measurement.
Each history line has 75 ASCII characters and a carriage return/newline pair.
The input cases use a real PTY child.
They send one acknowledgement request every 100 ms, with at most one outstanding request.
The child reserves the first screen row for acknowledgements.
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
| Input response | Synthetic Qt key dispatch until a submitted terminal snapshot contains the child acknowledgement |
| Callback duration | ScriptHost's measured callback invocation time |

Frame service includes scheduling waits inside the event handler.
It is not GPU-only execution time or physical display latency.
Qt GPU timestamps can lag several frames, repeat, or return zero when unavailable.
Do not sum these samples to calculate GPU utilization.
See [Qt command-buffer timing](https://doc.qt.io/qt-6/qrhicommandbuffer.html#lastCompletedGpuTime).
The terminal GPU metric excludes separate Qt Quick chrome commands.

An empty timing distribution has a count of zero and no percentile fields.
This can mean no frames were submitted or the timing source was unavailable.
NVIDIA process utilization fields remain null when the driver does not supply them.
GPU allocation samples describe the application process, not the complete desktop.
CPU measurements exclude the child workload and nested compositor.
Benchmark bookkeeping is included in application CPU time.

## Measurement limits

These workloads use no shaders, no audio, one visible terminal pane and scale 1.
The scripts case changes only its synthetic state snapshot size.
The tests do not measure physical keyboard latency, presentation latency, mixed-scale monitors or thermal stability.
The scroll case traverses the scrollbar position. It does not reproduce smooth wheel input.
The history case uses short ASCII rows. Images, styles and complex text change memory requirements.
The input case does not represent every full-screen application or output pattern.
The nested compositor shares the host GPU and normal power management.
Its additional presentation stage can affect frame pacing.
These timings do not establish latency with a direct connection to the host compositor.
Short repeated measurements do not establish a cross-terminal performance comparison.
