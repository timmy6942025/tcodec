#!/bin/bash
# run_benchmark.sh — TCodec benchmark harness
#
# Runs encode/decode across multiple codecs, QPs, and test clips,
# producing CSV output for quality evaluation and BD-rate computation.
#
# Usage: ./tools/run_benchmark.sh [OUTPUT_DIR]
#
# Output:
#   OUTPUT_DIR/results.csv    — per-frame bitrate and PSNR
#   OUTPUT_DIR/summary.csv    — per-clip average bitrate and PSNR
#   OUTPUT_DIR/encoded/       — encoded bitstreams
#   OUTPUT_DIR/decoded/       — decoded YUV files
#
# Requires: tcenc, tcdec, x264, x265, SvtAv1EncApp, ffmpeg

set -e

OUTDIR="${1:-benchmark_results}"
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
TCENC="./build/tcenc"
TCDEC="./build/tcdec"
CLIPS_DIR="golden"
FPS=30

mkdir -p "$OUTDIR/encoded" "$OUTDIR/decoded"

# ── Generate test clips if not present ──────────────────────
if [ ! -d "$CLIPS_DIR" ] || [ -z "$(ls "$CLIPS_DIR"/*.yuv 2>/dev/null)" ]; then
    echo "Generating test clips..."
    bash "$SCRIPT_DIR/gen_golden.sh" "$CLIPS_DIR"
fi

# ── Results CSV header ──────────────────────────────────────
RESULTS="$OUTDIR/results.csv"
echo "codec,clip,qp,width,height,frame,psnr_y,psnr_cb,psnr_cr,bitrate_kbps,size_bytes" > "$RESULTS"

SUMMARY="$OUTDIR/summary.csv"
echo "codec,clip,qp,width,height,avg_psnr_y,avg_bitrate_kbps,total_bytes,num_frames,encode_fps" > "$SUMMARY"

# ── Helper: compute PSNR using ffmpeg ───────────────────────
compute_psnr() {
    local ref_yuv="$1" dec_yuv="$2" w="$3" h="$4" out_prefix="$5"
    # ffmpeg lavfi ssim/psnr filter — outputs per-frame metrics to stderr
    ffmpeg -y -f rawvideo -pix_fmt yuv420p -s "${w}x${h}" -i "$dec_yuv" \
           -f rawvideo -pix_fmt yuv420p -s "${w}x${h}" -i "$ref_yuv" \
           -lavfi "psnr	stats_file=${out_prefix}_psnr.log" \
           -f null - 2>/dev/null || true
    # Parse per-frame PSNR from log
    if [ -f "${out_prefix}_psnr.log" ]; then
        cat "${out_prefix}_psnr.log"
    else
        echo "# PSNR log not found for $out_prefix"
    fi
}

# ── Encode+decode+measure for one clip at one QP ───────────
run_single() {
    local codec="$1" clip="$2" w="$3" h="$4" qp="$5"
    local clip_base=$(basename "$clip" .yuv)
    local tag="${codec}_${clip_base}_qp${qp}"
    local enc_out="$OUTDIR/encoded/${tag}"
    local dec_out="$OUTDIR/decoded/${tag}.yuv"

    local start_s=$(date +%s)
    local ok=0

    case "$codec" in
        tcodec)
            if [ -x "$TCENC" ] && [ -x "$TCDEC" ]; then
                "$TCENC" -w "$w" -h "$h" -q "$qp" -f "$FPS" -o "$enc_out.tcv" "$clip" 2>/dev/null && \
                "$TCDEC" "$enc_out.tcv" "$dec_out" 2>/dev/null && ok=1
            fi
            ;;
        x264)
            if which x264 >/dev/null 2>&1; then
                # x264 QP maps 1:1 to H.264 QP (0-51); cap at 51
                local xqp=$((qp > 51 ? 51 : qp))
                x264 --qp "$xqp" --keyint 30 --fps "$FPS" \
                     --input-res "${w}x${h}" --output "$enc_out.264" \
                     --quiet "$clip" 2>/dev/null && \
                ffmpeg -y -i "$enc_out.264" -f rawvideo -pix_fmt yuv420p "$dec_out" 2>/dev/null && ok=1
            fi
            ;;
        x265)
            if which x265 >/dev/null 2>&1; then
                local xqp=$((qp > 51 ? 51 : qp))
                x265 --qp "$xqp" --keyint 30 --fps "$FPS" \
                     --input-res "${w}x${h}" --output "$enc_out.265" \
                     "$clip" 2>/dev/null && \
                ffmpeg -y -i "$enc_out.265" -f rawvideo -pix_fmt yuv420p "$dec_out" 2>/dev/null && ok=1
            fi
            ;;
        svt-av1)
            if which SvtAv1EncApp >/dev/null 2>&1; then
                # SVT-AV1 uses CRF not QP; approximate: CRF ≈ QP * 0.8
                local crf=$((qp * 8 / 10))
                SvtAv1EncApp -w "$w" -h "$h" -q "$crf" \
                             -i "$clip" -b "$enc_out.obu" \
                             --keyint 30 --fps "$FPS" 2>/dev/null && \
                ffmpeg -y -i "$enc_out.obu" -f rawvideo -pix_fmt yuv420p "$dec_out" 2>/dev/null && ok=1
            fi
            ;;
    esac

    local end_s=$(date +%s)
    local elapsed_s=$((end_s - start_s))
    local elapsed_ms=$((elapsed_s * 1000))

    if [ "$ok" -eq 1 ] && [ -f "$dec_out" ]; then
        # Compute PSNR via ffmpeg
        local psnr_log="$OUTDIR/${tag}_psnr.log"
        ffmpeg -y -f rawvideo -pix_fmt yuv420p -s "${w}x${h}" -i "$dec_out" \
               -f rawvideo -pix_fmt yuv420p -s "${w}x${h}" -i "$clip" \
               -lavfi "psnrstats_file=${psnr_log}" \
               -f null - 2>/dev/null || true

        local size_bytes=0
        if [ -f "$enc_out.tcv" ]; then size_bytes=$(stat -c%s "$enc_out.tcv" 2>/dev/null || echo 0)
        elif [ -f "$enc_out.264" ]; then size_bytes=$(stat -c%s "$enc_out.264" 2>/dev/null || echo 0)
        elif [ -f "$enc_out.265" ]; then size_bytes=$(stat -c%s "$enc_out.265" 2>/dev/null || echo 0)
        elif [ -f "$enc_out.obu" ]; then size_bytes=$(stat -c%s "$enc_out.obu" 2>/dev/null || echo 0)
        fi

        # Parse average PSNR from log, or compute with Python
        local avg_psnr="0.0"
        if [ -f "$psnr_log" ] && grep -q 'psnr_y' "$psnr_log" 2>/dev/null; then
            avg_psnr=$(grep 'psnr_y' "$psnr_log" | tail -1 | grep -oP 'psnr_y:[\d.]+' | cut -d: -f2)
        else
            # Fallback: compute PSNR with python3
            avg_psnr=$(python3 -c "
import numpy as np, sys
w, h = $w, $h
ref = np.fromfile('$clip', dtype=np.uint8, count=w*h)
dec = np.fromfile('$dec_out', dtype=np.uint8, count=w*h)
if ref.size == 0 or dec.size == 0:
    print('0.0'); sys.exit()
mse = np.mean((ref.astype(float) - dec.astype(float))**2)
if mse == 0: print('100.0')
else: print(f'{10*np.log10(255**2/mse):.2f}')
" 2>/dev/null || echo "0.0")
        fi

        local n_frames=1  # Single-frame clips for now
        local bitrate_kbps=0
        if [ "$size_bytes" -gt 0 ]; then
            bitrate_kbps=$((size_bytes * 8 * FPS / 1000))
            if [ "$bitrate_kbps" -eq 0 ] && [ "$size_bytes" -gt 0 ]; then
                # Integer overflow or very small — use python for float
                bitrate_kbps=$(python3 -c "print(int($size_bytes * 8 * $FPS / 1000))" 2>/dev/null || echo "0")
            fi
        fi
        local encode_fps=0
        if [ "$elapsed_ms" -gt 0 ]; then
            encode_fps=$(python3 -c "print(int($n_frames * 1000 / $elapsed_ms))" 2>/dev/null || echo "0")
        fi

        echo "$codec,$clip_base,$qp,$w,$h,$avg_psnr,$bitrate_kbps,$size_bytes,$n_frames,$encode_fps" >> "$SUMMARY"
        echo "  ✓ $codec $clip_base QP=$qp PSNR=${avg_psnr}dB size=${size_bytes}B"
    else
        echo "  ✗ $codec $clip_base QP=$qp FAILED"
    fi
}

# ── Main benchmark loop ─────────────────────────────────────
# Default codecs and QPs — override with env vars
# TCODEC_BENCH_CODECS="tcodec x264" TCODEC_BENCH_QPS="22 32 42" ./tools/run_benchmark.sh
CODECS="${TCODEC_BENCH_CODECS:-tcodec x264 x265}"
QPS="${TCODEC_BENCH_QPS:-22 32 42}"

echo "════════════════════════════════════════════════════════"
echo "  TCodec Benchmark Harness"
echo "════════════════════════════════════════════════════════"
echo "  Codecs: $CODECS"
echo "  QPs:    $QPS"
echo "  Output: $OUTDIR/"
echo ""

for clip in "$CLIPS_DIR"/*.yuv; do
    clip_base=$(basename "$clip" .yuv)
    # Skip decoded files from previous golden corpus runs
    case "$clip_base" in *_dec) continue ;; esac
    # Skip if this is not a source clip (has extra suffixes from encoding)
    # Source clips have format: <pattern>_<W>x<H>.yuv
    if ! echo "$clip_base" | grep -qP '^\w+_\d+x\d+$'; then continue; fi
    # Parse dimensions from filename
    dims=$(echo "$clip_base" | grep -oP '\d+x\d+')
    w=$(echo "$dims" | cut -dx -f1)
    h=$(echo "$dims" | cut -dx -f2)

    echo "── Clip: $clip_base (${w}x${h}) ──"
    for codec in $CODECS; do
        for qp in $QPS; do
            run_single "$codec" "$clip" "$w" "$h" "$qp"
        done
    done
done

echo ""
echo "════════════════════════════════════════════════════════"
echo "  Results saved to:"
echo "    $SUMMARY"
echo "    $RESULTS"
echo ""
echo "  Next steps:"
echo "    python3 tools/evaluate_quality.py $OUTDIR"
echo "    python3 tools/bd_rate.py $OUTDIR"
echo "    python3 tools/plot_rd.py $OUTDIR"
echo "════════════════════════════════════════════════════════"
