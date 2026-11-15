"""Validate a one-iteration 1,800-record 3D benchmark smoke run."""
import csv
import json
from pathlib import Path
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
    assert int(row['tracks']) == 3 and float(row['elapsed_ms']) > 0
    for name in ('parent_cpu_us', 'worker_cpu_us', 'parent_peak_kb', 'worker_peak_kb'):
        assert int(row[name]) >= 0, f'{name} was unavailable: {row}'
print('All 12 timings contain parent/worker resource measurements.')
