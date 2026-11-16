import csv
import importlib.util
from pathlib import Path
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location('benchmark_ekf3d', ROOT / 'common/benchmark_ekf3d.py')
benchmark = importlib.util.module_from_spec(spec)
spec.loader.exec_module(benchmark)

FIELDS = ['timestamp', 'track_id', 'range_m', 'azimuth_rad', 'elevation_rad', 'available']
VALID = {'timestamp': '0', 'track_id': '0', 'range_m': '1000', 'azimuth_rad': '0',
         'elevation_rad': '0', 'available': '1'}


class BenchmarkValidationTests(unittest.TestCase):
    def test_invalid_source_is_not_reordered_into_validity(self):
        second = dict(VALID, timestamp='2')
        first = dict(VALID, timestamp='1')
        with self.assertRaisesRegex(ValueError, 'strictly increasing'):
            benchmark.validate_source(FIELDS, [second, first])
        for update in ({'timestamp': 'nan'}, {'track_id': '2junk'}, {'range_m': '-1'},
                       {'elevation_rad': '2'}, {'available': '0'}, {'range_m': 'inf'}):
            with self.subTest(update=update), self.assertRaises(ValueError):
                benchmark.validate_source(FIELDS, [dict(VALID, **update)])
        benchmark.validate_source(FIELDS, [VALID, dict(VALID, timestamp='1')])

    def test_telemetry_validation(self):
        timing = 'TIMING batch_size=128 launch_us=10 first_batch_us=20 remaining_batches_us=30 remaining_batches=1 shutdown_us=5\n'
        resource = 'RESOURCE parent_cpu_us=100 worker_cpu_us=150 parent_peak_kb=200 worker_peak_kb=300\n'
        result = benchmark.parse_telemetry(timing + resource, 128, 200, 1)
        self.assertEqual(result['remaining_batches'], 1)
        self.assertEqual(result['worker_cpu_us'], 150)
        for bad in (timing.replace('remaining_batches=1', 'remaining_batches=2') + resource,
                    timing.replace('launch_us=10', 'launch_us=1e999') + resource,
                    timing + resource.replace('parent_cpu_us=100', 'parent_cpu_us=-2'),
                    timing + resource + resource,
                    timing.replace('launch_us=10', 'launch_us=5000') + resource):
            with self.subTest(bad=bad), self.assertRaises(ValueError):
                benchmark.parse_telemetry(bad, 128, 200, 1)

    def test_timestamp_uses_absolute_tolerance(self):
        reference = [{'timestamp': '100000000000', 'track_id': '0', 'x': '1'}]
        with tempfile.TemporaryDirectory() as temporary:
            output = Path(temporary) / 'output.csv'
            with output.open('w', newline='', encoding='utf-8') as file:
                writer = csv.DictWriter(file, fieldnames=['timestamp', 'track_id', 'x'])
                writer.writeheader()
                writer.writerow({'timestamp': '100000000000.1', 'track_id': '0', 'x': '1'})
            with self.assertRaisesRegex(ValueError, 'timestamp'):
                benchmark.check_output(reference, output)


if __name__ == '__main__':
    unittest.main()
