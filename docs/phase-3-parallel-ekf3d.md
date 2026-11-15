# Phase 3: Native multi-process 3D EKF

The 3D prototype has separate native applications for Linux and Windows. Both read the Phase 2 measurement CSV using native file operations, place parsed measurements and output arrays in a shared mapping, and write the resulting CSV using native file operations. The existing 2D filter and sequential 3D driver remain available.

Linux uses `open`/`read`/`write`, a `MAP_SHARED` anonymous mapping, `fork`, pipes, and process waits. Windows uses `CreateFile`/`ReadFile`/`WriteFile`, a named file mapping, `CreateProcess`, events, and process waits. Worker processes persist across 128-record batches. A track is assigned to one worker by track ID modulo worker count, so its filter state persists across batches and observations are processed in input order.

The input accepts up to 100,000 records and 64 MiB. Track IDs must be nonnegative, each track must start with an observation, and timestamps for each track must be strictly increasing. Worker counts 1–8 are supported. Output is created only after successful processing and must not already exist. A write failure can leave a partial newly created output file; discard it before retrying. The applications report errors to standard error and use nonzero exit status on failure. Parent operations are logged to `<output.csv>.log`; each worker writes `<output.csv>.worker-<id>.log`, including its batch synchronization calls. Logs append to their paths and warn if they cannot be written. The logger's own file operations and some setup/cleanup paths are not individually traced.

## Build and verify

From the repository root, with a C++17 compiler and Python 3:

```sh
g++ -std=c++17 -O2 common/run_ekf3d.cpp -o build/run_ekf3d
g++ -std=c++17 -O2 Linux/Kalman_Filter/EKF3D_Linux.cpp -o build/EKF3D_Linux
python tests/check_ekf3d_parallel.py build/run_ekf3d build/EKF3D_Linux
```

On Windows, build `Windows/Kalman_Filter/EKF3D_Windows.cpp` with MinGW-w64 and pass its executable as the second argument. For example, the local Windows checks used `g++ -std=c++17 -O2 -static -static-libgcc -static-libstdc++ Windows/Kalman_Filter/EKF3D_Windows.cpp -o build/EKF3D_Windows.exe -lpsapi`. The test generates its inputs in a temporary directory, compares all exported state and covariance fields against the sequential driver for 1/2/4/8 workers, checks interleaved tracks across batch boundaries and existing-output protection, and exercises a worker computation failure.

To run on a generated scenario directly, invoke the native executable with `<measurements.csv> <new-output.csv> <workers>`. Evaluate the output using `python common/evaluate_3d_scenario.py <scenario-directory> <new-output.csv>`; ground truth is used only by the evaluator.

## End-to-end benchmark

`python common/benchmark_ekf3d.py --sequential <sequential-executable> --parallel <native-executable> --input <measurements.csv> --output-dir <new-directory> --build-command "<compiler commands and flags>" --notes "<hardware and power settings>"` produces raw `samples.csv` and `manifest.json`. The destination must not exist. The harness sorts observations by timestamp and track ID, then uses one-quarter, one-half, and all observations with 1/2/4/8 workers. It preserves order within each track and validates every warm-up and measured output against the sequential reference. The manifest records input/executable hashes and the supplied build details.

Timings cover the entire native process: startup, input, parent-side logging, mapping, worker creation, batches, cleanup, and output. They do not isolate steady-state processing. Each sample records total parent CPU, summed worker CPU, parent peak resident/working-set memory, and the **largest individual worker** peak; these cover process lifetime, with the parent sampled near exit. Memory values must not be summed as unique physical memory because shared mappings can appear in several working sets. Linux uses `getrusage` and Windows uses process times and peak working sets; platform accounting and Windows timer resolution differ, and `-1` means a measurement was unavailable. The one-worker case is a child process, not the sequential driver. Per-run output and system-call log files are temporary; retain a separate direct application run when detailed call logs are needed. A failed benchmark can leave an output directory; the manifest records `failed` and its samples must not be treated as a successful run. The default is five measured iterations and one warm-up; use more independent runs and record environment details for any reportable result.

## Current limitations

- Logging includes per-batch synchronization and affects the measured end-to-end time. It can fail independently of computation, and the audit does not yet cover every setup/cleanup call or the logger's own file operations.
- No controlled throughput or speedup claim has been established for the 3D implementation.
- Correctness tests include 1,800 observations across three tracks and an expanded 14,400-observation, 24-track interleaved workload. Test-instrumented builds simulate a process-start failure and a hung worker, checking that errors propagate, no output is created, and observed worker processes have exited. Successful runs also report parent descriptor/handle counts before and after; the test checks whether the delta increases with worker count. On the local Windows setup, the count increased by four regardless of whether 1, 2, 4, or 8 workers were used. A test-only `-DEKF3D_HANDLE_DIAGNOSTICS` build identified the four additional handles as `EtwRegistration` handles appearing during the first `CreateProcess` call; the returned process and thread handles were separately identified and closed. This is an observation on one Windows installation, not proof that all resources are leak-free or that other systems behave identically. The diagnostic uses Windows native process/object queries and is excluded from normal builds.
- The shared arena is fixed-size and is not a streaming design. The measurement parser caps file size to prevent unbounded input memory growth.
- Platform-specific builds are required. Existing Phase 1 caveats on OS benchmarking and remaining cleanup audit still apply.

## Local Windows example

One non-controlled local run on Windows 11, a Lenovo 21CUS07000 with an AMD Ryzen 5 5600U (6 cores, 12 logical CPUs), approximately 33.1 GB RAM, and the Balanced power plan produced 60 validated end-to-end timings (five samples per configuration after one warm-up). With 1,800 seed-42 observations, median elapsed times were 166.517, 196.271, 280.912, and 542.231 ms for 1, 2, 4, and 8 workers respectively. These include process startup, per-call logging, file I/O, and shutdown; they are not measurements of steady-state filter computation and show slowdown in this one session. The input/binary hashes, complete samples, build command, and notes are stored locally in the ignored `build/experiments/ekf3d-windows-phase3/` directory. Copy the evidence into an agreed shared location before relying on it for submission. No Linux–Windows performance conclusion can be drawn from this run alone.

The Linux GitHub Actions workflow retains a separate one-iteration 3D smoke benchmark artifact for 14 days. Hosted runner performance is not a controlled comparison with the local Windows machine.
