"""Validate the persisted multi-seed summary against its per-seed evidence."""
import hashlib
import json
import platform
import statistics
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
EVIDENCE = ROOT / 'results/phase5/multiseed-fe04715/manifest.json'


class MultiSeedEvidenceTests(unittest.TestCase):
    def test_paired_summary(self):
        data = json.loads(EVIDENCE.read_text(encoding='utf-8'))
        self.assertEqual(data['status'], 'complete')
        self.assertEqual(data['seeds'], list(range(20)))
        self.assertEqual([r['seed'] for r in data['runs']], data['seeds'])
        self.assertEqual(len({r['measurement_sha256'] for r in data['runs']}), 20)
        self.assertEqual(len({r['truth_sha256'] for r in data['runs']}), 1)
        for group, metrics in data['summary'].items():
            for metric, summary in metrics.items():
                values = {}
                for model in ('ekf', 'imm'):
                    values[model] = [r[model]['state_rmse'][metric] if group == 'all'
                                     else r[model]['state_by_segment'][group][metric] for r in data['runs']]
                    self.assertAlmostEqual(summary[f'mean_{model}_rmse'], statistics.mean(values[model]))
                self.assertEqual(summary['imm_lower_seed_count'], sum(i < e for i, e in zip(values['imm'], values['ekf'])))
                self.assertAlmostEqual(summary['paired_difference_imm_minus_ekf'],
                                       statistics.mean(i-e for i, e in zip(values['imm'], values['ekf'])))
        for row in data['runs']:
            for model in ('ekf', 'imm'):
                self.assertEqual(row[model]['input_sha256']['measurements'], row['measurement_sha256'])
                self.assertEqual(row[model]['input_sha256']['ground_truth'], row['truth_sha256'])
            for diagnostic in row['model_probability'].values():
                self.assertGreater(diagnostic['count'], 0)
                self.assertLessEqual(0, diagnostic['min_ca'])
                self.assertLessEqual(diagnostic['min_ca'], diagnostic['mean_ca'])
                self.assertLessEqual(diagnostic['mean_ca'], diagnostic['max_ca'])
                self.assertLessEqual(diagnostic['max_ca'], 1)

    def test_seed_zero_scenario_hashes(self):
        manifest = json.loads(EVIDENCE.read_text(encoding='utf-8'))
        # Transcendental math and random.gauss output can differ across runtimes.
        if platform.platform() != manifest['platform'] or platform.python_version() != manifest['python_version']:
            self.skipTest('exact scenario hashes require the recorded Python and platform')
        data = manifest['runs'][0]
        with tempfile.TemporaryDirectory(prefix='imm-evidence-') as tmp:
            scenario = Path(tmp) / 'scenario'
            subprocess.run([sys.executable, '-B', str(ROOT / 'common/generate_3d_scenario.py'),
                            '--output-dir', str(scenario), '--seed', '0'], check=True, stdout=subprocess.DEVNULL)
            for filename, key in (('measurements.csv', 'measurement_sha256'), ('ground_truth.csv', 'truth_sha256')):
                self.assertEqual(hashlib.sha256((scenario / filename).read_bytes()).hexdigest(), data[key])


if __name__ == '__main__':
    unittest.main()
