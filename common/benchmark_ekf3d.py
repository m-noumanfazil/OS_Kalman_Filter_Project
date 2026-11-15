#!/usr/bin/env python3
"""Measure complete native 3D runs and validate every output against sequential EKF."""
import argparse
import csv
import hashlib
import json
import math
import os
from pathlib import Path
import platform
import subprocess
import tempfile
import time


def digest(path):
    h = hashlib.sha256()
    with path.open('rb') as file:
        for block in iter(lambda: file.read(1024 * 1024), b''):
            h.update(block)
    return h.hexdigest()


def check_output(expected, output):
    with output.open(newline='', encoding='utf-8') as file:
        actual = list(csv.DictReader(file))
    if len(actual) != len(expected):
        raise ValueError('output row count mismatch')
    for index, (left, right) in enumerate(zip(expected, actual), 2):
        if left.keys() != right.keys() or left['track_id'] != right['track_id']:
            raise ValueError(f'output header or track mismatch at row {index}')
        for field in left.keys() - {'track_id'}:
            a, b = float(left[field]), float(right[field])
            if not math.isfinite(a) or not math.isfinite(b) or not math.isclose(a, b, rel_tol=1e-11, abs_tol=1e-9):
                raise ValueError(f'output mismatch at row {index}, field {field}')


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
    args = parser.parse_args()
    sequential, parallel, source, destination = (p.resolve() for p in (args.sequential, args.parallel, args.input, args.output_dir))
    if args.iterations < 1 or args.warmups < 0 or args.iterations > 1000 or args.warmups > 1000:
        parser.error('iterations must be 1–1000 and warmups 0–1000')
    if not all(p.is_file() for p in (sequential, parallel, source)):
        parser.error('both executables and the input must exist')
    with source.open(newline='', encoding='utf-8') as file:
        reader = csv.DictReader(file)
        fields = reader.fieldnames
        rows = list(reader)
    if not rows or len(rows) > 100000:
        parser.error('input must contain 1–100000 records')
    # Interleaving preserves chronological order within each track and allows
    # multiple workers to process each batch when tracks overlap in time.
    rows.sort(key=lambda row: (float(row['timestamp']), int(row['track_id'])))
    counts = sorted({max(1, len(rows)//4), max(1, len(rows)//2), len(rows)})
    destination.mkdir(parents=True, exist_ok=False)
    manifest = {
        'status': 'in_progress', 'platform': platform.platform(), 'processor': platform.processor(),
        'machine': platform.machine(), 'logical_cpus': os.cpu_count(), 'python_version': platform.python_version(),
        'input_sha256': digest(source), 'sequential_sha256': digest(sequential),
        'parallel_sha256': digest(parallel), 'build_command': args.build_command,
        'iterations': args.iterations, 'warmups': args.warmups, 'notes': args.notes,
        'workload_order': 'sorted by timestamp then track_id',
        'timing_scope': 'whole parallel process, including startup, input, logging, processing, shutdown and output',
        'cpu_memory_scope': 'not measured',
    }
    manifest_path = destination/'manifest.json'
    manifest_path.write_text(json.dumps(manifest, indent=2)+'\n', encoding='utf-8')
    samples = []
    try:
        with tempfile.TemporaryDirectory() as temporary:
            tmp = Path(temporary)
            for count in counts:
                workload = tmp/f'input-{count}.csv'
                with workload.open('w', newline='', encoding='utf-8') as file:
                    writer = csv.DictWriter(file, fieldnames=fields)
                    writer.writeheader()
                    writer.writerows(rows[:count])
                reference = tmp/f'reference-{count}.csv'
                subprocess.run([str(sequential), str(workload), str(reference)], check=True, capture_output=True, timeout=120)
                with reference.open(newline='', encoding='utf-8') as file:
                    expected = list(csv.DictReader(file))
                for workers in (1, 2, 4, 8):
                    for iteration in range(-args.warmups, args.iterations):
                        output = tmp/f'output-{count}-{workers}-{iteration}.csv'
                        start = time.perf_counter_ns()
                        result = subprocess.run([str(parallel), str(workload), str(output), str(workers)],
                                                capture_output=True, text=True, timeout=120)
                        elapsed_ms = (time.perf_counter_ns()-start)/1e6
                        if result.returncode:
                            raise RuntimeError(f'worker run failed: {result.stdout}\n{result.stderr}')
                        check_output(expected, output)
                        if iteration >= 0:
                            samples.append({'records': count, 'tracks': len({r['track_id'] for r in rows[:count]}),
                                            'workers': workers, 'iteration': iteration+1, 'elapsed_ms': elapsed_ms})
        with (destination/'samples.csv').open('w', newline='', encoding='utf-8') as file:
            writer = csv.DictWriter(file, fieldnames=['records','tracks','workers','iteration','elapsed_ms'])
            writer.writeheader()
            writer.writerows(samples)
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
