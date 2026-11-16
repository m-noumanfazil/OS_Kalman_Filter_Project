# Teammate guide: run, verify and compare the project

This guide lets a teammate run the native 3D EKF on Ubuntu (including an Ubuntu VM) or Windows and share results with the team. The native application runs the single 3D EKF with worker processes; the sequential IMM is a separate advanced estimator. The baseline 2D demonstrations and Part A interface demonstrations remain in the repository; this guide's measurements do not replace per-call Part A evidence.

## 1. Agree on the experiment

Before measuring, agree on the same commit, seed, input, worker counts, batch sizes, warm-ups and measured iterations. The commands below use seed 42, 1 and 3 tracks, batches 32 and 512, 1/2/4/8 workers, one warm-up and five measured invocations per configuration. The benchmark produces 240 measured samples across 48 configurations and checks each output against the sequential EKF. Allow time for it to finish; the 8-worker runs can be much slower.

If teammates use different computers, or one run uses an Ubuntu VM while another runs Windows on physical hardware, the results compare those **complete setups**. CPU, memory, host load, virtualization and OS are all potential influences; the results do not isolate OS effects. A stronger OS comparison needs comparable hardware, equivalent workloads and explicitly documented VM/host conditions; a VM versus a native OS is not an OS-only comparison even on the same physical computer. Avoid other demanding work during timed runs and record the conditions rather than claiming a perfectly controlled environment.

## 2. Get the same source and prerequisites

You need Git, a C++17-capable `g++`, and Python 3. The commands in this guide use Python's standard library; no Python packages are needed. The separate chart generator needs matplotlib only if regenerating charts. If a required tool is missing, arrange its installation according to your machine's setup. If Python packages are needed for other work, use a virtual environment and agree before installing them.

For a new checkout, on either platform:

```text
git clone --branch feature/kalman-improvements https://github.com/m-noumanfazil/OS_Kalman_Filter_Project.git
cd OS_Kalman_Filter_Project
git status --short --branch
git rev-parse HEAD
g++ --version
```

The team must use the **same full commit** from `git rev-parse HEAD`, not merely the same branch name: the branch can advance. If you already have the repository, fetch the branch and check out the agreed commit only after preserving any local work. A clean `git status` is recommended; record and share any changes. Do not commit generated benchmark files to `main`.

### Record the environment

Record the exact OS version, computer/CPU, physical and logical cores, RAM, compiler version and flags, Python version, power plan, date, and any significant background work. For an Ubuntu VM also record the host OS and hardware, VM software/version, allocated vCPUs and RAM, and whether the host was busy. Put a concise version into the `--notes` value in the commands below; retain any longer notes in a text file beside the results.

Ubuntu commands that may help:

```sh
uname -a
lscpu
free -h
python3 --version
```

Windows PowerShell commands that may help:

```powershell
Get-ComputerInfo | Select-Object WindowsProductName,WindowsVersion,OsBuildNumber,CsManufacturer,CsModel,CsTotalPhysicalMemory
Get-CimInstance Win32_Processor | Select-Object Name,NumberOfCores,NumberOfLogicalProcessors
powercfg /getactivescheme
python --version
```

## 3. Ubuntu: build and verify

Run these commands from the repository root in an Ubuntu terminal. The output paths must be new. `mkdir -p build` is safe when the directory already exists.

```sh
mkdir -p build
g++ -std=c++17 -O2 common/run_ekf3d.cpp -o build/run_ekf3d
g++ -std=c++17 -O2 common/run_imm3d.cpp -o build/run_imm3d
g++ -std=c++17 -O2 Linux/Kalman_Filter/EKF3D_Linux.cpp -o build/EKF3D_Linux
python3 -B common/generate_3d_scenario.py --output-dir build/team-scenario --seed 42
python3 -B -m unittest discover -s tests -v
python3 -B tests/check_ekf3d_parallel.py build/run_ekf3d build/EKF3D_Linux
python3 -B tests/check_imm3d_reference.py build/run_imm3d
```

The Python tests report `OK`. The native EKF check reports agreement for 1, 2, 4 and 8 workers and an expanded interleaved workload. The IMM reference reports agreement on 1,800 updates including missing observations. A skipped exact-hash test on a different Python/platform is expected; inspect other skips and failures. If a build or verification fails, stop and share the full error and command instead of using a failed run as evidence.

Run a direct native example and evaluate its output:

```sh
build/EKF3D_Linux build/team-scenario/measurements.csv build/team-linux-output.csv 4 128
python3 -B common/evaluate_3d_scenario.py build/team-scenario build/team-linux-output.csv --json-output build/team-linux-accuracy.json
```

The last two native arguments are worker count and batch size. Output CSV and JSON must not already exist. Logs are written beside the native output. Ground truth is used by the evaluator, not the filter.

## 4. Windows: build and verify

Run in PowerShell from the repository root, with MinGW-w64 `g++` available on `PATH`:

```powershell
New-Item -ItemType Directory -Force build | Out-Null
g++ -std=c++17 -O2 -static -static-libgcc -static-libstdc++ common/run_ekf3d.cpp -o build/run_ekf3d.exe
g++ -std=c++17 -O2 -static -static-libgcc -static-libstdc++ common/run_imm3d.cpp -o build/run_imm3d.exe
g++ -std=c++17 -O2 -static -static-libgcc -static-libstdc++ Windows/Kalman_Filter/EKF3D_Windows.cpp -o build/EKF3D_Windows.exe -lpsapi
python -B common/generate_3d_scenario.py --output-dir build/team-scenario --seed 42
python -B -m unittest discover -s tests -v
python -B tests/check_ekf3d_parallel.py build/run_ekf3d.exe build/EKF3D_Windows.exe
python -B tests/check_imm3d_reference.py build/run_imm3d.exe
```

The expected checks and failure handling are the same as in the Ubuntu section. If `python` is unavailable but the Windows Python launcher is configured, use `py -3` instead.

```powershell
build/EKF3D_Windows.exe build/team-scenario/measurements.csv build/team-windows-output.csv 4 128
python -B common/evaluate_3d_scenario.py build/team-scenario build/team-windows-output.csv --json-output build/team-windows-accuracy.json
```

These outputs also require new paths. Keep the native output, its logs and accuracy JSON as functional verification evidence.

## 5. Make sure both teams have identical input

Using the same seed is a starting point; library and platform differences can affect generated floating-point data. Compare the SHA-256 hashes of both `measurements.csv` and `ground_truth.csv` before comparing results:

```sh
# Ubuntu
sha256sum build/team-scenario/measurements.csv build/team-scenario/ground_truth.csv
```

```powershell
# Windows PowerShell
Get-FileHash build/team-scenario/measurements.csv, build/team-scenario/ground_truth.csv -Algorithm SHA256
```

If hashes differ, choose one generated scenario as the shared input and copy **both files without editing** to a new scenario directory on the other machine. Recheck both hashes. Use that directory for the following commands and rerun the direct verification if needed. Keep ground truth out of the estimator input. The benchmark manifest also records an input hash and individual workload hashes; compare those, too. Executable hashes will normally differ across OS builds.

## 6. Collect the native benchmark

Run only after verification succeeds. Edit `--notes` to include your actual hardware, OS, compiler version, power and VM information. The benchmark destination must not already exist. It contains all workload sizes; the full three-track input has 1,800 rows.

Ubuntu:

```sh
python3 -B common/benchmark_ekf3d.py --sequential build/run_ekf3d --parallel build/EKF3D_Linux --input build/team-scenario/measurements.csv --output-dir build/team-benchmark-linux --build-command 'g++ -std=c++17 -O2 common/run_ekf3d.cpp -o build/run_ekf3d; g++ -std=c++17 -O2 Linux/Kalman_Filter/EKF3D_Linux.cpp -o build/EKF3D_Linux' --notes 'REPLACE with OS, CPU, RAM, VM host and allocation, power and background conditions' --track-counts 1 3 --batch-sizes 32 512 --iterations 5 --warmups 1
```

Windows PowerShell:

```powershell
python -B common/benchmark_ekf3d.py --sequential build/run_ekf3d.exe --parallel build/EKF3D_Windows.exe --input build/team-scenario/measurements.csv --output-dir build/team-benchmark-windows --build-command 'g++ -std=c++17 -O2 -static -static-libgcc -static-libstdc++ common/run_ekf3d.cpp -o build/run_ekf3d.exe; g++ -std=c++17 -O2 -static -static-libgcc -static-libstdc++ Windows/Kalman_Filter/EKF3D_Windows.cpp -o build/EKF3D_Windows.exe -lpsapi' --notes 'REPLACE with OS, CPU, RAM, power and background conditions' --track-counts 1 3 --batch-sizes 32 512 --iterations 5 --warmups 1
```

A successful command reports 240 validated end-to-end timings and writes:

- `manifest.json`: completion status, source state, input and executable hashes, build commands, environment notes and workload hashes.
- `samples.csv`: each measured invocation, elapsed time, internal stages, CPU and memory values.
- `summary.csv`: per-configuration medians, variability and speedup against the matching **one-worker native process**.

Inspect `manifest.json`: `status` must be `complete`, the commit and input hashes must match the agreed source and input, and each matching workload must have the same hash. A failed run can leave a directory and partial data; use a new directory for a retry. The benchmark's individual output files and logs are temporary, so keep the separate direct native output and logs from sections 3–4 if needed for review.

To view the full-input results, open `summary.csv` in a spreadsheet and filter `records = 1800`, `requested_tracks = 3`, then group by `batch_size` and `workers`. Compare the same row in each machine's file. For example, compare batch 512 / 4 workers with batch 512 / 4 workers, not with a different batch or input size. Also inspect the five individual samples and their range; a median alone hides variability.

Within each machine, `speedup_vs_one_worker` is the one-worker native median divided by the matching multi-worker native median. Below 1 means slowdown. This number does **not** compare against the direct sequential driver, which the benchmark runs for correctness but does not time. `elapsed_ms` includes startup, I/O, mapping, logging, processing and shutdown. Launch is process creation, not confirmed readiness; later-batch times include synchronization and logging. CPU is parent and summed worker lifetime CPU; memory records the parent peak and largest individual worker peak. Shared pages can appear in multiple processes, and platform accounting differs; do not add working sets as unique physical memory. `-1` means unavailable. See the [measurement protocol](phase-4-measurements.md) for details.

Repeat the complete experiment in separately named directories on different occasions if feasible. Record the repetitions individually; do not overwrite or cherry-pick a favorable run. The timing samples can support conclusions about the measured setup and workload, but not an OS-only cause when environments differ.

## 7. Optional: compare sequential IMM and EKF

On the **same machine** and the same scenario, run the following after building both sequential drivers. The tool performs a warm-up and five measured runs for each, alternates order, saves accuracy and timing data, and generates an SVG probability plot. Replace the notes with real build and environment details.

Ubuntu:

```sh
python3 -B common/compare_imm3d.py --scenario build/team-scenario --ekf build/run_ekf3d --imm build/run_imm3d --output-dir build/team-imm-comparison --iterations 5 --notes 'REPLACE with compiler, OS, CPU and run conditions'
```

Windows:

```powershell
python -B common/compare_imm3d.py --scenario build/team-scenario --ekf build/run_ekf3d.exe --imm build/run_imm3d.exe --output-dir build/team-imm-comparison --iterations 5 --notes 'REPLACE with compiler, OS, CPU and run conditions'
```

Compare `ekf-accuracy.json` and `imm-accuracy.json` for state and segment RMSE, and `manifest.json` for individual timings and medians. Timings include executable startup and CSV I/O. Both estimators have different process-noise settings; a result on this seeded synthetic scenario is not proof of general superiority. Probability weights are not proven maneuver labels. See [IMM documentation](phase-5-imm.md) for model details and multi-seed experiments.

## 8. What to share with the team

Share the complete benchmark output directory, environment notes, `git rev-parse HEAD`, both input file hashes, the direct native output and its logs, and the accuracy JSON. Include the optional IMM output directory if run. Do not share only a screenshot or summary; the manifest and raw samples are necessary to check the result. `build/` is locally ignored, so copy or archive these artifacts for your teammates through your agreed channel. Review any paths or machine details in logs and notes before sharing.

The [current report](../report/final_report.md), [presentation source](../presentation/slides.md), and [assessment evidence map](assessment-evidence.md) identify existing results and remaining limitations. New results should be reviewed with their conditions before updating those artifacts or merging the feature branch.
