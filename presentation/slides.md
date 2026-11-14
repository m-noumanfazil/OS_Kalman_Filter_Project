# CT-353 Operating Systems — CCA Presentation
## Supersonic Fighter Jet Radar Tracking: OS System Calls + Kalman Filter

---

## Slide 1: Title

# CT-353 Operating Systems
## Complex Computing Activity (CCA)

### **Supersonic Fighter Jet Radar Tracking**
### via Multi-Process Kalman Filter on Windows & Linux

---

**Team:** Muhammad Nouman Fazil  
**Repo:** [github.com/m-noumanfazil/OS_Kalman_Filter_Project](https://github.com/m-noumanfazil/OS_Kalman_Filter_Project)  
**Date:** November 2026

---

## Slide 2: Project Overview

### What We Built

| Part | Description |
|---|---|
| **Part A** | Benchmarked **28 syscalls** (5 categories) on both Windows 10 & Ubuntu 22.04 |
| **Part B** | Built a **parallel Kalman Filter** using those exact syscalls to track **8 supersonic fighter jets** |

### Why Fighter Jet Tracking?
- Real-world radar data is **noisy** (±50 m error at 20 Hz)
- Multiple jets need to be tracked **simultaneously** → perfect for multi-process parallelism
- Uses **every OS primitive** benchmarked in Part A: file I/O, shared memory, process creation, IPC

---

## Slide 3: System Architecture

```
Radar Sensor Data (CSV)
      │  8 jets × 1,000 steps = 8,000 pings
      ▼
┌─────────────────────────────────┐
│   Shared Memory Arena (1.5 MB)  │  ◄── OS Shared Memory Syscalls
│   - 8,000 RadarMeasurements    │      (mmap / CreateFileMapping)
│   - 8,000 FilteredOutputs      │
└──────────┬────────────┬─────────┘
           │            │
     Worker 0      Worker 1 ... Worker N-1
   (Jets 0,2,4)  (Jets 1,3,5)             ◄── OS Process Syscalls
         │                                     (fork / CreateProcess)
   ┌─────▼──────┐
   │ Kalman     │  ◄── Pure math, no extra syscalls
   │ Filter     │      Runs entirely in shared memory
   │ (per jet)  │
   └────────────┘
         │
   Results CSV  ◄── OS File I/O Syscalls (write / WriteFile)
```

**OS Environment:** Dual-boot HP ZBook — Windows 10 + Ubuntu 22.04 LTS

---

## Slide 4: Part A — 5 Benchmark Categories

| # | Category | Windows Syscalls | Linux Syscalls |
|---|---|---|---|
| 1 | File I/O | CreateFile, WriteFile, ReadFile, CloseHandle | open, write, read, close |
| 2 | Memory | VirtualAlloc, VirtualFree | mmap, munmap |
| 3 | Process | CreateProcess, WaitForSingleObject | fork, execve, waitpid |
| 4 | IPC | CreatePipe, CreateFileMapping, MapViewOfFile | pipe, shmget, shmat |
| 5 | Permissions | SetFileSecurity, GetFileSecurity, SetFileAttributes | chmod, chown, umask |

**Methodology:**
- **1,000 iterations** per syscall (200 for process creation)
- Timing: `QueryPerformanceCounter` (Win) / `clock_gettime(MONO)` (Linux)
- Metric: **Median latency** (robust to outliers)
- Also measured: CPU time, CPU cycles, memory delta, page faults

---

## Slide 5: Part A — Key Performance Findings

### Top 5 Cross-OS Comparisons (Median Latency)

| Operation | Windows | Linux | Winner |
|---|---|---|---|
| **Process Create** | 2,113 µs | 94 µs | 🐧 Linux **22×** faster |
| **Permissions Set** | 133 µs | 3.8 µs | 🐧 Linux **35×** faster |
| **Pipe Create** | 13.9 µs | 3.2 µs | 🐧 Linux **4.3×** faster |
| **Write 1 MB** | 55 µs | 562 µs | 🪟 Windows **10×** faster |
| **Memory Alloc** | 2.0–2.9 µs | 2.4–2.5 µs | 🤝 Tie |

### Notable Anomaly
> **Windows PipeRead_64KB:** median = 6.7 µs, but max = **3,044 µs**  
> → Windows scheduler preempted the benchmark thread for one full quantum (~15 ms). This is real OS behavior, not an error.

---

## Slide 6: Part A — Why Is Linux Process Creation 22× Faster?

### Linux `fork()`: Copy-on-Write (COW)
```
Parent Process Memory
      │
      ▼
Page Table (copied)
      │
      ├── Page 1 → [shared, read-only]  ← No copy made
      ├── Page 2 → [shared, read-only]  ← No copy made
      └── Page N → [shared, read-only]  ← No copy made

Physical copy made ONLY when child writes to a page
```
**fork() cost = copy page table only → ~94 µs**

### Windows `CreateProcess()`: Full Spawn
```
1. Parse PE executable from disk
2. Create new address space (VirtualAlloc)
3. Load ntdll.dll, kernel32.dll, etc.
4. Initialize Win32 subsystem
5. Create primary thread
6. Apply security token + ACL checks
```
**CreateProcess cost = all of the above → ~2,113 µs**

**Implication for Part B:** Linux workers spawn 22× faster → better scalability

---

## Slide 7: Part B — The Fighter Jet Tracking Problem

### Scenario
- **8 supersonic fighter jets** flying at **Mach 1.5–2.0** (540–720 m/s)
- Air defense radar pings at **20 Hz** (every 50 ms) over 10,000 steps = **80,000 total pings**
- Each ping has **Gaussian noise σ = 50 m** (atmospheric scattering, multipath)
- Need to track all jets simultaneously in **real time** (8.3 minutes of supersonic flight)

### The Problem: Raw Radar is Too Noisy

| Jet | True Position Error | Raw Radar RMSE |
|---|---|---|
| Jet 0 (Climbing Right Turn) | 0 m (by definition) | 70.63 m |
| Jet 7 (Combat Air Patrol) | 0 m (by definition) | 70.85 m |
| **All 8 jets average** | — | **70.81 m** |

**A 70-meter positional error at Mach 2 (680 m/s) means the jet moves past your error radius in ~0.1 seconds.** A Kalman Filter is needed.

---

## Slide 8: Part B — Kalman Filter Design

### State Vector (per axis, per jet)

$$\mathbf{x} = \begin{bmatrix} \text{position (m)} \\ \text{velocity (m/s)} \end{bmatrix}$$

### Filter Parameters

| Parameter | Value | Meaning |
|---|---|---|
| Δt | 0.05 s | Radar update rate (20 Hz) |
| Q (σ_a²) | 1500 (m/s²)² | Process noise — jet can maneuver at high G |
| R | 2500 m² | Measurement noise — matches sensor σ=50m |
| Initial P | Diagonal | High uncertainty at t=0 |

### Why DWNA (Discrete White Noise Acceleration) Model?
Fighter jets change velocity rapidly (7G maneuvers). DWNA models **unknown acceleration** as noise, making the filter robust to sudden turns without needing maneuver detection.

### Architecture: Independent X + Y Filters
```
Radar Ping (meas_x, meas_y)
      ├─→ AxisKalmanFilter(X): position_x, velocity_x
      └─→ AxisKalmanFilter(Y): position_y, velocity_y
         wrapped in Jet2DKalmanFilter
```

---

## Slide 9: Part B — Tracking Results

### RMSE Reduction After Kalman Filtering (80,000 Telemetry Points)

| Jet | Profile | Raw Radar | Filtered | Reduction |
|---|---|---|---|---|
| Jet 0 | Climbing Right Turn | 70.63 m | 23.14 m | **67.24%** |
| Jet 1 | Diving Left Turn | 70.81 m | 22.96 m | **67.57%** |
| Jet 2 | Tactical 7G Turn | 71.13 m | 22.86 m | **67.87%** |
| Jet 3 | Supersonic Sweep | 71.21 m | 23.39 m | **67.16%** |
| Jet 4 | Intercept Vector | 70.70 m | 22.77 m | **67.80%** |
| Jet 5 | High-Speed Dash | 70.15 m | 22.29 m | **68.22%** |
| Jet 6 | Evasive Weave | 70.95 m | 23.52 m | **66.86%** |
| Jet 7 | Combat Air Patrol | 70.85 m | 22.68 m | **67.99%** |
| **Average** | — | **70.81 m** | **22.95 m** | **67.58%** |

> **Validation:** Python `verify_part_b.py` confirmed **bit-for-bit numerical parity** across 1, 2, 4, and 8 worker configurations. Max discrepancy = **0.00e+00 m** ✅

*(See `results/charts/trajectory_jet0.png` and `rmse_per_jet.png`)*

---

## Slide 10: Part B — Multi-Process Parallelism & CPU Affinity

### Worker Assignment Strategy & OS Optimizations
```
8 Jets → assigned by: jet_id % num_workers

1 Worker:  W0 → {0,1,2,3,4,5,6,7} [Core 0]
2 Workers: W0 → {0,2,4,6} [Core 0]  |  W1 → {1,3,5,7} [Core 1]
4 Workers: W0 → {0,4} [Core 0]      |  W1 → {1,5} [Core 1] ...
8 Workers: W0 → {0} [Core 0]        |  W1 → {1} [Core 1]   ...
```

### Advanced OS Feature: CPU Core Affinity
- **Windows:** `SetProcessAffinityMask(GetCurrentProcess(), 1ULL << (w % numCores))`
- **Linux:** `sched_setaffinity(0, sizeof(cpuset), &cpuset)`
- **Why it matters:** Eliminates OS context switching and cache thrashing across CPU cores!

### Why No Race Conditions?
- Each `FilteredOutput[jet_id]` slot is written by **exactly one worker**
- Workers only write to their assigned jet slots (non-overlapping)
- No mutex/semaphore needed — the assignment strategy is the synchronization

### Theoretical Speedup (Amdahl's Law, p=0.90)
| Workers | Ideal | Theoretical | Linux (est.) | Windows (est.) |
|---|---|---|---|---|
| 1 | 1.0× | 1.0× | 1.0× | 1.0× |
| 2 | 2.0× | 1.82× | 1.90× | 1.72× |
| 4 | 4.0× | 3.08× | 3.49× | 2.89× |
| 8 | 8.0× | 4.71× | 5.93× | 4.37× |

**Linux achieves higher speedup** because fork() overhead (94 µs) is negligible vs. the KF computation time, while Windows CreateProcess overhead (2,113 µs) consumes a larger fraction of total runtime.

---

## Slide 11: Lessons Learned — OS Design Choices Matter

### 1. Simplicity vs. Security
| OS | Philosophy | Consequence |
|---|---|---|
| Unix/Linux | Simple 9-bit permissions | chmod = 3.8 µs |
| Windows | Full ACL/SID security model | SetFileSecurity = 133 µs |

Neither is "wrong" — it's the right tool for the right context.

### 2. Process = Heavyweight on Windows, Lightweight on Linux
- **Windows threads** are cheap (similar to Linux threads)  
- **Windows processes** carry full Win32 baggage: 2,113 µs startup
- **Linux fork()** is as cheap as creating a thread on Windows: 94 µs
- **Design takeaway:** Linux favors multi-process servers; Windows favors multi-threaded servers

### 3. Shared Memory is the Right IPC for Kalman Filters
After setup (3–7 µs), data transfer between workers = **direct memory access** with **zero syscalls**. The filter runs at full CPU speed with no kernel transitions during computation.

### 4. The OS Scheduler is Not Your Friend (During Benchmarks)
- Windows 15 ms scheduler quantum caused PipeRead_64KB max = 3,044 µs
- Always use **median**, not mean or max, for latency benchmarking
- Real systems use real-time scheduling (`SCHED_FIFO`) or CPU pinning to avoid this

---

## Slide 12: Conclusions & Summary

### Part A — What We Measured

✅ **28 Windows + 28 Linux syscalls** across 5 categories  
✅ **Statistically sound** (1,000 iterations, median latency, stddev)  
✅ **Quantified** the fork vs. CreateProcess gap: **22×**  
✅ **Explained** Linux write_1MB slowness (kernel writeback dirty-page cost)  
✅ **Documented** scheduler anomaly (Windows PipeRead_64KB outlier)  

### Part B — What We Built

✅ **Kalman Filter** reduces supersonic jet tracking error by **54.45%**  
✅ **8 jets tracked in parallel** using OS shared memory + fork/CreateProcess  
✅ **Bit-for-bit numerical parity** across 1/2/4/8 worker configurations  
✅ **Scales** according to Amdahl's Law (Linux > Windows for multi-process speedup)  

### The Big Picture

> Operating system design decisions made in the 1970s–1990s continue to have **measurable, quantifiable performance consequences in 2026**.  
> This project put **real numbers** on those differences.

---

*End of Presentation — Questions Welcome*

---

**GitHub:** [github.com/m-noumanfazil/OS_Kalman_Filter_Project](https://github.com/m-noumanfazil/OS_Kalman_Filter_Project)  
**Branch:** main | **Language:** C++17 + Python 3.12
