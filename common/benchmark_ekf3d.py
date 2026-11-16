#!/usr/bin/env python3
"""Measure complete native 3D runs and validate every output against sequential EKF."""
import argparse
import csv
import hashlib
from itertools import product
import json
import math
import os
from pathlib import Path
import platform
import re
import statistics
import subprocess
import tempfile
import time


def digest(path):
    h = hashlib.sha256()
    with path.open('rb') as file:
        for block in iter(lambda: file.read(1024 * 1024), b''):
            h.update(block)
    return h.hexdigest()


def source_checkout():
    """Record the checkout state; binary hashes remain the source of truth for the run."""
    try:
        root = Path(__file__).resolve().parent
        commit = subprocess.run(['git', 'rev-parse', 'HEAD'], cwd=root, capture_output=True, text=True, timeout=5, check=True)
        status = subprocess.run(['git', 'status', '--porcelain'], cwd=root, capture_output=True, text=True, timeout=5, check=True)
        return {'commit': commit.stdout.strip(), 'tracked_or_untracked_changes': bool(status.stdout.strip())}
    except (OSError, subprocess.SubprocessError):
        return None


def check_output(expected, output):
    if not expected:
        raise ValueError('sequential reference is empty')
    with output.open(newline='', encoding='utf-8') as file:
        actual = list(csv.DictReader(file))
    if len(actual) != len(expected):
        raise ValueError('output row count mismatch')
    for index, (left, right) in enumerate(zip(expected, actual), 2):
        if left.keys() != right.keys() or left['track_id'] != right['track_id']:
            raise ValueError(f'output header or track mismatch at row {index}')
        for field in left.keys() - {'track_id'}:
            a, b = float(left[field]), float(right[field])
            relative_tolerance = 0.0 if field == 'timestamp' else 1e-11
            if not math.isfinite(a) or not math.isfinite(b) or not math.isclose(a, b, rel_tol=relative_tolerance, abs_tol=1e-9):
                raise ValueError(f'output mismatch at row {index}, field {field}')


def parse_telemetry(stdout, batch_size, record_count, elapsed_ms):
    """Validate native counters before admitting a run into benchmark results."""
    resources = re.findall(r'^RESOURCE parent_cpu_us=(-?\d+) worker_cpu_us=(-?\d+) parent_peak_kb=(-?\d+) worker_peak_kb=(-?\d+)\s*$', stdout, re.MULTILINE)
    timings = re.findall(r'^TIMING batch_size=(\d+) launch_us=([0-9.eE+-]+) first_batch_us=([0-9.eE+-]+) remaining_batches_us=([0-9.eE+-]+) remaining_batches=(\d+) shutdown_us=([0-9.eE+-]+)\s*$', stdout, re.MULTILINE)
    if len(resources) != 1 or len(timings) != 1:
        raise ValueError('native run must report exactly one resource and timing record')
    size, launch, first, remaining, batches, shutdown = timings[0]
    if int(size) != batch_size or int(batches) != (record_count + batch_size - 1) // batch_size - 1:
        raise ValueError('native batch size or batch count does not match workload')
    values = [float(launch), float(first), float(remaining), float(shutdown)]
    if not math.isfinite(elapsed_ms) or elapsed_ms <= 0 or any(not math.isfinite(v) or v < 0 for v in values):
        raise ValueError('native timing contains non-finite or negative values')
    # Native phase intervals are disjoint and lie inside the external interval.
    # Permit a small rounding allowance because native output uses limited precision.
    if sum(values) > elapsed_ms * 1000 + max(100.0, elapsed_ms):
        raise ValueError('native phase timings exceed complete process time')
    counters = [int(v) for v in resources[0]]
    if any(v < -1 for v in counters):
        raise ValueError('invalid resource counter (only -1 means unavailable)')
    return {
        'launch_us': values[0], 'first_batch_us': values[1],
        'remaining_batches_us': values[2], 'remaining_batches': int(batches), 'shutdown_us': values[3],
        **dict(zip(('parent_cpu_us','worker_cpu_us','parent_peak_kb','worker_peak_kb'), counters)),
    }


def validate_source(fields, rows):
    """Reject input that sorting would otherwise silently repair or corrupt."""
    required = ['timestamp', 'track_id', 'range_m', 'azimuth_rad', 'elevation_rad', 'available']
    if fields != required or not rows or len(rows) > 100000:
        raise ValueError('input header must match the 3D schema and contain 1–100000 records')
    previous = {}
    for line, row in enumerate(rows, 2):
        try:
            if None in row or any(row.get(k) is None for k in required):
                raise ValueError('missing or extra fields')
            timestamp = float(row['timestamp'])
            track_text = row['track_id']
            # Match the native integer parser: no decimals or suffixes.
            if not re.fullmatch(r'\+?\d+', track_text):
                raise ValueError('invalid track ID')
            track = int(track_text)
            if track > 2147483647 or not math.isfinite(timestamp) or timestamp < 0:
                raise ValueError('invalid track ID or timestamp')
            if track in previous and timestamp <= previous[track]:
                raise ValueError('track timestamps must be strictly increasing in source order')
            if row['available'] not in ('0', '1'):
                raise ValueError('invalid availability')
            if row['available'] == '0':
                if track not in previous or any(row[k] for k in required[2:5]):
                    raise ValueError('missing observation must follow an initial measurement and have empty values')
            else:
                values = [float(row[k]) for k in required[2:5]]
                if not all(math.isfinite(value) for value in values) or values[0] <= 1e-6 or abs(values[2]) >= math.pi/2:
                    raise ValueError('invalid radar observation')
            previous[track] = timestamp
        except (ValueError, TypeError, OverflowError) as error:
            raise ValueError(f'invalid input at row {line}: {error}') from error


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--sequential', type=Path, required=True)
    parser.add_argument('--parallel', type=Path, required=True)
    parser.add_argument('--input', type=Path, required=True)
    parser.add_argument('--output-dir', type=Path, required=True, help='new directory for results')
    parser.add_argument('--build-command', required=True, help='compiler commands and flags')
    parser.add_argument('--notes', default='', help='hardware, VM, power and environment details')
    parser.add_argument('--iterations', type=int, default=5)
    parser.add_argument('--warmups', type=int, default=1)
    parser.add_argument('--batch-sizes', nargs='+', type=int, default=[128], help='batch sizes to compare')
    parser.add_argument('--track-counts', nargs='+', type=int, help='numbers of tracks to compare; default uses all')
    args = parser.parse_args()
    sequential, parallel, source, destination = (p.resolve() for p in (args.sequential, args.parallel, args.input, args.output_dir))
    if args.iterations < 1 or args.warmups < 0 or args.iterations > 1000 or args.warmups > 1000:
        parser.error('iterations must be 1–1000 and warmups 0–1000')
    if not args.batch_sizes or len(set(args.batch_sizes)) != len(args.batch_sizes) or any(x < 1 or x > 100000 for x in args.batch_sizes):
        parser.error('batch sizes must be unique integers in 1–100000')
    if not all(p.is_file() for p in (sequential, parallel, source)):
        parser.error('both executables and the input must exist')
    with source.open(newline='', encoding='utf-8') as file:
        reader = csv.DictReader(file)
        fields = reader.fieldnames
        rows = list(reader)
    try:
        validate_source(fields, rows)
    except ValueError as error:
        parser.error(str(error))
    # Interleaving preserves chronological order within each track and allows
    # multiple workers to process each batch when tracks overlap in time.
    rows.sort(key=lambda row: (float(row['timestamp']), int(row['track_id'])))
    track_ids = sorted({int(row['track_id']) for row in rows})
    track_counts = args.track_counts or [len(track_ids)]
    if len(set(track_counts)) != len(track_counts) or any(n < 1 or n > len(track_ids) for n in track_counts):
        parser.error(f'track counts must be unique integers in 1–{len(track_ids)}')
    workloads = []
    for track_count in track_counts:
        selected_ids = set(track_ids[:track_count])
        selected = [row for row in rows if int(row['track_id']) in selected_ids]
        counts = sorted({max(1, len(selected)//4), max(1, len(selected)//2), len(selected)})
        workloads.extend((track_count, selected[:count]) for count in counts)
    checkout = source_checkout()
    destination.mkdir(parents=True, exist_ok=False)
    manifest = {
        'status': 'in_progress', 'platform': platform.platform(), 'processor': platform.processor(),
        'machine': platform.machine(), 'logical_cpus': os.cpu_count(), 'python_version': platform.python_version(),
        'input_sha256': digest(source), 'sequential_sha256': digest(sequential),
        'parallel_sha256': digest(parallel), 'build_command': args.build_command,
        'iterations': args.iterations, 'warmups': args.warmups, 'notes': args.notes, 'source_checkout': checkout,
        'batch_sizes': args.batch_sizes, 'track_counts': track_counts,
        'workloads': [],
        'workload_order': 'sorted by timestamp then track_id',
        'execution_order': 'per workload, warm-up and measured rounds rotate the worker/batch configuration order',
        'timing_scope': 'whole parallel process, including startup, input, logging, processing, shutdown and output',
        'cpu_memory_scope': 'whole-process parent and sum of worker CPU times; parent and largest individual worker peak memory (shared pages may overlap)',
    }
    manifest_path = destination/'manifest.json'
    manifest_path.write_text(json.dumps(manifest, indent=2)+'\n', encoding='utf-8')
    samples = []
    sequence = 0
    try:
        with tempfile.TemporaryDirectory() as temporary:
            tmp = Path(temporary)
            for requested_tracks, workload_rows in workloads:
                count = len(workload_rows)
                workload = tmp/f'input-{requested_tracks}-{count}.csv'
                with workload.open('w', newline='', encoding='utf-8') as file:
                    writer = csv.DictWriter(file, fieldnames=fields)
                    writer.writeheader()
                    writer.writerows(workload_rows)
                manifest['workloads'].append({'requested_tracks': requested_tracks, 'track_ids': track_ids[:requested_tracks],
                                              'records': count, 'sha256': digest(workload)})
                reference = tmp/f'reference-{requested_tracks}-{count}.csv'
                subprocess.run([str(sequential), str(workload), str(reference)], check=True, capture_output=True, timeout=120)
                with reference.open(newline='', encoding='utf-8') as file:
                    expected = list(csv.DictReader(file))
                configurations = list(product((1, 2, 4, 8), args.batch_sizes))
                for iteration in range(-args.warmups, args.iterations):
                    offset = (iteration + args.warmups) % len(configurations)
                    for workers, batch_size in configurations[offset:] + configurations[:offset]:
                        sequence += 1
                        output = tmp/f'output-{requested_tracks}-{count}-{workers}-{batch_size}-{iteration}.csv'
                        start = time.perf_counter_ns()
                        result = subprocess.run([str(parallel), str(workload), str(output), str(workers), str(batch_size)],
                                                capture_output=True, text=True, timeout=120)
                        elapsed_ms = (time.perf_counter_ns()-start)/1e6
                        if result.returncode:
                            raise RuntimeError(f'worker run failed: {result.stdout}\n{result.stderr}')
                        check_output(expected, output)
                        telemetry = parse_telemetry(result.stdout, batch_size, count, elapsed_ms)
                        if iteration >= 0:
                            samples.append({'records': count, 'requested_tracks': requested_tracks, 'tracks': len({r['track_id'] for r in workload_rows}),
                                            'workers': workers, 'batch_size': batch_size, 'iteration': iteration+1,
                                            'sequence': sequence, 'elapsed_ms': elapsed_ms, **telemetry})
        with (destination/'samples.csv').open('w', newline='', encoding='utf-8') as file:
            writer = csv.DictWriter(file, fieldnames=['records','requested_tracks','tracks','workers','batch_size','iteration','sequence','elapsed_ms',
                                                       'launch_us','first_batch_us','remaining_batches_us','remaining_batches','shutdown_us',
                                                       'parent_cpu_us','worker_cpu_us','parent_peak_kb','worker_peak_kb'])
            writer.writeheader()
            writer.writerows(samples)
        groups = {}
        for sample in samples:
            key = (sample['records'], sample['requested_tracks'], sample['tracks'], sample['batch_size'], sample['workers'])
            groups.setdefault(key, []).append(sample)
        with (destination/'summary.csv').open('w', newline='', encoding='utf-8') as file:
            summary_fields = ['records','requested_tracks','tracks','batch_size','workers','iterations','median_ms','mean_ms',
                              'stdev_ms','min_ms','max_ms','speedup_vs_one_worker',
                              'mean_launch_us','mean_first_batch_us','mean_later_batch_us','mean_shutdown_us']
            writer = csv.DictWriter(file, fieldnames=summary_fields)
            writer.writeheader()
            for (records, requested_tracks, tracks, batch_size, workers), group in sorted(groups.items()):
                times = [r['elapsed_ms'] for r in group]
                baseline = groups[(records, requested_tracks, tracks, batch_size, 1)]
                later = [r['remaining_batches_us']/r['remaining_batches'] for r in group if r['remaining_batches']]
                writer.writerow({'records': records, 'requested_tracks': requested_tracks, 'tracks': tracks, 'batch_size': batch_size,
                                 'workers': workers, 'iterations': len(times), 'median_ms': statistics.median(times),
                                 'mean_ms': statistics.mean(times), 'stdev_ms': statistics.stdev(times) if len(times)>1 else 0,
                                 'min_ms': min(times), 'max_ms': max(times),
                                 'speedup_vs_one_worker': statistics.median([r['elapsed_ms'] for r in baseline])/statistics.median(times),
                                 'mean_launch_us': statistics.mean(r['launch_us'] for r in group),
                                 'mean_first_batch_us': statistics.mean(r['first_batch_us'] for r in group),
                                 'mean_later_batch_us': statistics.mean(later) if later else '',
                                 'mean_shutdown_us': statistics.mean(r['shutdown_us'] for r in group)})
        manifest['status'] = 'complete'
        manifest['sample_count'] = len(samples)
    except (OSError, ValueError, RuntimeError, subprocess.SubprocessError) as error:
        manifest['status'] = 'failed'
        manifest['error'] = str(error)
        raise
    finally:
        manifest_path.write_text(json.dumps(manifest, indent=2)+'\n', encoding='utf-8')
    print(f'Validated {len(samples)} end-to-end timings in {destination}')


if __name__ == '__main__':
    main()
