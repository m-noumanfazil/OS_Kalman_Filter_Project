#include "../common/imm3d.hpp"
#include <cassert>
#include <cmath>
#include <iostream>

int main() {
    imm3d::Filter f;
    ekf3d::Measurement z{10000, 0.3, 0.1};
    f.step(0, &z);
    assert(f.probabilities()[0] == 0.5);
    f.step(1, nullptr);
    assert(std::abs(f.probabilities()[0] - 0.5) < 1e-12);
    for (int t=2;t<80;++t) {
        z[0]=10000+10*t+(t>40 ? 3*(t-40)*(t-40) : 0);
        f.step(t,&z);
        double sum=f.probabilities()[0]+f.probabilities()[1];
        assert(std::isfinite(sum) && std::abs(sum-1)<1e-12);
        for(double p:f.probabilities()) assert(p>=0 && p<=1);
        for(int i=0;i<9;++i) {
            assert(std::isfinite(f.state()[i]) && f.covariance()[i][i]>=0);
            for(int j=0;j<9;++j) assert(std::abs(f.covariance()[i][j]-f.covariance()[j][i])<1e-8);
        }
    }
    auto previous=f.state();
    try { f.step(79,&z); assert(false); } catch (const std::invalid_argument&) {}
    assert(f.state()==previous);
    // Both likelihoods may be extremely small, but normalization remains finite.
    z[0]=1e7; f.step(80,&z);
    assert(std::isfinite(f.probabilities()[0]) && std::isfinite(f.probabilities()[1]));
    assert(std::abs(f.probabilities()[0]+f.probabilities()[1]-1)<1e-12);
    std::cout << "IMM mixing, missing data, likelihood normalization and rollback passed.\n";
}
