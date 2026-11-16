#ifndef IMM3D_HPP
#define IMM3D_HPP

#include "ekf3d.hpp"
#include <algorithm>

namespace imm3d {
// Both hypotheses use the same nine coordinates. In the CV hypothesis the
// acceleration coordinates are zero placeholders after prediction; the CA
// hypothesis treats them as physical acceleration. Mixing is therefore an
// identity coordinate conversion followed by each model's own transition.
class Filter {
    std::array<ekf3d::Filter, 2> models_{{ekf3d::Filter(4.0, {2500., 1e-6, 1e-6}, true), ekf3d::Filter()}};
    std::array<double, 2> probabilities_{{0.5, 0.5}};
    ekf3d::State state_{};
    ekf3d::Covariance covariance_{};
    bool initialized_ = false;
    static constexpr double transition_[2][2] = {{0.97, 0.03}, {0.03, 0.97}};

    void combine() {
        state_ = {}; covariance_ = {};
        for (int m=0;m<2;++m) for (int i=0;i<9;++i) state_[i] += probabilities_[m]*models_[m].state()[i];
        for (int m=0;m<2;++m) for (int i=0;i<9;++i) for (int j=0;j<9;++j) {
            const auto& x=models_[m].state();
            covariance_[i][j] += probabilities_[m]*(models_[m].covariance()[i][j]+(x[i]-state_[i])*(x[j]-state_[j]));
        }
    }
public:
    const ekf3d::State& state() const { return state_; }
    const ekf3d::Covariance& covariance() const { return covariance_; }
    const std::array<double, 2>& probabilities() const { return probabilities_; }
    void step(double timestamp, const ekf3d::Measurement* z) {
        // Work on a copy so invalid input cannot leave one model advanced.
        Filter next=*this;
        if (!initialized_) {
            for (auto& model : next.models_) model.step(timestamp,z);
            next.initialized_=true;
        } else {
            double prior[2]{};
            for(int j=0;j<2;++j) for(int i=0;i<2;++i) prior[j]+=probabilities_[i]*transition_[i][j];
            for(int j=0;j<2;++j) {
                ekf3d::State mixed{}; ekf3d::Covariance mixed_p{};
                for(int i=0;i<2;++i) {
                    const double weight=probabilities_[i]*transition_[i][j]/prior[j];
                    for(int a=0;a<9;++a) mixed[a]+=weight*models_[i].state()[a];
                }
                for(int i=0;i<2;++i) {
                    const double weight=probabilities_[i]*transition_[i][j]/prior[j];
                    const auto& x=models_[i].state();
                    for(int a=0;a<9;++a) for(int b=0;b<9;++b)
                        mixed_p[a][b]+=weight*(models_[i].covariance()[a][b]+(x[a]-mixed[a])*(x[b]-mixed[b]));
                }
                next.models_[j].set_mixed(mixed,mixed_p);
                next.models_[j].step(timestamp,z);
            }
            double logp[2];
            for(int j=0;j<2;++j) logp[j]=std::log(prior[j])+next.models_[j].log_likelihood();
            const double maximum=std::max(logp[0],logp[1]);
            double weights[2]={std::exp(logp[0]-maximum),std::exp(logp[1]-maximum)};
            const double total=weights[0]+weights[1];
            for(int j=0;j<2;++j) next.probabilities_[j]=weights[j]/total;
        }
        next.combine();
        *this=next;
    }
};
}
#endif
