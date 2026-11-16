# IMM multi-seed accuracy comparison

The [manifest](manifest.json) records 20 seed-specific evaluations of the sequential 3D EKF and sequential IMM on identical inputs per seed. Seeds `0..19` change the simulated measurement noise; all runs share the same three deterministic trajectories, maneuver schedule, timestamps, observation gaps, radar noise model and filter parameters. This is a noise-realization sensitivity check for one synthetic scenario, not 20 independent trajectory classes or evidence of real-world performance. The models use different process-noise settings; this is an end-to-end comparison of these configurations, not an isolated test of model count.

The table reports the arithmetic mean of the 20 per-seed Euclidean vector RMSE values, with missing-observation rows included. It is not a pooled RMSE across all rows. Each run's metrics and count, dataset and output hashes, model-probability diagnostics and runtime environment are in the manifest.

| Group | EKF position RMSE (m) | IMM position RMSE (m) | Seeds with lower IMM RMSE |
|---|---:|---:|---:|
| All rows | 20.888 | 16.891 | 20/20 |
| Straight | 22.725 | 19.438 | 20/20 |
| Acceleration | 19.759 | 15.514 | 20/20 |
| Turn | 20.157 | 16.237 | 20/20 |
| Post-turn | 20.461 | 15.740 | 20/20 |
| Missing observations | 23.715 | 19.082 | 20/20 |

The mean per-seed all-row velocity RMSE was 24.655 m/s for EKF and 21.105 m/s for IMM, and acceleration RMSE was 6.245 m/s² and 1.907 m/s² respectively. IMM was lower on all 20 seeds for each. The synthetic trajectories and specific model tuning can favor one configuration; these results do not establish universal superiority.

The mean constant-acceleration probabilities, averaged first within each seed and then across seeds, were 0.358 (straight), 0.360 (acceleration), 0.395 (turn) and 0.375 (post-turn). These averages show model weight changes but do not establish reliable motion classification, nor do segment averages by themselves prove timely transition detection. The per-seed minimum, maximum, mean and sample counts are available in the manifest. The [seed-42 time-series plot](seed42-probabilities.svg) was regenerated from an output whose hash matches the earlier saved seed-42 output.

The source checkout was marked dirty because independent validation scripts were untracked at execution time. The manifest identifies the exact checked-out revision, source-file hashes, both executable hashes, generator and evaluator hashes, and compiler notes. A clean source state or matching hashes do not independently attest how a binary was built.

To reproduce after building both sequential drivers, run from the repository root (use the appropriate executable names and compiler flags for the platform):

```sh
python3 common/compare_imm3d_multiseed.py --ekf build/run_ekf3d --imm build/run_imm3d --output-dir build/imm-multiseed --seeds 0 1 2 3 4 5 6 7 8 9 10 11 12 13 14 15 16 17 18 19 --notes "Record OS, hardware, toolchain and build commands"
```

Only standard Python libraries are used. The destination directory must not already exist. Temporary generated inputs and outputs are removed after evaluation; the manifest retains each evaluator's full metrics and SHA-256 hashes so regenerated artifacts can be checked. The script marks the manifest failed if a subprocess or validation fails.
