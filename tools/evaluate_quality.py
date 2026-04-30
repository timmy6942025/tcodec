#!/usr/bin/env python3
"""evaluate_quality.py — Compute quality metrics from benchmark results.

Reads the summary.csv produced by run_benchmark.sh and enriches it with
additional quality metrics computed directly from the YUV files.

Usage:
    python3 tools/evaluate_quality.py BENCHMARK_DIR

Outputs:
    BENCHMARK_DIR/quality.csv — per-clip PSNR-Y, PSNR-U, PSNR-V, SSIM
"""

import csv
import os
import sys
import numpy as np


def read_yuv_plane(path, width, height, plane='y'):
    """Read a single YUV 4:2:0 plane from a raw file."""
    y_size = width * height
    u_size = (width // 2) * (height // 2)

    with open(path, 'rb') as f:
        data = f.read()

    if plane == 'y':
        return np.frombuffer(data[:y_size], dtype=np.uint8).reshape(height, width)
    elif plane == 'u':
        start = y_size
        return np.frombuffer(data[start:start + u_size], dtype=np.uint8).reshape(height // 2, width // 2)
    elif plane == 'v':
        start = y_size + u_size
        return np.frombuffer(data[start:start + u_size], dtype=np.uint8).reshape(height // 2, width // 2)


def compute_psnr(ref, dec):
    """Compute PSNR between two numpy arrays (higher is better)."""
    mse = np.mean((ref.astype(np.float64) - dec.astype(np.float64)) ** 2)
    if mse == 0:
        return float('inf')
    return 10.0 * np.log10(255.0 ** 2 / mse)


def compute_ssim_block(ref_block, dec_block):
    """Compute SSIM for a single block (simplified, no windowing)."""
    C1 = (0.01 * 255) ** 2
    C2 = (0.03 * 255) ** 2

    mu_ref = np.mean(ref_block.astype(np.float64))
    mu_dec = np.mean(dec_block.astype(np.float64))
    sigma_ref_sq = np.var(ref_block.astype(np.float64))
    sigma_dec_sq = np.var(dec_block.astype(np.float64))
    sigma_cross = np.cov(ref_block.astype(np.float64).flatten(),
                         dec_block.astype(np.float64).flatten())[0, 1]

    numerator = (2 * mu_ref * mu_dec + C1) * (2 * sigma_cross + C2)
    denominator = (mu_ref ** 2 + mu_dec ** 2 + C1) * (sigma_ref_sq + sigma_dec_sq + C2)

    if denominator == 0:
        return 1.0
    ssim_val = numerator / denominator
    # Clamp to [0, 1] — simplified block SSIM without Gaussian windowing
    # can exceed 1.0 on noise-free content
    return max(0.0, min(1.0, ssim_val))


def compute_ssim(ref, dec, block_size=8):
    """Compute mean SSIM across the image using sliding blocks."""
    h, w = ref.shape
    ssim_vals = []
    for y in range(0, h - block_size + 1, block_size):
        for x in range(0, w - block_size + 1, block_size):
            rb = ref[y:y + block_size, x:x + block_size]
            db = dec[y:y + block_size, x:x + block_size]
            ssim_vals.append(compute_ssim_block(rb, db))
    return np.mean(ssim_vals) if ssim_vals else 0.0


def main():
    if len(sys.argv) < 2:
        print(f"Usage: {sys.argv[0]} BENCHMARK_DIR")
        sys.exit(1)

    bench_dir = sys.argv[1]
    summary_csv = os.path.join(bench_dir, 'summary.csv')
    quality_csv = os.path.join(bench_dir, 'quality.csv')

    if not os.path.exists(summary_csv):
        print(f"Error: {summary_csv} not found. Run run_benchmark.sh first.")
        sys.exit(1)

    clips_dir = 'golden'
    decoded_dir = os.path.join(bench_dir, 'decoded')

    rows = []
    with open(summary_csv, 'r') as f:
        reader = csv.DictReader(f)
        for row in reader:
            codec = row['codec']
            clip = row['clip']
            qp = row['qp']
            w = int(row['width'])
            h = int(row['height'])

            ref_path = os.path.join(clips_dir, f"{clip}.yuv")
            dec_path = os.path.join(decoded_dir, f"{codec}_{clip}_qp{qp}.yuv")

            if not os.path.exists(ref_path):
                print(f"  Warning: reference {ref_path} not found, skipping")
                continue
            if not os.path.exists(dec_path):
                print(f"  Warning: decoded {dec_path} not found, skipping")
                continue

            # Compute per-plane PSNR
            ref_y = read_yuv_plane(ref_path, w, h, 'y')
            dec_y = read_yuv_plane(dec_path, w, h, 'y')
            psnr_y = compute_psnr(ref_y, dec_y)

            ref_u = read_yuv_plane(ref_path, w, h, 'u')
            dec_u = read_yuv_plane(dec_path, w, h, 'u')
            psnr_u = compute_psnr(ref_u, dec_u)

            ref_v = read_yuv_plane(ref_path, w, h, 'v')
            dec_v = read_yuv_plane(dec_path, w, h, 'v')
            psnr_v = compute_psnr(ref_v, dec_v)

            # Compute SSIM on luma
            ssim = compute_ssim(ref_y, dec_y)

            rows.append({
                'codec': codec,
                'clip': clip,
                'qp': qp,
                'width': w,
                'height': h,
                'psnr_y': f'{psnr_y:.2f}',
                'psnr_u': f'{psnr_u:.2f}',
                'psnr_v': f'{psnr_v:.2f}',
                'ssim_y': f'{ssim:.4f}',
                'avg_psnr_y': row.get('avg_psnr_y', ''),
                'avg_bitrate_kbps': row.get('avg_bitrate_kbps', ''),
                'total_bytes': row.get('total_bytes', ''),
                'encode_fps': row.get('encode_fps', ''),
            })

            print(f"  {codec} {clip} QP={qp}: PSNR-Y={psnr_y:.2f} PSNR-U={psnr_u:.2f} "
                  f"PSNR-V={psnr_v:.2f} SSIM={ssim:.4f}")

    if rows:
        with open(quality_csv, 'w', newline='') as f:
            writer = csv.DictWriter(f, fieldnames=rows[0].keys())
            writer.writeheader()
            writer.writerows(rows)
        print(f"\nQuality metrics saved to {quality_csv}")
    else:
        print("No quality metrics computed.")


if __name__ == '__main__':
    main()
