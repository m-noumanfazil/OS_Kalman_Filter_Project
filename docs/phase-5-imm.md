# Phase 5: sequential interacting multiple-model extension

The sequential prototype combines a constant-velocity (CV) and a constant-acceleration (CA) radar EKF. Both use the coordinate order `[x,y,z,vx,vy,vz,ax,ay,az]`; the CV acceleration coordinates are zero placeholders after prediction, while the CA acceleration coordinates represent physical acceleration. Mixing uses the identity coordinate mapping, including the between-model covariance term. The CV prediction subsequently projects acceleration to zero, with a small `1e-9` diagonal placeholder variance. This common embedding is a modeling choice, not an assertion that CV estimates target acceleration.

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

The tests cover finite and normalized probabilities, missing measurements, covariance symmetry, invalid timestamp rollback and extremely small likelihoods. The existing EKF still needs its independent reference test whenever the shared implementation changes. Broader multi-seed comparisons, an independent IMM reference, and controlled timing remain necessary before treating a benefit as general.
