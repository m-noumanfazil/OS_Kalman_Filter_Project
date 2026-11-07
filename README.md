# OS Kalman Filter Project

**Course:** CT-353 Operating Systems, Complex Computing Activity (CCA)

## What this project is about

This project shows how **Linux** and **Windows** handle the same basic operating system jobs: working with files, creating processes, managing memory, passing data between processes, and setting file permissions.

The work has two parts:

1. **Part A: System calls.** Implement a set of system calls natively on each OS, check every call for errors, log it, and measure its time, CPU and memory cost.
2. **Part B: Kalman Filter.** Build a Kalman Filter that uses those system calls for reading sensor data, memory-mapped arrays, multi-process parallelism with IPC, and writing output. Then compare how it performs on Linux and Windows.

Linux code is built and run on Ubuntu 22.04. Windows code is built and run on Windows 10 (dual-boot, same machine). All benchmarks run natively, not in a VM or WSL.

## System calls covered

| Category | Linux | Windows |
|---|---|---|
| File I/O | `open()`, `read()`, `write()`, `close()` | `CreateFile()`, `ReadFile()`, `WriteFile()`, `CloseHandle()` |
| Process | `fork()`, `exec()`, `wait()` | `CreateProcess()`, `WaitForSingleObject()` |
| Memory | `mmap()`, `munmap()` | `VirtualAlloc()`, `VirtualFree()` |
| IPC | `pipe()`, `shmget()`, `shmat()` | `CreatePipe()`, `CreateFileMapping()`, `MapViewOfFile()` |
| Permissions | `chmod()`, `chown()`, `umask()` | `SetFileSecurity()` |

Notes on the Windows side:
- Windows has no `fork()`. `CreateProcess()` does the job of `fork()` + `exec()` in one call.
- Windows has no direct equivalent of `umask()` or `chown()`. These are documented as OS differences in the report.
- `UnmapViewOfFile()` is used alongside `MapViewOfFile()` to release mapped memory.

## Repository structure

```
OS_Kalman_Filter_CCP/
├── Linux/
│   ├── Kalman_Filter/
│   ├── File_IO/
│   ├── Process/
│   ├── Memory/
│   ├── IPC/
│   └── Permissions/
├── Windows/
│   └── (same six folders)
├── common/
│   ├── logger.hpp            # logs every system call: success or failure + error code
│   ├── metrics.hpp           # timing, CPU and memory measurement, statistics, CSV output
│   ├── kalman_core.hpp       # Kalman Filter math, no OS-specific code
│   ├── generate_sensor_data.py
│   └── sensor_data.csv
├── results/
│   ├── Linux/
│   ├── Windows/
│   └── charts/
├── report/
├── presentation/
└── README.md
```

Each category folder holds one `.cpp` program (demo + benchmark) and one `.txt` file with its output.

## How each program works

Every category program does three things:

1. **Demo.** Uses its system calls, verifies the result (for example, writes a file, reads it back and compares), and logs each call.
2. **Benchmark.** Times each call separately over 1,000+ runs after a warm-up. No logging inside the timed loop. Reports min, median, mean, max and standard deviation.
3. **Save.** Appends results to a CSV in `results/<OS>/` and writes the console output to the folder's `.txt` file.

### Shared helpers

- **`logger.hpp`** checks the result of each call and records a line like `CreateFile("x.txt") -> FAILED, error 2: The system cannot find the file specified.` to the console and to a log file.
- **`metrics.hpp`** measures per-call time, CPU time and CPU cycles, memory change and page faults. CPU cycles are used because Windows process CPU time only updates every ~15.6 ms, which is too coarse for microsecond-length calls.

## Plan

### Part A: System calls
1. Build the logger and metrics helpers.
2. Implement the five categories, easiest to hardest: File_IO, Memory, Process, IPC, Permissions.
3. For each, demo, verify, benchmark, save results.

### Part B: Kalman Filter
1. Write the Kalman math once in `common/kalman_core.hpp` and test it on its own.
2. Generate one shared sensor data file used by both OSes.
3. Read the data with file system calls.
4. Keep the arrays in memory-mapped memory.
5. Split independent sensor tracks across worker processes that communicate through pipes and shared memory.
6. Write results with file system calls.
7. Time it with 1, 2, 4 and 8 worker processes and verify the parallel output matches the single-process output.

### Order of work
1. Part A on Windows
2. Part B on Windows
3. Switch to Ubuntu
4. Part A on Linux
5. Part B on Linux
6. Compare results, write the report and presentation

## Building and running

### Windows (g++ / MinGW, run from the repository root)

```
g++ -std=c++17 Windows\File_IO\File_IO_Windows.cpp -o Windows\File_IO\file_io.exe -lpsapi
Windows\File_IO\file_io.exe
```

Run from the repository root so that `logs/` and `results/` are created in the right place.

### Linux (Ubuntu 22.04)

To be added when the Linux side is implemented.

## Measuring fairly

- Both OSes read the same sensor data file.
- Close other applications and use mains power during benchmark runs.
- Each benchmark is repeated several times to confirm stable results.
- Per-call CPU numbers include the cleanup call and timer overhead. Timing numbers cover only the call itself.

## Status

- [x] Repository structure
- [x] Windows logger and metrics helpers
- [ ] Part A, Windows: File_IO, Memory, Process, IPC, Permissions
- [ ] Part B, Windows: Kalman Filter
- [ ] Part A, Linux
- [ ] Part B, Linux
- [ ] Linux vs Windows comparison
- [ ] Report
- [ ] Presentation
