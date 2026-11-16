# OS Kalman Filter Project

This CT-353 Operating Systems project demonstrates Linux and Windows operating-system interfaces and uses native processes, shared mappings, synchronization and file I/O to process sensor tracks. It includes a 2D Kalman filter baseline, a nine-state 3D radar extended Kalman filter (EKF), and a sequential interacting multiple-model (IMM) extension.

The current [report](report/final_report.md) and [presentation](presentation/slides.md) describe the evidence and its limits. Earlier reports and baseline results are historical. Linux CI runs on a GitHub-hosted Ubuntu VM; Windows experiments were performed on a different physical machine. These runs do not establish a controlled operating-system comparison or a parallel speedup.

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

## Build and run

Use a C++17 compiler and Python 3. The EKF and IMM tools below use only the Python standard library. Commands run from the repository root and create new output paths; choose unused paths on subsequent runs. `build/` is for local generated files.

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

## Submission evidence and limitations

The [assessment evidence map](docs/assessment-evidence.md) connects the five grading criteria to code, tests and artifacts. Remaining limitations include an unconfirmed Windows interface-count interpretation, incomplete cleanup/logging failure-path audit, no controlled Linux–Windows performance comparison, and no demonstrated parallel speedup for the measured 3D workload. The available Linux CI verifies functional behavior on Ubuntu but is not a same-machine timing comparison with Windows. Avoid presenting old dual-boot environment descriptions as independently verified facts about current measurements.
