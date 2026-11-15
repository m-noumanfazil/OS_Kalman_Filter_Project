"""Exercise the native EKF and calibration evaluator across independent seeds."""
from pathlib import Path
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[1]
binary = Path(sys.argv[1]).resolve()
with tempfile.TemporaryDirectory() as temporary:
    root = Path(temporary)
    for seed in (1, 7, 42, 101, 999):
        scenario = root / f'seed-{seed}'
        estimate = root / f'estimate-{seed}.csv'
        subprocess.run([sys.executable, '-B', str(ROOT / 'common/generate_3d_scenario.py'),
                        '--output-dir', str(scenario), '--seed', str(seed)], check=True)
        subprocess.run([str(binary), str(scenario / 'measurements.csv'), str(estimate)], check=True)
        subprocess.run([sys.executable, '-B', str(ROOT / 'common/evaluate_3d_scenario.py'),
                        str(scenario), str(estimate)], check=True)
