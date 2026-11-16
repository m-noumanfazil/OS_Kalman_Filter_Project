"""Independent pure-Python IMM reference using finite differences and pivoted solves.

Compares fused state, position covariance and both posterior model probabilities
against the native executable; no C++ filtering or Jacobian code is reused.
"""
import csv
import itertools
import math
from pathlib import Path
import subprocess
import sys
import tempfile

from check_ekf3d_reference import multiply, transpose, solve, radar, wrapped

ROOT = Path(__file__).resolve().parents[1]
VARIANCE = (2500.0, 1e-6, 1e-6)
TRANSITION = ((0.97, 0.03), (0.03, 0.97))
STATE_NAMES = ('x', 'y', 'z', 'vx', 'vy', 'vz', 'ax', 'ay', 'az')


def determinant(a):
    # Formula for a 3 by 3 determinant avoids the native Cholesky factor.
    return (a[0][0] * (a[1][1]*a[2][2]-a[1][2]*a[2][1])
            - a[0][1] * (a[1][0]*a[2][2]-a[1][2]*a[2][0])
            + a[0][2] * (a[1][0]*a[2][1]-a[1][1]*a[2][0]))


def update_model(x, p, dt, z, cv):
    f = [[float(i == j) for j in range(9)] for i in range(9)]
    for a in range(3):
        f[a][a+3] = dt
        if cv:
            f[a+6][a+6] = 0.0
        else:
            f[a][a+6], f[a+3][a+6] = 0.5*dt**2, dt
    x = [sum(f[i][j]*x[j] for j in range(9)) for i in range(9)]
    p = multiply(multiply(f, p), transpose(f))
    for a in range(3):
        if cv:
            p[a][a] += 4*dt**3/3
            p[a][a+3] += 4*dt**2/2
            p[a+3][a] += 4*dt**2/2
            p[a+3][a+3] += 4*dt
            p[a+6][a+6] += 1e-9
        else:
            q = ((dt**5/20, dt**4/8, dt**3/6),
                 (dt**4/8, dt**3/3, dt**2/2),
                 (dt**3/6, dt**2/2, dt))
            for i in range(3):
                for j in range(3):
                    p[a+3*i][a+3*j] += 25*q[i][j]
    if z is None:
        return x, p, 0.0
    expected = radar(x)
    h = [[0.0]*9 for _ in range(3)]
    for a in range(3):
        plus, minus = x[:], x[:]
        plus[a] += 0.001
        minus[a] -= 0.001
        high, low = radar(plus), radar(minus)
        for i in range(3):
            delta = high[i]-low[i]
            h[i][a] = (wrapped(delta) if i else delta)/0.002
    ph = multiply(p, transpose(h))
    s = multiply(h, ph)
    for i in range(3):
        s[i][i] += VARIANCE[i]
    innovation = [z[0]-expected[0], wrapped(z[1]-expected[1]), wrapped(z[2]-expected[2])]
    solution = solve(s, innovation)
    d = determinant(s)
    if d <= 0:
        raise AssertionError('invalid innovation determinant')
    log_likelihood = -0.5*(3*math.log(2*math.pi)+math.log(d)+sum(a*b for a, b in zip(innovation, solution)))
    gain = [solve(s, row) for row in ph]
    x = [x[i]+sum(gain[i][j]*innovation[j] for j in range(3)) for i in range(9)]
    kh = multiply(gain, h)
    a = [[float(i == j)-kh[i][j] for j in range(9)] for i in range(9)]
    p = multiply(multiply(a, p), transpose(a))
    for i in range(9):
        for j in range(9):
            p[i][j] += sum(gain[i][k]*VARIANCE[k]*gain[j][k] for k in range(3))
    return x, p, log_likelihood


class ReferenceIMM:
    def __init__(self):
        self.models = None
        self.probs = [0.5, 0.5]
        self.last = None

    def step(self, timestamp, z):
        if self.models is None:
            r, az, el = z
            x = [r*math.cos(el)*math.cos(az), r*math.cos(el)*math.sin(az), r*math.sin(el)] + [0.0]*6
            posvar = VARIANCE[0] + r*r*(VARIANCE[1]+VARIANCE[2])
            diag = [posvar]*3 + [10000.0]*3 + [400.0]*3
            p = [[diag[i] if i == j else 0.0 for j in range(9)] for i in range(9)]
            self.models = [(x[:], [row[:] for row in p]), (x[:], [row[:] for row in p])]
        else:
            dt = timestamp-self.last
            prior = [sum(self.probs[i]*TRANSITION[i][j] for i in range(2)) for j in range(2)]
            new_models, logp = [], []
            for j in range(2):
                w = [self.probs[i]*TRANSITION[i][j]/prior[j] for i in range(2)]
                x = [sum(w[i]*self.models[i][0][a] for i in range(2)) for a in range(9)]
                p = [[sum(w[i]*(self.models[i][1][a][b]
                         + (self.models[i][0][a]-x[a])*(self.models[i][0][b]-x[b]))
                         for i in range(2)) for b in range(9)] for a in range(9)]
                new_x, new_p, likelihood = update_model(x, p, dt, z, j == 0)
                new_models.append((new_x, new_p))
                logp.append(math.log(prior[j])+likelihood)
            weights = [math.exp(value-max(logp)) for value in logp]
            self.probs = [weight/sum(weights) for weight in weights]
            self.models = new_models
        self.last = timestamp
        x = [sum(self.probs[i]*self.models[i][0][a] for i in range(2)) for a in range(9)]
        p = [[sum(self.probs[i]*(self.models[i][1][a][b]
                 + (self.models[i][0][a]-x[a])*(self.models[i][0][b]-x[b]))
                 for i in range(2)) for b in range(9)] for a in range(9)]
        return x, p


def main():
    binary = Path(sys.argv[1]).resolve()
    with tempfile.TemporaryDirectory() as temporary:
        root = Path(temporary)
        scenario, output = root/'scenario', root/'output.csv'
        subprocess.run([sys.executable, '-B', str(ROOT/'common/generate_3d_scenario.py'),
                        '--output-dir', str(scenario), '--seed', '42'], check=True)
        subprocess.run([str(binary), str(scenario/'measurements.csv'), str(output)], check=True)
        filters, counts, missing = {}, {}, 0
        with (scenario/'measurements.csv').open(newline='', encoding='utf-8') as source, \
                output.open(newline='', encoding='utf-8') as result:
            for measured, actual in itertools.zip_longest(csv.DictReader(source), csv.DictReader(result)):
                if measured is None or actual is None:
                    raise AssertionError('measurement and output row counts differ')
                if int(measured['track_id']) != int(actual['track_id']) or not math.isclose(
                        float(measured['timestamp']), float(actual['timestamp']), rel_tol=0, abs_tol=1e-9):
                    raise AssertionError('measurement and output rows are not aligned')
                track = int(measured['track_id'])
                count = counts.get(track, 0)
                if count >= 600:
                    continue
                counts[track] = count+1
                reference = filters.setdefault(track, ReferenceIMM())
                z = [float(measured[k]) for k in ('range_m', 'azimuth_rad', 'elevation_rad')] if measured['available'] == '1' else None
                missing += z is None
                state, covariance = reference.step(float(measured['timestamp']), z)
                for i, name in enumerate(STATE_NAMES):
                    if not math.isclose(float(actual[name]), state[i], rel_tol=3e-6, abs_tol=3e-4):
                        raise AssertionError(f'track {track}, step {count}, {name}: {actual[name]} vs {state[i]}')
                for name, i, j in [('p_xx',0,0),('p_xy',0,1),('p_xz',0,2),('p_yy',1,1),('p_yz',1,2),('p_zz',2,2)]:
                    if not math.isclose(float(actual[name]), covariance[i][j], rel_tol=3e-5, abs_tol=3e-4):
                        raise AssertionError(f'track {track}, step {count}, covariance {name}: {actual[name]} vs {covariance[i][j]}')
                for i, name in enumerate(('prob_cv', 'prob_ca')):
                    if not math.isclose(float(actual[name]), reference.probs[i], rel_tol=3e-6, abs_tol=3e-6):
                        raise AssertionError(f'track {track}, step {count}, {name}: {actual[name]} vs {reference.probs[i]}')
        if sum(counts.values()) != 1800 or not missing:
            raise AssertionError(f'expected 1800 rows including gaps; got {counts} and {missing} gaps')
        print(f'Independent IMM reference agrees on 1800 updates including {missing} gaps, state, covariance, and probabilities.')


if __name__ == '__main__':
    main()
