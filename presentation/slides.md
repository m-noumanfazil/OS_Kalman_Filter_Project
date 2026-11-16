# CT-353 Operating Systems — Project Presentation
## Supersonic Fighter Jet Radar Tracking: OS System Calls + Kalman Filter

---

## Slide 1: Title & Team Information

# CT-353 Operating Systems
## Complex Computing Activity (CCA)

### **Supersonic Fighter Jet Radar Tracking**
### Native System Calls & Multi-Process Kalman Filter on Windows 10 & Ubuntu 22.04

---

**Department:** Department of Computer Science & Information Technology  
**Institution:** NED University of Engineering & Technology  

**Team Members (Group of 3):**
1. **Muhammad Nouman Fazil** (CT-24259) — Systems Architecture, Linux/Win32 Implementations, Benchmarking Harness
2. **Muhammad Usman Murtaza** (CT-24262) — Kalman Filter Kinematic Modeling, Telemetry Generation, Validation Analysis
3. **Muneeb ur Rehman** (CT-24283) — Performance Profiling, OS Anomaly Research, Documentation and Verification

**Repository:** [github.com/m-noumanfazil/OS_Kalman_Filter_Project](https://github.com/m-noumanfazil/OS_Kalman_Filter_Project)  
**Date:** November 2026

---

## Slide 2: Project Overview & Scope

### Two Interconnected Phases

| Phase | Scope | Core Engineering Objective |
|---|---|---|
| **Part A: System Micro-benchmarking** | **31 Windows APIs** vs **32 Linux Syscalls** across 5 core OS domains | Measure sub-microsecond latency, CPU time, memory deltas, and page faults on identical bare metal |
| **Part B: Real-World Radar Tracking** | **Multi-process 2D Kalman Filter** tracking **8 supersonic fighter jets** | Evaluate how native OS primitives (shared memory, process spawning, CPU affinity) perform under load |

### Experimental Platform & Tools Used
- **Bare-Metal Dual-Boot Hardware:** HP ZBook (Intel Core i7-10850H @ 2.70 GHz, 6 physical cores / 12 logical threads, 32 GB RAM, NVMe SSD). Identical hardware eliminates virtualization artifacts.
- **Operating Systems:** Ubuntu 22.04.4 LTS (Linux Kernel 5.15.0) vs Windows 10 Pro (Build 19045, NT 10.0).
- **Compilers & Flags:** GCC 11.4.0 (Linux) and MinGW-w64 GCC 13.2.0 (Windows) using `-O2 -Wall -std=c++17`.
- **System Documentation:** POSIX.1-2017 Base Specifications (`man 2`/`man 3`) and Microsoft Learn Win32 API Reference (MSDN).
- **High-Resolution Profiling Tools:** Windows `QueryPerformanceCounter` (QPC, ~100 ns) & PSAPI vs Linux `clock_gettime(CLOCK_MONOTONIC)` & `getrusage(RUSAGE_SELF)`.

---

## Slide 3: System Architecture & Code Modularity

### Modular Subsystem Layout
```
OS_Kalman_Filter_Project/
├── common/               ◄── Zero-Dependency Core
│   ├── common_types.hpp  (Shared structures: BenchmarkResult, RadarMeasurement, SharedMemoryArena)
│   ├── kalman_core.hpp   (Pure header-only 2D kinematic math; zero OS or external dependencies)
│   └── sensor_data.csv   (Standardized 8-jet radar telemetry dataset, 80,000 observations)
├── Windows/ & Linux/     ◄── Mirrored Subsystem Implementations (12 Standalone Executables)
│   ├── sys_file_io.cpp   (File operations: 4 KB & 1 MB block latency, sequential & random I/O)
│   ├── sys_memory.cpp    (VirtualAlloc/mmap, demand paging, physical page fault tracking)
│   ├── sys_process.cpp   (Process lifecycle, fork vs CreateProcess, execve, waitpid)
│   ├── sys_ipc.cpp       (Anonymous pipes vs zero-copy shared memory arenas)
│   ├── sys_permissions.cpp (NTFS DACL security descriptors vs POSIX 9-bit mode flags)
│   └── kalman_filter.cpp (Multi-process parallel jet tracking with core affinity)
└── results/ & logs/      ◄── Reproducible CSV metrics, execution transcripts, and generated plots
```

### 3-Phase Execution Model in Every Subsystem Binary
1. **Interactive Functional Demo:** Demonstrates system call correctness and validates outputs.
2. **Microsecond Timed Benchmark:** Executes 1,000 warm-up and timed iterations, capturing latency, CPU time, and memory usage.
3. **Structured Diagnostic Logging:** Emits machine-readable CSV records and full console execution transcripts.

---

## Slide 4: Part A — System Call Usage & Error Handling

### 5 Core OS Subsystems Evaluated

| Subsystem | Windows Win32 APIs | Linux POSIX Syscalls | Key Mechanism Evaluated |
|---|---|---|---|
| **1. File I/O** | `CreateFileA`, `ReadFile`, `WriteFile`, `CloseHandle` | `open`, `read`, `write`, `close` | 4 KB vs 1 MB block throughput & write-behind caching |
| **2. Memory** | `VirtualAlloc`, `VirtualFree` | `mmap`, `munmap` | Lazy demand paging vs first-touch physical page faults |
| **3. Process** | `CreateProcessA`, `WaitForSingleObject` | `fork`, `execve`, `waitpid` | Address space initialization vs Copy-on-Write (COW) |
| **4. IPC** | `CreatePipe`, `CreateFileMappingA`, `MapViewOfFile` | `pipe`, `shmget`, `shmat`, `shmdt` | Pipe buffer context switches vs zero-copy memory arenas |
| **5. Permissions** | `SetFileSecurityA`, `GetFileSecurityA` | `chmod`, `chown`, `umask` | NTFS DACL/SID security evaluation vs POSIX inode mode bits |

### Error Handling & Logging Implementation
- **Immediate Error Preservation:** Captured `errno` (Linux) or `GetLastError()` (Windows) immediately before any logging call could overwrite the thread-local error state.
- **Human-Readable Diagnostics:** Mapped numeric codes to descriptions via `strerror()` and `FormatMessageA()`.
- **Atomic File Logging:** Structured diagnostic messages appended to `logs/windows_run.log` and `logs/linux_run.log`.
- **Negative Edge-Case Tests:** Deliberately tested failure recovery on missing files, invalid handles, and child process non-zero exit codes.

---

## Slide 5: Part A — Performance Comparison: Latency, CPU & Memory

### Empirical Measurements on HP ZBook (1,000 Iterations per Syscall)

| Benchmark Operation | Windows Median Latency | Linux Median Latency | Measured Margin | Windows CPU Time | Linux CPU Time | Windows Memory Delta | Linux Memory Delta |
|---|---|---|---|---|---|---|---|
| **Process Creation** | 1,659.0 µs | **115.4 µs** | 🐧 **Linux 14.4× faster** | 1,328.1 µs | **263.8 µs** | +208 KB | **0 KB (COW)** |
| **Complete Spawn + Wait** | 10,670.0 µs | **2,248.9 µs** | 🐧 **Linux 4.7× faster** | 1,171.9 µs | 2,389.6 µs | 0 KB | 0 KB |
| **File Permissions** | 121.8 µs | **5.3 µs** | 🐧 **Linux 22.8× faster** | 234.4 µs | **12.1 µs** | 0 KB | 0 KB |
| **File Open / Create** | 29.7 µs | **2.8 µs** | 🐧 **Linux 10.6× faster** | 46.9 µs | **5.4 µs** | +4 KB | +4 KB |
| **Large Write (1 MB)** | **62.0 µs** | 200.4 µs | 🪟 **Windows 3.2× faster** | **93.8 µs** | 213.9 µs | 0 KB | +4 KB |
| **Virtual Memory Alloc (64 KB)** | 2.9 µs | **1.8 µs** | 🤝 **Parity (~1.6×)** | 15.6 µs | 7.1 µs | **0 KB (Lazy)** | **0 KB (Lazy)** |
| **First-Touch Physical Paging** | **39.7 µs** | 46.6 µs | 🪟 **Windows 1.2× faster** | 46.9 µs | 53.3 µs | **+64,000 KB** | **+64,000 KB** |

### Key Empirical Findings
- **Linux dominates process lifecycle:** `fork()` is 14.4× faster because it clones page table entries without allocating physical memory.
- **Windows dominates buffered writes:** The NT Cache Manager's asynchronous write-behind buffer acknowledged 1 MB writes in 62.0 µs vs Linux 200.4 µs.
- **Demand paging parity verified:** Both kernels allocate 0 physical frames at reservation (`VirtualAlloc`/`mmap`), triggering exactly 16,000 page faults upon first memory touch (+64 MB).

---

## Slide 6: OS Deep Dive — Process Lifecycle & Security Models

### Why Is Linux `fork()` 14.4× Faster Than Windows `CreateProcessA()`?

```
Linux fork() — Copy-on-Write (115.4 µs)
Parent Page Tables ──► Duplicated Read-Only (~115 µs)
                       ├── Frame 0x1A00 ──► Shared Read-Only (0 KB allocated)
                       └── Physical frame copied ONLY on memory write (Page Fault)

Windows CreateProcessA() — Full Subsystem Initialization (1,659.0 µs)
1. Read & parse PE executable headers from disk
2. Create fresh Virtual Address Descriptor (VAD) root tree
3. Map ntdll.dll, kernel32.dll, and C Runtime (CRT) subsystem dependencies
4. Initialize Win32 executive client structures & primary thread stack
5. Validate user security tokens and DACL access rights
```

### Security Subsystem Comparison: DACLs vs Inode Mode Bits
- **Linux (`chmod` = 5.3 µs):** 9 permission bits stored directly in the inode structure; updated with a single fast metadata write.
- **Windows (`SetFileSecurity` = 121.8 µs):** Constructs full Discretionary Access Control Lists (DACLs), verifies Security Identifiers (SIDs), and evaluates inheritance rules (23× slower, but enterprise-grade).

---

## Slide 7: Part B — Supersonic Fighter Jet Radar Tracking Scenario

### Tactical Air Defense Problem
- **Fleet Scope:** **8 supersonic fighter jets** flying combat maneuvers at **Mach 1.5 to 2.0** (540 to 680 m/s).
- **Radar Sensor:** 20 Hz pulse-Doppler air defense radar ($\Delta t = 0.05\text{ s}$).
- **Workload Volume:** **80,000 radar observations** (10,000 timesteps $\times$ 8 aircraft = 500 seconds of flight per jet).
- **Measurement Corruption:** Range and bearing degraded by zero-mean Gaussian radar noise ($\sigma = 50\text{ m}$, variance $R = 2500\text{ m}^2$).

### Tactical Radar Jitter Across the Fleet

| Aircraft ID | Tactical Flight Profile | True Position Error | Raw Radar Jitter (RMSE) |
|---|---|---|---|
| **Jet 0** | Climbing Right Turn | 0.00 m (Ground Truth) | 70.63 m |
| **Jet 2** | Tactical 7G Combat Turn | 0.00 m (Ground Truth) | 71.13 m |
| **Jet 5** | High-Speed Mach 2 Dash | 0.00 m (Ground Truth) | 70.15 m |
| **Jet 7** | Combat Air Patrol (CAP Orbit) | 0.00 m (Ground Truth) | 70.85 m |
| **Fleet Average** | **All 8 Supersonic Jets (80,000 Pings)** | — | **70.81 m** |

**Operational Danger:** At Mach 2, an aircraft travels 34 meters per radar scan. A 70-meter sensor jitter exceeds the physical displacement per timestep, making missile fire control impossible without kinematic filtering.

---

## Slide 8: Part B — Kalman Filter Kinematic Implementation

### State Vector & Constant Velocity Kinematic Model
Independent 2D Cartesian axis filtering implemented in `common/kalman_core.hpp`:
$$\mathbf{x} = \begin{bmatrix} p \\ v \end{bmatrix} = \begin{bmatrix} \text{position (m)} \\ \text{velocity (m/s)} \end{bmatrix}, \quad \mathbf{F} = \begin{bmatrix} 1 & \Delta t \\ 0 & 1 \end{bmatrix}, \quad \mathbf{H} = \begin{bmatrix} 1 & 0 \end{bmatrix}$$

### Complete 2D Discrete White Noise Acceleration (DWNA) Formulation
1. **Prediction Step (State & Covariance Extrapolation):**
   $$p_{\text{pred}} = p + v \cdot \Delta t, \quad v_{\text{pred}} = v$$
   Continuous process noise intensity ($q = 1500.0\text{ m}^2/\text{s}^3$) projected over $\Delta t = 0.05\text{ s}$:
   $$p_{00,\text{pred}} = p_{00} + 2\Delta t \cdot p_{01} + \Delta t^2 \cdot p_{11} + q \frac{\Delta t^3}{3}$$
   $$p_{01,\text{pred}} = p_{01} + \Delta t \cdot p_{11} + q \frac{\Delta t^2}{2}, \quad p_{11,\text{pred}} = p_{11} + q \cdot \Delta t$$

2. **Measurement Correction Step ($r = 2500.0\text{ m}^2$ matching radar $\sigma = 50\text{ m}$):**
   - Innovation residual: $y = z_{\text{meas}} - p_{\text{pred}}$
   - Optimal Kalman Gains: $k_0 = \frac{p_{00,\text{pred}}}{p_{00,\text{pred}} + r}, \quad k_1 = \frac{p_{01,\text{pred}}}{p_{00,\text{pred}} + r}$
   - Posterior State: $p = p_{\text{pred}} + k_0 \cdot y, \quad v = v_{\text{pred}} + k_1 \cdot y$

---

## Slide 9: Part B — Tracking Accuracy & Mathematical Parity

### Kinematic Error Reduction Across 80,000 Telemetry Coordinates

| Aircraft ID | Tactical Flight Profile | Raw Radar Jitter (RMSE) | Filtered Track (RMSE) | Noise Reduction |
|---|---|---|---|---|
| **Jet 0** | Climbing Right Turn | 70.63 m | 23.14 m | **67.24%** |
| **Jet 1** | Diving Left Turn | 70.81 m | 22.96 m | **67.57%** |
| **Jet 2** | Tactical 7G Combat Turn | 71.13 m | 22.86 m | **67.87%** |
| **Jet 3** | Supersonic Sweep | 71.21 m | 23.39 m | **67.16%** |
| **Jet 4** | Intercept Vector | 70.70 m | 22.77 m | **67.80%** |
| **Jet 5** | High-Speed Dash | 70.15 m | 22.29 m | **68.22%** |
| **Jet 6** | Evasive Weave | 70.95 m | 23.52 m | **66.86%** |
| **Jet 7** | Combat Air Patrol | 70.85 m | 22.68 m | **67.99%** |
| **Fleet Average** | **All 8 Supersonic Jets (80,000 Pings)** | **70.81 m** | **22.95 m** | **67.58% Error Reduction** |

### Numerical Parity Across Workers & Platforms
- **Bit-for-Bit Mathematical Equivalence:** Single-process vs 8-worker execution outputs were verified across all 80,000 coordinates. Maximum discrepancy = **`0.00e+00 m`**.
- Cross-platform numerical parity between Windows and Linux verified to 10 decimal places.

---

## Slide 10: Part B — Multi-Process Parallel Scalability & CPU Affinity

### Measured Multi-Process Speedup on HP ZBook (Dual-Scale Evaluation)

| Configuration | Ideal Linear | Linux Speedup (Measured) | Windows Speedup (Measured) | Linux Wall-Clock | Windows Wall-Clock |
|---|---|---|---|---|---|
| **1 Worker (Baseline)** | 1.00× | **1.00×** | **1.00×** | 1.93 ms | 1.95 ms |
| **2 Workers** | 2.00× | **0.40×** | **0.08×** | 5.23 ms (2.5× slower) | 29.95 ms (12.8× slower) |
| **4 Workers** | 4.00× | **0.39×** | **0.06×** | 5.35 ms (2.6× slower) | 33.82 ms (15.4× slower) |
| **8 Workers** | 8.00× | **0.25×** | **0.05×** | 7.95 ms (4.0× slower) | 43.46 ms (20.8× slower) |

### Amdahl's Law & Granularity Analysis
- **C++ Kalman Math is ultra-fast:** 80,000 matrix operations execute in only **~1.9 ms** of raw CPU time.
- **Process Spawning Latency Dominates:** Spawning 8 worker processes costs **~25–40 ms** on Windows (`CreateProcessA`) and **~4–6 ms** on Linux (`fork`). The lifecycle overhead exceeds the computation.
- **Linux vs Windows Multi-Processing:** Linux parallel filtering is **5× faster than Windows** (0.25× vs 0.05×) because `fork()` is vastly cheaper than Win32 subsystem initialization.
- **Hardware Core Pinning:** Workers locked to physical cores via `SetProcessAffinityMask` / `sched_setaffinity` eliminated thread migration and L1/L2 cache invalidations.

---

## Slide 11: Key OS Architecture Lessons & Engineering Tradeoffs

### 1. Security Subsystem vs File Access Simplicity
- **Linux (`chmod` = 5.3 µs):** 9-bit permission flags stored directly within the inode struct; modified by a single metadata write.
- **Windows (`SetFileSecurity` = 121.8 µs):** Traverses complex Discretionary Access Control Lists (DACLs) with security identifier (SID) validation and inheritance recalculations (23× slower, but enterprise-grade).

### 2. Process Weight & Production Server Architectures
- **Windows:** Heavy process initialization (1,659 µs) explains why Windows services (IIS, SQL Server) rely on **multi-threaded thread pools**.
- **Linux:** Lightweight `fork()` (115 µs) underpins the classic Unix **multi-process architecture** (PostgreSQL, NGINX worker models).

### 3. Zero-Copy Shared Memory vs Pipe IPC
- Memory-mapped arenas (`mmap` / `CreateFileMapping`) require only a 3–7 µs setup. Telemetry processing then runs at **raw bus memory bandwidth with zero context switches**.

### 4. Lock-Free Workload Partitioning
- Partitioning by `jet_id % num_workers` gives each process disjoint array bounds in shared memory, achieving **100% thread safety without mutexes, semaphores, or spinlocks**.

---

## Slide 12: Conclusions & Summary of Achievements

### Key Project Outcomes

1. **System Call Usage & Evaluation:**
   - Implemented and benchmarked **31 Windows APIs** and **32 Linux Syscalls** across 5 core OS domains with microsecond timers and hardware performance counters.
   - Built robust error handling with thread-local error state preservation and atomic logging.

2. **Kalman Filter Implementation:**
   - Developed an ultra-fast, zero-dependency 2D DWNA Kalman Filter tracking 8 supersonic jets over 80,000 radar observations.
   - Reduced radar tracking error from **70.81 m to 22.95 m (67.58% error reduction)**.

3. **Performance Comparison & OS Insights:**
   - Quantified the **14.4× process creation gap** and **22.8× permission evaluation gap** on identical bare-metal hardware.
   - Proved bit-for-bit mathematical parity (**`0.00e+00 m` deviation**) across single-worker, 8-worker, and cross-OS runs.
   - Empirically demonstrated why process lifecycle overhead causes Amdahl's Law slowdown when computational grain size is fine (~1.9 ms).

---

### *Thank you — We invite your questions.*

**Project Deliverables:**
- **Code Repository:** [github.com/m-noumanfazil/OS_Kalman_Filter_Project](https://github.com/m-noumanfazil/OS_Kalman_Filter_Project)
- **Technical Report:** [Final_report_draft.md](file:///D:/UNBACKED%20DATA/CCPs%205th%20Sem/OS_CCP/report/Final_report_draft.md)
- **Benchmark Charts:** [results/charts/](file:///D:/UNBACKED%20DATA/CCPs%205th%20Sem/OS_CCP/results/charts/)