#include "../common/imm3d.hpp"
#include <cassert>
#include <cmath>
#include <iostream>

int main() {
    imm3d::Filter filter;
    ekf3d::Measurement z{12000., 0.4, 0.15};
    filter.step(0., &z);
    for (int t=1;t<=35;++t) {
        z[0]=12000.+20.*t+2.*t*t;
        filter.step(t, &z);
    }
    // Evidence must affect model weights; normalization alone cannot establish this.
    const auto before=filter.probabilities();
    assert(std::abs(before[0]-0.5)>1e-4);
    filter.step(36., nullptr);
    const auto after=filter.probabilities();
    assert(std::abs(after[0]-(0.97*before[0]+0.03*before[1]))<1e-12);
    assert(std::abs(after[1]-(0.03*before[0]+0.97*before[1]))<1e-12);
    // Probe full fused covariance, including mixed off-diagonal terms, in
    // deterministic directions. This is a useful PSD check, not a proof.
    const auto& covariance=filter.covariance();
    for (int k=0;k<200;++k) {
        double direction[9]{};
        for(int i=0;i<9;++i) direction[i]=std::sin((k+1.)*(i+2.));
        double quadratic=0., scale=0.;
        for(int i=0;i<9;++i) for(int j=0;j<9;++j) {
            double term=direction[i]*covariance[i][j]*direction[j];
            quadratic+=term; scale+=std::abs(term);
        }
        assert(quadratic>=-1e-12*std::max(1.,scale));
    }
    std::cout << "IMM evidence response, missing-data Markov step and covariance probes passed.\n";
}
