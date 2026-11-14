#!/usr/bin/env python3
"""
generate_sensor_data.py
Part of the CT-353 Operating Systems Complex Computing Activity (Part B).

Generates a realistic multi-target radar tracking dataset for supersonic fighter jets.
Each jet cruises at high speed (Mach 1.5 - 2.0, ~500 to 650 m/s) with high-g turns.
A ground-based radar station tracks the jets with measurement noise (Gaussian jitter).

Columns in output CSV:
  timestamp,track_id,true_x,true_y,meas_x,meas_y,true_vx,true_vy
"""

import math
import random
import csv
import sys
import os

def generate_fighter_jet_tracks(
    num_tracks=8,
    num_points_per_track=1000,
    dt=0.05,
    radar_noise_std=50.0,
    seed=42
):
    random.seed(seed)
    records = []

    # Initial kinematics for 8 distinct fighter jets in airspace
    # Speeds: 500 to 650 m/s (supersonic Mach 1.5 - 2.0)
    # Headings: distributed across quadrants
    initial_configs = [
        # (x0, y0, speed, heading_rad, turn_rate_rad_per_s)
        (  10000.0,  15000.0, 580.0,  0.25 * math.pi,  0.015), # Jet 1: climbing right turn
        ( -15000.0,  20000.0, 620.0, -0.15 * math.pi, -0.012), # Jet 2: diving left turn
        (  25000.0, -10000.0, 540.0,  0.85 * math.pi,  0.020), # Jet 3: tactical high-g turn
        ( -20000.0, -15000.0, 600.0,  0.40 * math.pi, -0.018), # Jet 4: supersonic sweep
        (      0.0,  30000.0, 560.0, -0.60 * math.pi,  0.010), # Jet 5: intercept vector
        ( -10000.0,      0.0, 640.0,  0.10 * math.pi, -0.008), # Jet 6: high-speed dash
        (  15000.0, -25000.0, 590.0,  0.70 * math.pi,  0.022), # Jet 7: evasive weave
        (  30000.0,  10000.0, 550.0, -0.90 * math.pi, -0.014), # Jet 8: combat air patrol
    ]

    for track_id in range(num_tracks):
        cfg = initial_configs[track_id % len(initial_configs)]
        x, y = cfg[0], cfg[1]
        speed = cfg[2]
        heading = cfg[3]
        turn_rate = cfg[4]

        # Simulate trajectory over time
        for step in range(num_points_per_track):
            t = step * dt

            # Slight dynamic variation in turn rate (maneuvers)
            current_turn = turn_rate + 0.005 * math.sin(0.1 * t + track_id)
            heading += current_turn * dt

            # Compute true velocity components
            vx = speed * math.cos(heading)
            vy = speed * math.sin(heading)

            # Update true position (Euler-Maruyama kinematic integration)
            x += vx * dt
            y += vy * dt

            # Add ground-station radar sensor noise (Gaussian range & bearing error)
            noise_x = random.gauss(0.0, radar_noise_std)
            noise_y = random.gauss(0.0, radar_noise_std)
            meas_x = x + noise_x
            meas_y = y + noise_y

            records.append({
                'timestamp': round(t, 4),
                'track_id': track_id,
                'true_x': round(x, 3),
                'true_y': round(y, 3),
                'meas_x': round(meas_x, 3),
                'meas_y': round(meas_y, 3),
                'true_vx': round(vx, 3),
                'true_vy': round(vy, 3)
            })

    # Sort primarily by timestamp, secondarily by track_id for stream emulation
    records.sort(key=lambda r: (r['timestamp'], r['track_id']))
    return records

def main():
    script_dir = os.path.dirname(os.path.abspath(__file__))
    output_path = os.path.join(script_dir, 'sensor_data.csv')

    num_tracks = 8
    points_per_track = 1000
    dt = 0.05
    noise_std = 50.0

    print(f"Generating supersonic radar telemetry:")
    print(f"  Tracks: {num_tracks} fighter jets")
    print(f"  Points per track: {points_per_track} (total {num_tracks * points_per_track} pings)")
    print(f"  Sampling interval: {dt} s (20 Hz)")
    print(f"  Radar noise: +/- {noise_std} m")

    data = generate_fighter_jet_tracks(
        num_tracks=num_tracks,
        num_points_per_track=points_per_track,
        dt=dt,
        radar_noise_std=noise_std,
        seed=42
    )

    with open(output_path, 'w', newline='', encoding='utf-8') as f:
        fieldnames = ['timestamp', 'track_id', 'true_x', 'true_y', 'meas_x', 'meas_y', 'true_vx', 'true_vy']
        writer = csv.DictWriter(f, fieldnames=fieldnames)
        writer.writeheader()
        writer.writerows(data)

    file_size_kb = os.path.getsize(output_path) / 1024.0
    print(f"Successfully generated {output_path} ({file_size_kb:.1f} KB, {len(data)} rows)")

if __name__ == '__main__':
    main()
