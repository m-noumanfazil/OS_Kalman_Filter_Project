# Phase 5: sequential interacting multiple-model extension

The sequential prototype combines a constant-velocity (CV) and a constant-acceleration (CA) radar EKF. Both use the coordinate order `[x,y,z,vx,vy,vz,ax,ay,az]`; the CV acceleration coordinates are zero placeholders after prediction, while the CA acceleration coordinates represent physical acceleration. Both models initially assign acceleration variance 400 m²/s⁴. Mixing uses the identity coordinate mapping, including the between-model covariance term. The CV prediction subsequently projects acceleration to zero, with a small `1e-9` diagonal placeholder variance. This common embedding is a modeling choice, not an assertion that CV estimates target acceleration.

At each observation, the fixed row-stochastic model transition matrix is `[[0.97,0.03],[0.03,0.97]]`, applied once per observation rather than per second. Model-conditioned states and covariances are mixed using the transition-weighted prior, then predicted and updated with the same radar observations. Gaussian innovation likelihoods use the Cholesky factor and wrapped angular residuals; probabilities are normalized in the log domain. Missing observations perform prediction and transition-probability updates without a measurement likelihood. The fused covariance includes uncertainty between model means. Initialization gives each model probability 0.5.

The CV process uses integrated white acceleration with spectral density 4 m²/s³; the CA process retains the existing white-jerk spectral density 25 m²/s⁵. Both retain the existing radar noise and initialization. These parameters and transition rates are illustrative rather than optimized. The filter is sequential; it is not currently integrated into the native persistent-worker programs, which continue to use the single EKF.

Build and compare using a fresh scenario and output directory:

```sh
g++ -std=c++17 -O2 common/run_ekf3d.cpp -o build/run_ekf3d
g++ -std=c++17 -O2 common/run_imm3d.cpp -o build/run_imm3d
g++ -std=c++17 -O2 tests/test_imm3d.cpp -o build/test_imm3d
build/test_imm3d
python3 common/generate_3d_scenario.py --output-dir build/imm-scenario --seed 42
python3 common/compare_imm3d.py --scenario build/imm-scenario --ekf build/run_ekf3d --imm build/run_imm3d --output-dir build/imm-comparison --notes "Record OS, CPU, compiler and flags"
```

On Windows, use `.exe` paths and the local compiler's required flags. The comparison performs one warm-up per model and alternates execution order, saves full sequential process timings including file I/O, evaluates both outputs against the same separate truth, records executable and input hashes, and generates a dependency-free SVG model-probability plot. A single short run is not a stable measurement of compute cost. The output files are new and never overwrite an existing experiment directory.

The tests cover finite and normalized probabilities, evidence-driven probability changes, the transition-only update during missing observations, covariance symmetry and quadratic-form probes, invalid timestamp rollback and extremely small likelihoods. An independent pure-Python reference uses finite-difference radar derivatives, pivoted solves and a separate innovation determinant calculation; it checks all nine fused state components, six position covariance entries and both probabilities on 1,800 seed-42 updates, including 123 missing observations. This numerical cross-check and the covariance probes are not proofs of correctness for all inputs. Extreme finite innovations whose squared norm overflows are rejected. The existing EKF also retains its independent reference test.

## Initial comparison

The [saved experiment](../results/phase5/windows-0e59a69/) used the seed-42 scenario, source commit `0e59a69`, and a Windows 11 Ryzen 5 5600U laptop. It contains the manifest with individual process timings and executable/input hashes, machine-readable accuracy metrics, compressed first measured output for each model, and a [model probability plot](../results/phase5/windows-0e59a69/model-probabilities.svg). Executables were compiled from the same source before the checkpoint; hashes and source state do not independently attest the build. All figures below are from one synthetic scenario and one timing session.

| Measure | Single EKF | IMM |
|---|---:|---:|
| All-row position vector RMSE (m) | 20.774 | 16.392 |
| All-row velocity vector RMSE (m/s) | 25.832 | 22.472 |
| All-row acceleration vector RMSE (m/s²) | 6.451 | 1.844 |
| Straight position RMSE (m) | 21.631 | 18.143 |
| Acceleration-segment position RMSE (m) | 18.876 | 14.019 |
| Turn-segment position RMSE (m) | 19.833 | 15.476 |
| Missing-observation position RMSE (m) | 24.252 | 18.353 |
| Median complete sequential invocation (ms) | 90.230 | 103.952 |

The median uses five measured invocations after one warm-up per model, includes process startup and CSV I/O, and is not an isolated estimate of filter computation. The IMM is slower and has lower error in this particular run; neither result establishes a general tradeoff. The fixed transition matrix depends on observation frequency and the comparison uses differently tuned model noise. In evaluator JSON, fields named `ekf_xy_m` and `ekf_xyz_m` describe the supplied estimator output, which may be an IMM output.

## Multiple seeded noise realizations

The [multi-seed evidence](../results/phase5/multiseed-fe04715/README.md) repeats the comparison for seeds 0–19. It varies measurement noise while retaining the same synthetic trajectories, maneuver schedule and observation gaps. The mean of the per-seed all-row position RMSEs is 20.888 m for the single EKF and 16.891 m for the IMM; the IMM is lower in all 20 runs for this scenario family. Full per-seed results and segment-level comparisons are in the manifest. Mean CA probabilities differ modestly by segment; they do not establish reliable maneuver classification. Results depend on this trajectory family and the chosen model parameters and do not demonstrate general or real-world superiority. Comparable hardware and more independent timing sessions would be needed for a general compute-cost conclusion.
