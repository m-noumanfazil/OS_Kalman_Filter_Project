#!/usr/bin/env python3
"""Create reproducible 3D radar measurements and separate ground truth."""
import argparse
import csv
import math
from pathlib import Path
import random


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output-dir', type=Path, required=True, help='new directory for scenario CSVs')
    parser.add_argument('--seed', type=int, default=42)
    args = parser.parse_args()
    args.output_dir.mkdir(parents=True, exist_ok=False)
    rng = random.Random(args.seed)
    with (args.output_dir / 'measurements.csv').open('w', newline='', encoding='utf-8') as mf, \
         (args.output_dir / 'ground_truth.csv').open('w', newline='', encoding='utf-8') as tf:
        measurements = csv.writer(mf)
        truth = csv.writer(tf)
        measurements.writerow(['timestamp', 'track_id', 'range_m', 'azimuth_rad', 'elevation_rad', 'available'])
        truth.writerow(['timestamp', 'track_id', 'x_m', 'y_m', 'z_m', 'vx_mps', 'vy_mps', 'vz_mps', 'ax_mps2', 'ay_mps2', 'az_mps2', 'segment'])
        for track in range(3):
            position = [6000.0 + 1800*track, -3000.0 + 2000*track, 1200.0 + 400*track]
            velocity = [180.0 + 20*track, 80.0 - 30*track, 10.0 - 5*track]
            time = 0.0
            for step in range(600):
                # Deterministic segments: straight, acceleration/climb, turn, straight.
                acceleration = ([0.0, 0.0, 0.0] if step < 150 or step >= 450 else
                                ([1.5, 0.4, 0.7] if step < 300 else [-1.0, 2.0, -0.5]))
                segment = ('straight' if step < 150 else 'acceleration' if step < 300
                           else 'turn' if step < 450 else 'post-turn')
                if step:
                    dt = 0.15 if step % 7 == 0 else 0.1
                    time += dt
                    for axis in range(3):
                        position[axis] += velocity[axis]*dt + 0.5*acceleration[axis]*dt*dt
                        velocity[axis] += acceleration[axis]*dt
                x, y, z = position
                horizontal = math.hypot(x, y)
                distance = math.hypot(horizontal, z)
                azimuth, elevation = math.atan2(y, x), math.atan2(z, horizontal)
                available = step == 0 or step % 29 not in (0, 1)
                truth.writerow([f'{time:.6f}', track, *[f'{v:.9f}' for v in position + velocity + acceleration], segment])
                if available:
                    measurements.writerow([f'{time:.6f}', track, f'{distance+rng.gauss(0,50):.9f}',
                                           f'{azimuth+rng.gauss(0,0.001):.12f}',
                                           f'{elevation+rng.gauss(0,0.001):.12f}', 1])
                else:
                    measurements.writerow([f'{time:.6f}', track, '', '', '', 0])
    print(f'Scenario saved to {args.output_dir} (seed {args.seed}).')


if __name__ == '__main__':
    main()
