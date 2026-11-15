# CT-353 Operating Systems — CCA Presentation
## Supersonic Fighter Jet Radar Tracking: OS System Calls + Kalman Filter

---

## Slide 1: Title & Team

# CT-353 Operating Systems
## Complex Computing Activity (CCA)

### **Supersonic Fighter Jet Radar Tracking**
### Native System Calls & Multi-Process Kalman Filter on Windows 10 & Ubuntu 22.04

---

**Department:** Department of Computer Science & Information Technology  
**Institution:** NED University of Engineering & Technology  

**Team Members:**
1. **Muhammad Nouman Fazil** (CT-24259) — Systems Architecture, Linux/Win32 Implementations, Benchmarking Harness
2. **Muhammad Usman Murtaze** (CT-24262) — Kalman Filter Kinematic Modeling, Telemetry Generation, Validation Analysis
3. **Muneeb ur Rehman** (CT-24282) — Performance Profiling, OS Anomaly Research, Documentation and Verification

**Repository:** [github.com/m-noumanfazil/OS_Kalman_Filter_Project](https://github.com/m-noumanfazil/OS_Kalman_Filter_Project)  
**Date:** November 2026

---

## Slide 2: Project Overview

### Two Interconnected Phases

| Phase | Scope | Objective |
|---|---|---|
| **Part A — Micro-benchmarking** | **31 Windows APIs** vs **32 Linux Syscalls** across 5 categories | Measure sub-microsecond latency, CPU time, cycles, memory deltas, and page faults |
| **Part B — Real-World Application** | **Multi-process 2D Kalman Filter** tracking **8 supersonic fighter jets** | Evaluate how native OS primitives (shared memory, process spawning, CPU affinity) perform in practice |

### Why Supersonic Fighter Jet Tracking?
- Air defense radar operates at **20 Hz** ($\Delta t = 0.05\text{ s}$), but pings suffer from atmospheric and sensor noise ($\sigma = 50\text{ m}$).
- 8 supersonic jets (Mach 1.5–2.0, 540–680 m/s) maneuvering concurrently $\rightarrow$ **perfect candidate for multi-process parallelism**.
- Exercises all 5 Part A OS domains: **File I/O**, **Shared Memory**, **Process Lifecycle**, **IPC**, and **Hardware Probes**.

---

## Slide 3: System Architecture

```
Radar Telemetry Dataset (common/sensor_data.csv)
      │  8 fighter jets × 10,000 steps = 80,000 pings (500 s flight)
      ▼
┌──────────────────────────────────────────┐
│   Shared Memory Arena (~8 MB in RAM)     │  ◄── OS Zero-Copy Memory
│   - recordCount, trackCount              │      Windows: CreateFileMapping + MapViewOfFile
│   - 80,000 RadarMeasurements (input)     │      Linux:   mmap(MAP_SHARED|MAP_ANONYMOUS)
│   - 80,000 FilteredOutputs (output)      │
└────────────────────┬─────────────────────┘
                     │
     ┌───────────────┼───────────────┐
     ▼               ▼               ▼
 Worker Process 0  Worker Process 1 ... Worker Process N-1  ◄── OS Process Spawning
  Windows: Core 0   Windows: Core 1      Windows: Core N-1      Win: CreateProcessA + SetProcessAffinityMask
  Linux:   Core 0   Linux:   Core 1      Linux:   Core N-1      Lin: fork() + sched_setaffinity
   (Jets 0, 4)       (Jets 1, 5)          (Jets ...)
         │               │                    │
         └───────────────┼────────────────────┘
                         ▼
             Filtered Trajectory CSV  ◄── OS Native File I/O (WriteFile / write)
```

**Benchmarking Environment:** Bare-metal dual-boot **HP ZBook** (Intel Core i7) running Windows 10 Pro & Ubuntu 22.04 LTS natively (no VMs, no WSL).

---

## Slide 4: Part A — 5 System Call Benchmark Categories

| # | Category | Windows Win32 APIs | Linux POSIX Syscalls | Key Metric Evaluated |
|---|---|---|---|---|
| 1 | **File I/O** | `CreateFileA`, `ReadFile`, `WriteFile`, `CloseHandle` | `open`, `read`, `write`, `close` | 4 KB vs 1 MB block latency & kernel writeback |
| 2 | **Memory** | `VirtualAlloc`, `VirtualFree` | `mmap`, `munmap` | Lazy demand paging vs first-touch page faults |
| 3 | **Process** | `CreateProcessA`, `WaitForSingleObject` | `fork`, `execve`, `waitpid` | Address space initialization vs COW cloning |
| 4 | **IPC** | `CreatePipe`, `CreateFileMappingA`, `MapViewOfFile` | `pipe`, `shmget`, `shmat`, `shmdt` | Pipe buffer throughput vs shared memory zero-copy |
| 5 | **Permissions** | `SetFileSecurityA`, `GetFileSecurityA`, `SetFileAttributesA` | `chmod`, `chown`, `umask` | NTFS DACL/SID security evaluation vs 9-bit mode bits |

**Statistical Rigor:** 1,000 iterations per call (200 for process routines) with warm-up runs. Median latency used as the robust primary metric.

---

## Slide 5: Part A — Empirical Cross-OS Findings

### Key Benchmark Measurements (Median Latencies on HP ZBook)

| Operation | Windows 10 | Ubuntu 22.04 | Measured Margin | Architectural Cause |
|---|---|---|---|---|
| **Process Creation** | 1,659.0 µs | **115.4 µs** | 🐧 Linux **14.4× faster** | Linux COW page-table duplication vs Win32 PE loading |
| **Full Spawn+Wait** | 10,670.0 µs | **2,248.9 µs** | 🐧 Linux **4.7× faster** | Win32 subsystem & DLL initialization overhead |
| **File Permissions** | 121.8 µs | **5.3 µs** | 🐧 Linux **22.8× faster** | Simple inode mode write vs complex NTFS DACL traversal |
| **File Creation** | 19.7 µs | **1.9 µs** | 🐧 Linux **10.6× faster** | dentry/inode cache vs NTFS MFT metadata transaction |
| **Large Write (1 MB)** | **62.0 µs** | 200.4 µs | 🪟 Windows **3.2× faster** | NT Cache Manager asynchronous write-behind buffering |
| **Demand Page Alloc** | 2.1 µs | 2.5 µs | 🤝 **Parity (~1.0×)** | Both OSes defer physical frame allocation to first touch |

> **Documented OS Scheduler Anomaly:** Windows `PipeRead_64KB` displayed median 6.7 µs but a maximum of **3,044.3 µs** ($\text{stddev} = 146\text{ µs}$) due to thread preemption by the 15 ms Windows client scheduler quantum.

---

## Slide 6: Part A Deep Dive — Why Is Linux Process Creation 14.4× Faster?

### Linux `fork()`: Copy-on-Write (COW) Semantics
```
Parent Address Space
      │
      ▼
Page Table Duplication (~115 µs)
      ├── Frame 0x1A00 → [Shared Read-Only] ◄── Zero memory copied
      ├── Frame 0x1B00 → [Shared Read-Only] ◄── Zero memory copied
      └── Frame 0x1C00 → [Shared Read-Only] ◄── Zero memory copied

Physical copy occurs ONLY when a process performs a write (Page Fault)
```

### Windows `CreateProcessA()`: Full Subsystem Initialization
```
1. Parse PE binary header from disk
2. Allocate entirely new virtual address space (VAD tree)
3. Map ntdll.dll, kernel32.dll, and CRT dependencies
4. Initialize the Win32 subsystem client structures
5. Construct primary thread stack and register context (~1,659 µs)
6. Security token and DACL validation
```

**Crucial Takeaway for Part B:** Because `fork()` is 14× lighter, Linux can orchestrate multi-process workers with dramatically less OS friction than Windows.

---

## Slide 7: Part B — Supersonic Fighter Jet Radar Scenario

### Mission Context
- **8 supersonic fighter jets** flying at **Mach 1.5 to 2.0** (540 to 680 m/s).
- Tracking duration: **10,000 steps** at 20 Hz ($\Delta t = 0.05\text{ s}$) = **80,000 total observations** (~8.3 minutes of supersonic flight).
- Radar sensor corrupted by Gaussian range and bearing noise ($\sigma = 50\text{ m}$).

### The Air Defense Challenge: Radar Jitter at Mach 2

| Aircraft ID | Tactical Profile | True Position Error | Raw Radar RMSE |
|---|---|---|---|
| **Jet 0** | Climbing Right Turn | 0.00 m (Ground Truth) | 70.63 m |
| **Jet 2** | Tactical 7G Combat Turn | 0.00 m (Ground Truth) | 71.13 m |
| **Jet 7** | Combat Air Patrol (CAP Orbit) | 0.00 m (Ground Truth) | 70.85 m |
| **Fleet Average** | **All 8 Supersonic Jets** | — | **70.81 m** |

**Operational Danger:** At Mach 2 (680 m/s), an aircraft travels 34 meters per radar scan. A 70-meter sensor error exceeds the physical displacement per timestep, making missile fire control impossible without kinematic filtering.

---

## Slide 8: Part B — Kalman Filter Kinematic Design

### State Vector & Motion Model
Kinematic state vector for each independent Cartesian axis in `AxisKalmanFilter`:
$$\mathbf{x} = \begin{bmatrix} p \\ v \end{bmatrix} = \begin{bmatrix} \text{position (m)} \\ \text{velocity (m/s)} \end{bmatrix}$$

### Complete Prediction & Update Formulation (`common/kalman_core.hpp`)
1. **Prediction Step (Constant Velocity Extrapolation):**
   $$p_{\text{pred}} = p + v \cdot \Delta t, \quad v_{\text{pred}} = v$$
   Continuous White Noise Acceleration (DWNA) covariance projection ($q = 1500.0\text{ m}^2/\text{s}^3$):
   $$p_{00,\text{pred}} = p_{00} + 2\Delta t \cdot p_{01} + \Delta t^2 \cdot p_{11} + q \frac{\Delta t^3}{3}$$
   $$p_{01,\text{pred}} = p_{01} + \Delta t \cdot p_{11} + q \frac{\Delta t^2}{2}, \quad p_{11,\text{pred}} = p_{11} + q \cdot \Delta t$$

2. **Measurement Update Step ($r = 2500.0\text{ m}^2$ matching radar $\sigma = 50\text{ m}$):**
   - Innovation residual: $y = z_{\text{meas}} - p_{\text{pred}}$
   - Kalman Gains: $k_0 = \frac{p_{00,\text{pred}}}{p_{00,\text{pred}} + r}, \quad k_1 = \frac{p_{01,\text{pred}}}{p_{00,\text{pred}} + r}$
   - Posterior State: $p = p_{\text{pred}} + k_0 \cdot y, \quad v = v_{\text{pred}} + k_1 \cdot y$

---

## Slide 9: Part B — Tracking Accuracy & Noise Reduction

### Kinematic Error Reduction Across 80,000 Telemetry Points

| Aircraft ID | Flight Profile | Raw Radar Jitter (RMSE) | Filtered Track (RMSE) | Noise Reduction |
|---|---|---|---|---|
| **Jet 0** | Climbing Right Turn | 70.63 m | 23.14 m | **67.24%** |
| **Jet 1** | Diving Left Turn | 70.81 m | 22.96 m | **67.57%** |
| **Jet 2** | Tactical 7G Combat Turn | 71.13 m | 22.86 m | **67.87%** |
| **Jet 3** | Supersonic Sweep | 71.21 m | 23.39 m | **67.16%** |
| **Jet 4** | Intercept Vector | 70.70 m | 22.77 m | **67.80%** |
| **Jet 5** | High-Speed Dash | 70.15 m | 22.29 m | **68.22%** |
| **Jet 6** | Evasive Weave | 70.95 m | 23.52 m | **66.86%** |
| **Jet 7** | Combat Air Patrol | 70.85 m | 22.68 m | **67.99%** |
| **Overall** | **80,000 Telemetry Points** | **70.81 m** | **22.95 m** | **67.58% Error Reduction** |

> **Numerical Parity Verification:** Single-process vs 8-worker execution outputs were verified across all 80,000 coordinates. Maximum discrepancy = **`0.00e+00 m`** (Bit-for-bit mathematical equivalence).

*(Evidence charts: `results/charts/trajectory_jet0.png` and `rmse_per_jet.png`)*

---

## Slide 10: Part B — Multi-Process Parallel Scalability

### Empirical Speedup Measurements on HP ZBook (Dual-Scale Evaluation)

| Configuration | Ideal Linear | Linux (Measured) | Windows (Measured) | Relative Slowdown |
|---|---|---|---|---|
| **1 Worker (Baseline)** | 1.00× | **1.00×** (1.93 ms) | **1.00×** (1.95 ms) | Baseline (In-Process CPU) |
| **2 Workers** | 2.00× | **0.40×** (5.23 ms) | **0.08×** (29.95 ms) | Linux 2.5× / Win 12.8× slower |
| **4 Workers** | 4.00× | **0.39×** (5.35 ms) | **0.06×** (33.82 ms) | Linux 2.6× / Win 15.4× slower |
| **8 Workers** | 8.00× | **0.25×** (7.95 ms) | **0.05×** (43.46 ms) | Linux 4.0× / Win 20.8× slower |

### Core OS Lesson: Workload Granularity vs Process Lifecycle Overhead
- **C++ Kalman Math is extremely fast:** Processing 80,000 points takes only **~1.9 ms** of pure CPU arithmetic.
- **Process spawn overhead dominates:** In an on-demand batch benchmark, creating 8 processes costs **~25–40 ms** on Windows and **~4–6 ms** on Linux.
- **Empirical Architecture Proof:** Linux multi-processing is **5× faster than Windows** (0.25× vs 0.05× speedup) because `fork()` is vastly cheaper than Win32 subsystem initialization.
- **CPU Affinity Pinning:** Workers locked to cores (`SetProcessAffinityMask` / `sched_setaffinity`) eliminated core migration and cache thrashing.

*(Evidence chart: `results/charts/speedup_curve.png`)*

---

## Slide 11: Architectural Lessons Learned

### 1. Security vs Simplicity Tradeoff
- **Linux (`chmod` = 5.3 µs):** Simple 9-bit permission flags stored directly in inode metadata.
- **Windows (`SetFileSecurity` = 121.8 µs):** Full Discretionary Access Control Lists (DACLs) evaluated against security tokens (SIDs) and inheritance rules. 23× slower, but provides granular enterprise control.

### 2. Process Weight & Server Architecture Implications
- **Windows:** Heavy process initialization (1,659 µs) drives Windows server software (IIS, SQL Server) toward **multi-threaded thread pools**.
- **Linux:** Ultra-lightweight `fork()` (115 µs) enables the classic Unix **multi-process architecture** (PostgreSQL, NGINX worker models).

### 3. Shared Memory is the Optimal Zero-Copy IPC
- Memory-mapped arenas (`mmap` / `CreateFileMapping`) incur only a one-time 3–7 µs setup. During filtering, data transfer occurs at **direct DRAM speed with zero kernel syscalls**.

### 4. Non-Blocking Partitioning Eliminates Synchronization Costs
- Partitioning work by `track_id % num_workers` assigned mutually exclusive array indices to each worker, achieving **zero lock contention and zero race conditions without mutexes**.

---

## Slide 12: Conclusions & Project Verification

### Comprehensive CCA Deliverables Completed

✅ **Part A Micro-benchmarks:** 31 Windows APIs and 32 Linux Syscalls measured natively with sub-microsecond precision.  
✅ **Empirical Quantifications:** Verified the **14.4× process creation gap** and **22.8× permission evaluation gap** between Linux and Windows.  
✅ **Part B Real-World Integration:** Built a high-throughput, multi-process radar tracking system attenuating supersonic jet noise by **67.58%**.  
✅ **Bit-for-Bit Mathematical Parity:** Exact coordinate equivalence ($0.00\text{ m}$ deviation) across single-process, 8-worker, and cross-OS runs.  
✅ **Operating System Insights:** Successfully explained why batch process lifecycle overhead causes Amdahl's Law slowdown when computational grain size is fine.  

---

### *Thank you — We invite your questions.*

**Repository:** [github.com/m-noumanfazil/OS_Kalman_Filter_Project](https://github.com/m-noumanfazil/OS_Kalman_Filter_Project)  
**Deliverables:** [Final Report](file:///D:/UNBACKED%20DATA/CCPs%205th%20Sem/OS_CCP/report/Final_report_draft.md) | [Presentation Slides](file:///D:/UNBACKED%20DATA/CCPs%205th%20Sem/OS_CCP/presentation/slides.md) | [Performance Charts](file:///D:/UNBACKED%20DATA/CCPs%205th%20Sem/OS_CCP/results/charts/)
