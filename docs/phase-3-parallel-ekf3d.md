# Phase 3: Native multi-process 3D EKF

The 3D prototype has separate native applications for Linux and Windows. Both read the Phase 2 measurement CSV using native file operations, place parsed measurements and output arrays in a shared mapping, and write the resulting CSV using native file operations. The existing 2D filter and sequential 3D driver remain available.

Linux uses `open`/`read`/`write`, a `MAP_SHARED` anonymous mapping, `fork`, pipes, and process waits. Windows uses `CreateFile`/`ReadFile`/`WriteFile`, a named file mapping, `CreateProcess`, events, and process waits. Worker processes persist across 128-record batches. A track is assigned to one worker by track ID modulo worker count, so its filter state persists across batches and observations are processed in input order.

The input accepts up to 100,000 records and 64 MiB. Track IDs must be nonnegative, each track must start with an observation, and timestamps for each track must be strictly increasing. Worker counts 1–8 are supported. Output is created only after successful processing and must not already exist. A write failure can leave a partial newly created output file; discard it before retrying. The applications report errors to standard error and use nonzero exit status on failure. Parent-side operating-system operations are logged to `<output.csv>.log`; repeated reads/writes and successful batch synchronization are summarized. The existing logger appends to that path and warns on log-write failure; worker-side operations and every individual synchronization call are not fully logged.

## Build and verify

From the repository root, with a C++17 compiler and Python 3:

```sh
g++ -std=c++17 -O2 common/run_ekf3d.cpp -o build/run_ekf3d
g++ -std=c++17 -O2 Linux/Kalman_Filter/EKF3D_Linux.cpp -o build/EKF3D_Linux
python tests/check_ekf3d_parallel.py build/run_ekf3d build/EKF3D_Linux
```

On Windows, build `Windows/Kalman_Filter/EKF3D_Windows.cpp` with MinGW-w64 and pass its executable as the second argument. For example, the local Windows checks used `g++ -std=c++17 -O2 -static -static-libgcc -static-libstdc++ Windows/Kalman_Filter/EKF3D_Windows.cpp -o build/EKF3D_Windows.exe`. The test generates its inputs in a temporary directory, compares all exported state and covariance fields against the sequential driver for 1/2/4/8 workers, checks interleaved tracks across batch boundaries and existing-output protection, and exercises a worker computation failure.

To run on a generated scenario directly, invoke the native executable with `<measurements.csv> <new-output.csv> <workers>`. Evaluate the output using `python common/evaluate_3d_scenario.py <scenario-directory> <new-output.csv>`; ground truth is used only by the evaluator.

## End-to-end benchmark

`python common/benchmark_ekf3d.py --sequential <sequential-executable> --parallel <native-executable> --input <measurements.csv> --output-dir <new-directory> --build-command "<compiler commands and flags>" --notes "<hardware and power settings>"` produces raw `samples.csv` and `manifest.json`. The destination must not exist. The harness sorts observations by timestamp and track ID, then uses one-quarter, one-half, and all observations with 1/2/4/8 workers. It preserves order within each track and validates every warm-up and measured output against the sequential reference. The manifest records input/executable hashes and the supplied build details.

Timings cover the entire native process: startup, input, parent-side logging, mapping, worker creation, batches, cleanup, and output. They do not isolate steady-state processing, nor measure CPU or memory. The one-worker case is a child process, not the sequential driver. Per-run output and system-call log files are temporary; retain a separate direct application run when detailed call logs are needed. A failed benchmark can leave an output directory; the manifest records `failed` and its samples must not be treated as a successful run. The default is five measured iterations and one warm-up; use more independent runs and record environment details for any reportable result.

## Current limitations

- Parent-side logs summarize repeated operations and can fail independently; full per-call logging and parent/worker resource accounting remain to be added.
- No controlled throughput or speedup claim has been established for the 3D implementation.
- Correctness tests include 1,800 observations across three tracks and an expanded 14,400-observation, 24-track interleaved workload. Timeouts, process-start failures, and resource-leak checks remain to be tested.
- The shared arena is fixed-size and is not a streaming design. The measurement parser caps file size to prevent unbounded input memory growth.
- Platform-specific builds are required. Existing Phase 1 caveats on OS benchmarking and remaining cleanup audit still apply.
