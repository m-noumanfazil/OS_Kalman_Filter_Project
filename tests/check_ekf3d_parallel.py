"""Compare native parallel 3D results with the sequential driver."""
import csv
import math
import os
import re
from pathlib import Path
import subprocess
import sys
import tempfile
from process_checks import process_alive

ROOT = Path(__file__).resolve().parents[1]
sequential, parallel = (Path(value).resolve() for value in sys.argv[1:3])
with tempfile.TemporaryDirectory() as temporary:
    root = Path(temporary)
    scenario = root / 'scenario'
    subprocess.run([sys.executable, '-B', str(ROOT/'common/generate_3d_scenario.py'),
                    '--output-dir', str(scenario)], check=True)
    source, reference = scenario/'measurements.csv', root/'reference.csv'
    subprocess.run([str(sequential), str(source), str(reference)], check=True)
    with reference.open(newline='', encoding='utf-8') as file:
        expected = list(csv.DictReader(file))
    handle_deltas = []
    for workers in (1, 2, 4, 8):
        output = root/f'workers-{workers}.csv'
        run = subprocess.run([str(parallel), str(source), str(output), str(workers)], check=True, timeout=90,
                             capture_output=True, text=True)
        match = re.search(r'^RESOURCE_COUNTS before=(-?\d+) after=(-?\d+)\s*$', run.stdout, re.MULTILINE)
        if not match or int(match[1]) < 0 or int(match[2]) < 0:
            raise AssertionError(f'parent descriptor/handle count unavailable: {run.stdout}')
        handle_deltas.append(int(match[2]) - int(match[1]))
        pids = [int(pid) for pid in re.findall(r'^WORKER_PID (\d+)\s*$', run.stdout, re.MULTILINE)]
        if len(pids) != workers or any(process_alive(pid) for pid in pids):
            raise AssertionError(f'workers were not cleaned up: {pids}')
        log = Path(str(output) + '.log')
        entries = log.read_text(encoding='utf-8')
        names = ('CreateFileA', 'CreateFileMappingA', 'CreateProcessA', 'UnmapViewOfFile') if os.name == 'nt' else ('open', 'mmap', 'fork', 'munmap')
        if not all(name in entries for name in names):
            raise AssertionError(f'missing expected system-call log entries: {names}')
        child_names = ('OpenFileMappingA', 'WaitForMultipleObjects', 'SetEvent', 'UnmapViewOfFile') if os.name == 'nt' else ('read', 'write', 'close')
        for worker_id in range(workers):
            worker_log = Path(str(output) + f'.worker-{worker_id}.log')
            child_entries = worker_log.read_text(encoding='utf-8')
            if not all(name in child_entries for name in child_names):
                raise AssertionError(f'missing worker {worker_id} log entries: {child_names}')
        with output.open(newline='', encoding='utf-8') as file:
            actual = list(csv.DictReader(file))
        if len(actual) != len(expected) or not expected:
            raise AssertionError(f'row count mismatch for {workers} workers')
        for row_number, (left, right) in enumerate(zip(expected, actual), 2):
            if left.keys() != right.keys() or left['track_id'] != right['track_id']:
                raise AssertionError(f'header or track mismatch at row {row_number}')
            for name in left.keys() - {'track_id'}:
                a, b = float(left[name]), float(right[name])
                if not math.isfinite(a) or not math.isfinite(b) or not math.isclose(a, b, rel_tol=1e-11, abs_tol=1e-9):
                    raise AssertionError(f'{workers} workers, row {row_number}, {name}: {a} vs {b}')
        print(f'{workers} workers: {len(actual)} rows match all sequential state and covariance fields')
    if max(handle_deltas) - min(handle_deltas) > 2:
        raise AssertionError(f'parent handle/descriptor growth varies with worker count: {handle_deltas}')
    print(f'Parent descriptor/handle deltas for 1/2/4/8 workers: {handle_deltas} (diagnostic, not a leak proof).')
    before = reference.read_bytes()
    result = subprocess.run([str(parallel), str(source), str(reference), '2'], timeout=90, capture_output=True)
    if result.returncode == 0 or reference.read_bytes() != before:
        raise AssertionError('existing output must not be overwritten')
    print('Existing output protection passed.')
    invalid = root/'invalid.csv'
    invalid.write_text('timestamp,track_id,range_m,azimuth_rad,elevation_rad,available\n0,0,-1,0,0,1\n', encoding='utf-8')
    rejected = root/'rejected.csv'
    result = subprocess.run([str(parallel), str(invalid), str(rejected), '2'], timeout=30, capture_output=True)
    if result.returncode == 0 or rejected.exists():
        raise AssertionError('worker computation failure must not create output')
    print('Worker error propagation passed.')
    for invalid_batch in ('0', '-1', '100001', 'bad'):
        result = subprocess.run([str(parallel), str(source), str(root/f'invalid-batch-{invalid_batch}.csv'), '2', invalid_batch],
                                timeout=30, capture_output=True)
        if result.returncode == 0:
            raise AssertionError(f'invalid batch size {invalid_batch} was accepted')
    print('Invalid batch sizes rejected.')
    # Interleave tracks in time to exercise simultaneous worker activity and
    # state continuity over many batch boundaries, instead of only grouped input.
    with source.open(newline='', encoding='utf-8') as file:
        reader = csv.DictReader(file)
        fields = reader.fieldnames
        rows = sorted(reader, key=lambda row: (float(row['timestamp']), int(row['track_id'])))
    interleaved = root/'interleaved.csv'
    with interleaved.open('w', newline='', encoding='utf-8') as file:
        writer = csv.DictWriter(file, fieldnames=fields)
        writer.writeheader()
        writer.writerows(rows)
    interleaved_reference = root/'interleaved-reference.csv'
    interleaved_output = root/'interleaved-parallel.csv'
    subprocess.run([str(sequential), str(interleaved), str(interleaved_reference)], check=True, timeout=90)
    subprocess.run([str(parallel), str(interleaved), str(interleaved_output), '4'], check=True, timeout=90, capture_output=True)
    with interleaved_reference.open(newline='', encoding='utf-8') as left, \
         interleaved_output.open(newline='', encoding='utf-8') as right:
        if list(csv.DictReader(left)) != list(csv.DictReader(right)):
            raise AssertionError('interleaved parallel output differs from sequential output')
    print('Interleaved tracks match across batch boundaries.')
    expanded = root/'expanded.csv'
    expanded_rows = []
    for replica in range(8):
        for row in rows:
            item = dict(row)
            item['track_id'] = str(int(item['track_id']) + replica * 3)
            expanded_rows.append(item)
    expanded_rows.sort(key=lambda row: (float(row['timestamp']), int(row['track_id'])))
    with expanded.open('w', newline='', encoding='utf-8') as file:
        writer = csv.DictWriter(file, fieldnames=fields)
        writer.writeheader()
        writer.writerows(expanded_rows)
    large_reference, large_output = root/'large-reference.csv', root/'large-parallel.csv'
    subprocess.run([str(sequential), str(expanded), str(large_reference)], check=True, timeout=90)
    subprocess.run([str(parallel), str(expanded), str(large_output), '4'], check=True, timeout=90, capture_output=True)
    with large_reference.open(newline='', encoding='utf-8') as left, large_output.open(newline='', encoding='utf-8') as right:
        reference_rows, output_rows = list(csv.DictReader(left)), list(csv.DictReader(right))
    if len(reference_rows) != 14400 or reference_rows != output_rows:
        raise AssertionError('expanded 24-track output differs from sequential output')
    print('Expanded workload: 14,400 interleaved observations across 24 tracks match.')
