"""
generate_charts.py — CT-353 OS CCA Chart Generator
Produces 6 PNG charts for the OS benchmarking + Kalman Filter project.
Run from any directory; uses absolute paths internally.
"""

import os
import csv
import math
import matplotlib
matplotlib.use('Agg')
import matplotlib.pyplot as plt
import matplotlib.patches as mpatches
import numpy as np

# ─── Paths ────────────────────────────────────────────────────────────────────
ROOT = r"D:\UNBACKED DATA\CCPs 5th Sem\OS_CCP"
SENSOR_CSV          = os.path.join(ROOT, "common", "sensor_data.csv")
FILTERED_WIN_CSV    = os.path.join(ROOT, "results", "Windows", "filtered_sensor_data.csv")
WIN_RESULTS_CSV     = os.path.join(ROOT, "results", "Windows", "windows_results.csv")
LINUX_RESULTS_CSV   = os.path.join(ROOT, "results", "Linux",   "linux_results.csv")
CHARTS_DIR          = os.path.join(ROOT, "results", "charts")
os.makedirs(CHARTS_DIR, exist_ok=True)

# ─── Helper: load CSV → dict keyed by first column ────────────────────────────
def load_csv_dict(path):
    rows = {}
    with open(path, newline='', encoding='utf-8') as f:
        reader = csv.DictReader(f)
        for row in reader:
            k = row['call_name']
            rows[k] = row
    return rows

# ─── Helper: load sensor CSV columns for a given track_id ─────────────────────
def load_track(csv_path, track_id, cols):
    result = {c: [] for c in cols}
    with open(csv_path, newline='', encoding='utf-8') as f:
        reader = csv.DictReader(f)
        for row in reader:
            if int(row['track_id']) == track_id:
                for c in cols:
                    result[c].append(float(row[c]))
    return result

FIGSIZE = (10, 6)
DPI     = 150

# ═══════════════════════════════════════════════════════════════════════════════
# Chart 1: Trajectory — Jet 0
# ═══════════════════════════════════════════════════════════════════════════════
print("[1/6] Generating trajectory_jet0.png ...")

true_data     = load_track(SENSOR_CSV,       0, ['true_x', 'true_y'])
noisy_data    = load_track(SENSOR_CSV,       0, ['meas_x', 'meas_y'])
filtered_data = load_track(FILTERED_WIN_CSV, 0, ['est_x',  'est_y'])

fig, ax = plt.subplots(figsize=FIGSIZE)
ax.plot(true_data['true_x'],     true_data['true_y'],     'g-',  linewidth=2.0, label='True Flight Path', zorder=3)
ax.scatter(noisy_data['meas_x'], noisy_data['meas_y'],    c='r', s=4,  alpha=0.4, label='Raw Radar Pings (σ=50 m)', zorder=2)
ax.plot(filtered_data['est_x'],  filtered_data['est_y'],  'b-',  linewidth=1.8, label='Kalman Filtered Path', zorder=4)

ax.set_title('Jet 0 (Climbing Right Turn): Kalman Filter Tracking', fontsize=14, fontweight='bold')
ax.set_xlabel('X Position (m)', fontsize=12)
ax.set_ylabel('Y Position (m)', fontsize=12)
ax.legend(fontsize=11)
ax.grid(True, alpha=0.3)
plt.tight_layout()
out = os.path.join(CHARTS_DIR, 'trajectory_jet0.png')
plt.savefig(out, dpi=DPI)
plt.close()
print(f"    Saved: {out} ({os.path.getsize(out)//1024} KB)")

# ═══════════════════════════════════════════════════════════════════════════════
# Chart 2: RMSE per jet (grouped bars)
# ═══════════════════════════════════════════════════════════════════════════════
print("[2/6] Generating rmse_per_jet.png ...")

jet_labels = [
    "Jet 0\nClimbing\nRight Turn",
    "Jet 1\nDiving\nLeft Turn",
    "Jet 2\nTactical\n7G Turn",
    "Jet 3\nSupersonic\nSweep",
    "Jet 4\nIntercept\nVector",
    "Jet 5\nHigh-Speed\nDash",
    "Jet 6\nEvasive\nWeave",
    "Jet 7\nCombat Air\nPatrol",
]
raw_rmse      = [71.08, 70.73, 70.11, 70.79, 70.71, 70.12, 70.79, 69.51]
filtered_rmse = [32.20, 33.55, 29.32, 33.92, 32.63, 32.93, 33.16, 28.69]

x    = np.arange(len(jet_labels))
w    = 0.35
fig, ax = plt.subplots(figsize=FIGSIZE)
bars_r = ax.bar(x - w/2, raw_rmse,      w, label='Raw Radar RMSE',      color='tomato',    edgecolor='black', linewidth=0.5)
bars_f = ax.bar(x + w/2, filtered_rmse, w, label='Kalman Filtered RMSE', color='steelblue', edgecolor='black', linewidth=0.5)

for bar in bars_r:
    ax.text(bar.get_x() + bar.get_width()/2, bar.get_height() + 0.5,
            f'{bar.get_height():.1f}', ha='center', va='bottom', fontsize=8)
for bar in bars_f:
    ax.text(bar.get_x() + bar.get_width()/2, bar.get_height() + 0.5,
            f'{bar.get_height():.1f}', ha='center', va='bottom', fontsize=8)

ax.set_title('Radar Tracking Error Reduction per Fighter Jet', fontsize=14, fontweight='bold')
ax.set_xlabel('Fighter Jet', fontsize=12)
ax.set_ylabel('RMSE (meters)', fontsize=12)
ax.set_xticks(x)
ax.set_xticklabels(jet_labels, fontsize=8)
ax.legend(fontsize=11)
ax.grid(True, axis='y', alpha=0.3)
ax.set_ylim(0, 85)
plt.tight_layout()
out = os.path.join(CHARTS_DIR, 'rmse_per_jet.png')
plt.savefig(out, dpi=DPI)
plt.close()
print(f"    Saved: {out} ({os.path.getsize(out)//1024} KB)")

# ═══════════════════════════════════════════════════════════════════════════════
# Chart 3: File I/O comparison
# ═══════════════════════════════════════════════════════════════════════════════
print("[3/6] Generating syscall_comparison_fileio.png ...")

win_data   = load_csv_dict(WIN_RESULTS_CSV)
linux_data = load_csv_dict(LINUX_RESULTS_CSV)

fileio_pairs = [
    ('CreateFile',    'open',      'File Open'),
    ('WriteFile_4KB', 'write_4KB', 'Write 4 KB'),
    ('WriteFile_1MB', 'write_1MB', 'Write 1 MB'),
    ('ReadFile_4KB',  'read_4KB',  'Read 4 KB'),
    ('ReadFile_1MB',  'read_1MB',  'Read 1 MB'),
    ('CloseHandle',   'close',     'File Close'),
]
labels    = [p[2] for p in fileio_pairs]
win_vals  = [float(win_data[p[0]]['median_us'])   for p in fileio_pairs]
linux_vals= [float(linux_data[p[1]]['median_us']) for p in fileio_pairs]

x = np.arange(len(labels)); w = 0.35
fig, ax = plt.subplots(figsize=FIGSIZE)
ax.bar(x - w/2, win_vals,   w, label='Windows 10',   color='steelblue',  edgecolor='black', linewidth=0.5)
ax.bar(x + w/2, linux_vals, w, label='Ubuntu 22.04', color='darkorange', edgecolor='black', linewidth=0.5)
ax.set_title('File I/O System Call Latency: Linux vs Windows', fontsize=14, fontweight='bold')
ax.set_xlabel('Operation', fontsize=12)
ax.set_ylabel('Median Latency (µs)', fontsize=12)
ax.set_xticks(x)
ax.set_xticklabels(labels, fontsize=10)
ax.legend(fontsize=11)
ax.grid(True, axis='y', alpha=0.3)
# Annotate bars
mx = max(win_vals + linux_vals)
for i, (wv, lv) in enumerate(zip(win_vals, linux_vals)):
    ax.text(i - w/2, wv + mx*0.01, f'{wv:.1f}', ha='center', va='bottom', fontsize=8, color='steelblue')
    ax.text(i + w/2, lv + mx*0.01, f'{lv:.1f}', ha='center', va='bottom', fontsize=8, color='darkorange')
plt.tight_layout()
out = os.path.join(CHARTS_DIR, 'syscall_comparison_fileio.png')
plt.savefig(out, dpi=DPI)
plt.close()
print(f"    Saved: {out} ({os.path.getsize(out)//1024} KB)")

# ═══════════════════════════════════════════════════════════════════════════════
# Chart 4: Memory Management comparison
# ═══════════════════════════════════════════════════════════════════════════════
print("[4/6] Generating syscall_comparison_memory.png ...")

mem_pairs = [
    ('VirtualAlloc_4KB',         'mmap_4KB',          'Alloc 4 KB'),
    ('VirtualAlloc_64KB',        'mmap_64KB',         'Alloc 64 KB'),
    ('VirtualAlloc_1MB',         'mmap_1MB',          'Alloc 1 MB'),
    ('FirstTouch_64KB',          'FirstTouch_64KB',   'First Touch\n64 KB'),
    ('VirtualFree_64KB_touched', 'munmap_64KB_touched','Free 64 KB\n(touched)'),
]
labels    = [p[2] for p in mem_pairs]
win_vals  = [float(win_data[p[0]]['median_us'])   for p in mem_pairs]
linux_vals= [float(linux_data[p[1]]['median_us']) for p in mem_pairs]

x = np.arange(len(labels)); w = 0.35
fig, ax = plt.subplots(figsize=FIGSIZE)
ax.bar(x - w/2, win_vals,   w, label='Windows 10',   color='steelblue',  edgecolor='black', linewidth=0.5)
ax.bar(x + w/2, linux_vals, w, label='Ubuntu 22.04', color='darkorange', edgecolor='black', linewidth=0.5)
ax.set_title('Memory Management Latency: Linux vs Windows', fontsize=14, fontweight='bold')
ax.set_xlabel('Operation', fontsize=12)
ax.set_ylabel('Median Latency (µs)', fontsize=12)
ax.set_xticks(x)
ax.set_xticklabels(labels, fontsize=10)
ax.legend(fontsize=11)
ax.grid(True, axis='y', alpha=0.3)
plt.tight_layout()
out = os.path.join(CHARTS_DIR, 'syscall_comparison_memory.png')
plt.savefig(out, dpi=DPI)
plt.close()
print(f"    Saved: {out} ({os.path.getsize(out)//1024} KB)")

# ═══════════════════════════════════════════════════════════════════════════════
# Chart 5: Process & IPC comparison (log scale)
# ═══════════════════════════════════════════════════════════════════════════════
print("[5/6] Generating syscall_comparison_process.png ...")

proc_pairs = [
    ('CreateProcess',           'fork',            'Create\nProcess'),
    ('CreateProcess_plus_Wait', 'fork_exec_wait',  'Spawn+\nWait'),
    ('CreatePipe',              'pipe',            'Create\nPipe'),
    ('PipeWrite_64KB',          'pipe_write_64KB', 'Pipe Write\n64 KB'),
    ('PipeRead_64KB',           'pipe_read_64KB',  'Pipe Read\n64 KB'),
    ('CreateFileMapping_1MB',   'shmget_1MB',      'SHM Create\n1 MB'),
    ('MapViewOfFile_1MB',       'shmat_1MB',       'SHM Map\n1 MB'),
]
labels    = [p[2] for p in proc_pairs]
win_vals  = [float(win_data[p[0]]['median_us'])   for p in proc_pairs]
linux_vals= [float(linux_data[p[1]]['median_us']) for p in proc_pairs]

x = np.arange(len(labels)); w = 0.35
fig, ax = plt.subplots(figsize=FIGSIZE)
ax.bar(x - w/2, win_vals,   w, label='Windows 10',   color='steelblue',  edgecolor='black', linewidth=0.5)
ax.bar(x + w/2, linux_vals, w, label='Ubuntu 22.04', color='darkorange', edgecolor='black', linewidth=0.5)
ax.set_title('Process & IPC Latency: Linux vs Windows (log scale)', fontsize=14, fontweight='bold')
ax.set_xlabel('Operation', fontsize=12)
ax.set_ylabel('Median Latency (µs) — log scale', fontsize=12)
ax.set_yscale('log')
ax.set_xticks(x)
ax.set_xticklabels(labels, fontsize=9)
ax.legend(fontsize=11)
ax.grid(True, axis='y', alpha=0.3, which='both')
for i, (wv, lv) in enumerate(zip(win_vals, linux_vals)):
    ax.text(i - w/2, wv * 1.1, f'{wv:.1f}', ha='center', va='bottom', fontsize=7.5, color='steelblue')
    ax.text(i + w/2, lv * 1.1, f'{lv:.1f}', ha='center', va='bottom', fontsize=7.5, color='darkorange')
plt.tight_layout()
out = os.path.join(CHARTS_DIR, 'syscall_comparison_process.png')
plt.savefig(out, dpi=DPI)
plt.close()
print(f"    Saved: {out} ({os.path.getsize(out)//1024} KB)")

# ═══════════════════════════════════════════════════════════════════════════════
# Chart 6: Speedup Curve (Amdahl's Law)
# ═══════════════════════════════════════════════════════════════════════════════
print("[6/6] Generating speedup_curve.png ...")

workers = [1, 2, 4, 8]

def amdahl(p, n):
    return 1.0 / ((1.0 - p) + p / n)

ideal       = [float(n) for n in workers]
theoretical = [amdahl(0.90, n) for n in workers]
linux_est   = [amdahl(0.95, n) for n in workers]
windows_est = [amdahl(0.88, n) for n in workers]

fig, ax = plt.subplots(figsize=FIGSIZE)
ax.plot(workers, ideal,       'k--',  linewidth=1.5, label='Ideal Linear Scaling', marker='s', markersize=5)
ax.plot(workers, theoretical, 'g-o',  linewidth=2.0, label="Amdahl's Law (p=0.90)", markersize=7)
ax.plot(workers, linux_est,   'b-^',  linewidth=2.0, label='Linux fork() estimate (p=0.95)', markersize=7)
ax.plot(workers, windows_est, 'r-v',  linewidth=2.0, label='Windows CreateProcess estimate (p=0.88)', markersize=7)

for n, sp in zip(workers, linux_est):
    ax.annotate(f'{sp:.2f}×', (n, sp), textcoords="offset points", xytext=(5, 8),   fontsize=9, color='blue')
for n, sp in zip(workers, windows_est):
    ax.annotate(f'{sp:.2f}×', (n, sp), textcoords="offset points", xytext=(5, -16), fontsize=9, color='red')

ax.text(0.98, 0.15, '* Actual runtime benchmarks pending\n  (compile + run on HP ZBook)',
        transform=ax.transAxes, fontsize=9, ha='right', va='bottom',
        bbox=dict(boxstyle='round', facecolor='lightyellow', edgecolor='gray', alpha=0.8))

ax.set_title("Kalman Filter Parallel Speedup (Amdahl's Law)", fontsize=14, fontweight='bold')
ax.set_xlabel('Number of Worker Processes', fontsize=12)
ax.set_ylabel('Speedup (×)', fontsize=12)
ax.set_xticks(workers)
ax.legend(fontsize=10, loc='upper left')
ax.grid(True, alpha=0.3)
ax.set_ylim(0, 9)
plt.tight_layout()
out = os.path.join(CHARTS_DIR, 'speedup_curve.png')
plt.savefig(out, dpi=DPI)
plt.close()
print(f"    Saved: {out} ({os.path.getsize(out)//1024} KB)")

print("\n✅ All 6 charts generated successfully.")
print(f"   Charts directory: {CHARTS_DIR}")
