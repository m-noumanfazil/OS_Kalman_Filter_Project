#include "../common/ekf3d.hpp"
#include <algorithm>
#include <cassert>
#include <cmath>
#include <iostream>
#include <limits>

int main() {
    using namespace ekf3d;
    for (State x : {State{1200,-800,500,0,0,0,0,0,0},
                    State{-4500,3000,-2000,0,0,0,0,0,0},
                    State{30,40,900,0,0,0,0,0,0}}) {
        Jacobian h = jacobian(x);
        for (int axis=0; axis<3; ++axis) {
            State plus=x, minus=x; plus[axis]+=0.001; minus[axis]-=0.001;
            auto a=observe(plus), b=observe(minus);
            for(int i=0;i<3;++i) {
                double delta = i ? wrap(a[i]-b[i]) : a[i]-b[i];
                assert(std::abs(delta/0.002-h[i][axis]) < 1e-7);
            }
        }
    }
    assert(std::abs(wrap(-pi+0.01-(pi-0.01))-0.02)<1e-12);
    bool failed=false;
    try { observe(State{}); } catch(const std::domain_error&) { failed=true; }
    assert(failed);
    Filter invalidInit;
    Measurement enormous{1e308, 0, 0};
    failed=false;
    try { invalidInit.step(0, &enormous); } catch(const std::domain_error&) { failed=true; }
    assert(failed && !invalidInit.initialized());
    Filter filter;
    Measurement first{1000, pi-0.001, 0.1};
    filter.step(0, &first);
    State before=filter.state();
    filter.step(0.1, nullptr);
    assert(std::abs(filter.state()[0]-before[0])<1e-9);
    Measurement second{1000, -pi+0.001, 0.1};
    filter.step(0.2, &second);
    assert(std::hypot(filter.state()[0]-before[0],filter.state()[1]-before[1])<20);
    for (int step=3; step<1500; ++step) {
        double t=step*0.1;
        State truth{1000+25*t, 200+5*t, 400+2*t, 0,0,0,0,0,0};
        Measurement measurement=observe(truth);
        filter.step(t, step % 13 == 0 ? nullptr : &measurement);
        const auto& p=filter.covariance();
        for(int i=0;i<9;++i) {
            assert(std::isfinite(filter.state()[i]) && p[i][i]>=0);
            for(int j=0;j<9;++j) assert(std::isfinite(p[i][j]) && std::abs(p[i][j]-p[j][i])<1e-8);
        }
        for (int trial=0; trial<8; ++trial) {
            double v[9], quadratic=0;
            for(int i=0;i<9;++i) v[i]=std::sin((i+1)*(trial+1)*1.37);
            for(int i=0;i<9;++i) for(int j=0;j<9;++j) quadratic+=v[i]*p[i][j]*v[j];
            assert(quadratic >= -1e-7);
        }
    }
    State saved = filter.state();
    failed=false;
    try { filter.step(1.0, nullptr); } catch(const std::invalid_argument&) { failed=true; }
    assert(failed && filter.state() == saved);
    auto savedCovariance = filter.covariance();
    Measurement invalid{1000, std::numeric_limits<double>::quiet_NaN(), 0};
    failed=false;
    try { filter.step(150.0, &invalid); } catch(const std::invalid_argument&) { failed=true; }
    assert(failed && filter.state() == saved && filter.covariance() == savedCovariance);
    failed=false;
    try { filter.step(1e308, nullptr); } catch(const std::domain_error&) { failed=true; }
    assert(failed && filter.state() == saved && filter.covariance() == savedCovariance);
    Filter zeroJerk(0.0);
    zeroJerk.step(0, &first);
    zeroJerk.step(1, nullptr);
    assert(std::isfinite(zeroJerk.covariance()[0][0]));
    std::cout << "3D EKF Jacobian, wrapping, prediction, covariance and timestamp checks passed.\n";
}
