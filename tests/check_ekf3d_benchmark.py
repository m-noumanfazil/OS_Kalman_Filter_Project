"""Validate a one-iteration 1,800-record 3D benchmark smoke run."""
import csv
import json
from pathlib import Path
import statistics
import sys

root = Path(sys.argv[1])
manifest = json.loads((root/'manifest.json').read_text(encoding='utf-8'))
with (root/'samples.csv').open(newline='', encoding='utf-8') as file:
    rows = list(csv.DictReader(file))
assert manifest['status'] == 'complete' and manifest['sample_count'] == 12
assert len(rows) == 12
assert {(int(r['records']), int(r['workers'])) for r in rows} == {
    (records, workers) for records in (450, 900, 1800) for workers in (1, 2, 4, 8)
}
for row in rows:
    assert int(row['tracks']) == 3 and int(row['requested_tracks']) == 3 and float(row['elapsed_ms']) > 0 and int(row['batch_size']) == 128
    assert int(row['remaining_batches']) == (int(row['records']) + 127)//128 - 1
    for name in ('launch_us', 'first_batch_us', 'remaining_batches_us', 'shutdown_us'):
        assert float(row[name]) >= 0, f'negative timing: {row}'
    for name in ('parent_cpu_us', 'worker_cpu_us', 'parent_peak_kb', 'worker_peak_kb'):
        assert int(row[name]) >= 0, f'{name} was unavailable: {row}'
with (root/'summary.csv').open(newline='', encoding='utf-8') as file:
    summary = list(csv.DictReader(file))
assert len(summary) == len(rows)
for row in summary:
    matching = [r for r in rows if all(r[key] == row[key] for key in ('records','requested_tracks','tracks','batch_size','workers'))]
    assert len(matching) == 1
    assert abs(float(row['median_ms']) - statistics.median(float(r['elapsed_ms']) for r in matching)) < 1e-9
    baseline = next(item for item in summary if item['records'] == row['records'] and
                    item['requested_tracks'] == row['requested_tracks'] and item['batch_size'] == row['batch_size']
                    and item['workers'] == '1')
    assert abs(float(row['speedup_vs_one_worker']) - float(baseline['median_ms']) / float(row['median_ms'])) < 1e-9
print('All 12 timings contain parent/worker resource measurements.')
