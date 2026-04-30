#!/usr/bin/env python3
"""bd_rate.py — Compute BD-rate (Bjøntegaard Delta rate) between codecs.

BD-rate measures the average bitrate savings of one codec relative to a
reference codec at the same objective quality. A negative BD-rate means
the test codec uses fewer bits for the same quality (better compression).

Uses the piecewise-cubic interpolation method from:
  Bjøntegaard, "Calculation of Average PSNR Differences between RD-Curves" (VCEG-AI11, 2001)

Usage:
    python3 tools/bd_rate.py BENCHMARK_DIR [REFERENCE_CODEC]

Args:
    BENCHMARK_DIR     Directory containing quality.csv from evaluate_quality.py
    REFERENCE_CODEC   Reference codec (default: x264)

Output:
    BD-rate table comparing each codec vs the reference, per clip and overall.
"""

import csv
import sys
import os
import numpy as np
from scipy.interpolate import interp1d
from scipy.integrate import quad


def load_rd_points(csv_path, codec, clip=None):
    """Load (bitrate, psnr) RD points for a codec from quality.csv."""
    points = []
    with open(csv_path, 'r') as f:
        reader = csv.DictReader(f)
        for row in reader:
            if row['codec'] != codec:
                continue
            if clip is not None and row['clip'] != clip:
                continue
            try:
                rate = float(row['avg_bitrate_kbps']) if row.get('avg_bitrate_kbps') else float(row['total_bytes']) * 8 / 1000
                psnr = float(row['psnr_y'])
            except (ValueError, KeyError):
                continue
            if rate > 0 and psnr > 0:
                points.append((rate, psnr))
    # Sort by rate
    points.sort(key=lambda p: p[0])
    return points


def compute_bd_rate(ref_points, test_points):
    """Compute BD-rate between two RD curves.

    Args:
        ref_points:  List of (rate, psnr) for reference codec
        test_points: List of (rate, psnr) for test codec

    Returns:
        BD-rate as a percentage. Negative = test codec is better.
        Returns None if curves don't overlap.
    """
    if len(ref_points) < 2 or len(test_points) < 2:
        return None

    # Convert to log-rate domain
    ref_log = [(np.log(r), p) for r, p in ref_points]
    test_log = [(np.log(r), p) for r, p in test_points]

    # Find overlap in PSNR range
    ref_psnr_min = min(p for _, p in ref_log)
    ref_psnr_max = max(p for _, p in ref_log)
    test_psnr_min = min(p for _, p in test_log)
    test_psnr_max = max(p for _, p in test_log)

    p_min = max(ref_psnr_min, test_psnr_min)
    p_max = min(ref_psnr_max, test_psnr_max)

    if p_max <= p_min:
        return None  # No overlap

    # Interpolate log-rate as function of PSNR
    # RD curves are monotonically non-decreasing in PSNR with increasing rate,
    # but real data may not be perfectly monotonic. Sort by PSNR.
    ref_sorted = sorted(ref_log, key=lambda p: p[1])
    test_sorted = sorted(test_log, key=lambda p: p[1])

    ref_ps = [p for _, p in ref_sorted]
    ref_lr = [r for r, _ in ref_sorted]
    test_ps = [p for _, p in test_sorted]
    test_lr = [r for r, _ in test_sorted]

    try:
        ref_interp = interp1d(ref_ps, ref_lr, kind='linear', fill_value='extrapolate')
        test_interp = interp1d(test_ps, test_lr, kind='linear', fill_value='extrapolate')
    except Exception:
        return None

    # Integrate difference in log-rate over overlap region
    def integrand(p):
        return test_interp(p) - ref_interp(p)

    try:
        area, _ = quad(integrand, p_min, p_max)
    except Exception:
        return None

    # BD-rate = exp(area / (p_max - p_min)) - 1, as percentage
    bd_rate = (np.exp(area / (p_max - p_min)) - 1) * 100
    return bd_rate


def main():
    if len(sys.argv) < 2:
        print(f"Usage: {sys.argv[0]} BENCHMARK_DIR [REFERENCE_CODEC]")
        sys.exit(1)

    bench_dir = sys.argv[1]
    ref_codec = sys.argv[2] if len(sys.argv) > 2 else 'x264'

    # Try quality.csv first, fall back to summary.csv
    quality_csv = os.path.join(bench_dir, 'quality.csv')
    summary_csv = os.path.join(bench_dir, 'summary.csv')

    if os.path.exists(quality_csv):
        csv_path = quality_csv
    elif os.path.exists(summary_csv):
        csv_path = summary_csv
    else:
        print(f"Error: No quality.csv or summary.csv in {bench_dir}")
        sys.exit(1)

    # Discover codecs and clips
    codecs = set()
    clips = set()
    with open(csv_path, 'r') as f:
        reader = csv.DictReader(f)
        for row in reader:
            codecs.add(row['codec'])
            clips.add(row['clip'])

    codecs = sorted(codecs)
    clips = sorted(clips)

    if ref_codec not in codecs:
        print(f"Error: Reference codec '{ref_codec}' not found in data. Available: {codecs}")
        sys.exit(1)

    print("════════════════════════════════════════════════════════")
    print(f"  BD-Rate Analysis (reference: {ref_codec})")
    print("════════════════════════════════════════════════════════")
    print()

    # Per-clip BD-rate
    print(f"{'Clip':<30}", end='')
    for codec in codecs:
        if codec != ref_codec:
            print(f"  {codec:>12}", end='')
    print()
    print("-" * (30 + 14 * (len(codecs) - 1)))

    overall_bd = {codec: [] for codec in codecs if codec != ref_codec}

    for clip in clips:
        ref_points = load_rd_points(csv_path, ref_codec, clip)
        print(f"{clip:<30}", end='')
        for codec in codecs:
            if codec == ref_codec:
                continue
            test_points = load_rd_points(csv_path, codec, clip)
            bd = compute_bd_rate(ref_points, test_points)
            if bd is not None:
                print(f"  {bd:>+11.1f}%", end='')
                overall_bd[codec].append(bd)
            else:
                print(f"  {'N/A':>12}", end='')
        print()

    # Overall average BD-rate
    print()
    print(f"{'OVERALL':<30}", end='')
    for codec in codecs:
        if codec == ref_codec:
            continue
        if overall_bd[codec]:
            avg_bd = np.mean(overall_bd[codec])
            print(f"  {avg_bd:>+11.1f}%", end='')
        else:
            print(f"  {'N/A':>12}", end='')
    print()

    print()
    print("  Negative BD-rate = test codec uses fewer bits at same quality (better)")
    print("  Positive BD-rate = test codec uses more bits at same quality (worse)")
    print("════════════════════════════════════════════════════════")


if __name__ == '__main__':
    main()
