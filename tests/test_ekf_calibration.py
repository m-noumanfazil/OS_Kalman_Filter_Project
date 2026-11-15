import sys
from pathlib import Path
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'common'))
from evaluate_3d_scenario import position_nees


class CalibrationTests(unittest.TestCase):
    def test_diagonal_and_correlated_covariance(self):
        self.assertAlmostEqual(position_nees([2, 3, 4], [[4, 0, 0], [0, 9, 0], [0, 0, 16]]), 3)
        self.assertAlmostEqual(position_nees([1, 1, 0], [[2, 1, 0], [1, 2, 0], [0, 0, 1]]), 2/3)

    def test_rejects_invalid_covariance(self):
        with self.assertRaises(ValueError):
            position_nees([1, 1, 1], [[1, 2, 0], [2, 1, 0], [0, 0, 1]])


if __name__ == '__main__':
    unittest.main()
