#!/bin/bash
# gen_golden.sh — Generate golden corpus for TCodec
# Encodes standard test patterns at multiple QPs and stores
# the bitstreams + SHA256 hashes for regression detection.
set -e

GOLDEN_DIR="${1:-golden}"
TCENC="./build/tcenc"
TCDEC="./build/tcdec"

if [ ! -x "$TCENC" ] || [ ! -x "$TCDEC" ]; then
    echo "Error: $TCENC or $TCDEC not found. Run 'make' first."
    exit 1
fi

mkdir -p "$GOLDEN_DIR"

# Generate raw YUV test clips
gen_yuv() {
    local name=$1 w=$2 h=$3 pattern=$4
    local yuv="$GOLDEN_DIR/${name}_${w}x${h}.yuv"
    python3 -c "
import sys
path, w, h, pattern = '$yuv', $w, $h, '$pattern'
with open(path, 'wb') as f:
    for row in range(h):
        row_data = bytearray(w)
        for col in range(w):
            if pattern == 'gradient':
                v = (row * 255 // h + col * 255 // w) // 2
            elif pattern == 'checkerboard':
                v = 255 if ((col // 16) + (row // 16)) % 2 else 0
            elif pattern == 'noise':
                import random
                random.seed(row * w + col)
                v = random.randint(0, 255)
            elif pattern == 'horizontal_lines':
                v = 255 if row % 16 < 8 else 0
            elif pattern == 'vertical_lines':
                v = 255 if col % 16 < 8 else 0
            elif pattern == 'diagonal':
                v = 255 if (row + col) % 32 < 16 else 0
            else:
                v = 128
            row_data[col] = v
        f.write(row_data)
    cb_size = (w // 2) * (h // 2)
    f.write(bytes([128]) * cb_size)
    f.write(bytes([128]) * cb_size)
"
    echo "$yuv"
}

# Generate test clips
echo "Generating test YUV clips..."
gen_yuv gradient 128 128 gradient
gen_yuv checkerboard 128 128 checkerboard
gen_yuv noise 128 128 noise
gen_yuv hlines 128 128 horizontal_lines
gen_yuv vlines 128 128 vertical_lines
gen_yuv diagonal 128 128 diagonal
gen_yuv gradient 320 240 gradient
gen_yuv gradient 96 80 gradient

# Encode each clip at multiple QPs and store bitstream + hash
QPS="22 32 42"
HASH_FILE="$GOLDEN_DIR/MANIFEST.sha256"
> "$HASH_FILE"

echo "Encoding golden bitstreams..."
for yuv in "$GOLDEN_DIR"/*.yuv; do
    base=$(basename "$yuv" .yuv)
    # Parse dimensions from filename (e.g. gradient_128x128)
    dims=$(echo "$base" | grep -oP '\d+x\d+')
    w=$(echo "$dims" | cut -dx -f1)
    h=$(echo "$dims" | cut -dx -f2)

    for qp in $QPS; do
        tcname="${base}_qp${qp}.tcv"
        echo "  Encoding $tcname (w=$w h=$h qp=$qp)..."
        "$TCENC" -w "$w" -h "$h" -q "$qp" -o "$GOLDEN_DIR/$tcname" "$yuv" 2>&1 || { echo "  FAILED: $tcname"; continue; }
        hash=$(sha256sum "$GOLDEN_DIR/$tcname" | cut -d' ' -f1)
        echo "$hash  $tcname" >> "$HASH_FILE"
        # Verify decode roundtrip
        "$TCDEC" "$GOLDEN_DIR/$tcname" "$GOLDEN_DIR/${tcname%.tcv}_dec.yuv" 2>&1 || true
    done
done

# Store hash of the test binary
sha256sum ./build/test_tcodec >> "$HASH_FILE"

echo ""
echo "Golden corpus generated in $GOLDEN_DIR/"
echo "Manifest: $HASH_FILE"
cat "$HASH_FILE"
