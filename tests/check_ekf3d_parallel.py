"""Compare native parallel 3D results with the sequential driver."""
import csv
import math
from pathlib import Path
import subprocess
import sys
import tempfile

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
    for workers in (1, 2, 4, 8):
        output = root/f'workers-{workers}.csv'
        subprocess.run([str(parallel), str(source), str(output), str(workers)], check=True, timeout=90)
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
    subprocess.run([str(parallel), str(interleaved), str(interleaved_output), '4'], check=True, timeout=90)
    with interleaved_reference.open(newline='', encoding='utf-8') as left, \
         interleaved_output.open(newline='', encoding='utf-8') as right:
        if list(csv.DictReader(left)) != list(csv.DictReader(right)):
            raise AssertionError('interleaved parallel output differs from sequential output')
    print('Interleaved tracks match across batch boundaries.')
