# Phase 3: Native multi-process 3D EKF

The 3D prototype has separate native applications for Linux and Windows. Both read the Phase 2 measurement CSV using native file operations, place parsed measurements and output arrays in a shared mapping, and write the resulting CSV using native file operations. The existing 2D filter and sequential 3D driver remain available.

Linux uses `open`/`read`/`write`, a `MAP_SHARED` anonymous mapping, `fork`, pipes, and process waits. Windows uses `CreateFile`/`ReadFile`/`WriteFile`, a named file mapping, `CreateProcess`, events, and process waits. Worker processes persist across 128-record batches. A track is assigned to one worker by track ID modulo worker count, so its filter state persists across batches and observations are processed in input order.

The input accepts up to 100,000 records and 64 MiB. Track IDs must be nonnegative, each track must start with an observation, and timestamps for each track must be strictly increasing. Worker counts 1–8 are supported. Output is created only after successful processing and must not already exist. A write failure can leave a partial newly created output file; discard it before retrying. The applications report errors to standard error and use nonzero exit status on failure.

## Build and verify

From the repository root, with a C++17 compiler and Python 3:

```sh
g++ -std=c++17 -O2 common/run_ekf3d.cpp -o build/run_ekf3d
g++ -std=c++17 -O2 Linux/Kalman_Filter/EKF3D_Linux.cpp -o build/EKF3D_Linux
python tests/check_ekf3d_parallel.py build/run_ekf3d build/EKF3D_Linux
```

On Windows, build `Windows/Kalman_Filter/EKF3D_Windows.cpp` with MinGW-w64 and pass its executable as the second argument. For example, the local Windows checks used `g++ -std=c++17 -O2 -static -static-libgcc -static-libstdc++ Windows/Kalman_Filter/EKF3D_Windows.cpp -o build/EKF3D_Windows.exe`. The test generates its inputs in a temporary directory, compares all exported state and covariance fields against the sequential driver for 1/2/4/8 workers, checks interleaved tracks across batch boundaries and existing-output protection, and exercises a worker computation failure.

To run on a generated scenario directly, invoke the native executable with `<measurements.csv> <new-output.csv> <workers>`. Evaluate the output using `python common/evaluate_3d_scenario.py <scenario-directory> <new-output.csv>`; ground truth is used only by the evaluator.

## Current limitations

- This first integration reports errors to standard error; it does not yet provide the full structured per-system-call logging or benchmark instrumentation of the Phase 1 applications.
- No controlled throughput or speedup claim has been established for the 3D implementation.
- The current test uses three tracks and 1,800 records; broader loads, timeouts, process-start failures, and resource-leak checks remain to be tested.
- The shared arena is fixed-size and is not a streaming design. The measurement parser caps file size to prevent unbounded input memory growth.
- Platform-specific builds are required. Existing Phase 1 caveats on OS benchmarking and remaining cleanup audit still apply.
