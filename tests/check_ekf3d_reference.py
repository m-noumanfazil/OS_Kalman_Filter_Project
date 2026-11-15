"""Compare native EKF output to an independent small-matrix Python reference.

The reference uses finite-difference measurement derivatives and pivoted
Gaussian elimination, not the C++ analytic Jacobian and Cholesky solver.
"""
import csv
import math
from pathlib import Path
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[1]


def multiply(a, b):
    return [[sum(a[i][k] * b[k][j] for k in range(len(b)))
             for j in range(len(b[0]))] for i in range(len(a))]


def transpose(a):
    return [list(row) for row in zip(*a)]


def solve(a, b):
    rows = [list(row) + [value] for row, value in zip(a, b)]
    for i in range(3):
        pivot = max(range(i, 3), key=lambda r: abs(rows[r][i]))
        rows[i], rows[pivot] = rows[pivot], rows[i]
        if abs(rows[i][i]) < 1e-30:
            raise ValueError('singular innovation covariance')
        divisor = rows[i][i]
        rows[i] = [v / divisor for v in rows[i]]
        for j in range(3):
            if j != i:
                factor = rows[j][i]
                rows[j] = [a - factor*b for a, b in zip(rows[j], rows[i])]
    return [row[-1] for row in rows]


def radar(x):
    h = math.hypot(x[0], x[1])
    return [math.hypot(h, x[2]), math.atan2(x[1], x[0]), math.atan2(x[2], h)]


def wrapped(v):
    return math.remainder(v, 2 * math.pi)


class Reference:
    def __init__(self):
        self.state = None
        self.p = None
        self.last = None

    def step(self, timestamp, z):
        variance = [2500.0, 1e-6, 1e-6]
        if self.state is None:
            distance, azimuth, elevation = z
            horizontal = distance * math.cos(elevation)
            self.state = [horizontal * math.cos(azimuth), horizontal * math.sin(azimuth),
                          distance * math.sin(elevation)] + [0.0] * 6
            position_var = variance[0] + distance * distance * (variance[1] + variance[2])
            diagonal = [position_var] * 3 + [10000.0] * 3 + [400.0] * 3
            self.p = [[diagonal[i] if i == j else 0.0 for j in range(9)] for i in range(9)]
            self.last = timestamp
            return
        dt = timestamp - self.last
        f = [[float(i == j) for j in range(9)] for i in range(9)]
        for axis in range(3):
            f[axis][axis+3], f[axis][axis+6], f[axis+3][axis+6] = dt, 0.5*dt*dt, dt
        x = [sum(f[i][j] * self.state[j] for j in range(9)) for i in range(9)]
        p = multiply(multiply(f, self.p), transpose(f))
        q = [[dt**5/20, dt**4/8, dt**3/6], [dt**4/8, dt**3/3, dt**2/2],
             [dt**3/6, dt**2/2, dt]]
        for axis in range(3):
            for i in range(3):
                for j in range(3):
                    p[axis+3*i][axis+3*j] += 25*q[i][j]
        if z is not None:
            expected = radar(x)
            h = [[0.0]*9 for _ in range(3)]
            for axis in range(3):
                plus, minus = x[:], x[:]
                plus[axis] += 0.001
                minus[axis] -= 0.001
                high, low = radar(plus), radar(minus)
                for i in range(3):
                    delta = high[i] - low[i]
                    h[i][axis] = (wrapped(delta) if i else delta) / 0.002
            ph = multiply(p, transpose(h))
            innovation = multiply(h, ph)
            for i in range(3):
                innovation[i][i] += variance[i]
            gain = [solve(innovation, row) for row in ph]
            residual = [z[0]-expected[0], wrapped(z[1]-expected[1]), wrapped(z[2]-expected[2])]
            x = [x[i] + sum(gain[i][j]*residual[j] for j in range(3)) for i in range(9)]
            kh = multiply(gain, h)
            a = [[float(i == j)-kh[i][j] for j in range(9)] for i in range(9)]
            p = multiply(multiply(a, p), transpose(a))
            for i in range(9):
                for j in range(9):
                    p[i][j] += sum(gain[i][k]*variance[k]*gain[j][k] for k in range(3))
        self.state, self.p, self.last = x, p, timestamp


def main():
    binary = Path(sys.argv[1]).resolve()
    with tempfile.TemporaryDirectory() as temporary:
        root = Path(temporary)
        scenario, output = root/'scenario', root/'output.csv'
        subprocess.run([sys.executable, '-B', str(ROOT/'common/generate_3d_scenario.py'),
                        '--output-dir', str(scenario), '--seed', '42'], check=True)
        subprocess.run([str(binary), str(scenario/'measurements.csv'), str(output)], check=True)
        filters = {}
        tested = 0
        with (scenario/'measurements.csv').open(newline='', encoding='utf-8') as source, \
                output.open(newline='', encoding='utf-8') as result:
            for measured, actual in zip(csv.DictReader(source), csv.DictReader(result)):
                track = int(measured['track_id'])
                reference = filters.setdefault(track, Reference())
                count = getattr(reference, 'count', 0)
                if count >= 40:
                    continue
                reference.count = count+1
                z = [float(measured[k]) for k in ('range_m', 'azimuth_rad', 'elevation_rad')] if measured['available'] == '1' else None
                reference.step(float(measured['timestamp']), z)
                for i, name in enumerate(('x','y','z','vx','vy','vz','ax','ay','az')):
                    value = float(actual[name])
                    if not math.isclose(value, reference.state[i], rel_tol=1e-6, abs_tol=1e-4):
                        raise AssertionError(f'track {track}, step {count}, state {name}: {value} vs {reference.state[i]}')
                for name, i, j in [('p_xx',0,0),('p_xy',0,1),('p_xz',0,2),('p_yy',1,1),('p_yz',1,2),('p_zz',2,2)]:
                    if not math.isclose(float(actual[name]), reference.p[i][j], rel_tol=1e-5, abs_tol=1e-4):
                        raise AssertionError(f'track {track}, step {count}, covariance {name}')
                tested += 1
        if tested != 120:
            raise AssertionError(f'expected 120 checked rows, got {tested}')
        print('Independent finite-difference and pivoted-solver reference agrees on 120 updates, including gaps.')


if __name__ == '__main__':
    main()
