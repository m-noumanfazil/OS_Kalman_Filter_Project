#!/usr/bin/env python3
"""Compare 3D EKF output and the existing 2D baseline on common observations."""
import argparse
import csv
import hashlib
from itertools import zip_longest
import json
import math
from pathlib import Path
from verify_part_b import Jet2DKalmanFilter


def sha256(path):
    digest = hashlib.sha256()
    with path.open('rb') as file:
        for block in iter(lambda: file.read(1024 * 1024), b''):
            digest.update(block)
    return digest.hexdigest()


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
    parser.add_argument('--json-output', type=Path, help='new file for reproducible metrics and input hashes')
    args = parser.parse_args()
    sums = [0.0, 0.0, 0.0, 0.0]
    count = 0
    state_sums = [0.0, 0.0, 0.0]
    state_count = 0
    filters = {}
    last_timestamp = {}
    segments = {name: [0.0, 0] for name in ('straight', 'acceleration', 'turn', 'post-turn', 'missing')}
    state_groups = {name: [0.0, 0.0, 0.0, 0] for name in ('straight', 'acceleration', 'turn', 'post-turn', 'missing')}
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
            if m['available'] not in ('0', '1'):
                raise ValueError(f'invalid availability at line {line}')
            if m['available'] == '0' and any(m[name] for name in ('range_m', 'azimuth_rad', 'elevation_rad')):
                raise ValueError(f'missing observation has measurement values at line {line}')
            track = int(m['track_id'])
            if track < 0 or times[0] < 0 or (track in last_timestamp and times[0] <= last_timestamp[track]):
                raise ValueError(f'invalid track or timestamp ordering at line {line}')
            if track not in last_timestamp and m['available'] != '1':
                raise ValueError(f'first observation is missing at line {line}')
            last_timestamp[track] = times[0]
            segment = t.get('segment')
            if segment not in ('straight', 'acceleration', 'turn', 'post-turn'):
                raise ValueError(f'missing or invalid ground-truth segment at line {line}; regenerate the scenario')
            x,y,z = (float(t[k]) for k in ('x_m','y_m','z_m'))
            ex,ey,ez = (float(e[k]) for k in ('x','y','z'))
            if not all(math.isfinite(v) for v in (x,y,z,ex,ey,ez)):
                raise ValueError(f'non-finite state at line {line}')
            velocity = [float(t[k]) for k in ('vx_mps', 'vy_mps', 'vz_mps')]
            acceleration = [float(t[k]) for k in ('ax_mps2', 'ay_mps2', 'az_mps2')]
            estimate_velocity = [float(e[k]) for k in ('vx', 'vy', 'vz')]
            estimate_acceleration = [float(e[k]) for k in ('ax', 'ay', 'az')]
            if not all(math.isfinite(v) for v in velocity + acceleration + estimate_velocity + estimate_acceleration):
                raise ValueError(f'non-finite velocity or acceleration at line {line}')
            errors = [(ex-x)**2 + (ey-y)**2 + (ez-z)**2,
                      sum((a-b)**2 for a,b in zip(estimate_velocity, velocity)),
                      sum((a-b)**2 for a,b in zip(estimate_acceleration, acceleration))]
            state_sums = [a+b for a,b in zip(state_sums, errors)]
            for name in ([segment, 'missing'] if m['available'] == '0' else [segment]):
                for i in range(3): state_groups[name][i] += errors[i]
                state_groups[name][3] += 1
            state_count += 1
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
            if not all(math.isfinite(v) for v in (distance, az, el)) or distance <= 1e-6 or abs(el) >= math.pi/2:
                raise ValueError(f'invalid radar observation at line {line}')
            mx, my, mz = distance*math.cos(el)*math.cos(az), distance*math.cos(el)*math.sin(az), distance*math.sin(el)
            baseline = filters.setdefault(int(m['track_id']), Jet2DKalmanFilter())
            bx,by,_,_ = baseline.process(float(m['timestamp']),mx,my)
            values = [mx,my,mz,x,y,z,ex,ey,ez,bx,by]
            if not all(math.isfinite(v) for v in values): raise ValueError(f'non-finite value at line {line}')
            sums[0] += (mx-x)**2+(my-y)**2
            sums[1] += (bx-x)**2+(by-y)**2
            sums[2] += (ex-x)**2+(ey-y)**2
            sums[3] += (ex-x)**2+(ey-y)**2+(ez-z)**2
            segments[segment][0] += (ex-x)**2+(ey-y)**2+(ez-z)**2
            segments[segment][1] += 1
            count += 1
    if not count: raise ValueError('no available observations')
    for label, total in zip(('Position (m)', 'Velocity (m/s)', 'Acceleration (m/s^2)'), state_sums):
        print(f'All-row {label} vector RMSE: {math.sqrt(total/state_count):.3f}')
    for label, (p, v, a, n) in state_groups.items():
        if n: print(f'{label} state vector RMSE: position {math.sqrt(p/n):.3f} m, velocity {math.sqrt(v/n):.3f} m/s, acceleration {math.sqrt(a/n):.3f} m/s^2 (n={n})')
    for label, total in zip(('Raw XY', '2D baseline XY', '3D EKF XY', '3D EKF XYZ'), sums):
        print(f'{label} RMSE: {math.sqrt(total/count):.3f} m')
    for label, (total, n) in segments.items():
        if n: print(f'3D EKF {label} XYZ RMSE: {math.sqrt(total/n):.3f} m ({n} observations)')
    for label, (total, covered, n) in calibration.items():
        if n: print(f'Position NEES ({label}): mean {total/n:.3f}; 95% ellipsoid coverage {100*covered/n:.1f}% (n={n})')
    print(f'Compared {count} observations; ground truth was used only for evaluation.')
    if args.json_output:
        report = {
            'schema_version': 1,
            'input_sha256': {
                'measurements': sha256(args.scenario/'measurements.csv'),
                'ground_truth': sha256(args.scenario/'ground_truth.csv'),
                'estimates': sha256(args.estimates),
            },
            'scope': {
                'state_rmse': 'all rows including missing observations',
                'baseline_rmse': 'available observations only; converted polar measurements',
                'segment_labels': 'explicit ground-truth labels; missing rows also counted separately',
                'nees_coverage': 'descriptive; time-correlated samples are not independent trials',
            },
            'all_row_count': state_count,
            'available_row_count': count,
            'state_rmse': dict(zip(('position_m', 'velocity_mps', 'acceleration_mps2'),
                                   (math.sqrt(value/state_count) for value in state_sums))),
            'available_rmse': dict(zip(('raw_xy_m', 'baseline_xy_m', 'ekf_xy_m', 'ekf_xyz_m'),
                                       (math.sqrt(value/count) for value in sums))),
            'state_by_segment': {
                name: {'count': n, 'position_m': math.sqrt(p/n), 'velocity_mps': math.sqrt(v/n),
                       'acceleration_mps2': math.sqrt(a/n)}
                for name, (p, v, a, n) in state_groups.items() if n
            },
            'position_nees': {
                name: {'count': n, 'mean': total/n, 'coverage_95_fraction': covered/n}
                for name, (total, covered, n) in calibration.items() if n
            },
        }
        with args.json_output.open('x', encoding='utf-8') as file:
            json.dump(report, file, indent=2, allow_nan=False)
            file.write('\n')


if __name__ == '__main__':
    main()
