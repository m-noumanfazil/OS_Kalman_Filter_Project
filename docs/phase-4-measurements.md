# Phase 4: 3D performance and accuracy evaluation

Phase 4 evaluates the 3D EKF without changing its mathematics. The earlier 2D results are historical and use a different application and benchmark protocol. The Linux GitHub runner and local Windows machine are different environments; their timing results must not be presented as a controlled OS comparison.

## Workload matrix

Generate a fresh seeded scenario and run `common/benchmark_ekf3d.py` with the sequential and platform-native executables. Its `--track-counts` option selects a prefix of sorted track IDs, `--batch-sizes` selects native batch sizes, and it tests 1/2/4/8 workers. For each selected track count, one-quarter, one-half, and all of the selected observations are used. Rows are sorted by timestamp and track ID; order within each track is preserved. For a given workload and batch size, every worker count receives exactly the same observations and every output, including warm-ups, is checked against the sequential reference.

Example after building the executables and generating `build/scenario3d`:

```sh
python common/benchmark_ekf3d.py --sequential build/run_ekf3d --parallel build/EKF3D_Linux --input build/scenario3d/measurements.csv --output-dir build/phase4-linux --build-command "g++ -std=c++17 -O2 ..." --track-counts 1 3 --batch-sizes 32 128 512 --iterations 5 --warmups 1 --notes "record OS, CPU, RAM, VM and power settings here"
```

Use the exact compiler commands and executable paths for the actual run, including `.exe` and `-lpsapi` on Windows. The output directory must be new. `samples.csv` contains individual timings, CPU and memory values; `summary.csv` derives statistics and median speedup relative to the **one-worker native process** for the same workload and batch size. `manifest.json` records executable/input hashes, selection parameters, hashes of each generated workload, platform details and supplied build/environment notes. A manifest marked `failed` is not a successful experiment.

## Timing and resource interpretation

- `elapsed_ms` measures a complete native invocation: startup, input, mapping, worker launch, processing, logging, shutdown and output.
- `launch_us` covers process creation calls, not proof of worker readiness. `first_batch_us` includes the initial dispatch and any remaining worker startup. `remaining_batches_us` is the total after the first batch, and `remaining_batches` is its count. Divide the former by the latter for the mean later-batch latency; it includes IPC, scheduling and logging, and is not isolated filter compute time.
- `shutdown_us` measures stopping and joining workers, including associated logging and, on Windows, worker resource queries. The internal stages do not sum to end-to-end time because input, mapping, formatting and output are outside them.
- CPU values cover parent and summed workers over their process lifetimes. Memory values are the parent's peak and the largest individual worker peak. Shared mappings can be counted by several processes, so do not sum working sets as unique physical memory. Platform accounting differs; `-1` denotes unavailable data.
- The benchmark runs the sequential driver only for validation; it does not time the sequential driver. Speedup versus one worker is a native multi-process comparison, not a speedup against a direct sequential execution.

## Accuracy

`common/evaluate_3d_scenario.py` reads the generated ground truth only after estimation. It reports whole-run position, velocity, and acceleration vector RMSE, as well as the same state errors for motion segments and missing-observation rows. The 2D baseline comparison is limited to XY on common available observations, and the 3D position NEES and nominal 95% ellipsoid coverage are descriptive diagnostics. Their mathematical assumptions and correlation caveats are documented in [phase-2-ekf3d.md](phase-2-ekf3d.md).

## Evidence status

Windows smoke checks have validated the matrix and output parity. The Linux CI workflow exercises the same matrix, but CI timings are functional checks rather than controlled performance evidence. Preserve independent experiment output directories and record conditions before making conclusions; do not infer speedup merely because workers run concurrently. Full report reconciliation and comparable-environment measurements remain open.

### Initial local Windows run

One local Windows 11 run on a Lenovo 21CUS07000 with an AMD Ryzen 5 5600U (6 cores / 12 logical CPUs), 33,099,800,576 bytes RAM and the Balanced power plan used g++ 15.1.0, seed 42, three measured iterations and one warm-up per configuration. The 144 samples and their manifests and summaries are stored locally under the ignored `build/experiments/ekf3d-phase4-windows-v1/`; share or archive them separately before using them as submission evidence. This was one session, and the source checkout had uncommitted Phase 4 changes at execution time; executable and input hashes are recorded but source-to-binary provenance is not independently attested.

For the full three-track, 1,800-observation workload, the end-to-end median times (ms) were:

| Batch size | 1 worker | 2 workers | 4 workers | 8 workers |
|---|---:|---:|---:|---:|
| 32 | 236.19 | 330.82 | 529.66 | 1,399.41 |
| 512 | 172.30 | 175.29 | 208.47 | 344.39 |

This run shows slowdown as worker count increases, and lower elapsed times for the larger batches in the listed configurations. It includes detailed logging and process startup, uses only three timing samples per configuration, and cannot isolate the cause or support a general conclusion about other machines or workloads.

For the same seeded scenario, the sequential evaluator measured all-row 3D vector RMSE of 20.774 m for position, 25.832 m/s for velocity, and 6.451 m/s² for acceleration. These include missing-observation rows. They describe synthetic ground truth, not measured real-world radar performance.
