"""Check a filter benchmark run's raw samples against its summary CSV."""
import csv
from collections import defaultdict
from pathlib import Path
import statistics
import sys

directory = Path(sys.argv[1])
with (directory / 'persistent_samples.csv').open(newline='', encoding='utf-8') as file:
    rows = list(csv.DictReader(file))
assert len(rows) == 600, f'expected 600 samples, got {len(rows)}'
groups = defaultdict(list)
run_ids = set()
for row in rows:
    run_ids.add(row['run_id'])
    groups[(int(row['records']), int(row['workers']))].append(float(row['elapsed_us']))
assert len(run_ids) == 1
assert set(groups) == {(records, workers) for records in (20000, 40000, 80000)
                       for workers in (1, 2, 4, 8)}
with (directory / 'persistent_summary.csv').open(newline='', encoding='utf-8') as file:
    summary = {row['call_name']: row for row in csv.DictReader(file)}
for (records, workers), samples in groups.items():
    assert len(samples) == 50
    name = f'KalmanFilter_{workers}Worker{"s" if workers != 1 else ""}_{records}Records'
    assert abs(statistics.mean(samples) - float(summary[name]['mean_us'])) < 0.001
    assert abs(statistics.median(samples) - float(summary[name]['median_us'])) < 0.001
print('Raw samples and summary agree for all 12 workload/worker configurations.')
