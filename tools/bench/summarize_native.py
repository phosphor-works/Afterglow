#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 fuddlesworth
# SPDX-License-Identifier: GPL-3.0-or-later
"""Archive a completed native benchmark and summarize variation between runs."""
import argparse
from collections import defaultdict
import gzip
import json
from pathlib import Path
import statistics


def spread(values):
    values = [value for value in values if value is not None]
    if not values:
        return {'count': 0}
    return {'count': len(values), 'median': statistics.median(values),
            'min': min(values), 'max': max(values)}


def summarize(environment, results):
    if not environment.get('finished_utc') or not results:
        raise ValueError('The benchmark has not completed')
    groups = defaultdict(list)
    for result in results:
        key = (result['width'], result['height'], result['case'],
               result.get('history_rows', 0), result.get('snapshot_panes', 1))
        groups[key].append(result)
    cases = []
    for key, runs in groups.items():
        if sorted(run['repeat'] for run in runs) != list(range(1, environment['repeats'] + 1)):
            raise ValueError(f'Missing or duplicate repeats: {key}')
        case = dict(zip(('width', 'height', 'case', 'history_rows', 'snapshot_panes'), key))
        case['repeats'] = len(runs)
        for name in ('cpu_percent_one_core', 'wall_ms', 'history_fill_ms'):
            case[name] = spread([run.get(name) for run in runs])
        for metric in ('frame_service', 'frame_intervals', 'terminal_gpu_elapsed',
                       'input_to_submitted_snapshot', 'operation'):
            for run in runs:
                data = run.get(metric, {'count': 0})
                if len(data.get('samples_ms', [])) != data['count']:
                    raise ValueError(f'Sample count mismatch: {key}, {metric}')
            case[metric] = {
                statistic: spread([run.get(metric, {}).get(statistic) for run in runs])
                for statistic in ('count', 'mean_ms', 'p50_ms', 'p95_ms', 'p99_ms', 'max_ms')}
        for name in ('Rss_kib', 'Pss_kib', 'Private_Dirty_kib', 'peak_rss_kib'):
            case[name] = spread([run['memory_after'].get(name) for run in runs])
        case['resident_growth_mib'] = spread([
            (run['memory_after']['Rss_kib'] - run['memory_before']['Rss_kib']) / 1024 for run in runs])
        case['operations_per_second'] = spread([
            run['operations'] * 1000 / run['wall_ms'] for run in runs])
        case['submitted_events_per_second'] = spread([
            run['frame_service']['count'] * 1000 / run['wall_ms'] for run in runs])
        case['output_mib_per_second'] = spread([
            run['pty_output']['bytes_since_first_ack'] / run['pty_output']['seconds_since_first_ack'] / 1048576
            for run in runs if run.get('pty_output', {}).get('seconds_since_first_ack', 0) > 0])
        case['gpu_framebuffer_mib'] = spread([
            sample['framebuffer_mib'] for run in runs for sample in run['gpu_process_samples']])
        samples = [sample for run in runs for sample in run['gpu_process_samples']]
        case['gpu_process_sample_count'] = len(samples)
        for metric in ('sm_percent', 'memory_activity_percent'):
            case['gpu_' + metric] = spread([sample[metric] for sample in samples])
        if key[2].startswith('input'):
            if any(run['input_to_submitted_snapshot']['count'] != environment['input_samples'] for run in runs):
                raise ValueError(f'Missing input acknowledgements: {key}')
        cases.append(case)
    return {'schema_version': 1, 'environment': environment, 'case_runs': len(results),
            'aggregation': 'Median, minimum and maximum across per-run statistics; percentiles are not pooled. GPU allocation and activity statistics pool available external samples. Activity excludes missing values and is not time-weighted; vendor readings can repeat.',
            'cases': cases}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('input', type=Path, help='Completed benchmark output directory')
    parser.add_argument('prefix', type=Path, help='Output path prefix for .summary.json and .raw.json.gz')
    args = parser.parse_args()
    try:
        environment = json.loads((args.input / 'environment.json').read_text())
        results = json.loads((args.input / 'results.json').read_text())
        summary = summarize(environment, results)
    except (OSError, ValueError, KeyError) as error:
        parser.error(str(error))
    summary_path = Path(str(args.prefix) + '.summary.json')
    raw_path = Path(str(args.prefix) + '.raw.json.gz')
    if summary_path.exists() or raw_path.exists():
        parser.error('Output already exists')
    summary_path.parent.mkdir(parents=True, exist_ok=True)
    raw = {'schema_version': 1, 'environment': environment, 'results': results}
    raw_path.write_bytes(gzip.compress(json.dumps(raw, separators=(',', ':')).encode(), mtime=0))
    summary_path.write_text(json.dumps(summary, indent=2) + '\n')
    print(f'Saved {len(results)} case runs: {summary_path}, {raw_path}')


if __name__ == '__main__':
    main()
