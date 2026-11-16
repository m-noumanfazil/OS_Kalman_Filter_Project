"""Compare sequential EKF and IMM on identical input without external packages."""
import argparse
import csv
import hashlib
import json
import math
import platform
import statistics
import subprocess
import sys
import time
from pathlib import Path


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--scenario', type=Path, required=True)
    p.add_argument('--ekf', type=Path, required=True)
    p.add_argument('--imm', type=Path, required=True)
    p.add_argument('--output-dir', type=Path, required=True)
    p.add_argument('--iterations', type=int, default=5)
    p.add_argument('--notes', default='')
    args = p.parse_args()
    if args.iterations < 1:
        p.error('iterations must be positive')
    args.output_dir.mkdir(parents=True, exist_ok=False)
    measurements = args.scenario / 'measurements.csv'
    commands = {'ekf': args.ekf.resolve(), 'imm': args.imm.resolve()}
    timings = {'ekf': [], 'imm': []}
    output_hashes = {}
    # Alternate order to reduce (not eliminate) ordering effects; one warm-up each.
    for iteration in range(args.iterations + 1):
        order = ('ekf', 'imm') if iteration % 2 == 0 else ('imm', 'ekf')
        for name in order:
            output = args.output_dir / f'{name}-{iteration}.csv'
            start = time.perf_counter_ns()
            subprocess.run([str(commands[name]), str(measurements), str(output)], check=True, stdout=subprocess.DEVNULL)
            elapsed = (time.perf_counter_ns() - start) / 1e6
            digest = sha(output)
            if name in output_hashes and digest != output_hashes[name]:
                raise ValueError(f'{name} output changed between repetitions')
            output_hashes[name] = digest
            if iteration:
                timings[name].append(elapsed)
    for name in commands:
        subprocess.run([sys.executable, '-B', str(Path(__file__).with_name('evaluate_3d_scenario.py')),
                        str(args.scenario), str(args.output_dir / f'{name}-1.csv'), '--json-output',
                        str(args.output_dir / f'{name}-accuracy.json')], check=True)
    tracks = {}
    with (args.output_dir / 'imm-1.csv').open(newline='', encoding='utf-8') as f:
        for row in csv.DictReader(f):
            timestamp = float(row['timestamp'])
            ca, cv = float(row['prob_ca']), float(row['prob_cv'])
            if (not all(math.isfinite(v) for v in (timestamp, ca, cv)) or
                    not (0 <= ca <= 1 and 0 <= cv <= 1) or abs(ca + cv - 1) > 1e-9):
                raise ValueError('invalid IMM timestamp or model probabilities')
            tracks.setdefault(row['track_id'], []).append((timestamp, ca))
    # SVG is portable, reproducible, and needs no plotting dependency.
    all_times = [t for values in tracks.values() for t, _ in values]
    lo, hi = min(all_times), max(all_times)
    colors = ['#1261a0', '#bd5d00', '#26734d', '#8a3d9a']
    svg = ['<svg xmlns="http://www.w3.org/2000/svg" width="900" height="360" viewBox="0 0 900 360">',
           '<rect width="900" height="360" fill="white"/><path d="M70 30V300H870" fill="none" stroke="black"/>',
           '<text x="70" y="20">Constant-acceleration model probability</text>',
           '<text x="45" y="35">1</text><text x="45" y="305">0</text>',
           f'<text x="70" y="318">{lo:.2f}</text><text x="820" y="318">{hi:.2f}</text>',
           '<text x="730" y="345">Time (s)</text>']
    for index, (track, values) in enumerate(sorted(tracks.items())):
        points = ' '.join(f'{70+800*(t-lo)/(hi-lo or 1):.2f},{300-270*v:.2f}' for t, v in values)
        color = colors[index % len(colors)]
        svg.append(f'<polyline points="{points}" fill="none" stroke="{color}" stroke-width="1"/>')
        svg.append(f'<text x="{80+index*130}" y="345" fill="{color}">Track {int(track)}</text>')
    svg.append('</svg>')
    (args.output_dir / 'model-probabilities.svg').write_text('\n'.join(svg), encoding='utf-8')
    manifest = {'platform': platform.platform(), 'processor': platform.processor(), 'python': platform.python_version(),
                'measurement_sha256': sha(measurements), 'truth_sha256': sha(args.scenario / 'ground_truth.csv'),
                'executable_sha256': {name: sha(path) for name, path in commands.items()},
                'iterations': args.iterations, 'warmups_per_model': 1, 'timing_scope': 'complete sequential process including CSV I/O',
                'order': 'alternating', 'samples_ms': timings,
                'output_sha256': output_hashes,
                'median_ms': {name: statistics.median(values) for name, values in timings.items()}, 'notes': args.notes}
    (args.output_dir / 'manifest.json').write_text(json.dumps(manifest, indent=2) + '\n', encoding='utf-8')
    print('Results saved to', args.output_dir)


if __name__ == '__main__':
    main()
