#!/usr/bin/env python3
"""
verify_part_b.py
Compare saved native output with an independent Python reference. This script
does not execute native programs or write to their output files.
"""

import csv
import math
import os
import sys
import argparse
from pathlib import Path
from itertools import zip_longest

class AxisKalmanFilter:
    def __init__(self, process_noise_q=1500.0, meas_noise_r=2500.0):
        self.p = 0.0
        self.v = 0.0
        self.p00 = 1000.0
        self.p01 = 0.0
        self.p11 = 500.0
        self.q = process_noise_q
        self.r = meas_noise_r
        self.initialized = False

    def init(self, pos, vel=0.0):
        self.p = pos
        self.v = vel
        self.p00 = 1000.0
        self.p01 = 0.0
        self.p11 = 500.0
        self.initialized = True

    def step(self, meas, dt):
        if not self.initialized:
            self.init(meas, 0.0)
            return self.p, self.v

        # 1. Predict
        p_pred = self.p + self.v * dt
        v_pred = self.v

        dt2 = dt * dt
        dt3 = dt2 * dt
        q00 = self.q * (dt3 / 3.0)
        q01 = self.q * (dt2 / 2.0)
        q11 = self.q * dt

        p00_pred = self.p00 + 2.0 * dt * self.p01 + dt2 * self.p11 + q00
        p01_pred = self.p01 + dt * self.p11 + q01
        p11_pred = self.p11 + q11

        # 2. Update
        s = p00_pred + self.r
        inv_s = 1.0 / s
        k0 = p00_pred * inv_s
        k1 = p01_pred * inv_s

        y = meas - p_pred
        self.p = p_pred + k0 * y
        self.v = v_pred + k1 * y

        self.p00 = (1.0 - k0) * p00_pred
        self.p01 = (1.0 - k0) * p01_pred
        self.p11 = p11_pred - k1 * p01_pred

        return self.p, self.v

class Jet2DKalmanFilter:
    def __init__(self, q=1500.0, r=2500.0):
        self.kx = AxisKalmanFilter(q, r)
        self.ky = AxisKalmanFilter(q, r)
        self.last_timestamp = -1.0

    def process(self, timestamp, mx, my):
        if self.last_timestamp < 0.0:
            dt = 0.05
        else:
            dt = timestamp - self.last_timestamp
            if dt <= 0.0:
                dt = 0.05
        self.last_timestamp = timestamp

        px, vx = self.kx.step(mx, dt)
        py, vy = self.ky.step(my, dt)
        return px, py, vx, vy


def verify(input_path, output_path, tolerance):
    filters = {}
    count = 0
    max_error = 0.0
    with input_path.open(newline='', encoding='utf-8') as input_file, \
            output_path.open(newline='', encoding='utf-8') as output_file:
        inputs = csv.DictReader(input_file)
        outputs = csv.DictReader(output_file)
        for number, (source, result) in enumerate(zip_longest(inputs, outputs), 2):
            if source is None or result is None:
                raise ValueError(f'row count mismatch at line {number}')
            track = int(source['track_id'])
            timestamp = float(source['timestamp'])
            if track < 0 or track >= 8 or int(result['track_id']) != track:
                raise ValueError(f'invalid or mismatched track at line {number}')
            output_timestamp = float(result['timestamp'])
            if not math.isfinite(timestamp) or not math.isfinite(output_timestamp) or abs(output_timestamp - timestamp) > tolerance:
                raise ValueError(f'timestamp mismatch at line {number}')
            filt = filters.setdefault(track, Jet2DKalmanFilter())
            if timestamp < 0 or (filt.last_timestamp >= 0 and timestamp <= filt.last_timestamp):
                raise ValueError(f'invalid timestamp ordering at line {number}')
            measured_x, measured_y = float(source['meas_x']), float(source['meas_y'])
            if not math.isfinite(measured_x) or not math.isfinite(measured_y):
                raise ValueError(f'non-finite measurement at line {number}')
            expected = filt.process(timestamp, measured_x, measured_y)
            actual = tuple(float(result[name]) for name in ('est_x', 'est_y', 'est_vx', 'est_vy'))
            if not all(math.isfinite(value) for value in (*expected, *actual)):
                raise ValueError(f'non-finite state at line {number}')
            error = max(abs(a - b) for a, b in zip(expected, actual))
            max_error = max(max_error, error)
            if error > tolerance:
                raise ValueError(f'state mismatch at line {number}: {error:.9g} > {tolerance}')
            count += 1
    if count == 0:
        raise ValueError('empty input or output')
    print(f'{output_path}: {count} rows matched; max state error {max_error:.9g}')


def main():
    root = Path(__file__).resolve().parent.parent
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--input', type=Path, default=root / 'common/sensor_data.csv')
    parser.add_argument('--output', type=Path, action='append', help='native CSV (repeat for multiple outputs)')
    parser.add_argument('--tolerance', type=float, default=0.00051,
                        help='absolute tolerance (default accounts for 3-decimal native export)')
    args = parser.parse_args()
    if not math.isfinite(args.tolerance) or args.tolerance < 0:
        parser.error('tolerance must be finite and nonnegative')
    outputs = args.output or [root / 'results/Linux/filtered_sensor_data.csv',
                              root / 'results/Windows/filtered_sensor_data.csv']
    try:
        for output in outputs:
            verify(args.input, output, args.tolerance)
    except (OSError, ValueError, KeyError, TypeError) as exc:
        print(f'Verification failed: {exc}', file=sys.stderr)
        return 1
    print('Saved outputs agree with the reference; native execution provenance is not verified.')
    return 0


if __name__ == '__main__':
    sys.exit(main())
