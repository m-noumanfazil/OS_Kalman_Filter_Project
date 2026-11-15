# OS Kalman Filter Project

**Course:** CT-353 Operating Systems — Complex Computing Activity (CCA)

## 1. Project Overview

This project compares how Linux and Windows provide operating-system facilities for file handling, process management, memory management, inter-process communication (IPC), and file permissions.

It consists of two main parts:

- **Part A — System Call and API Implementation:** Implement native operating-system operations, demonstrate their functionality, handle errors, record execution logs, and measure performance.
- **Part B — Kalman Filter Integration:** Apply operating-system facilities to a practical Kalman Filter application involving sensor-data processing, memory mapping, multi-process execution, IPC, and output generation.

Linux development and testing are performed natively on **Ubuntu 22.04**. Windows development and testing are performed natively on **Windows 10** using a dual-boot setup on the same HP ZBook machine.

All operating-system benchmarks are conducted natively, without using a virtual machine or WSL.

## 2. System Calls and APIs Covered

The project implements five categories of operating-system operations.

| Category | Linux | Windows |
|---|---|---|
| File I/O | `open()`, `read()`, `write()`, `close()` | `CreateFile()`, `ReadFile()`, `WriteFile()`, `CloseHandle()` |
| Process | `fork()`, `exec()`, `wait()` | `CreateProcess()`, `WaitForSingleObject()` |
| Memory | `mmap()`, `munmap()` | `VirtualAlloc()`, `VirtualFree()` |
| IPC | `pipe()`, `shmget()`, `shmat()` | `CreatePipe()`, `CreateFileMapping()`, `MapViewOfFile()` |
| Permissions | `chmod()`, `chown()`, `umask()` | `SetFileSecurity()` |

The assignment requires 15 operations per operating system. The CCP explicitly lists 15 Linux interfaces but only 12 Windows APIs. The Windows implementation also uses supporting APIs where required by the implementation.

### 2.1 Operating-System Differences

The two operating systems provide different interfaces and resource-management mechanisms.

- Windows does not provide a direct equivalent of Linux `fork()`. `CreateProcess()` provides process creation through a different mechanism.
- Windows does not provide direct equivalents of Linux `umask()` and `chown()`.
- Windows uses `UnmapViewOfFile()` to release a mapped view created through `MapViewOfFile()`.
- Linux implementations use POSIX/Linux interfaces, while Windows implementations use native Win32 APIs.

Linux functions such as `open()`, `read()`, and `mmap()` are library interfaces that invoke underlying kernel functionality. Windows functions such as `CreateFile()` and `CreateProcess()` are Win32 APIs. The comparison therefore focuses on the native operating-system interfaces rather than claiming that every function is a direct kernel system call.

## 3. Repository Structure

```text
OS_Kalman_Filter_CCP/
│
├── Linux/
│   ├── File_IO/
│   ├── Process/
│   ├── Memory/
│   ├── IPC/
│   ├── Permissions/
│   └── Kalman_Filter/
│
├── Windows/
│   ├── File_IO/
│   ├── Process/
│   ├── Memory/
│   ├── IPC/
│   ├── Permissions/
│   └── Kalman_Filter/
│
├── common/
│   ├── logger.hpp
│   ├── logger_linux.hpp
│   ├── metrics.hpp
│   ├── metrics_linux.hpp
│   ├── kalman_core.hpp
│   ├── generate_sensor_data.py
│   ├── sensor_data.csv
│   ├── generate_charts.py
│   └── verify_part_b.py
│
├── logs/
│   ├── syscalls_linux.log
│   └── syscalls_windows.log
│
├── results/
│   ├── Linux/
│   │   ├── linux_results.csv
│   │   └── filtered_sensor_data.csv
│   │
│   ├── Windows/
│   │   ├── windows_results.csv
│   │   └── filtered_sensor_data.csv
│   │
│   └── charts/
│       ├── trajectory_jet0.png
│       ├── rmse_per_jet.png
│       ├── syscall_comparison_fileio.png
│       ├── syscall_comparison_memory.png
│       ├── syscall_comparison_process.png
│       └── speedup_curve.png
│
├── report/
│   └── Final_report_draft.md
│
├── presentation/
│   └── slides.md
│
├── README.md
└── .gitignore
```

Each operating-system category contains:

- A C++ source file (`.cpp`) implementing demonstrations and benchmarks.
- A text output file (`.txt`) containing the corresponding execution output.

Windows executable files may also be present locally. Generated executables are not required to be committed if they are excluded through `.gitignore`.

## 4. How the Programs Work

Each system-call category performs three main tasks.

### 4.1 Demonstration

- Executes the relevant native operating-system operations.
- Checks return values and handles errors.
- Verifies expected operation results.
- Records successful and failed operations.

### 4.2 Benchmarking

- Measures operation execution times.
- Uses repeated iterations and warm-up runs.
- Reports statistical measurements, including minimum, median, mean, maximum, and standard deviation where implemented.
- Collects additional CPU and memory-related metrics where supported.
- Keeps logging outside timed benchmark operations to reduce measurement interference.

### 4.3 Saving Results

- Stores benchmark measurements in the appropriate CSV file.
- Saves console output in the corresponding category's text file.
- Maintains separate Linux and Windows results.

## 5. Shared Components

The `common/` directory contains reusable components.

| Component | Purpose |
|---|---|
| `logger.hpp` and `logger_linux.hpp` | Logging utilities for operating-system operations |
| `metrics.hpp` and `metrics_linux.hpp` | Performance measurement utilities |
| `kalman_core.hpp` | Shared, operating-system-independent Kalman Filter mathematics |
| `generate_sensor_data.py` | Generates the sensor dataset |
| `verify_part_b.py` | Supports verification of Kalman Filter outputs |
| `generate_charts.py` | Generates project performance and Kalman Filter charts |

The shared Kalman Filter core ensures that both operating-system implementations use the same underlying mathematical model.

## 6. Building and Running

Commands should be executed from the repository root unless otherwise specified.

### 6.1 Prerequisites

**Linux**

- Ubuntu 22.04
- G++ with C++17 support

Install G++ if necessary:

```bash
sudo apt update
sudo apt install g++
```

**Windows**

- Windows 10
- MinGW-w64 G++ compiler with C++17 support
- G++ available through the terminal PATH

### 6.2 Linux — Ubuntu 22.04

Example: compile the Linux File I/O program.

```bash
g++ -std=c++17 Linux/File_IO/File_IO_Linux.cpp -o Linux/File_IO/file_io_linux
```

Run it:

```bash
./Linux/File_IO/file_io_linux
```

The same general compilation approach applies to other Linux categories. Additional source files, compiler options, or libraries may be required depending on the implementation.

### 6.3 Windows — MinGW-w64

Example: compile the Windows File I/O program.

```powershell
g++ -std=c++17 Windows/File_IO/File_IO_Windows.cpp -o Windows/File_IO/file_io_windows.exe -lpsapi -ladvapi32
```

Run it:

```powershell
.\Windows\File_IO\file_io_windows.exe
```

Additional libraries may be required by individual Windows implementations:

- `-lpsapi`: Windows process and memory performance information.
- `-ladvapi32`: Windows security and access-control functionality.

The exact compiler and linker options depend on the source file being built.

**Important:** Compile and execute Windows programs natively on Windows and Linux programs natively on Ubuntu. Do not use WSL or a virtual machine for the native performance comparison.

### 6.4 Output Locations

Example execution logs:

```text
Linux/Permissions/Permissions_Linux.txt
Windows/Permissions/Permissions_Windows.txt
```

Benchmark CSV files:

```text
results/Linux/linux_results.csv
results/Windows/windows_results.csv
```

Run programs from the repository root to ensure that relative paths for logs, transcripts, and result files resolve correctly.

## 7. Part A — System Call and API Implementation

Part A covers five categories on both operating systems:

1. File I/O
2. Memory Management
3. Process Management
4. Inter-Process Communication
5. Permissions

Each category includes demonstrations, error handling, verification, benchmarking, and result logging.

The implementation uses native operating-system interfaces rather than simulated system-call operations.

## 8. Part B — Kalman Filter Integration

Part B applies operating-system facilities to a multi-process Kalman Filter application.

The implementation includes:

1. A shared Kalman Filter mathematical core in `common/kalman_core.hpp`.
2. A common generated sensor dataset used by both operating systems.
3. Native file operations for reading sensor data.
4. Memory-mapped arrays for managing shared data.
5. Distribution of independent sensor tracks among worker processes.
6. Inter-process communication and shared-memory coordination.
7. Native file operations for writing filtered results.
8. Single-process and multi-process execution.
9. Performance measurements using 1, 2, 4, and 8 worker processes.
10. Verification of consistency between single-process and multi-process filtering results.

### 8.1 Dataset

The sensor dataset contains:

- 8 simulated jet tracks.
- 10,000 observations per track.
- 80,000 observations in total.
- A sampling interval of 0.05 seconds (20 Hz).

The same generated dataset is used for both operating-system implementations to support a consistent comparison.

### 8.2 Multi-Process Architecture

Independent tracks are distributed among worker processes.

The worker configurations are:

| Workers | Work distribution |
|---:|---|
| 1 | All eight tracks handled by one worker |
| 2 | Four tracks per worker |
| 4 | Two tracks per worker |
| 8 | One track per worker |

The parent process coordinates execution, shared-memory access, worker synchronization, and output collection.

### 8.3 Kalman Filter Results

The recorded positional RMSE measurements are:

| Measurement | RMSE |
|---|---:|
| Raw sensor measurements | 70.81 m |
| Filtered measurements | 22.95 m |
| RMSE reduction | 67.58% |

The positional RMSE measures the difference between the actual and estimated two-dimensional positions.

### 8.4 Parallel Performance Findings

The multi-process implementation was evaluated with 1, 2, 4, and 8 workers.

The measured results showed that increasing the worker count did not improve execution time in the tested environment. Process creation, synchronization, and coordination overhead outweighed the computational benefit for this workload.

This is an observed performance limitation, not a claim of successful parallel speed-up. The project documents the measured behaviour rather than presenting theoretical speed-up as an achieved result.

## 9. Performance Evaluation

The comparison uses native execution on the same physical machine, with separate operating-system runs.

The evaluation considers:

- System-call/API execution time.
- Minimum, median, mean, maximum, and standard deviation where collected.
- CPU usage and memory-related metrics where supported.
- Kalman Filter execution time.
- Single-process versus multi-process performance.
- Filtering accuracy and output consistency.

### 9.1 Fairness Considerations

To make the comparison meaningful:

- Both operating systems use the same sensor dataset.
- Benchmarks use consistent iteration and warm-up settings wherever applicable.
- Resource-intensive background applications should be minimized.
- Runs should use similar power conditions.
- Repeated measurements are used to observe performance consistency.
- Platform-specific metrics are interpreted with care.

Some metrics are collected through different operating-system mechanisms. For example, CPU and memory measurements may not be directly equivalent between Linux and Windows.

### 9.2 Performance Charts

The generated charts are stored in `results/charts/`.

| Chart | Purpose |
|---|---|
| `trajectory_jet0.png` | Example actual, measured, and filtered trajectory |
| `rmse_per_jet.png` | Filtering accuracy across all eight tracks |
| `syscall_comparison_fileio.png` | Linux and Windows File I/O comparison |
| `syscall_comparison_memory.png` | Linux and Windows memory-operation comparison |
| `syscall_comparison_process.png` | Linux and Windows process-operation comparison |
| `speedup_curve.png` | Single-process versus multi-process execution performance |

The charts present measured results. In particular, speed-up values below 1 indicate that the corresponding multi-process configuration was slower than the single-worker baseline.

## 10. Project Status

### Part A — System Call and API Implementation

- [x] Repository structure
- [x] Linux logging and metrics utilities
- [x] Windows logging and metrics utilities
- [x] Linux File I/O
- [x] Linux Memory Management
- [x] Linux Process Management
- [x] Linux IPC
- [x] Linux Permissions
- [x] Windows File I/O
- [x] Windows Memory Management
- [x] Windows Process Management
- [x] Windows IPC
- [x] Windows Permissions
- [x] Demonstration programs and execution logs
- [x] Benchmark result collection

### Part B — Kalman Filter Integration

- [x] Shared Kalman Filter mathematical implementation
- [x] Sensor data generation
- [x] Linux Kalman Filter implementation
- [x] Windows Kalman Filter implementation
- [x] Native file input and output
- [x] Memory-mapped data management
- [x] Multi-process execution and IPC integration
- [x] 1, 2, 4, and 8 worker configurations
- [x] Filtered output generation
- [x] Filtering accuracy evaluation
- [x] Single-process and multi-process performance measurements
- [x] Performance comparison charts

### Final Documentation and Submission

- [x] Final project report (`report/Final_report_draft.md`)
- [x] Final presentation (`presentation/slides.md`)
- [x] Performance and verification charts
- [x] Native Linux execution
- [x] Native Windows execution

## 11. Conclusion

This project demonstrates how Linux and Windows provide different mechanisms for file operations, process management, memory management, IPC, and permissions.

It also demonstrates how these operating-system facilities can be integrated into a practical Kalman Filter application using memory-mapped data and multiple worker processes.

The performance evaluation highlights that native API behaviour and execution overhead differ between operating systems. It also shows that multi-process execution does not automatically improve performance: the overhead of process management and synchronization can outweigh the benefits for relatively small computational workloads.

The project provides a practical comparison of operating-system facilities through implementation, measurement, and analysis.