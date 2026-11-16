#ifndef EKF3D_HPP
#define EKF3D_HPP

#include <array>
#include <cmath>
#include <stdexcept>

// Cartesian coordinates in metres; angles are radians. State ordering:
// [x,y,z,vx,vy,vz,ax,ay,az]. Radar is fixed at the origin.
namespace ekf3d {
using State = std::array<double, 9>;
using Covariance = std::array<std::array<double, 9>, 9>;
using Measurement = std::array<double, 3>;
using Jacobian = std::array<std::array<double, 9>, 3>;
constexpr double pi = 3.14159265358979323846;

inline double wrap(double angle) { return std::remainder(angle, 2 * pi); }

inline Measurement observe(const State& x) {
    const double horizontal = std::hypot(x[0], x[1]);
    const double range = std::hypot(horizontal, x[2]);
    if (!std::isfinite(range) || horizontal < 1e-6 || range < 1e-6)
        throw std::domain_error("radar geometry is singular");
    return {range, std::atan2(x[1], x[0]), std::atan2(x[2], horizontal)};
}

inline Jacobian jacobian(const State& x) {
    const double h = std::hypot(x[0], x[1]);
    const double r = std::hypot(h, x[2]);
    if (!std::isfinite(r) || h < 1e-6 || r < 1e-6)
        throw std::domain_error("radar geometry is singular");
    Jacobian j{};
    j[0][0] = x[0] / r; j[0][1] = x[1] / r; j[0][2] = x[2] / r;
    j[1][0] = -x[1] / (h*h); j[1][1] = x[0] / (h*h);
    j[2][0] = -x[0]*x[2] / (r*r*h); j[2][1] = -x[1]*x[2] / (r*r*h); j[2][2] = h / (r*r);
    return j;
}

class Filter {
    State x_{};
    Covariance p_{};
    double last_ = 0;
    bool initialized_ = false;
    double jerk_;
    Measurement variance_;
    bool constant_velocity_;
    double log_likelihood_ = 0.0;
public:
    // jerk spectral density in m^2/s^5; measurement variances in m^2, rad^2, rad^2.
    explicit Filter(double jerk = 25.0, Measurement variance = {2500.0, 1e-6, 1e-6}, bool constant_velocity = false)
        : jerk_(jerk), variance_(variance), constant_velocity_(constant_velocity) {
        if (!std::isfinite(jerk_) || jerk_ < 0) throw std::invalid_argument("invalid jerk noise");
        for (double v : variance_) if (!std::isfinite(v) || v <= 0) throw std::invalid_argument("invalid measurement variance");
    }
    const State& state() const { return x_; }
    const Covariance& covariance() const { return p_; }
    bool initialized() const { return initialized_; }
    double log_likelihood() const { return log_likelihood_; }
    // IMM mixing occurs at the previous timestamp; validation is performed on step.
    void set_mixed(const State& state, const Covariance& covariance) {
        if (!initialized_) throw std::logic_error("cannot mix an uninitialized filter");
        x_ = state; p_ = covariance;
    }

    void step(double timestamp, const Measurement* z) {
        log_likelihood_ = 0.0;
        if (!std::isfinite(timestamp) || timestamp < 0 || (initialized_ && timestamp <= last_))
            throw std::invalid_argument("timestamps must be finite and increasing");
        if (z) {
            for (double v : *z) if (!std::isfinite(v)) throw std::invalid_argument("non-finite measurement");
            if ((*z)[0] <= 1e-6 || std::abs((*z)[2]) >= pi/2) throw std::invalid_argument("invalid radar measurement");
        }
        if (!initialized_) {
            if (!z) throw std::invalid_argument("first observation is required");
            double h = (*z)[0] * std::cos((*z)[2]);
            // Conservative diagonal initialization, including angular uncertainty at range.
            double positionVar = variance_[0] + (*z)[0]*(*z)[0] * (variance_[1] + variance_[2]);
            if (!std::isfinite(h) || h < 1e-6 || !std::isfinite(positionVar))
                throw std::domain_error("invalid initial radar geometry or covariance");
            x_[0] = h * std::cos((*z)[1]); x_[1] = h * std::sin((*z)[1]); x_[2] = (*z)[0] * std::sin((*z)[2]);
            for (int i=0;i<9;++i) p_[i][i] = i < 3 ? positionVar : (i < 6 ? 10000.0 : 400.0);
            last_ = timestamp; initialized_ = true; return;
        }
        double dt = timestamp - last_;
        if (!std::isfinite(dt)) throw std::invalid_argument("invalid time interval");
        Covariance f{};
        for (int i=0;i<9;++i) f[i][i]=1;
        for (int a=0;a<3;++a) {
            f[a][a+3]=dt;
            if (constant_velocity_) f[a+6][a+6]=0;
            else { f[a][a+6]=0.5*dt*dt; f[a+3][a+6]=dt; }
        }
        State predicted{};
        for (int i=0;i<9;++i) for (int j=0;j<9;++j) predicted[i]+=f[i][j]*x_[j];
        Covariance tmp{}, predictedP{};
        for(int i=0;i<9;++i) for(int j=0;j<9;++j) for(int k=0;k<9;++k) tmp[i][j]+=f[i][k]*p_[k][j];
        for(int i=0;i<9;++i) for(int j=0;j<9;++j) for(int k=0;k<9;++k) predictedP[i][j]+=tmp[i][k]*f[j][k];
        // Integrated continuous white jerk covariance for each independent axis.
        const double d2=dt*dt,d3=d2*dt,d4=d3*dt,d5=d4*dt;
        const double q[3][3]={{d5/20,d4/8,d3/6},{d4/8,d3/3,d2/2},{d3/6,d2/2,dt}};
        if (constant_velocity_) {
            // White acceleration spectral density in m^2/s^3; acceleration is a zero-valued placeholder.
            for(int a=0;a<3;++a) {
                predictedP[a][a] += jerk_*d3/3;
                predictedP[a][a+3] += jerk_*d2/2;
                predictedP[a+3][a] += jerk_*d2/2;
                predictedP[a+3][a+3] += jerk_*dt;
                predictedP[a+6][a+6] += 1e-9;
            }
        } else for(int a=0;a<3;++a) for(int i=0;i<3;++i) for(int j=0;j<3;++j) predictedP[a+3*i][a+3*j]+=jerk_*q[i][j];
        if (z) {
            auto expected = observe(predicted); auto h = jacobian(predicted);
            double ph[9][3]{}; double s[3][3]{};
            for(int i=0;i<9;++i) for(int j=0;j<3;++j) for(int k=0;k<9;++k) ph[i][j]+=predictedP[i][k]*h[j][k];
            for(int i=0;i<3;++i) for(int j=0;j<3;++j) { for(int k=0;k<9;++k) s[i][j]+=h[i][k]*ph[k][j]; if(i==j) s[i][j]+=variance_[i]; }
            // Cholesky solve S*K^T = (P*H^T)^T without explicitly inverting S.
            double l[3][3]{};
            for(int i=0;i<3;++i) for(int j=0;j<=i;++j) {
                double v=s[i][j]; for(int k=0;k<j;++k) v-=l[i][k]*l[j][k];
                if(i==j) { if (!(v>0) || !std::isfinite(v)) throw std::domain_error("invalid innovation covariance"); l[i][j]=std::sqrt(v); }
                else l[i][j]=v/l[j][j];
            }
            double gain[9][3]{};
            for(int row=0;row<9;++row) {
                double y[3]{};
                for(int i=0;i<3;++i) { double v=ph[row][i]; for(int k=0;k<i;++k) v-=l[i][k]*y[k]; y[i]=v/l[i][i]; }
                for(int i=2;i>=0;--i) { double v=y[i]; for(int k=i+1;k<3;++k) v-=l[k][i]*gain[row][k]; gain[row][i]=v/l[i][i]; }
            }
            double residual[3]={(*z)[0]-expected[0],wrap((*z)[1]-expected[1]),wrap((*z)[2]-expected[2])};
            double whitened[3]{};
            double logdet = 0, mahalanobis = 0;
            for(int i=0;i<3;++i) {
                double v=residual[i]; for(int k=0;k<i;++k) v-=l[i][k]*whitened[k];
                whitened[i]=v/l[i][i]; mahalanobis+=whitened[i]*whitened[i]; logdet+=2*std::log(l[i][i]);
            }
            log_likelihood_=-0.5*(3*std::log(2*pi)+logdet+mahalanobis);
            if (!std::isfinite(log_likelihood_)) throw std::domain_error("invalid innovation likelihood");
            for(int i=0;i<9;++i) for(int j=0;j<3;++j) predicted[i]+=gain[i][j]*residual[j];
            // Joseph form: (I-KH)P(I-KH)^T + KRK^T.
            Covariance a{}, ap{}, updated{};
            for(int i=0;i<9;++i) for(int j=0;j<9;++j) { a[i][j]=(i==j); for(int k=0;k<3;++k) a[i][j]-=gain[i][k]*h[k][j]; }
            for(int i=0;i<9;++i) for(int j=0;j<9;++j) for(int k=0;k<9;++k) ap[i][j]+=a[i][k]*predictedP[k][j];
            for(int i=0;i<9;++i) for(int j=0;j<9;++j) {
                for(int k=0;k<9;++k) updated[i][j]+=ap[i][k]*a[j][k];
                for(int k=0;k<3;++k) updated[i][j]+=gain[i][k]*variance_[k]*gain[j][k];
            }
            predictedP=updated;
        }
        for(int i=0;i<9;++i) {
            if (!std::isfinite(predicted[i])) throw std::domain_error("non-finite state");
            for(int j=i;j<9;++j) {
                double v=(predictedP[i][j]+predictedP[j][i])*0.5;
                if(!std::isfinite(v) || (i==j && v<0)) throw std::domain_error("invalid covariance");
                predictedP[i][j]=predictedP[j][i]=v;
            }
        }
        x_=predicted; p_=predictedP; last_=timestamp;
    }
};
}
#endif
