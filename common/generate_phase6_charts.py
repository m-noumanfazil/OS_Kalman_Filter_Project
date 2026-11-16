"""Generate Phase 6 figures from the saved Phase 4 and Phase 5 experiments.

Requires the project's existing matplotlib installation; no network access is
used. The historical Phase 1 figures in generate_charts.py are not modified.
"""
import argparse
import csv
import json
import math
import statistics
from pathlib import Path

import matplotlib
matplotlib.use('Agg')
import matplotlib.pyplot as plt

ROOT = Path(__file__).resolve().parents[1]
PHASE4 = ROOT / 'results/phase4/windows-clean-f6435cf'
PHASE5 = ROOT / 'results/phase5/multiseed-fe04715/manifest.json'


def windows_scaling(output):
    manifest = json.loads((PHASE4 / 'manifest.json').read_text(encoding='utf-8'))
    if manifest['status'] != 'complete':
        raise ValueError('Phase 4 experiment is not complete')
    with (PHASE4 / 'samples.csv').open(newline='', encoding='utf-8') as f:
        samples = list(csv.DictReader(f))
    with (PHASE4 / 'summary.csv').open(newline='', encoding='utf-8') as f:
        summary = list(csv.DictReader(f))
    fig, ax = plt.subplots(figsize=(9.5, 5.8), layout='constrained')
    colors = {32: '#1f6eac', 512: '#d06726'}
    for batch in (32, 512):
        medians = []
        for index, workers in enumerate((1, 2, 4, 8)):
            rows = [r for r in samples if int(r['records']) == 1800
                    and int(r['requested_tracks']) == 3 and int(r['tracks']) == 3
                    and int(r['batch_size']) == batch and int(r['workers']) == workers]
            if len(rows) != manifest['iterations']:
                raise ValueError(f'Unexpected sample count for batch {batch}, workers {workers}')
            values = [float(r['elapsed_ms']) for r in rows]
            if not all(math.isfinite(v) and v > 0 for v in values):
                raise ValueError('Invalid elapsed time')
            median = statistics.median(values)
            entries = [r for r in summary if int(r['records']) == 1800 and int(r['requested_tracks']) == 3
                       and int(r['tracks']) == 3 and int(r['batch_size']) == batch and int(r['workers']) == workers]
            if len(entries) != 1 or not math.isclose(median, float(entries[0]['median_ms']), rel_tol=1e-10):
                raise ValueError('Raw Phase 4 samples and summary disagree')
            medians.append(median)
            # Each point is a measured process invocation; slight fixed offsets aid visibility.
            offsets = [(i - (len(values)-1)/2)*0.035 for i in range(len(values))]
            ax.scatter([index + o for o in offsets], values, color=colors[batch], alpha=.42, s=24, zorder=3)
        ax.plot(range(4), medians, '-o', color=colors[batch], linewidth=2,
                label=f'Batch {batch}: median')
        for index, value in enumerate(medians):
            ax.annotate(f'{value:.1f}', (index, value), xytext=(5, 7 if batch == 32 else -14),
                        textcoords='offset points', fontsize=8, color=colors[batch])
    ax.set_xticks(range(4), ['1', '2', '4', '8'])
    ax.set(xlabel='Worker processes', ylabel='End-to-end elapsed time (ms)',
           title='Windows 3D EKF: full 1,800-observation workload')
    ax.set_ylim(bottom=0)
    ax.grid(axis='y', alpha=.25)
    ax.legend(loc='upper left', frameon=False)
    fig.text(.5, -.045, 'One Windows host, one session; three measured invocations per point. '
             'Dots show individual timings; includes startup, synchronization, logging and file I/O.',
             ha='center', fontsize=8, wrap=True)
    fig.savefig(output, dpi=180, bbox_inches='tight')
    plt.close(fig)


def multiseed_accuracy(output):
    data = json.loads(PHASE5.read_text(encoding='utf-8'))
    if data['status'] != 'complete' or len(data['runs']) != len(data['seeds']):
        raise ValueError('Phase 5 experiment is incomplete')
    groups = [('all', 'All rows'), ('straight', 'Straight'), ('acceleration', 'Acceleration'),
              ('turn', 'Turn'), ('post-turn', 'Post-turn'), ('missing', 'Missing')]
    fig, ax = plt.subplots(figsize=(11.5, 6), layout='constrained')
    colors = {'ekf': '#1f6eac', 'imm': '#d06726'}
    for index, (key, label) in enumerate(groups):
        values = {}
        for model in ('ekf', 'imm'):
            values[model] = [float(r[model]['state_rmse']['position_m'] if key == 'all'
                                   else r[model]['state_by_segment'][key]['position_m']) for r in data['runs']]
            if not all(math.isfinite(v) and v >= 0 for v in values[model]):
                raise ValueError(f'Invalid RMSE for {key}/{model}')
            expected = data['summary'][key]['position_m'][f'mean_{model}_rmse']
            if not math.isclose(statistics.mean(values[model]), expected, rel_tol=1e-10):
                raise ValueError(f'Phase 5 summary mismatch for {key}/{model}')
        for i in range(len(values['ekf'])):
            # Paired lines connect the same sensor-noise realization.
            ax.plot([index-.15, index+.15], [values['ekf'][i], values['imm'][i]],
                    color='#aab4bb', alpha=.4, linewidth=.6, zorder=1)
        for model, offset in (('ekf', -.15), ('imm', .15)):
            ax.scatter([index+offset]*len(values[model]), values[model], color=colors[model],
                       alpha=.38, s=15, zorder=2)
            mean = statistics.mean(values[model])
            ax.scatter([index+offset], [mean], marker='_', s=220, linewidths=2.5,
                       color=colors[model], zorder=4, label=model.upper() + ' mean' if index == 0 else None)
    ax.set_xticks(range(len(groups)), [label for _, label in groups])
    ax.set(xlabel='Ground-truth segment or observation condition', ylabel='3D position vector RMSE (m)',
           title='Sequential EKF and IMM: 20 paired noise realizations')
    ax.set_ylim(bottom=0)
    ax.grid(axis='y', alpha=.25)
    ax.legend(loc='upper right', frameon=False)
    fig.text(.5, -.055, 'Same synthetic trajectories, times and gaps for all seeds; dots are per-seed RMSE, '
             'short marks are their arithmetic means. Models use different noise settings.',
             ha='center', fontsize=8, wrap=True)
    fig.savefig(output, dpi=180, bbox_inches='tight')
    plt.close(fig)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output-dir', type=Path, default=ROOT / 'results/charts/phase6')
    args = parser.parse_args()
    args.output_dir.mkdir(parents=True, exist_ok=True)
    windows_scaling(args.output_dir / 'windows_ekf3d_scaling.png')
    multiseed_accuracy(args.output_dir / 'imm_ekf_multiseed_accuracy.png')
    print(f'Generated Phase 6 charts in {args.output_dir}')


if __name__ == '__main__':
    main()
