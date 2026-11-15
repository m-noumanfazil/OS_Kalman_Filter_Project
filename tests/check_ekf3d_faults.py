"""Exercise 3D start and timeout failures using a test-instrumented binary."""
import os
from pathlib import Path
import re
import subprocess
import sys
import tempfile
import time
from process_checks import process_alive

ROOT = Path(__file__).resolve().parents[1]
binary = Path(sys.argv[1]).resolve()


with tempfile.TemporaryDirectory() as temporary:
    root = Path(temporary)
    scenario = root/'scenario'
    subprocess.run([sys.executable, '-B', str(ROOT/'common/generate_3d_scenario.py'),
                    '--output-dir', str(scenario)], check=True, capture_output=True)
    for variable, expected_count in [('EKF3D_TEST_FAIL_START', 1), ('EKF3D_TEST_HANG_WORKER', 2)]:
        env = os.environ.copy()
        env[variable] = '1'
        output = root/f'{variable}.csv'
        start = time.monotonic()
        result = subprocess.run([str(binary), str(scenario/'measurements.csv'), str(output), '2'],
                                env=env, capture_output=True, text=True, timeout=20)
        if result.returncode == 0 or output.exists():
            raise AssertionError(f'{variable}: failure was not propagated')
        if time.monotonic() - start >= 20:
            raise AssertionError(f'{variable}: shutdown exceeded timeout')
        pids = [int(value) for value in re.findall(r'^WORKER_PID (\d+)\s*$', result.stdout, re.MULTILINE)]
        if len(pids) != expected_count:
            raise AssertionError(f'{variable}: expected {expected_count} worker PIDs, got {pids}')
        if any(process_alive(pid) for pid in pids):
            raise AssertionError(f'{variable}: worker still running after parent exit: {pids}')
        log = Path(str(output)+'.log').read_text(encoding='utf-8')
        if 'FAILED' not in log:
            raise AssertionError(f'{variable}: missing failure log')
        print(f'{variable}: error propagated, {len(pids)} workers exited and no output produced')
