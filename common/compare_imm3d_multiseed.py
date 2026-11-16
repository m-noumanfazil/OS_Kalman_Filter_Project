#!/usr/bin/env python3
"""Compare IMM and EKF on independently seeded measurement noise.

The scenario geometry, maneuver schedule and observation gaps are fixed across
seeds. The resulting trials are not independent trajectory or real-world tests.
"""
import argparse
import csv
import hashlib
import json
import math
import platform
import statistics
import subprocess
import sys
import tempfile
from pathlib import Path


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def run(command):
    subprocess.run([str(x) for x in command], check=True, stdout=subprocess.DEVNULL)


def probability_diagnostics(scenario, estimates):
    groups = {k: [] for k in ('straight', 'acceleration', 'turn', 'post-turn', 'missing')}
    with (scenario / 'ground_truth.csv').open(newline='', encoding='utf-8') as tf, \
         (scenario / 'measurements.csv').open(newline='', encoding='utf-8') as mf, \
         estimates.open(newline='', encoding='utf-8') as ef:
        for truth, measure, estimate in zip(csv.DictReader(tf), csv.DictReader(mf), csv.DictReader(ef), strict=True):
            if truth['track_id'] != estimate['track_id'] or truth['track_id'] != measure['track_id']:
                raise ValueError('track mismatch')
            if abs(float(truth['timestamp']) - float(estimate['timestamp'])) > 1e-9 or truth['timestamp'] != measure['timestamp']:
                raise ValueError('timestamp mismatch')
            cv, ca = float(estimate['prob_cv']), float(estimate['prob_ca'])
            if not all(math.isfinite(v) and 0 <= v <= 1 for v in (cv, ca)) or abs(cv + ca - 1) > 1e-12:
                raise ValueError('invalid model probabilities')
            groups[truth['segment']].append(ca)
            if measure['available'] == '0':
                groups['missing'].append(ca)
    return {key: {'count': len(values), 'mean_ca': statistics.mean(values),
                  'min_ca': min(values), 'max_ca': max(values)} for key, values in groups.items()}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--ekf', type=Path, required=True)
    parser.add_argument('--imm', type=Path, required=True)
    parser.add_argument('--output-dir', type=Path, required=True, help='must not already exist')
    parser.add_argument('--seeds', type=int, nargs='+', default=list(range(20)))
    parser.add_argument('--notes', default='')
    args = parser.parse_args()
    if len(set(args.seeds)) != len(args.seeds):
        parser.error('seeds must be distinct')
    args.output_dir.mkdir(parents=True, exist_ok=False)
    script = Path(__file__).resolve().parent
    ekf, imm = args.ekf.resolve(), args.imm.resolve()
    try:
        root = script.parent
        revision = subprocess.check_output(['git', 'rev-parse', 'HEAD'], cwd=root, text=True).strip()
        dirty = bool(subprocess.check_output(['git', 'status', '--porcelain'], cwd=root, text=True).strip())
        checkout = {'revision': revision, 'tracked_or_untracked_changes': dirty}
    except (OSError, subprocess.CalledProcessError):
        checkout = None
    manifest = {
        'status': 'running', 'platform': platform.platform(), 'processor': platform.processor(),
        'python_version': platform.python_version(), 'seeds': args.seeds,
        'ekf_executable_sha256': digest(ekf), 'imm_executable_sha256': digest(imm),
        'generator_sha256': digest(script / 'generate_3d_scenario.py'),
        'evaluator_sha256': digest(script / 'evaluate_3d_scenario.py'),
        'source_checkout': checkout,
        'source_sha256': {name: digest(script / name) for name in ('ekf3d.hpp', 'imm3d.hpp', 'run_ekf3d.cpp', 'run_imm3d.cpp')},
        'method': 'same deterministic trajectories, observation times and missing-data pattern; different seeded measurement noise',
        'notes': args.notes, 'runs': []}
    manifest_path = args.output_dir / 'manifest.json'
    def save():
        manifest_path.write_text(json.dumps(manifest, indent=2, allow_nan=False) + '\n', encoding='utf-8')
    save()
    try:
        with tempfile.TemporaryDirectory(prefix='imm-multiseed-') as temp:
            root = Path(temp)
            for seed in args.seeds:
                scenario = root / f'scenario-{seed}'
                run([sys.executable, '-B', script / 'generate_3d_scenario.py', '--output-dir', scenario, '--seed', seed])
                row = {'seed': seed, 'measurement_sha256': digest(scenario / 'measurements.csv'),
                       'truth_sha256': digest(scenario / 'ground_truth.csv')}
                for name, executable in (('ekf', ekf), ('imm', imm)):
                    output = root / f'{name}-{seed}.csv'
                    evaluation = root / f'{name}-{seed}.json'
                    run([executable, scenario / 'measurements.csv', output])
                    run([sys.executable, '-B', script / 'evaluate_3d_scenario.py', scenario, output, '--json-output', evaluation])
                    row[name] = json.loads(evaluation.read_text(encoding='utf-8'))
                    if name == 'imm': row['model_probability'] = probability_diagnostics(scenario, output)
                manifest['runs'].append(row)
                print(f'Seed {seed}: EKF {row["ekf"]["state_rmse"]["position_m"]:.3f} m; IMM {row["imm"]["state_rmse"]["position_m"]:.3f} m', flush=True)
        summary = {}
        for group in ('all', 'straight', 'acceleration', 'turn', 'post-turn', 'missing'):
            summary[group] = {}
            for metric in ('position_m', 'velocity_mps', 'acceleration_mps2'):
                values = {}
                for model in ('ekf', 'imm'):
                    values[model] = [r[model]['state_rmse'][metric] if group == 'all' else r[model]['state_by_segment'][group][metric]
                                     for r in manifest['runs']]
                summary[group][metric] = {
                    'mean_ekf_rmse': statistics.mean(values['ekf']), 'mean_imm_rmse': statistics.mean(values['imm']),
                    'imm_lower_seed_count': sum(i < e for i, e in zip(values['imm'], values['ekf'])),
                    'paired_difference_imm_minus_ekf': statistics.mean(i-e for i, e in zip(values['imm'], values['ekf'])),
                }
        manifest['summary'] = summary
        manifest['status'] = 'complete'
        save()
    except Exception:
        manifest['status'] = 'failed'
        save()
        raise
    print(f'Wrote {manifest_path}')


if __name__ == '__main__':
    main()
