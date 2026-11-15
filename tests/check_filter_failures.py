"""Exercise native filter failure paths in disposable working directories."""
import argparse
import csv
import os
from pathlib import Path
import shutil
import subprocess
import tempfile


def workspace(root, platform):
    for relative in ('common', 'logs', f'results/{platform}', f'{platform}/Kalman_Filter'):
        (root / relative).mkdir(parents=True, exist_ok=True)


def run(binary, root, env=None):
    result = subprocess.run([str(binary)], cwd=root, stdout=subprocess.PIPE,
                            stderr=subprocess.STDOUT, text=True, timeout=120, env=env)
    if result.returncode == 0:
        raise AssertionError(f'Expected failure, got exit 0:\n{result.stdout[-3000:]}')
    if result.returncode < 0 or result.returncode >= 128:
        raise AssertionError(f'Filter crashed (exit {result.returncode}):\n{result.stdout[-3000:]}')
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('binary', type=Path)
    parser.add_argument('dataset', type=Path)
    parser.add_argument('platform', choices=['Linux', 'Windows'])
    parser.add_argument('--test-worker-failure', action='store_true',
                        help='binary must be compiled with -DKALMAN_TEST_FAILURES')
    args = parser.parse_args()
    binary, dataset = args.binary.resolve(), args.dataset.resolve()
    header = 'timestamp,track_id,true_x,true_y,meas_x,meas_y,true_vx,true_vy\n'
    cases = {
        'missing': None,
        'track out of range': header + '0,8,0,0,0,0,0,0\n',
        'non-finite value': header + '0,0,0,0,nan,0,0,0\n',
        'repeated timestamp': header + '1,0,0,0,1,1,0,0\n1,0,0,0,1,1,0,0\n',
        'malformed row': header + '0,0,0\n',
        'trailing numeric garbage': header + '0,0junk,0,0,0,0,0,0\n',
        'record limit exceeded': header + ''.join(f'{i},0,0,0,0,0,0,0\n' for i in range(100001)),
    }
    for name, content in cases.items():
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            workspace(root, args.platform)
            if content is not None:
                (root / 'common/sensor_data.csv').write_text(content, encoding='utf-8')
            run(binary, root)
            assert not (root / f'results/{args.platform}/filtered_sensor_data.csv').exists()
            print(f'{name}: rejected without producing output')
    with tempfile.TemporaryDirectory() as temporary:
        root = Path(temporary)
        workspace(root, args.platform)
        shutil.copyfile(dataset, root / 'common/sensor_data.csv')
        destination = root / f'results/{args.platform}/filtered_sensor_data.csv'
        destination.mkdir()
        run(binary, root)
        assert destination.is_dir()
        print('unwritable output path: reported failure')
    for filename in ('persistent_summary.csv', 'persistent_samples.csv'):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            workspace(root, args.platform)
            shutil.copyfile(dataset, root / 'common/sensor_data.csv')
            destination = root / f'results/{args.platform}/{filename}'
            destination.mkdir()
            run(binary, root)
            assert destination.is_dir()
            assert not (root / f'results/{args.platform}/filtered_sensor_data.csv').exists()
            if filename == 'persistent_samples.csv':
                assert not (root / f'results/{args.platform}/persistent_summary.csv').exists()
            print(f'unwritable {filename}: reported failure')
    if args.test_worker_failure:
        cases = [('KALMAN_TEST_FAIL_POOL_WORKER', 'Persistent worker startup or parity failed.'),
                 ('KALMAN_TEST_FAIL_POOL_SHUTDOWN', 'Worker failure; benchmark results discarded.')]
        for variable, diagnostic in cases:
            with tempfile.TemporaryDirectory() as temporary:
                root = Path(temporary)
                workspace(root, args.platform)
                shutil.copyfile(dataset, root / 'common/sensor_data.csv')
                env = os.environ.copy()
                env[variable] = '1'
                result = run(binary, root, env)
                if diagnostic not in result.stdout:
                    raise AssertionError('Expected a persistent-worker failure diagnostic')
                summary = root / f'results/{args.platform}/persistent_summary.csv'
                if summary.exists():
                    with summary.open(newline='', encoding='utf-8') as file:
                        assert all('_2Workers_' not in row['call_name'] for row in csv.DictReader(file))
                assert not (root / f'results/{args.platform}/filtered_sensor_data.csv').exists()
                print(f'{variable}: rejected without recording its benchmark')


if __name__ == '__main__':
    main()
