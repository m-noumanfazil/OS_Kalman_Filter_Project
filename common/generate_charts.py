"""
generate_charts.py — CT-353 OS CCA Chart Generator

Produces six PNG charts:
1. Jet 0 trajectory with a zoomed inset
2. RMSE per jet, calculated from the CSV data
3. File I/O API/system-operation comparison
4. Memory-management API comparison
5. Process and IPC API comparison
6. Actual Kalman Filter parallel speedup

Run from any directory.
"""

import os
import csv

import matplotlib
matplotlib.use("Agg")

import matplotlib.pyplot as plt
import numpy as np


# =============================================================================
# Paths
# =============================================================================

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

SENSOR_CSV = os.path.join(ROOT, "common", "sensor_data.csv")
FILTERED_WIN_CSV = os.path.join(
    ROOT, "results", "Windows", "filtered_sensor_data.csv"
)
WIN_RESULTS_CSV = os.path.join(
    ROOT, "results", "Windows", "windows_results.csv"
)
LINUX_RESULTS_CSV = os.path.join(
    ROOT, "results", "Linux", "linux_results.csv"
)

CHARTS_DIR = os.path.join(ROOT, "results", "charts")
os.makedirs(CHARTS_DIR, exist_ok=True)

FIGSIZE = (10, 6)
DPI = 150


# =============================================================================
# Helper functions
# =============================================================================

def check_file(path):
    """Ensure an input file exists."""
    if not os.path.isfile(path):
        raise FileNotFoundError(f"Required input file not found: {path}")


def load_csv_dict(path):
    """Load benchmark CSV records keyed by call_name."""
    check_file(path)
    rows = {}

    with open(path, newline="", encoding="utf-8-sig") as f:
        reader = csv.DictReader(f)

        if not reader.fieldnames or "call_name" not in reader.fieldnames:
            raise ValueError(
                f"{path} must contain a 'call_name' column. "
                f"Found: {reader.fieldnames}"
            )

        if "median_us" not in reader.fieldnames:
            raise ValueError(
                f"{path} must contain a 'median_us' column."
            )

        for line_number, row in enumerate(reader, start=2):
            name = row["call_name"].strip()

            if not name:
                raise ValueError(
                    f"Empty call_name in {path}, line {line_number}"
                )

            if name in rows:
                raise ValueError(
                    f"Duplicate call_name '{name}' in {path}. "
                    "The chart generator needs one benchmark record "
                    "per operation."
                )

            try:
                median = float(row["median_us"])
            except (TypeError, ValueError):
                raise ValueError(
                    f"Invalid median_us for '{name}' in {path}, "
                    f"line {line_number}"
                )

            if not np.isfinite(median) or median <= 0:
                raise ValueError(
                    f"Invalid or non-positive median_us for '{name}' "
                    f"in {path}: {median}"
                )

            rows[name] = row

    return rows


def load_track(csv_path, track_id, cols):
    """Load selected numeric columns for one track."""
    check_file(csv_path)
    result = {column: [] for column in cols}

    with open(csv_path, newline="", encoding="utf-8-sig") as f:
        reader = csv.DictReader(f)

        required = {"track_id", *cols}
        missing = required - set(reader.fieldnames or [])

        if missing:
            raise ValueError(
                f"Missing columns in {csv_path}: {sorted(missing)}"
            )

        for line_number, row in enumerate(reader, start=2):
            try:
                current_track = int(row["track_id"])
            except (TypeError, ValueError):
                raise ValueError(
                    f"Invalid track_id in {csv_path}, line {line_number}"
                )

            if current_track != track_id:
                continue

            for column in cols:
                try:
                    value = float(row[column])
                except (TypeError, ValueError):
                    raise ValueError(
                        f"Invalid {column} in {csv_path}, "
                        f"line {line_number}"
                    )

                if not np.isfinite(value):
                    raise ValueError(
                        f"Non-finite {column} in {csv_path}, "
                        f"line {line_number}"
                    )

                result[column].append(value)

    return result


def get_median(data, call_name, platform):
    """Retrieve one validated benchmark median."""
    if call_name not in data:
        raise KeyError(
            f"Missing operation '{call_name}' in {platform} results CSV."
        )

    return float(data[call_name]["median_us"])


def validate_equal_lengths(description, arrays):
    """Reject missing or differently sized data arrays."""
    lengths = [len(array) for array in arrays]

    if not lengths or lengths[0] == 0:
        raise ValueError(f"No data available for {description}.")

    if len(set(lengths)) != 1:
        raise ValueError(
            f"Data length mismatch for {description}: {lengths}"
        )


def positional_rmse(actual_x, actual_y, estimated_x, estimated_y):
    """
    Calculate 2D positional RMSE.

    Each observation contributes dx^2 + dy^2, matching the shared
    C++ computeRMSE() convention used in this project.
    """
    actual_x = np.asarray(actual_x, dtype=float)
    actual_y = np.asarray(actual_y, dtype=float)
    estimated_x = np.asarray(estimated_x, dtype=float)
    estimated_y = np.asarray(estimated_y, dtype=float)

    validate_equal_lengths(
        "2D positional RMSE",
        [actual_x, actual_y, estimated_x, estimated_y]
    )

    squared_errors = (
        (actual_x - estimated_x) ** 2
        + (actual_y - estimated_y) ** 2
    )

    return float(np.sqrt(np.mean(squared_errors)))


def add_bar_labels(ax, bars, values, color="black", fontsize=8, offset=0.01):
    """Add numeric labels above bars."""
    ymax = max(values) if values else 1
    for bar, value in zip(bars, values):
        ax.annotate(
            f"{value:.1f}",
            (bar.get_x() + bar.get_width() / 2, bar.get_height()),
            xytext=(0, max(2, ymax * offset)),
            textcoords="offset points",
            ha="center",
            va="bottom",
            fontsize=fontsize,
            color=color
        )


def add_interface_note(fig, text, y=0.015):
    """Add a concise note below a comparison chart."""
    fig.text(
        0.5, y, text,
        ha="center",
        va="bottom",
        fontsize=8,
        color="dimgray",
        wrap=True
    )


def save_chart(fig, filename):
    """Save and close a chart consistently."""
    out = os.path.join(CHARTS_DIR, filename)
    fig.savefig(out, dpi=DPI, bbox_inches="tight")
    plt.close(fig)
    print(f"    Saved: {out} ({os.path.getsize(out) // 1024} KB)")


# =============================================================================
# Load benchmark data
# =============================================================================

win_data = load_csv_dict(WIN_RESULTS_CSV)
linux_data = load_csv_dict(LINUX_RESULTS_CSV)


# =============================================================================
# Chart 1: Trajectory — Jet 0
# =============================================================================

print("[1/6] Generating trajectory_jet0.png ...")

true_data = load_track(SENSOR_CSV, 0, ["true_x", "true_y"])
noisy_data = load_track(SENSOR_CSV, 0, ["meas_x", "meas_y"])
filtered_data = load_track(
    FILTERED_WIN_CSV, 0, ["est_x", "est_y"]
)

validate_equal_lengths(
    "Jet 0 trajectory",
    [
        true_data["true_x"], true_data["true_y"],
        noisy_data["meas_x"], noisy_data["meas_y"],
        filtered_data["est_x"], filtered_data["est_y"]
    ]
)

true_x = np.asarray(true_data["true_x"])
true_y = np.asarray(true_data["true_y"])
meas_x = np.asarray(noisy_data["meas_x"])
meas_y = np.asarray(noisy_data["meas_y"])
est_x = np.asarray(filtered_data["est_x"])
est_y = np.asarray(filtered_data["est_y"])

fig, ax = plt.subplots(figsize=FIGSIZE)

ax.plot(
    true_x, true_y,
    "g-", linewidth=2.0,
    label="True Flight Path", zorder=3
)
ax.scatter(
    meas_x, meas_y,
    c="red", s=2, alpha=0.25,
    label="Raw Radar Pings (σ=50 m)", zorder=2
)
ax.plot(
    est_x, est_y,
    "b-", linewidth=1.8,
    label="Kalman Filtered Path (Windows output)", zorder=4
)

ax.set_title(
    "Jet 0 (Climbing Right Turn): Kalman Filter Tracking",
    fontsize=14, fontweight="bold"
)
ax.set_xlabel("X Position (m)", fontsize=12)
ax.set_ylabel("Y Position (m)", fontsize=12)
ax.set_aspect("equal", adjustable="datalim")
ax.legend(fontsize=9, loc="upper right")
ax.grid(True, alpha=0.3)

# Zoomed inset around the middle of the true trajectory.
mid = len(true_x) // 2
inset_center_x = true_x[mid]
inset_center_y = true_y[mid]
half_window = 250.0

axins = ax.inset_axes([0.32, 0.28, 0.36, 0.36])
axins.plot(true_x, true_y, "g-", linewidth=1.5)
axins.scatter(meas_x, meas_y, c="red", s=3, alpha=0.25)
axins.plot(est_x, est_y, "b-", linewidth=1.3)
axins.set_xlim(inset_center_x - half_window, inset_center_x + half_window)
axins.set_ylim(inset_center_y - half_window, inset_center_y + half_window)
axins.set_aspect("equal", adjustable="box")
axins.set_title("500 m × 500 m detail", fontsize=8)
axins.tick_params(axis="both", labelsize=6)
axins.grid(True, alpha=0.25)
ax.indicate_inset_zoom(axins, edgecolor="gray", linewidth=0.8)

fig.tight_layout()
save_chart(fig, "trajectory_jet0.png")


# =============================================================================
# Chart 2: RMSE per jet — calculated from actual CSV data
# =============================================================================

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

raw_rmse = []
filtered_rmse = []

for jet_id in range(8):
    true_data = load_track(
        SENSOR_CSV, jet_id, ["true_x", "true_y"]
    )
    noisy_data = load_track(
        SENSOR_CSV, jet_id, ["meas_x", "meas_y"]
    )
    filtered_data = load_track(
        FILTERED_WIN_CSV, jet_id, ["est_x", "est_y"]
    )

    validate_equal_lengths(
        f"Jet {jet_id} RMSE",
        [
            true_data["true_x"], true_data["true_y"],
            noisy_data["meas_x"], noisy_data["meas_y"],
            filtered_data["est_x"], filtered_data["est_y"]
        ]
    )

    raw_error = positional_rmse(
        true_data["true_x"], true_data["true_y"],
        noisy_data["meas_x"], noisy_data["meas_y"]
    )
    filtered_error = positional_rmse(
        true_data["true_x"], true_data["true_y"],
        filtered_data["est_x"], filtered_data["est_y"]
    )

    raw_rmse.append(raw_error)
    filtered_rmse.append(filtered_error)

    print(
        f"    Jet {jet_id}: "
        f"Raw RMSE = {raw_error:.2f} m, "
        f"Filtered RMSE = {filtered_error:.2f} m"
    )

x = np.arange(len(jet_labels))
w = 0.35

fig, ax = plt.subplots(figsize=FIGSIZE)
bars_raw = ax.bar(
    x - w / 2, raw_rmse, w,
    label="Raw Radar RMSE",
    color="tomato", edgecolor="black", linewidth=0.5
)
bars_filtered = ax.bar(
    x + w / 2, filtered_rmse, w,
    label="Kalman Filtered RMSE",
    color="steelblue", edgecolor="black", linewidth=0.5
)

for bar in list(bars_raw) + list(bars_filtered):
    ax.annotate(
        f"{bar.get_height():.1f}",
        (bar.get_x() + bar.get_width() / 2, bar.get_height()),
        xytext=(0, 3),
        textcoords="offset points",
        ha="center", va="bottom", fontsize=8
    )

ax.set_title(
    "Radar Tracking Error Reduction per Fighter Jet",
    fontsize=14, fontweight="bold"
)
ax.set_xlabel("Fighter Jet", fontsize=12)
ax.set_ylabel("2D Positional RMSE (meters)", fontsize=12)
ax.set_xticks(x)
ax.set_xticklabels(jet_labels, fontsize=8)
ax.legend(fontsize=10)
ax.grid(True, axis="y", alpha=0.3)
ax.set_ylim(0, max(raw_rmse) * 1.2)

fig.text(
    0.5, 0.015,
    "Per-jet values are calculated from the Windows filtered output CSV. "
    "The aggregate Linux and Windows RMSE results agree; this chart does "
    "not establish row-by-row output identity.",
    ha="center", va="bottom", fontsize=7.5, color="dimgray", wrap=True
)
fig.tight_layout(rect=[0, 0.07, 1, 1])
save_chart(fig, "rmse_per_jet.png")


# =============================================================================
# Chart 3: File I/O API and system-operation comparison
# =============================================================================

print("[3/6] Generating syscall_comparison_fileio.png ...")

fileio_pairs = [
    ("CreateFile", "open", "File Open"),
    ("WriteFile_4KB", "write_4KB", "Write 4 KB"),
    ("WriteFile_1MB", "write_1MB", "Write 1 MB"),
    ("ReadFile_4KB", "read_4KB", "Read 4 KB"),
    ("ReadFile_1MB", "read_1MB", "Read 1 MB"),
    ("CloseHandle", "close", "File Close"),
]

labels = [pair[2] for pair in fileio_pairs]
win_vals = [
    get_median(win_data, pair[0], "Windows")
    for pair in fileio_pairs
]
linux_vals = [
    get_median(linux_data, pair[1], "Linux")
    for pair in fileio_pairs
]

x = np.arange(len(labels))
w = 0.35

fig, ax = plt.subplots(figsize=FIGSIZE)
bars_win = ax.bar(
    x - w / 2, win_vals, w,
    label="Windows 10 (Win32 API)",
    color="steelblue", edgecolor="black", linewidth=0.5
)
bars_linux = ax.bar(
    x + w / 2, linux_vals, w,
    label="Ubuntu 22.04 (libc interface)",
    color="darkorange", edgecolor="black", linewidth=0.5
)

add_bar_labels(ax, bars_win, win_vals, color="steelblue")
add_bar_labels(ax, bars_linux, linux_vals, color="darkorange")

ax.set_title(
    "File I/O API and System Operation Latency: Linux vs Windows",
    fontsize=12, fontweight="bold"
)
ax.set_xlabel("Operation", fontsize=11)
ax.set_ylabel("Median Latency (µs)", fontsize=11)
ax.set_xticks(x)
ax.set_xticklabels(labels, fontsize=9)
ax.legend(fontsize=9)
ax.grid(True, axis="y", alpha=0.3)

add_interface_note(
    fig,
    "Windows measurements use Win32 APIs; Linux measurements use libc "
    "interfaces to OS services. Paired operations have similar purposes, "
    "but are not necessarily identical kernel operations."
)
fig.tight_layout(rect=[0, 0.08, 1, 1])
save_chart(fig, "syscall_comparison_fileio.png")


# =============================================================================
# Chart 4: Memory-management API comparison
# =============================================================================

print("[4/6] Generating syscall_comparison_memory.png ...")

mem_pairs = [
    ("VirtualAlloc_4KB", "mmap_4KB", "Allocate 4 KB"),
    ("VirtualAlloc_64KB", "mmap_64KB", "Allocate 64 KB"),
    ("VirtualAlloc_1MB", "mmap_1MB", "Allocate 1 MB"),
    ("FirstTouch_64KB", "FirstTouch_64KB", "First Touch\n64 KB"),
    (
        "VirtualFree_64KB_touched",
        "munmap_64KB_touched",
        "Free 64 KB\n(touched)"
    ),
]

labels = [pair[2] for pair in mem_pairs]
win_vals = [
    get_median(win_data, pair[0], "Windows")
    for pair in mem_pairs
]
linux_vals = [
    get_median(linux_data, pair[1], "Linux")
    for pair in mem_pairs
]

x = np.arange(len(labels))
w = 0.35

fig, ax = plt.subplots(figsize=FIGSIZE)
bars_win = ax.bar(
    x - w / 2, win_vals, w,
    label="Windows 10 (Win32 API)",
    color="steelblue", edgecolor="black", linewidth=0.5
)
bars_linux = ax.bar(
    x + w / 2, linux_vals, w,
    label="Ubuntu 22.04 (libc interface)",
    color="darkorange", edgecolor="black", linewidth=0.5
)

add_bar_labels(ax, bars_win, win_vals, color="steelblue")
add_bar_labels(ax, bars_linux, linux_vals, color="darkorange")

ax.set_title(
    "Memory Management API Latency: Linux vs Windows",
    fontsize=13, fontweight="bold"
)
ax.set_xlabel("Operation", fontsize=11)
ax.set_ylabel("Median Latency (µs)", fontsize=11)
ax.set_xticks(x)
ax.set_xticklabels(labels, fontsize=9)
ax.legend(fontsize=9)
ax.grid(True, axis="y", alpha=0.3)

add_interface_note(
    fig,
    "Windows VirtualAlloc separates reservation and commitment; reservation "
    "is typically aligned to allocation granularity (64 KB here), while "
    "pages are 4 KB. Linux mmap uses page-based mapping. First-touch timings "
    "also include page-fault/initialization effects."
)
fig.tight_layout(rect=[0, 0.10, 1, 1])
save_chart(fig, "syscall_comparison_memory.png")


# =============================================================================
# Chart 5: Process and IPC API comparison
# =============================================================================

print("[5/6] Generating syscall_comparison_process.png ...")

proc_pairs = [
    ("CreateProcess", "fork", "Create process\nvs fork"),
    ("CreateProcess_plus_Wait", "fork_exec_wait", "Spawn +\nwait"),
    ("CreatePipe", "pipe", "Create\npipe"),
    ("PipeWrite_64KB", "pipe_write_64KB", "Pipe write\n64 KB"),
    ("PipeRead_64KB", "pipe_read_64KB", "Pipe read\n64 KB"),
    ("CreateFileMapping_1MB", "shmget_1MB", "Shared memory\ncreate 1 MB"),
    ("MapViewOfFile_1MB", "shmat_1MB", "Shared memory\nmap 1 MB"),
]

labels = [pair[2] for pair in proc_pairs]
win_vals = [
    get_median(win_data, pair[0], "Windows")
    for pair in proc_pairs
]
linux_vals = [
    get_median(linux_data, pair[1], "Linux")
    for pair in proc_pairs
]

x = np.arange(len(labels))
w = 0.35

fig, ax = plt.subplots(figsize=(11, 6.5))
bars_win = ax.bar(
    x - w / 2, win_vals, w,
    label="Windows 10 (Win32 API)",
    color="steelblue", edgecolor="black", linewidth=0.5
)
bars_linux = ax.bar(
    x + w / 2, linux_vals, w,
    label="Ubuntu 22.04 (libc interface)",
    color="darkorange", edgecolor="black", linewidth=0.5
)

# Log scale keeps both small IPC values and slower process operations visible.
ax.set_yscale("log")
for bar, value in zip(bars_win, win_vals):
    ax.annotate(
        f"{value:.1f}",
        (bar.get_x() + bar.get_width() / 2, value),
        xytext=(0, 3), textcoords="offset points",
        ha="center", va="bottom", fontsize=7, color="steelblue"
    )
for bar, value in zip(bars_linux, linux_vals):
    ax.annotate(
        f"{value:.1f}",
        (bar.get_x() + bar.get_width() / 2, value),
        xytext=(0, 3), textcoords="offset points",
        ha="center", va="bottom", fontsize=7, color="darkorange"
    )

ax.set_title(
    "Process and IPC API Latency: Linux vs Windows (log scale)",
    fontsize=13, fontweight="bold"
)
ax.set_xlabel("Operation", fontsize=11)
ax.set_ylabel("Median Latency (µs) — logarithmic scale", fontsize=10)
ax.set_xticks(x)
ax.set_xticklabels(labels, fontsize=8)
ax.legend(fontsize=9)
ax.grid(True, axis="y", alpha=0.3, which="both")

add_interface_note(
    fig,
    "Important: CreateProcess creates a process and loads an executable image; "
    "fork creates a child process using copy-on-write and does not itself "
    "replace the program image. These first bars are not equivalent operations. "
    "CreateProcess + Wait vs fork + exec + wait is the closer end-to-end comparison."
)
fig.tight_layout(rect=[0, 0.12, 1, 1])
save_chart(fig, "syscall_comparison_process.png")


# =============================================================================
# Chart 6: Actual Kalman Filter speedup
# =============================================================================

print("[6/6] Generating speedup_curve.png ...")

workers = np.array([1, 2, 4, 8], dtype=float)

# Actual measured mean execution times in microseconds.
# Order: 1, 2, 4 and 8 worker processes.
linux_times = np.array([
    2131.355,
    5313.664,
    5494.792,
    8502.792
])

windows_times = np.array([
    2350.510,
    30210.732,
    36403.950,
    49416.734
])

# Speedup = single-worker time / execution time with N workers.
linux_speedup = linux_times[0] / linux_times
windows_speedup = windows_times[0] / windows_times
ideal = workers.copy()

print("    Actual measured speedup:")
for i, n in enumerate(workers.astype(int)):
    print(
        f"    {n} workers: "
        f"Linux = {linux_speedup[i]:.3f}x, "
        f"Windows = {windows_speedup[i]:.3f}x"
    )

fig, (ax_full, ax_zoom) = plt.subplots(
    1, 2, figsize=(12, 5.8), sharex=True,
    gridspec_kw={"width_ratios": [1.15, 1]}
)

# Full scale panel.
ax_full.plot(
    workers, ideal, "k--",
    linewidth=1.4, label="Ideal linear scaling"
)
ax_full.plot(
    workers, linux_speedup, color="blue",
    linewidth=2, label="Linux measured"
)
ax_full.plot(
    workers, windows_speedup, color="red",
    linewidth=2, label="Windows measured"
)

# Draw first-worker markers slightly apart so both 1.0x points are visible.
ax_full.scatter([0.94], [linux_speedup[0]], color="blue", marker="o", zorder=5)
ax_full.scatter([1.06], [windows_speedup[0]], color="red", marker="v", zorder=5)
ax_full.scatter(workers[1:], linux_speedup[1:], color="blue", marker="o", zorder=5)
ax_full.scatter(workers[1:], windows_speedup[1:], color="red", marker="v", zorder=5)

ax_full.set_title("Full scale (0–9×)", fontsize=11, fontweight="bold")
ax_full.set_ylabel("Speedup (×)")
ax_full.set_xlabel("Number of worker processes")
ax_full.set_xlim(0.5, 8.5)
ax_full.set_ylim(0, 9)
ax_full.set_xticks(workers)
ax_full.grid(True, alpha=0.3)
ax_full.legend(fontsize=8, loc="upper left")

# Zoomed panel makes the measured values below 1x easier to distinguish.
ax_zoom.plot(
    workers, linux_speedup, color="blue",
    linewidth=2, label="Linux measured"
)
ax_zoom.plot(
    workers, windows_speedup, color="red",
    linewidth=2, label="Windows measured"
)
ax_zoom.axhline(
    1.0, color="gray", linestyle=":",
    linewidth=1.2, label="Single-worker baseline"
)
ax_zoom.scatter([0.94], [linux_speedup[0]], color="blue", marker="o", zorder=5)
ax_zoom.scatter([1.06], [windows_speedup[0]], color="red", marker="v", zorder=5)
ax_zoom.scatter(workers[1:], linux_speedup[1:], color="blue", marker="o", zorder=5)
ax_zoom.scatter(workers[1:], windows_speedup[1:], color="red", marker="v", zorder=5)

ax_zoom.set_title("Measured range (0–1.15×)", fontsize=11, fontweight="bold")
ax_zoom.set_xlabel("Number of worker processes")
ax_zoom.set_xlim(0.5, 8.5)
ax_zoom.set_ylim(0, 1.15)
ax_zoom.set_xticks(workers)
ax_zoom.grid(True, alpha=0.3)
ax_zoom.legend(fontsize=8, loc="upper right")

# Label the 1-worker points separately to avoid overlap.
ax_zoom.annotate(
    f"Linux {linux_speedup[0]:.2f}×",
    (0.94, 1.0), xytext=(6, -16),
    textcoords="offset points", ha="left",
    fontsize=8, color="blue"
)
ax_zoom.annotate(
    f"Windows {windows_speedup[0]:.2f}×",
    (1.06, 1.0), xytext=(8, 8),
    textcoords="offset points", ha="left",
    fontsize=8, color="red"
)

for n, sp in zip(workers[1:], linux_speedup[1:]):
    ax_zoom.annotate(
        f"{sp:.2f}×", (n, sp),
        textcoords="offset points", xytext=(0, 7),
        ha="center", fontsize=8, color="blue"
    )
for n, sp in zip(workers[1:], windows_speedup[1:]):
    ax_zoom.annotate(
        f"{sp:.2f}×", (n, sp),
        textcoords="offset points", xytext=(0, 8),
        ha="center", fontsize=8, color="red"
    )

fig.suptitle(
    "Kalman Filter Parallel Speedup: Actual Measurements",
    fontsize=14, fontweight="bold"
)
fig.text(
    0.5, 0.015,
    "Speedup below 1.0× means slower execution than the single-worker "
    "baseline. Process creation, scheduling and IPC overhead can outweigh "
    "the parallel workload.",
    ha="center", va="bottom", fontsize=8, color="dimgray"
)
fig.tight_layout(rect=[0, 0.07, 1, 0.93])
save_chart(fig, "speedup_curve.png")


# =============================================================================
# Completion
# =============================================================================

print("\nAll 6 charts generated successfully.")
print(f"Charts directory: {CHARTS_DIR}")
