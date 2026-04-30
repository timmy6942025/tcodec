#!/usr/bin/env python3
"""plot_rd.py — Plot rate-distortion curves from benchmark results.

Generates RD curve plots (bitrate vs PSNR) for each test clip,
comparing TCodec against x264, x265, and SVT-AV1.

Usage:
    python3 tools/plot_rd.py BENCHMARK_DIR [OUTPUT_DIR]

Args:
    BENCHMARK_DIR  Directory containing quality.csv from evaluate_quality.py
    OUTPUT_DIR     Directory to save plots (default: BENCHMARK_DIR/plots)
"""

import csv
import sys
import os
import numpy as np
import matplotlib
matplotlib.use('Agg')  # Non-interactive backend
import matplotlib.pyplot as plt


# Color and marker scheme for each codec
CODEC_STYLE = {
    'tcodec':   {'color': '#2196F3', 'marker': 'o', 'label': 'TCodec', 'linestyle': '-'},
    'x264':     {'color': '#4CAF50', 'marker': 's', 'label': 'x264', 'linestyle': '--'},
    'x265':     {'color': '#FF9800', 'marker': '^', 'label': 'x265', 'linestyle': '-.'},
    'svt-av1':  {'color': '#9C27B0', 'marker': 'D', 'label': 'SVT-AV1', 'linestyle': ':'},
}


def load_rd_data(csv_path):
    """Load RD data from quality.csv or summary.csv.

    Returns dict: clip -> codec -> [(rate_kbps, psnr_y), ...]
    """
    data = {}
    with open(csv_path, 'r') as f:
        reader = csv.DictReader(f)
        for row in reader:
            codec = row['codec']
            clip = row['clip']
            try:
                rate = float(row['avg_bitrate_kbps']) if row.get('avg_bitrate_kbps') else float(row['total_bytes']) * 8 / 1000
                psnr = float(row['psnr_y'])
            except (ValueError, KeyError):
                continue
            if rate <= 0 or psnr <= 0:
                continue
            if clip not in data:
                data[clip] = {}
            if codec not in data[clip]:
                data[clip][codec] = []
            data[clip][codec].append((rate, psnr))

    # Sort by rate
    for clip in data:
        for codec in data[clip]:
            data[clip][codec].sort(key=lambda p: p[0])

    return data


def plot_rd_curve(ax, clip, codec_data):
    """Plot RD curves for one clip on the given axes."""
    for codec, style in CODEC_STYLE.items():
        if codec not in codec_data or len(codec_data[codec]) < 1:
            continue
        points = codec_data[codec]
        rates = [p[0] for p in points]
        psnrs = [p[1] for p in points]

        ax.plot(rates, psnrs, **{k: v for k, v in style.items() if k != 'label'},
                label=style['label'], markersize=6, linewidth=1.5)
        # Annotate QP values
        for rate, psnr in points:
            # Infer QP from position in sorted list (approximate)
            pass  # QP annotation optional — cluttery with 5 QPs

    ax.set_xlabel('Bitrate (kbps)', fontsize=10)
    ax.set_ylabel('PSNR-Y (dB)', fontsize=10)
    ax.set_title(clip.replace('_', ' ').title(), fontsize=11, fontweight='bold')
    ax.legend(fontsize=8, loc='lower right')
    ax.grid(True, alpha=0.3)
    ax.set_xscale('log')


def main():
    if len(sys.argv) < 2:
        print(f"Usage: {sys.argv[0]} BENCHMARK_DIR [OUTPUT_DIR]")
        sys.exit(1)

    bench_dir = sys.argv[1]
    out_dir = sys.argv[2] if len(sys.argv) > 2 else os.path.join(bench_dir, 'plots')

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

    os.makedirs(out_dir, exist_ok=True)
    data = load_rd_data(csv_path)

    if not data:
        print("No RD data found.")
        sys.exit(1)

    clips = sorted(data.keys())

    # ── Per-clip plots ──────────────────────────────────────
    for clip in clips:
        fig, ax = plt.subplots(figsize=(8, 5))
        plot_rd_curve(ax, clip, data[clip])
        fig.tight_layout()

        safe_name = clip.replace('/', '_')
        out_path = os.path.join(out_dir, f"rd_{safe_name}.png")
        fig.savefig(out_path, dpi=150)
        plt.close(fig)
        print(f"  Saved: {out_path}")

    # ── Combined overview plot ───────────────────────────────
    n_clips = len(clips)
    n_cols = min(3, n_clips)
    n_rows = (n_clips + n_cols - 1) // n_cols

    fig, axes = plt.subplots(n_rows, n_cols, figsize=(6 * n_cols, 4 * n_rows))
    if n_clips == 1:
        axes = np.array([[axes]])
    elif n_rows == 1:
        axes = axes.reshape(1, -1)

    for idx, clip in enumerate(clips):
        row = idx // n_cols
        col = idx % n_cols
        plot_rd_curve(axes[row, col], clip, data[clip])

    # Hide empty subplots
    for idx in range(n_clips, n_rows * n_cols):
        row = idx // n_cols
        col = idx % n_cols
        axes[row, col].set_visible(False)

    fig.suptitle('TCodec RD-Curve Comparison', fontsize=14, fontweight='bold', y=1.02)
    fig.tight_layout()

    out_path = os.path.join(out_dir, 'rd_overview.png')
    fig.savefig(out_path, dpi=150, bbox_inches='tight')
    plt.close(fig)
    print(f"  Saved: {out_path}")

    print(f"\nAll plots saved to {out_dir}/")


if __name__ == '__main__':
    main()
