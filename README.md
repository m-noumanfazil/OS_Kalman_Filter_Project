# OS Kalman Filter Project

**Course:** CT-353 Operating Systems — Complex Computing Activity (CCA)

## What This Project Is About

This project compares how **Linux** and **Windows** handle common operating system operations, including file handling, process management, memory management, inter-process communication, and file permissions.

The project has two main parts:

1. **Part A — System Calls:** Implement native system calls on both operating systems, demonstrate their functionality, handle errors, log operations, and measure their execution time and resource usage.
2. **Part B — Kalman Filter:** Implement a Kalman Filter that uses operating system facilities for reading sensor data, memory-mapped arrays, multi-process parallelism, inter-process communication, and writing results. Its performance will be compared across Linux and Windows.

Linux development and testing are performed natively on Ubuntu 22.04. Windows development and testing are performed natively on Windows 10 using a dual-boot setup on the same machine.

All benchmarks are conducted natively, without using a virtual machine or WSL.

## System Calls Covered

| Category | Linux | Windows |
|---|---|---|
| File I/O | `open()`, `read()`, `write()`, `close()` | `CreateFile()`, `ReadFile()`, `WriteFile()`, `CloseHandle()` |
| Process | `fork()`, `exec()`, `wait()` | `CreateProcess()`, `WaitForSingleObject()` |
| Memory | `mmap()`, `munmap()` | `VirtualAlloc()`, `VirtualFree()` |
| IPC | `pipe()`, `shmget()`, `shmat()` | `CreatePipe()`, `CreateFileMapping()`, `MapViewOfFile()` |
| Permissions | `chmod()`, `chown()`, `umask()` | `SetFileSecurity()` |

### Windows and Linux Differences

- Windows does not provide `fork()`. `CreateProcess()` provides process creation and program execution through a different mechanism.
- Windows has no direct equivalent of Linux `umask()` or `chown()`. These differences are documented in the project report.
- Windows uses `UnmapViewOfFile()` to release memory mapped through `MapViewOfFile()`.
- Linux and Windows use different APIs and resource-management mechanisms, so their implementations are native to their respective operating systems.

## Repository Structure

```text
OS_Kalman_Filter_CCP/
│
├── Linux/
│   ├── Kalman_Filter/
│   ├── File_IO/
│   ├── Process/
│   ├── Memory/
│   ├── IPC/
│   └── Permissions/
│
├── Windows/
│   ├── Kalman_Filter/
│   ├── File_IO/
│   ├── Process/
│   ├── Memory/
│   ├── IPC/
│   └── Permissions/
│
├── common/
│   ├── logger.hpp
│   ├── metrics.hpp
│   ├── kalman_core.hpp
│   ├── generate_sensor_data.py
│   └── sensor_data.csv
│
├── results/
│   ├── Linux/
│   ├── Windows/
│   └── charts/
│
├── report/
├── presentation/
└── README.md
```

Each system-call category contains:
- A C++ source file (`.cpp`) containing the demonstration and benchmark implementation.
- A text file (`.txt`) containing the corresponding console output.

Shared helper files are maintained in the `common/` directory.

## How Each Program Works

Each system-call implementation performs three main tasks:

### 1. Demonstration

- Executes the relevant operating system calls.
- Checks their return values and handles errors.
- Verifies that the operations produce the expected results.
- Logs successful and failed operations.

### 2. Benchmarking

- Measures individual system-call execution times.
- Uses repeated iterations with warm-up runs.
- Reports minimum, median, mean, maximum, and standard deviation.
- Collects additional CPU and memory-related metrics where supported.
- Keeps logging outside timed benchmark operations.

### 3. Saving Results

- Appends benchmark statistics to a CSV file under `results/`.
- Saves console output in the corresponding category's `.txt` file.
- Maintains separate results for Linux and Windows.

## Shared Helpers

The `common/` directory contains reusable components:

- **Logger:** Records system-call execution results, including success or failure information and error details.
- **Metrics:** Measures execution time, CPU usage, memory changes, and other performance statistics.
- **Kalman Core:** Contains the shared Kalman Filter mathematical implementation, designed to remain independent of operating-system-specific code.
- **Sensor Data Generator:** Generates sensor data for the Kalman Filter implementation.

## Building and Running

All commands below should be executed from the **repository root directory**.

### Prerequisites

**Linux:**
- Ubuntu 22.04
- G++ compiler with C++17 support

Install the compiler if necessary:

```bash
sudo apt update
sudo apt install g++
```

**Windows:**
- Windows 10
- MinGW-w64 G++ compiler with C++17 support
- G++ available through the terminal PATH

### Linux — Ubuntu 22.04

Compile a category using G++:

```bash
g++ -std=c++17 Linux/File_IO/File_IO_Linux.cpp -o Linux/File_IO/file_io_linux
```

Run the compiled program:

```bash
./Linux/File_IO/file_io_linux
```

The same compilation pattern applies to the other Linux categories. Change the source-file path and output executable path accordingly.

### Windows — MinGW-w64

Compile a category using G++:

```bash
g++ -std=c++17 Windows/File_IO/File_IO_Windows.cpp -o Windows/File_IO/file_io.exe -lpsapi -ladvapi32
```

Run the compiled program from the repository root:

```powershell
.\Windows\File_IO\file_io.exe
```

The same compilation pattern applies to the other Windows categories. Change the source-file path and output executable path accordingly.

The additional libraries are:
- `-lpsapi`: Provides access to Windows process and memory performance information.
- `-ladvapi32`: Provides access to Windows security and access-control APIs.

**Important:** Compile and execute the Windows programs natively on Windows. Similarly, compile and execute Linux programs on Linux.

### Output Locations

Each program saves its console output in its category folder.

For example:

```text
Linux/Permissions/Permissions_Linux.txt
Windows/Permissions/Permissions_Windows.txt
```

Benchmark results are appended to the corresponding CSV files:

```text
results/Linux/linux_results.csv
results/Windows/windows_results.csv
```

Run programs from the repository root to ensure that relative paths for logs, transcripts, and results work correctly.

## Part A — System Calls

Part A implements and benchmarks five system-call categories on both operating systems:

1. File I/O
2. Memory Management
3. Process Management
4. Inter-Process Communication (IPC)
5. Permissions

Each category includes demonstrations, error handling, verification, benchmarking, and result logging.

## Part B — Kalman Filter

Part B will integrate the system-call concepts into a practical Kalman Filter application.

The planned implementation includes:

1. Implementing shared Kalman Filter mathematics in `common/kalman_core.hpp`.
2. Generating a common sensor dataset for both operating systems.
3. Reading sensor data using native file operations.
4. Managing arrays using memory-mapped memory.
5. Distributing independent sensor tracks among worker processes.
6. Using pipes and shared memory for inter-process communication.
7. Writing the filtered results using native file operations.
8. Comparing single-process and multi-process execution.
9. Measuring performance with 1, 2, 4, and 8 worker processes.
10. Verifying that parallel execution produces results consistent with single-process execution.

The implementation will be developed separately for Linux and Windows while maintaining the same underlying Kalman Filter mathematics.

## Measuring Performance Fairly

To make the comparison meaningful:

- Both operating systems will use the same sensor dataset.
- Benchmarks will use consistent iteration and warm-up settings wherever applicable.
- Other resource-intensive applications should be closed during testing.
- Benchmark runs should be conducted under similar power conditions.
- Multiple runs should be performed to observe performance consistency.
- Results will be compared using execution time and available CPU and memory metrics.

Some metrics are platform-specific. For example, CPU-cycle measurements may use different mechanisms on Linux and Windows, so they should not automatically be treated as directly equivalent.

## Project Status

### Part A — System Calls

- [x] Repository structure
- [x] Linux logger and metrics helpers
- [x] Windows logger and metrics helpers
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

### Part B — Kalman Filter

- [ ] Shared Kalman Filter mathematical implementation
- [ ] Sensor data generation
- [ ] Windows Kalman Filter implementation
- [ ] Linux Kalman Filter implementation
- [ ] Multi-process execution and IPC integration
- [ ] Performance comparison

### Final Deliverables

- [ ] Linux vs. Windows performance comparison
- [ ] Project report
- [ ] Project presentation (10–12 slides)