import csv
import json
import math
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


def write_csv(path, fields, rows):
    with path.open('w', newline='', encoding='utf-8') as file:
        writer = csv.DictWriter(file, fieldnames=fields)
        writer.writeheader()
        writer.writerows(rows)


class AccuracyTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.scenario = self.root/'scenario'
        self.scenario.mkdir()
        self.measurements = [
            {'timestamp':'0','track_id':'0','range_m':'5','azimuth_rad':str(math.atan2(4,3)),'elevation_rad':'0','available':'1'},
            {'timestamp':'1','track_id':'0','range_m':'','azimuth_rad':'','elevation_rad':'','available':'0'},
        ]
        base_truth = {'track_id':'0','x_m':'3','y_m':'4','z_m':'0','vx_mps':'0','vy_mps':'0',
                      'vz_mps':'0','ax_mps2':'0','ay_mps2':'0','az_mps2':'0'}
        self.truth = [dict(base_truth, timestamp='0', segment='turn'),
                      dict(base_truth, timestamp='1', segment='post-turn')]
        cov = {'p_xx':'1','p_xy':'0','p_xz':'0','p_yy':'1','p_yz':'0','p_zz':'1'}
        self.estimates = [dict(cov,timestamp='0',track_id='0',x='4',y='6',z='2',vx='3',vy='4',vz='0',ax='0',ay='0',az='2'),
                          dict(cov,timestamp='1',track_id='0',x='3',y='4',z='0',vx='0',vy='0',vz='0',ax='0',ay='0',az='0')]
        self.output = self.root/'estimates.csv'
        self.report = self.root/'report.json'

    def run_evaluator(self):
        write_csv(self.scenario/'measurements.csv', self.measurements[0].keys(), self.measurements)
        write_csv(self.scenario/'ground_truth.csv', self.truth[0].keys(), self.truth)
        write_csv(self.output, self.estimates[0].keys(), self.estimates)
        return subprocess.run([sys.executable, '-B', str(ROOT/'common/evaluate_3d_scenario.py'),
                               str(self.scenario), str(self.output), '--json-output', str(self.report)],
                              capture_output=True, text=True)

    def test_exact_metrics_and_explicit_segments(self):
        result = self.run_evaluator()
        self.assertEqual(result.returncode, 0, result.stderr)
        report = json.loads(self.report.read_text(encoding='utf-8'))
        self.assertEqual((report['all_row_count'], report['available_row_count']), (2, 1))
        self.assertAlmostEqual(report['state_rmse']['position_m'], math.sqrt(9/2))
        self.assertAlmostEqual(report['state_rmse']['velocity_mps'], math.sqrt(25/2))
        self.assertAlmostEqual(report['state_rmse']['acceleration_mps2'], math.sqrt(4/2))
        self.assertEqual(report['state_by_segment']['turn']['count'], 1)
        self.assertEqual(report['state_by_segment']['missing']['count'], 1)
        self.assertEqual(report['position_nees']['available']['coverage_95_fraction'], 0)
        self.assertEqual(report['position_nees']['missing']['coverage_95_fraction'], 1)
        self.assertTrue(all(len(value) == 64 for value in report['input_sha256'].values()))

    def test_invalid_segment_and_timestamp_fail(self):
        self.truth[0]['segment'] = 'unknown'
        self.assertNotEqual(self.run_evaluator().returncode, 0)
        self.assertFalse(self.report.exists())
        self.truth[0]['segment'] = 'turn'
        self.measurements[1]['timestamp'] = self.truth[1]['timestamp'] = self.estimates[1]['timestamp'] = '0'
        self.assertNotEqual(self.run_evaluator().returncode, 0)
        self.assertFalse(self.report.exists())

    def test_existing_report_is_not_overwritten(self):
        self.report.write_text('keep me', encoding='utf-8')
        self.assertNotEqual(self.run_evaluator().returncode, 0)
        self.assertEqual(self.report.read_text(encoding='utf-8'), 'keep me')

    def test_invalid_missing_and_radar_values_fail(self):
        self.measurements[1]['range_m'] = '5'
        self.assertNotEqual(self.run_evaluator().returncode, 0)
        self.measurements[1]['range_m'] = ''
        self.measurements[0]['range_m'] = '-5'
        self.assertNotEqual(self.run_evaluator().returncode, 0)
        self.assertFalse(self.report.exists())


if __name__ == '__main__':
    unittest.main()
