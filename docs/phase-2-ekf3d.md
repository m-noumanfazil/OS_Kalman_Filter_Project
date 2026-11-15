# Phase 2: Sequential 3D radar EKF

This is a separate sequential prototype. The existing 2D filter and native multi-process applications remain the Phase 1 baseline; the 3D filter is not yet integrated with their system-call or worker infrastructure.

## Model and scenario

State order is `[x,y,z,vx,vy,vz,ax,ay,az]`: positions in metres, velocities in m/s and accelerations in m/s². The radar is fixed at the Cartesian origin. Measurements are range in metres, azimuth `atan2(y,x)` and elevation `atan2(z,hypot(x,y))`, both in radians. Angles use wrapped residuals; singular geometry near zero horizontal range is rejected.

The motion transition assumes constant acceleration over each observed interval. Process covariance is the integrated continuous white-jerk model, with default spectral density `q=25 m²/s⁵`. Measurement variance defaults to `[2500 m², 10⁻⁶ rad², 10⁻⁶ rad²]`. The first available measurement initializes Cartesian position; initial velocity and acceleration are zero with conservative diagonal covariance. Subsequent missing measurements use prediction only. Timestamps must be finite, nonnegative and strictly increasing within each track. The first record of a track must contain a measurement.

The update uses the analytic radar Jacobian, a Cholesky solve for the innovation covariance, and the Joseph covariance formula. Invalid measurements, non-finite values, invalid time intervals, and singular radar geometry are rejected. The covariance is symmetrized after each step.

The generator creates three tracks with straight, acceleration/climb, and turning-like acceleration segments, irregular time intervals, Gaussian radar noise, and deterministic missing observations. It writes `measurements.csv` and `ground_truth.csv` separately into a **new** directory. Ground truth is only consumed by the evaluator, not the EKF driver.

## Build and run

From the repository root with a C++17 compiler and Python 3 (no additional packages):

```sh
g++ -std=c++17 -O2 common/run_ekf3d.cpp -o build/run_ekf3d
g++ -std=c++17 -O2 tests/test_ekf3d.cpp -o build/test_ekf3d
./build/test_ekf3d
python common/generate_3d_scenario.py --output-dir build/scenario3d --seed 42
./build/run_ekf3d build/scenario3d/measurements.csv build/estimates3d.csv
python common/evaluate_3d_scenario.py build/scenario3d build/estimates3d.csv
```

Create the `build` directory first if needed. On Windows use the corresponding `.exe` paths. The generator refuses an existing output directory and the driver refuses an existing output file. Remove or choose a new location for a repeat run. If the driver encounters an error, it may leave an incomplete output file; discard that file before retrying.

## Comparison and limitations

The evaluator converts the same available noisy range/angle measurements into Cartesian X/Y, then feeds the existing 2D filter; it compares raw XY, 2D baseline XY, and EKF XY on those same rows. It separately reports EKF XYZ error for motion segments and missing-observation rows. Missing observations are prediction-only for the EKF but are excluded from the baseline accuracy comparison; the 2D filter is updated only on available measurements and uses elapsed time between them. The baseline's constant Cartesian noise model is not identical to the range-dependent covariance of converted radar measurements, so this is an illustrative same-observation comparison, not a claim of an optimally tuned baseline.

The driver also outputs the upper triangle of the 3 × 3 position covariance. The evaluator computes position NEES, `errorᵀ P⁻¹ error`, using a Cholesky solve, and reports the fraction inside the chi-square 3-degree-of-freedom 95% ellipsoid (threshold 7.8147), separately for available and missing observations. For a perfectly calibrated Gaussian position estimate, the reference mean NEES is 3 and the nominal coverage is 95%. These time-series observations are correlated, and the simulated trajectories do not exactly follow the stochastic process model; the percentages are descriptive diagnostics, not independent-sample confidence tests or proof of calibration.

One local Windows run with seed 42 yielded raw XY RMSE 52.535 m, 2D baseline XY 24.069 m, 3D EKF XY 19.443 m, and 3D EKF XYZ 20.496 m across 1,677 available observations. These numbers describe this synthetic scenario only.

`tests/test_ekf3d.cpp` checks the analytic Jacobian against finite differences, angle wrapping, singular geometry, prediction through missing observations, timestamp rejection, covariance symmetry and selected quadratic forms over a long sequence. These tests provide evidence, not a proof of positive semidefiniteness for all possible inputs. GitHub Actions also builds the prototype and runs the seeded scenario.

`tests/check_ekf3d_reference.py <native-driver>` compares 120 native updates, including missing measurements, with an independent Python implementation. The reference uses numerical measurement derivatives and pivoted Gaussian elimination instead of the C++ analytic Jacobian and Cholesky solve. It checks all nine state components and the six exported position-covariance entries against documented tolerances. It exercises one scenario rather than proving correctness over all geometries.

`tests/check_ekf3d_multiseed.py <native-driver>` exercises five reproducible noise seeds and prints separate accuracy and calibration diagnostics. This finite sample does not establish performance or calibration for real sensors. Remaining Phase 2 work includes broader adversarial and numeric-stability testing and more independent Monte Carlo trials. Phase 3 will integrate the validated filter with native file I/O, mapped memory, persistent processes, and IPC.
