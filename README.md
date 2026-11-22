# OS Kalman Filter Project

This CT-353 Operating Systems project demonstrates Linux and Windows operating-system interfaces and uses native processes, shared mappings, synchronization and file I/O to process sensor tracks. It includes a 2D Kalman filter baseline, a nine-state 3D radar extended Kalman filter (EKF), and a sequential interacting multiple-model (IMM) extension.

The assignment has two connected parts:

1. **Part A — System-call and API demonstrations.** Standalone programs exercise file I/O, process management, memory management, inter-process communication (IPC), and permissions on both platforms. They demonstrate the operations, record logs, and collect timing and resource measurements.
2. **Part B — Kalman filter integration.** Native Linux and Windows programs read sensor data, place work and output in mapped memory, distribute independent tracks across processes, synchronize workers, and write filter results. The repository retains the original 2D baseline and adds a 3D radar EKF; a separate sequential IMM explores multiple motion models.

The current [report](report/final_report.md) and [presentation](presentation/slides.md) describe the evidence and its limits. Earlier reports and baseline results are historical. Linux CI runs on a GitHub-hosted Ubuntu VM; Windows experiments were performed on a different physical machine. These runs do not establish a controlled operating-system comparison or a parallel speedup.

## Repository layout

```text
Linux/                    Native Linux programs, organized by OS category
Windows/                  Corresponding native Windows programs
common/                   Shared filter cores, data generation, verification and benchmarking
tests/                    Mathematical, output, worker and failure-path checks
docs/                     Protocols, implementation notes and teammate run guide
results/Linux/            Historical Linux baseline results and filtered output
results/Windows/          Historical Windows baseline results and filtered output
results/phase4/           Saved Windows 3D EKF timing and accuracy evidence
results/phase5/           Saved sequential IMM/EKF comparisons and model probabilities
results/charts/           Historical 2D charts and current Phase 6 figures
report/final_report.md    Current consolidated report
presentation/slides.md    Current 12-slide presentation source
build/                    Locally generated executables and experiment output (ignored by Git)
```

Both platform directories contain `File_IO`, `Process`, `Memory`, `IPC`, `Permissions`, and `Kalman_Filter` categories. The `Kalman_Filter` directories contain both the original 2D application and the newer 3D EKF application.

## Part A — System calls and APIs

Part A implements small, platform-specific demonstrations and benchmarks for five operating-system topics. The demonstrations exercise the interfaces and record successes or failures; the benchmark programs collect timing and, where supported, CPU and memory-related metrics. Exact iterations, warm-ups, cleanup, and measured scope vary by program. For example, the File I/O program demonstrates opening, writing, reading, and closing files and benchmarks several I/O operations separately.

| Category | What the programs demonstrate | Source |
|---|---|---|
| File I/O | Create/open, read, write, close, and handling a missing file | [Linux](Linux/File_IO/), [Windows](Windows/File_IO/) |
| Processes | Start and wait for processes; execute another program on Linux | [Linux](Linux/Process/), [Windows](Windows/Process/) |
| Memory | Allocate or map memory and release it | [Linux](Linux/Memory/), [Windows](Windows/Memory/) |
| IPC | Pipes and shared-memory mechanisms | [Linux](Linux/IPC/), [Windows](Windows/IPC/) |
| Permissions | File permission and access-control operations | [Linux](Linux/Permissions/), [Windows](Windows/Permissions/) |

The category programs write transcripts and logs and append results to the platform CSVs. A run may overwrite a category transcript, append benchmark rows, and create temporary files. Preserve existing evidence or use an isolated copy before running experiments. Some demonstrations depend on the executing user's permissions and environment. Individual setup and cleanup failures have not all been audited, so a log file alone is not proof that every path is handled.

## Required interfaces

The rubric names 15 Linux interfaces and 12 Windows APIs. Supporting calls used by the programs are not represented as additional rubric requirements; instructor clarification of the count is outstanding. The [coverage and evidence inventory](docs/phase-0-baseline.md#requirement-to-source-to-evidence-matrix) links categories to source and saved results. Its Phase 0 findings describe the historical baseline; see [Phase 1 status](docs/phase-1-status.md) for later fixes and open audit work.

| Category | Linux interfaces | Windows APIs |
|---|---|---|
| File I/O | `open`, `read`, `write`, `close` | `CreateFile`, `ReadFile`, `WriteFile`, `CloseHandle` |
| Process | `fork`, `exec` (`execv` in the demonstration), `wait` | `CreateProcess`, `WaitForSingleObject` |
| Memory | `mmap`, `munmap` | `VirtualAlloc`, `VirtualFree` |
| IPC | `pipe`, `shmget`, `shmat` | `CreatePipe`, `CreateFileMapping`, `MapViewOfFile` |
| Permissions | `chmod`, `chown`, `umask` | `SetFileSecurity` |

The interfaces are not all one-to-one equivalents. The category demonstrations are under `Linux/` and `Windows/`. Historical per-call aggregate measurements are under `results/Linux/` and `results/Windows/`; their provenance and measurement scope are qualified in the report. Logger coverage and cleanup on every possible error path have not been established.

For example, Linux `fork` duplicates a process, while Windows `CreateProcess` starts a program using different semantics. Linux permission modes and ownership do not map directly to Windows security descriptors. Windows `VirtualAlloc` demonstrates allocation; its mapped shared-memory facility uses file mapping APIs. Supporting calls such as `UnmapViewOfFile` are needed for cleanup and are distinct from the named rubric entries.

### Building a Part A example

Run commands from the repository root using a C++17 compiler. The following build the File I/O demonstration; the executable also runs benchmarks and writes the files described above.

```sh
# Linux
mkdir -p build
g++ -std=c++17 -O2 Linux/File_IO/File_IO_Linux.cpp -o build/file_io_linux
./build/file_io_linux
```

```powershell
# Windows, using MinGW-w64
New-Item -ItemType Directory -Force build | Out-Null
g++ -std=c++17 -O2 Windows/File_IO/File_IO_Windows.cpp -o build/file_io_windows.exe -lpsapi
.\build\file_io_windows.exe
```

Other categories have their own source files and may need additional platform libraries or permissions; for example, the Windows permissions program uses `-ladvapi32`. Check the comments in each source file and the [coverage inventory](docs/phase-0-baseline.md#requirement-to-source-to-evidence-matrix) for its demonstration and saved evidence. Compile the platform-specific programs for their own operating system.

## Part B — Kalman filter integration

Part B applies the OS facilities to sensor tracking. A parent process reads input through native file operations, uses mapped memory to share records with worker processes, coordinates processing and completion, and writes the resulting estimates. The model calculations are in shared C++ code; OS-specific applications implement file handling, process creation, mapping, and IPC.

```text
sensor measurements → native file input → shared mapped records
                                             ↓
                         persistent workers process assigned tracks
                                             ↓
                     parent waits → native output CSV → verification
```

Workers are assigned entire tracks, so observations for a track remain ordered and its filter state persists between batches. The 3D application supports 1–8 workers and defaults to 128 records per batch. Linux uses a shared `mmap` arena, `fork` and pipes; Windows uses a named file mapping, `CreateProcess` and events. The parent waits for batch completion before reusing the shared data. The [native integration notes](docs/phase-3-parallel-ekf3d.md) cover input limits, logging and failure handling.

### Baseline 2D Kalman filter

The original [shared filter](common/kalman_core.hpp) tracks X and Y independently, with position and velocity as the state for each axis. It uses a constant-velocity transition and continuous white-acceleration process noise. The retained [sensor dataset](common/sensor_data.csv) contains 80,000 observations for eight simulated tracks, with a 0.05-second sampling interval. The platform-specific 2D applications and [persistent-worker benchmark](docs/persistent-benchmark.md) remain available.

The historical saved data gives approximately **70.81 m raw** versus **22.95 m filtered** 2D positional RMSE, a 67.58% reduction on that dataset. These are baseline figures with the provenance limitations described in the [baseline inventory](docs/phase-0-baseline.md); they should not be combined with the newer 3D measurements.

### 3D radar EKF and IMM

The [3D EKF](common/ekf3d.hpp) estimates `[x, y, z, vx, vy, vz, ax, ay, az]` from radar range, azimuth and elevation. It predicts motion using actual time intervals and uses a full covariance matrix, a measurement Jacobian, wrapped angle residuals, a Cholesky solve and a Joseph-form covariance update. Missing observations receive prediction without measurement correction. The seeded example scenario contains three synthetic tracks, irregular intervals, observation gaps and multiple motion segments; measurements and ground truth are stored separately.

The [IMM extension](common/imm3d.hpp) combines constant-velocity and constant-acceleration EKFs by mixing states and covariances, updating model probabilities from measurement likelihoods, and fusing their estimates. It runs through a **sequential driver**; the native multi-process applications currently use the single EKF. The [EKF notes](docs/phase-2-ekf3d.md) and [IMM notes](docs/phase-5-imm.md) document parameters, assumptions and verification.

### Shared tools

| File | Purpose |
|---|---|
| `common/kalman_core.hpp` | Original shared 2D filter |
| `common/ekf3d.hpp`, `common/imm3d.hpp` | Shared 3D EKF and sequential IMM mathematics |
| `common/generate_sensor_data.py`, `common/generate_3d_scenario.py` | Generate the 2D and 3D synthetic datasets |
| `common/run_ekf3d.cpp`, `common/run_imm3d.cpp` | Sequential reference and IMM drivers |
| `common/verify_part_b.py`, `common/evaluate_3d_scenario.py` | Verify 2D native output and evaluate 3D estimates against separate truth |
| `common/benchmark_ekf3d.py`, `common/compare_imm3d.py` | Measure native 3D worker configurations and compare sequential estimators |
| `common/logger*.hpp`, `common/metrics*.hpp` | Platform-specific logging and performance measurement helpers |

## Build and run

Use a C++17 compiler and Python 3. The EKF and IMM tools below use only the Python standard library. Commands run from the repository root and create new output paths; choose unused paths on subsequent runs. `build/` is for local generated files. Teammates collecting and comparing results can follow the [step-by-step run and comparison guide](docs/TEAMMATE_RUN_AND_COMPARISON_GUIDE.md).

### Linux (including Ubuntu)

```sh
mkdir -p build
g++ -std=c++17 -O2 common/run_ekf3d.cpp -o build/run_ekf3d
g++ -std=c++17 -O2 common/run_imm3d.cpp -o build/run_imm3d
g++ -std=c++17 -O2 Linux/Kalman_Filter/EKF3D_Linux.cpp -o build/EKF3D_Linux
python3 -B common/generate_3d_scenario.py --output-dir build/scenario3d --seed 42
build/run_ekf3d build/scenario3d/measurements.csv build/ekf.csv
build/run_imm3d build/scenario3d/measurements.csv build/imm.csv
build/EKF3D_Linux build/scenario3d/measurements.csv build/native-ekf.csv 4 128
python3 -B common/evaluate_3d_scenario.py build/scenario3d build/native-ekf.csv
python3 -B tests/check_ekf3d_parallel.py build/run_ekf3d build/EKF3D_Linux
python3 -B tests/check_imm3d_reference.py build/run_imm3d
python3 -B -m unittest discover -s tests -v
```

### Windows (MinGW-w64)

```powershell
New-Item -ItemType Directory -Force build | Out-Null
g++ -std=c++17 -O2 -static -static-libgcc -static-libstdc++ common/run_ekf3d.cpp -o build/run_ekf3d.exe
g++ -std=c++17 -O2 -static -static-libgcc -static-libstdc++ common/run_imm3d.cpp -o build/run_imm3d.exe
g++ -std=c++17 -O2 -static -static-libgcc -static-libstdc++ Windows/Kalman_Filter/EKF3D_Windows.cpp -o build/EKF3D_Windows.exe -lpsapi
python -B common/generate_3d_scenario.py --output-dir build/scenario3d --seed 42
build/run_ekf3d.exe build/scenario3d/measurements.csv build/ekf.csv
build/run_imm3d.exe build/scenario3d/measurements.csv build/imm.csv
build/EKF3D_Windows.exe build/scenario3d/measurements.csv build/native-ekf.csv 4 128
python -B common/evaluate_3d_scenario.py build/scenario3d build/native-ekf.csv
python -B tests/check_ekf3d_parallel.py build/run_ekf3d.exe build/EKF3D_Windows.exe
python -B tests/check_imm3d_reference.py build/run_imm3d.exe
python -B -m unittest discover -s tests -v
```

The native arguments are input CSV, new output CSV, worker count (1–8), and optional batch size (1–100,000; default 128). Linux uses `mmap`, `fork` and pipes; Windows uses a file mapping, `CreateProcess` and events. Workers own whole tracks and preserve their state across batches. The native applications implement the single 3D EKF; the IMM driver is a separate sequential extension. The input file contains measurements only; evaluation reads separately generated ground truth. For input limits, logging, failure behavior and verification details, see [native integration](docs/phase-3-parallel-ekf3d.md).

## Estimators and evidence

- The [3D EKF](docs/phase-2-ekf3d.md) estimates Cartesian position, velocity and acceleration from range, azimuth and elevation. It uses a constant-acceleration motion model with white-jerk process noise, wrapped angular residuals and a Joseph covariance update.
- The [IMM](docs/phase-5-imm.md) combines constant-velocity and constant-acceleration EKFs using mixed states and covariances and log-domain likelihoods. Its numerical reference and multi-seed comparisons are reproducible using the commands in that document.
- The [Phase 4 report](report/phase4_results.md) links the clean-checkout Windows samples and accuracy evaluation. Its native speedup uses the one-worker **native process** as baseline, not the sequential driver; complete invocation timings include startup, logging and I/O. The measured workload did not show multi-worker speedup.
- The [Phase 5 multi-seed evidence](results/phase5/multiseed-fe04715/README.md) compares the sequential IMM and EKF on the same synthetic trajectories with varying measurement noise. It is not evidence of general or real-world superiority; the model probabilities are diagnostics, not proven maneuver labels.
- The [persistent 2D benchmark protocol](docs/persistent-benchmark.md) and historical `results/charts/` relate to other implementations and should not be mixed with the current 3D results. [Phase 4 measurement instructions](docs/phase-4-measurements.md) describe the 3D workload matrix, raw samples, resource scope and reproducibility.

### Example 3D accuracy results

The seed-42 synthetic scenario contains 1,800 rows across three tracks, including missing-observation rows. The sequential EKF and IMM were evaluated against the same generated measurements and separate ground truth:

| All-row vector RMSE | 3D EKF | Sequential IMM |
|---|---:|---:|
| Position | 20.774 m | 16.392 m |
| Velocity | 25.832 m/s | 22.472 m/s |
| Acceleration | 6.451 m/s² | 1.844 m/s² |

The [saved comparison](results/phase5/windows-0e59a69/) includes individual timing samples and accuracy JSON. A [20-seed comparison](results/phase5/multiseed-fe04715/README.md) varies measurement noise on the same synthetic trajectory family; it does not establish accuracy on other trajectories or real radar data. The models use different process-noise settings, and IMM probabilities are not established maneuver labels.

### Example native 3D performance

For the clean-checkout Windows experiment with all 1,800 seed-42 observations, the median complete native invocation from three measured runs per configuration was:

| Batch size | 1 worker | 2 workers | 4 workers | 8 workers |
|---|---:|---:|---:|---:|
| 32 | 234.933 ms | 320.372 ms | 524.553 ms | 1,194.482 ms |
| 512 | 148.167 ms | 164.849 ms | 207.446 ms | 322.066 ms |

This one Windows session shows slower multi-worker medians than its corresponding one-worker native runs. Timing includes process startup, synchronization, logging, file I/O and shutdown, so it does not isolate the reason for the slowdown. The [Phase 4 report](report/phase4_results.md) links the raw samples, environment, workload hashes and measurement limitations. The [scaling chart](results/charts/phase6/windows_ekf3d_scaling.png) and [accuracy chart](results/charts/phase6/imm_ekf_multiseed_accuracy.png) can be regenerated using the [chart guide](docs/phase-6-charts.md).

### What is verified

- The native 3D EKF checks compare exported state and position-covariance fields against the sequential driver for 1, 2, 4 and 8 workers, including interleaved tracks and batch boundaries.
- Independent numerical references exercise the EKF and IMM; the IMM check covers all 1,800 seed-42 observations, including gaps and model probabilities.
- Failure and output-verification tests cover selected invalid inputs, worker failures, output protection and cleanup behavior. They do not exhaust every system-call failure or prove complete resource cleanup.
- GitHub Actions builds and tests the Linux implementation on an Ubuntu runner. Local Windows runs have exercised the Windows programs; timing on those different environments is not a controlled operating-system comparison.

The [teammate guide](docs/TEAMMATE_RUN_AND_COMPARISON_GUIDE.md) gives commands for running these checks, recording environment details, collecting raw samples and comparing matching configurations.

## Submission evidence and limitations

The [assessment evidence map](docs/assessment-evidence.md) connects the five grading criteria to code, tests and artifacts. Remaining limitations include an unconfirmed Windows interface-count interpretation, incomplete cleanup/logging failure-path audit, no controlled Linux–Windows performance comparison, and no demonstrated parallel speedup for the measured 3D workload. The available Linux CI verifies functional behavior on Ubuntu but is not a same-machine timing comparison with Windows. Avoid presenting old dual-boot environment descriptions as independently verified facts about current measurements.
