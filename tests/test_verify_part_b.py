import importlib.util
from pathlib import Path
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location('verifier', ROOT / 'common/verify_part_b.py')
verifier = importlib.util.module_from_spec(spec)
spec.loader.exec_module(verifier)


class VerifyNativeOutputTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.source = Path(self.temp.name) / 'input.csv'
        self.output = Path(self.temp.name) / 'output.csv'
        self.source.write_text('timestamp,track_id,meas_x,meas_y\n0,0,3,4\n', encoding='utf-8')
        self.output.write_text('timestamp,track_id,est_x,est_y,est_vx,est_vy\n0,0,3,4,0,0\n', encoding='utf-8')

    def test_matching_and_preserved_output(self):
        before = self.output.read_bytes()
        verifier.verify(self.source, self.output, 0.00051)
        self.assertEqual(before, self.output.read_bytes())

    def test_corrupt_output_fails(self):
        for row in ['0,0,3,4,1,0', 'nan,0,3,4,0,0', '0,0,nan,4,0,0', '0,1,3,4,0,0']:
            with self.subTest(row=row):
                self.output.write_text('timestamp,track_id,est_x,est_y,est_vx,est_vy\n' + row + '\n', encoding='utf-8')
                with self.assertRaises(ValueError):
                    verifier.verify(self.source, self.output, 0.00051)

    def test_missing_row_fails(self):
        self.output.write_text('timestamp,track_id,est_x,est_y,est_vx,est_vy\n', encoding='utf-8')
        with self.assertRaises(ValueError):
            verifier.verify(self.source, self.output, 0.00051)


if __name__ == '__main__':
    unittest.main()
