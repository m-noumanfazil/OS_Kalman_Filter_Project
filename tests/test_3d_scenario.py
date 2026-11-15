import csv
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


class ScenarioTests(unittest.TestCase):
    def test_seeded_scenario_is_separate_and_reproducible(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            for name in ('first', 'second'):
                subprocess.run([sys.executable, '-B', str(ROOT / 'common/generate_3d_scenario.py'),
                                '--output-dir', str(root / name), '--seed', '42'],
                               check=True, capture_output=True)
            for name in ('measurements.csv', 'ground_truth.csv'):
                self.assertEqual((root/'first'/name).read_bytes(), (root/'second'/name).read_bytes())
            with (root/'first'/'measurements.csv').open(newline='', encoding='utf-8') as file:
                rows = list(csv.DictReader(file))
            self.assertEqual(len(rows), 1800)
            self.assertTrue(any(row['available'] == '0' for row in rows))
            self.assertNotIn('x_m', rows[0])
            with (root/'first'/'ground_truth.csv').open(newline='', encoding='utf-8') as file:
                self.assertEqual(sum(1 for _ in csv.DictReader(file)), len(rows))


if __name__ == '__main__':
    unittest.main()
