#!/usr/bin/env python3
"""Compare 3D EKF output and the existing 2D baseline on common observations."""
import argparse
import csv
from itertools import zip_longest
import math
from pathlib import Path
from verify_part_b import Jet2DKalmanFilter


def position_nees(error, covariance):
    """Solve P y = error using Cholesky and return error.T y."""
    lower = [[0.0] * 3 for _ in range(3)]
    for i in range(3):
        for j in range(i + 1):
            value = covariance[i][j] - sum(lower[i][k] * lower[j][k] for k in range(j))
            if i == j:
                if not math.isfinite(value) or value <= 0:
                    raise ValueError('position covariance is not positive definite')
                lower[i][j] = math.sqrt(value)
            else:
                lower[i][j] = value / lower[j][j]
    whitened = [0.0] * 3
    for i in range(3):
        whitened[i] = (error[i] - sum(lower[i][k] * whitened[k] for k in range(i))) / lower[i][i]
    result = sum(value * value for value in whitened)
    if not math.isfinite(result):
        raise ValueError('non-finite position NEES')
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('scenario', type=Path, help='directory with measurements.csv and ground_truth.csv')
    parser.add_argument('estimates', type=Path, help='output from run_ekf3d')
    args = parser.parse_args()
    sums = [0.0, 0.0, 0.0, 0.0]
    count = 0
    filters = {}
    track_steps = {}
    segments = {name: [0.0, 0] for name in ('straight', 'acceleration', 'turn', 'post-turn', 'missing')}
    calibration = {'available': [0.0, 0, 0], 'missing': [0.0, 0, 0]}
    with (args.scenario/'measurements.csv').open(newline='', encoding='utf-8') as mf, \
         (args.scenario/'ground_truth.csv').open(newline='', encoding='utf-8') as tf, \
         args.estimates.open(newline='', encoding='utf-8') as ef:
        for line, (m,t,e) in enumerate(zip_longest(csv.DictReader(mf), csv.DictReader(tf), csv.DictReader(ef)), 2):
            if m is None or t is None or e is None:
                raise ValueError(f'row count mismatch at line {line}')
            if int(m['track_id']) != int(t['track_id']) or int(m['track_id']) != int(e['track_id']):
                raise ValueError(f'track mismatch at line {line}')
            times = [float(row['timestamp']) for row in (m, t, e)]
            if not all(math.isfinite(v) for v in times) or abs(times[0]-times[1]) > 1e-9 or abs(times[0]-times[2]) > 1e-9:
                raise ValueError(f'timestamp mismatch at line {line}')
            track = int(m['track_id'])
            step = track_steps.get(track, 0)
            track_steps[track] = step + 1
            x,y,z = (float(t[k]) for k in ('x_m','y_m','z_m'))
            ex,ey,ez = (float(e[k]) for k in ('x','y','z'))
            if not all(math.isfinite(v) for v in (x,y,z,ex,ey,ez)):
                raise ValueError(f'non-finite state at line {line}')
            if m['available'] not in ('0', '1'):
                raise ValueError(f'invalid availability at line {line}')
            xx, xy, xz, yy, yz, zz = (float(e[k]) for k in ('p_xx','p_xy','p_xz','p_yy','p_yz','p_zz'))
            if not all(math.isfinite(v) for v in (xx, xy, xz, yy, yz, zz)):
                raise ValueError(f'non-finite covariance at line {line}')
            nees = position_nees([ex-x, ey-y, ez-z], [[xx,xy,xz],[xy,yy,yz],[xz,yz,zz]])
            group = calibration['available' if m['available'] == '1' else 'missing']
            group[0] += nees
            group[1] += nees <= 7.814727903
            group[2] += 1
            if m['available'] != '1':
                segments['missing'][0] += (ex-x)**2+(ey-y)**2+(ez-z)**2
                segments['missing'][1] += 1
                continue
            distance, az, el = (float(m[k]) for k in ('range_m','azimuth_rad','elevation_rad'))
            mx, my, mz = distance*math.cos(el)*math.cos(az), distance*math.cos(el)*math.sin(az), distance*math.sin(el)
            baseline = filters.setdefault(int(m['track_id']), Jet2DKalmanFilter())
            bx,by,_,_ = baseline.process(float(m['timestamp']),mx,my)
            values = [mx,my,mz,x,y,z,ex,ey,ez,bx,by]
            if not all(math.isfinite(v) for v in values): raise ValueError(f'non-finite value at line {line}')
            sums[0] += (mx-x)**2+(my-y)**2
            sums[1] += (bx-x)**2+(by-y)**2
            sums[2] += (ex-x)**2+(ey-y)**2
            sums[3] += (ex-x)**2+(ey-y)**2+(ez-z)**2
            segment = 'straight' if step < 150 else 'acceleration' if step < 300 else 'turn' if step < 450 else 'post-turn'
            segments[segment][0] += (ex-x)**2+(ey-y)**2+(ez-z)**2
            segments[segment][1] += 1
            count += 1
    if not count: raise ValueError('no available observations')
    for label, total in zip(('Raw XY', '2D baseline XY', '3D EKF XY', '3D EKF XYZ'), sums):
        print(f'{label} RMSE: {math.sqrt(total/count):.3f} m')
    for label, (total, n) in segments.items():
        if n: print(f'3D EKF {label} XYZ RMSE: {math.sqrt(total/n):.3f} m ({n} observations)')
    for label, (total, covered, n) in calibration.items():
        if n: print(f'Position NEES ({label}): mean {total/n:.3f}; 95% ellipsoid coverage {100*covered/n:.1f}% (n={n})')
    print(f'Compared {count} observations; ground truth was used only for evaluation.')


if __name__ == '__main__':
    main()
