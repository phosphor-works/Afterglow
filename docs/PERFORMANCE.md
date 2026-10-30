<!-- SPDX-FileCopyrightText: 2026 fuddlesworth -->
<!-- SPDX-License-Identifier: GPL-3.0-or-later -->

# Native terminal performance

The native benchmark measures the application in an isolated KWin desktop.
See [testing](TESTING.md#native-performance-measurements) for commands.
See [Phosphor scene performance](../data/examples/phosphor/PERFORMANCE.md) for separate shader measurements.

## Workloads

| Case | Work |
| --- | --- |
| Idle | 75-character viewport rows, no output, static cursor |
| Idle with blink | 75-character viewport rows, no output, blinking cursor |
| Scroll | Repeated scrollbar traversal through 100,000 history rows; requests every 16 ms |
| Resize | Repeated width changes of 160 pixels and height changes of 80 pixels; requests every 50 ms |
| History | Retain exactly 0, 100,000 or 1,000,000 rows, then measure idle memory |
| Input | Send ASCII key events through Qt and the PTY; wait for a visible acknowledgement |
| Input with output | Repeat the input case while the child targets 4 MiB/s of colored output |
| Scripts | Run the Phosphor command-finished callback 10,000 times with 1, 8 or 32 pane snapshots |

Resize uses 100,000 history rows and enables normal text reflow.
History population feeds the terminal parser directly, before measurement.
Each history line has 75 ASCII characters and a carriage return/newline pair.
The input cases use a real PTY child.
They send one acknowledgement request every 100 ms, with at most one outstanding request.
The child reserves the first screen row for acknowledgements.
Its partial scrolling region does not add output to scrollback.
The recorded output rate covers the interval between the first and last requests.

The script case loads the shipped Phosphor callback through ScriptHost.
All pane snapshots belong to the same tab and have no active command.
The harness discards returned effects to isolate callback execution.
It does not measure effect application, real pane creation or a complete plugin workload.

## Metrics

| Metric | Meaning |
| --- | --- |
| CPU percent | Application process CPU time divided by elapsed time; 100% means one full CPU core |
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
The scroll case traverses the scrollbar position; it does not reproduce smooth wheel input.
The history case uses short ASCII rows; images, styles and complex text change memory requirements.
The input case does not represent every full-screen application or output pattern.
The nested compositor shares the host GPU and normal power management.
Short repeated measurements do not establish a cross-terminal performance comparison.
