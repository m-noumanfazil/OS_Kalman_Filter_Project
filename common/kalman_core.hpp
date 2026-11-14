// =============================================================================
// kalman_core.hpp
//
// Shared, OS-independent 2D Kalman Filter core for supersonic target tracking.
// Part of the CT-353 Operating Systems Complex Computing Activity (Part B).
//
// Pure C++17 standard library only: zero platform-specific dependencies.
// Designed for high-throughput zero-copy execution across worker processes.
// =============================================================================

#ifndef KALMAN_CORE_HPP
#define KALMAN_CORE_HPP

#include <cmath>
#include <cstddef>
#include <vector>

namespace kalman {

// Single radar measurement record parsed from raw sensor telemetry
struct RadarMeasurement {
    double timestamp;
    int    track_id;
    double true_x;
    double true_y;
    double meas_x;
    double meas_y;
    double true_vx;
    double true_vy;
};

// Filtered kinematic output state
struct FilteredOutput {
    double timestamp;
    int    track_id;
    double est_x;
    double est_y;
    double est_vx;
    double est_vy;
};

// 1D Kinematic Constant-Velocity (CV) Kalman Filter with Discrete White Noise Acceleration
class AxisKalmanFilter {
public:
    AxisKalmanFilter(double init_pos = 0.0, double init_vel = 0.0,
                     double process_noise_q = 1500.0, double meas_noise_r = 2500.0)
        : p_(init_pos), v_(init_vel),
          p00_(1000.0), p01_(0.0), p11_(500.0),
          q_(process_noise_q), r_(meas_noise_r),
          initialized_(false) {}

    void init(double initial_pos, double initial_vel = 0.0) {
        p_ = initial_pos;
        v_ = initial_vel;
        p00_ = 1000.0;
        p01_ = 0.0;
        p11_ = 500.0;
        initialized_ = true;
    }

    bool isInitialized() const {
        return initialized_;
    }

    // Predict and update in one optimal step
    void step(double measurement, double dt) {
        if (!initialized_) {
            init(measurement, 0.0);
            return;
        }

        // 1. PREDICT: State extrapolation
        // x_pred = x + v * dt
        // v_pred = v
        double p_pred = p_ + v_ * dt;
        double v_pred = v_;

        // Continuous White Noise Acceleration Q matrix:
        // Q = q * [ dt^3 / 3,  dt^2 / 2 ]
        //         [ dt^2 / 2,  dt      ]
        double dt2 = dt * dt;
        double dt3 = dt2 * dt;
        double q00 = q_ * (dt3 / 3.0);
        double q01 = q_ * (dt2 / 2.0);
        double q11 = q_ * dt;

        // P_pred = F * P * F^T + Q
        double p00_pred = p00_ + 2.0 * dt * p01_ + dt2 * p11_ + q00;
        double p01_pred = p01_ + dt * p11_ + q01;
        double p11_pred = p11_ + q11;

        // 2. UPDATE: Innovation & Kalman Gain
        // S = H * P_pred * H^T + R = p00_pred + r
        double s = p00_pred + r_;
        double inv_s = 1.0 / s;
        double k0 = p00_pred * inv_s;
        double k1 = p01_pred * inv_s;

        // Innovation residual: y = z - H * x_pred
        double y = measurement - p_pred;

        // Posterior state
        p_ = p_pred + k0 * y;
        v_ = v_pred + k1 * y;

        // Posterior covariance: P = (I - K*H) * P_pred
        p00_ = (1.0 - k0) * p00_pred;
        p01_ = (1.0 - k0) * p01_pred;
        p11_ = p11_pred - k1 * p01_pred;
    }

    double position() const { return p_; }
    double velocity() const { return v_; }

private:
    double p_;    // Estimated position
    double v_;    // Estimated velocity
    double p00_;  // Covariance matrix P[0][0]
    double p01_;  // Covariance matrix P[0][1] = P[1][0]
    double p11_;  // Covariance matrix P[1][1]
    double q_;    // Process noise spectral density
    double r_;    // Measurement noise variance (sigma^2)
    bool   initialized_;
};

// 2D Supersonic Fighter Jet Kinematic Kalman Filter
class Jet2DKalmanFilter {
public:
    Jet2DKalmanFilter(double q = 1500.0, double r = 2500.0)
        : filter_x_(0.0, 0.0, q, r),
          filter_y_(0.0, 0.0, q, r),
          last_timestamp_(-1.0) {}

    void processMeasurement(double timestamp, double meas_x, double meas_y) {
        if (last_timestamp_ < 0.0) {
            filter_x_.init(meas_x, 0.0);
            filter_y_.init(meas_y, 0.0);
            last_timestamp_ = timestamp;
            return;
        }

        double dt = timestamp - last_timestamp_;
        if (dt <= 0.0) {
            dt = 0.05; // Default fallback to 20 Hz if timestamps repeat
        }

        filter_x_.step(meas_x, dt);
        filter_y_.step(meas_y, dt);
        last_timestamp_ = timestamp;
    }

    double posX() const { return filter_x_.position(); }
    double posY() const { return filter_y_.position(); }
    double velX() const { return filter_x_.velocity(); }
    double velY() const { return filter_y_.velocity(); }

private:
    AxisKalmanFilter filter_x_;
    AxisKalmanFilter filter_y_;
    double           last_timestamp_;
};

// Evaluation helper: Root Mean Squared Error (RMSE) calculation
struct ErrorMetrics {
    double raw_rmse;      // Raw noisy sensor RMSE vs ground truth
    double filtered_rmse; // Kalman filtered RMSE vs ground truth
    double noise_reduction_pct;
};

inline ErrorMetrics computeRMSE(const std::vector<RadarMeasurement>& inputs,
                                const std::vector<FilteredOutput>& outputs) {
    if (inputs.empty() || inputs.size() != outputs.size()) {
        return {0.0, 0.0, 0.0};
    }

    double sum_raw_sq = 0.0;
    double sum_filt_sq = 0.0;
    size_t count = inputs.size();

    for (size_t i = 0; i < count; ++i) {
        double raw_dx = inputs[i].meas_x - inputs[i].true_x;
        double raw_dy = inputs[i].meas_y - inputs[i].true_y;
        sum_raw_sq += (raw_dx * raw_dx + raw_dy * raw_dy);

        double filt_dx = outputs[i].est_x - inputs[i].true_x;
        double filt_dy = outputs[i].est_y - inputs[i].true_y;
        sum_filt_sq += (filt_dx * filt_dx + filt_dy * filt_dy);
    }

    double raw_rmse = std::sqrt(sum_raw_sq / static_cast<double>(count));
    double filt_rmse = std::sqrt(sum_filt_sq / static_cast<double>(count));
    double reduction = (raw_rmse > 0.0) ? ((raw_rmse - filt_rmse) / raw_rmse) * 100.0 : 0.0;

    return {raw_rmse, filt_rmse, reduction};
}

} // namespace kalman

#endif // KALMAN_CORE_HPP
