# TCodec Bitstream Syntax — Version 0

**Status**: Working prototype. Not yet frozen.  
**Bitstream Version**: 0  
**Magic**: `0x54 0x43 0x56` ("TCV")

---

## 1. Container Format

A TCodec bitstream file (`.tcv`) consists of a sequence of **packets**,
one per frame. Each packet is prefixed with a 4-byte little-endian size:

```
┌─────────────┐
│ uint32 size  │  4 bytes — size of the following frame data
├─────────────┤
│ Frame data   │  `size` bytes — one encoded frame
├─────────────┤
│ uint32 size  │  Next frame...
│ Frame data   │
│ ...          │
└─────────────┘
```

**NOTE**: The size prefix is written by the CLI tools (`tcenc`/`tcdec`),
not by the codec library itself. The library API operates on individual
frame packets without the size prefix.

---

## 2. Frame Data Layout

Each frame packet contains:

```
┌──────────────────────┐
│ Frame Header         │  12 bytes fixed
├──────────────────────┤
│ CTU Row 0            │
│   CTU (0,0) data     │
│   CTU (1,0) data     │
│   ...                │
├──────────────────────┤
│ CTU Row 1            │
│   ...                │
├──────────────────────┤
│ ...                  │
└──────────────────────┘
```

CTUs are coded in **raster scan order**: left-to-right within a row,
top-to-bottom across rows. All blocks within a CTU are coded before
moving to the next CTU.

---

## 3. Frame Header (12 bytes = 96 bits)

| Bit Offset | Width | Field | Description |
|-----------|-------|-------|-------------|
| 0–7 | 8 | `magic[0]` | `0x54` ('T') |
| 8–15 | 8 | `magic[1]` | `0x43` ('C') |
| 16–23 | 8 | `magic[2]` | `0x56` ('V') |
| 24–31 | 8 | `version` | `0x00` (current version) |
| 32–47 | 16 | `width` | Frame width in pixels (1–4096) |
| 48–63 | 16 | `height` | Frame height in pixels (1–4096) |
| 64–71 | 8 | `flags` | Bitfield (see below) |
| 72–79 | 8 | `qp_delta` | Signed offset from default QP (32) |
| 80–87 | 8 | `frame_num` | Frame counter, low 8 bits |
| 88–95 | 8 | `reserved` | `0x00` (must be ignored by decoders) |

After the header, the bitstream is **byte-aligned** (padding bits to the
next byte boundary).

### 3.1 Flags Byte

| Bit | Mask | Field | Description |
|-----|------|-------|-------------|
| 7 | `0x80` | `key_frame` | 1 = I-frame (intra-only), 0 = P-frame |
| 6 | `0x40` | `wpp` | 1 = WPP row entry points present |
| 3–5 | — | `reserved` | Must be 0 |
| 2–3 | `0x0C` | `tile_cols_log2` | log2 of tile columns (0 = 1 col) |
| 0–1 | `0x03` | `tile_rows_log2` | log2 of tile rows (0 = 1 row) |

**NOTE**: Tile fields are coded but **not actually used** by the
encoder or decoder. The entire frame is processed as a single tile.
These bits are reserved for future tile-based parallelism.

### 3.2 Derived Fields

Decoders derive these from the header:

- `frame_type` = KEY if `key_frame` bit set, else INTER
- `qp` = `TC_QP_DEFAULT + (int8_t)qp_delta` = `32 + (int8_t)qp_delta`
  - Clamped to [TC_QP_MIN, TC_QP_MAX] = [0, 63] by the decoder
  - qp_delta is stored as uint8_t but was encoded as `(uint8_t)(int8_t)(qp - 32)`,
    so values > 127 represent negative deltas (QP < 32)
- `tile_cols` = `1 << tile_cols_log2`
- `tile_rows` = `1 << tile_rows_log2`

---

## 4. CTU Data

Each CTU contains data for all 8×8 blocks within it (8×8 = 64 blocks
per CTU). Blocks that extend beyond the frame boundaries are **skipped**
entirely (no data written or read).

### 4.1 Per-CTU Structure

```
For each 8×8 block (by = 0..7, bx = 0..7):
    if block is outside frame: skip
    dct_size_flag: 1 bit           (0 = 8×8 WHT, 1 = 4×4 WHT)
    block_data (see below)
```

### 4.2 Per-Block Data

#### 4.2.1 Mode Decision

**Key frames** (I-frames): No mode flag. All blocks are intra.

**Inter frames** (P-frames):
```
block_mode: 2 bits                 (0=skip, 1=inter, 2=intra, 3=merge)
```

| Value | Mode | Residual | MV Source |
|-------|------|----------|-----------|
| 0 | Skip | Zero | MVD + ref_idx signaled |
| 1 | Inter | Non-zero | MVD + ref_idx signaled |
| 2 | Intra | Non-zero | None (intra prediction) |
| 3 | Merge | Zero | Derived from median of spatial neighbors |

#### 4.2.2 Intra Block

Read only when `block_mode == 2` (intra) or on key frames.

```
intra_mode: 5 bits (unsigned)     (0..17, see intra mode table)
```

If `intra_mode >= 18`, decoder falls back to DC (mode 1).

| Value | Name | Direction | Displacement |
|-------|------|-----------|-------------|
| 0 | Planar | Bilinear | — |
| 1 | DC | Average | — |
| 2 | Angular NE | ~45° vertical | +1.0 px/row |
| 3 | Angular NNE | ~26° vertical | +0.5 px/row |
| 4 | Angular NNW | ~11° vertical | +0.19 px/row |
| 5 | Angular N (vert) | 0° vertical | 0 px/row |
| 6 | Angular NWW | ~-11° vertical | -0.19 px/row |
| 7 | Angular NW | ~-26° vertical | -0.5 px/row |
| 8 | Angular WN | ~-45° vertical | -1.0 px/row |
| 9 | Angular EN | ~45° horizontal | +1.0 px/col |
| 10 | Angular EEN | ~26° horizontal | +0.5 px/col |
| 11 | Angular EE | ~11° horizontal | +0.19 px/col |
| 12 | Angular E (horiz) | 0° horizontal | 0 px/col |
| 13 | Angular WW | ~-11° horizontal | -0.19 px/col |
| 14 | Angular WWN | ~-26° horizontal | -0.5 px/col |
| 15 | Angular WN (horiz) | ~-45° horizontal | -1.0 px/col |
| 16 | Angular NNW (horiz) | ~-56° horizontal | -1.5 px/col |
| 17 | Angular NNE (horiz) | ~56° horizontal | +1.5 px/col |

#### 4.2.3 Inter/Skip Block (mode 0 or 1)

Read only when `block_mode == 0` (skip) or `block_mode == 1` (inter).

```
ref_idx: 2 bits (unsigned)         Reference frame index (0..3)
mvd_x: se(v)  (signed Exp-Golomb)  Motion vector difference X
mvd_y: se(v)  (signed Exp-Golomb)  Motion vector difference Y
```

The actual motion vector is reconstructed as:
- `predictor_mv` = median of spatial neighbors (left, above, above-right)
  - Default: `{frame_x * 4, frame_y * 4}` (collocated) if no inter neighbors
- `mv.x = mvd_x + predictor_mv.x`  (quarter-pel)
- `mv.y = mvd_y + predictor_mv.y`  (quarter-pel)

MVD is coded **relative to the median predictor**, not the collocated
position. This produces significantly smaller MVDs when spatial
neighbors are available, since the predictor captures the local motion
field. The median computation uses the same neighbor selection and
sorting as merge mode (see §4.2.4).

**Skip blocks** (mode 0) carry the actual MV via MVD — skip means
zero residual, not zero MV. The decoder needs the correct MV to
produce the right prediction.

**Safety**: If `ref_idx >= TC_REF_FRAMES`, the decoder clamps to 0.

#### 4.2.4 Merge Block (mode 3)

No ref_idx or MVD is signaled. The MV is derived from the median
of spatial neighbors' MVs, identical to the predictor computation:

```
// No additional bits read for merge mode
mv = median(left.mv, above.mv, above_right.mv)
```

Neighbor selection rules:
1. Left neighbor: block at (bx-1, by) if bx > 0 and that block is not intra
2. Above neighbor: block at (bx, by-1) if by > 0 and that block is not intra
3. Above-right neighbor: block at (bx+1, by-1) if by > 0 and bx < 7 and not intra

If any neighbor is unavailable, it is replaced by another available
neighbor. If no inter neighbors exist, the default is the collocated
position `{frame_x * 4, frame_y * 4}`.

The median is computed by sorting the three x-components and taking
the middle value, then similarly for y-components.

Merge always uses reference frame index 0 (most recent DPB entry).

#### 4.2.5 Coefficient Data (Luma)

**If dct_size_flag = 0** (8×8 WHT):

```
coeff_data: 64 coefficients coded as one block
    last_nz_plus1: ue(v)    Last nonzero position + 1 (0 = all zero)
    for i = last_nz down to 0 (zigzag order):
        magnitude: ue(v)     Absolute coefficient value
        if magnitude > 0:
            sign: 1 bit      0 = positive, 1 = negative
```

**If dct_size_flag = 1** (4×4 WHT):

Four separate sub-blocks, each 16 coefficients, coded in order:
```
for sy = 0..1, sx = 0..1:
    sub_coeff_data: 16 coefficients (same format as above, 4×4 zigzag)
```

The 4×4 sub-blocks are extracted from the 8×8 residual in raster order
within the block:
- Sub-block (0,0): rows 0–3, cols 0–3
- Sub-block (1,0): rows 0–3, cols 4–7
- Sub-block (0,1): rows 4–7, cols 0–3
- Sub-block (1,1): rows 4–7, cols 4–7

#### 4.2.6 Coefficient Data (Chroma)

After luma, each 8×8 luma block has two chroma components (Cb, Cr).
Each chroma component is a 4×4 block:

```
for comp = 0..1:    (0 = Cb, 1 = Cr)
    chroma_coeff_data: 16 coefficients (4×4 zigzag, same format)
```

**Chroma QP** = `clip(luma_qp + 1, 0, 63)`

**Chroma prediction**:
- **Intra blocks**: CfL (Chroma-from-Luma) — uses reconstructed luma
  to predict chroma: `c_pred = c_dc + ((luma_val - luma_avg) >> 3)`,
  where `c_dc` is derived from neighboring chroma samples, `luma_avg`
  is the average of the corresponding 4×4 luma block, and alpha ≈ 0.125.
- **Inter/skip/merge blocks**: Fixed DC value of 128.

**NOTE**: Chroma always uses 4×4 WHT, regardless of the luma dct_size_flag.
Chroma always uses `tc_quantize()`/`tc_dequantize()` with `band=0` (no JND
weighting), unlike luma which applies JND weighting inline.

---

## 5. Coefficient Coding (tANS)

### 5.1 tANS Encoding

Coefficient data is encoded using tANS (tabled Asymmetric Numeral Systems)
via `tc_tans_enc_coeffs()` / `tc_tans_dec_coeffs()`. tANS provides better
compression than universal codes (Exp-Golomb) by adapting to the actual
coefficient distribution.

**Current status**: tANS is used for all coefficient coding in the
pipeline, but context modeling is not yet implemented. The encoder and
decoder allocate context structures but use default/fixed probability
tables. This means tANS is functioning but not yet reaching its full
compression potential — context modeling is planned for Phase 3.

### 5.2 Motion Vector Coding (Exp-Golomb)

MVD components (mvd_x, mvd_y) are still coded as signed Exp-Golomb (se(v)).
This is a less critical compression path since MVD values are typically
small after median predictor differencing.

### 5.3 Exp-Golomb Reference

Unsigned (ue):
```
code_num = val + 1
leading_zeros = floor(log2(code_num))
bit pattern: [0 × leading_zeros] [1] [suffix of leading_zeros bits]
```

| Value | Code | Bits |
|-------|------|------|
| 0 | `1` | 1 |
| 1 | `010` | 3 |
| 2 | `011` | 3 |
| 3 | `00100` | 5 |
| n | `0×floor(log2(n+1)) 1 (n+1)%2^floor(log2(n+1))` | 2×floor(log2(n+1))+1 |

Signed (se):

| ue value | se value |
|----------|----------|
| 0 | 0 |
| 1 | +1 |
| 2 | −1 |
| 3 | +2 |
| 4 | −2 |
| n | (−1)^(n+1) × ceil(n/2) |

Formula: `se = (mapped & 1) ? (mapped+1)/2 : -(mapped/2)`
where `mapped = ue(val)`.

---

## 6. Zigzag Scan Order

### 6.1 4×4 Zigzag

```
 0  1  4  8
 5  2  3  6
 9 12 13 10
 7 11 14 15
```

Position index → raster index mapping:
```c
{0, 1, 4, 8, 5, 2, 3, 6, 9, 12, 13, 10, 7, 11, 14, 15}
```

### 6.2 8×8 Zigzag

```
 0  1  8 16  9  2  3 10
17 24 32 25 18 11  4  5
12 19 26 33 40 48 41 34
27 20 13  6  7 14 21 28
35 42 49 56 57 50 43 36
29 22 15 23 30 37 44 51
58 59 52 45 38 31 39 46
53 60 61 54 47 55 62 63
```

Position index → raster index mapping:
```c
{0, 1, 8, 16, 9, 2, 3, 10, 17, 24, 32, 25, 18, 11, 4, 5,
 12, 19, 26, 33, 40, 48, 41, 34, 27, 20, 13, 6, 7, 14, 21, 28,
 35, 42, 49, 56, 57, 50, 43, 36, 29, 22, 15, 23, 30, 37, 44, 51,
 58, 59, 52, 45, 38, 31, 39, 46, 53, 60, 61, 54, 47, 55, 62, 63}
```

---

## 7. Complete Bitstream Syntax (Pseudo-code)

```
frame_packet() {
    // Header
    magic[0]       = u(8)    // 0x54
    magic[1]       = u(8)    // 0x43
    magic[2]       = u(8)    // 0x56
    version        = u(8)    // 0
    width          = u(16)
    height         = u(16)
    flags          = u(8)
    qp_delta       = u(8)
    frame_num      = u(8)
    reserved       = u(8)
    byte_align()

    // Derived
    is_key = (flags >> 7) & 1
    qp = clip(32 + qp_delta, 0, 63)

    // CTU data
    for (row = 0; row < num_ctu_rows; row++) {
        for (col = 0; col < num_ctu_cols; col++) {
            ctu_data(row, col)
        }
    }
}

ctu_data(row, col) {
    for (by = 0; by < 8; by++) {
        for (bx = 0; bx < 8; bx++) {
            blk_x = col * 64 + bx * 8
            blk_y = row * 64 + by * 8
            if (blk_x + 8 > width || blk_y + 8 > height)
                continue    // Skip out-of-bounds blocks

            // Mode decision (P-frames only)
            if (!is_key) {
                block_mode = u(2)   // 0=skip, 1=inter, 2=intra, 3=merge
            } else {
                block_mode = 2      // KEY frames: always intra
            }

            // Prediction data
            if (block_mode == 2) {         // intra
                intra_mode = u(5)   // 0..17
            } else if (block_mode == 3) {  // merge
                // MV derived from median of spatial neighbors
                // No ref_idx or MVD signaled
            } else {                       // skip (0) or inter (1)
                ref_idx = u(2)     // Reference frame index (0..3)
                mvd_x = se()        // Signed Exp-Golomb
                mvd_y = se()        // Signed Exp-Golomb
            }

            // Luma coefficients (skip/merge have no coefficients)
            if (block_mode == 0 || block_mode == 3) {
                // Skip/merge: zero residual, no coefficient data
            } else {
                dct_size_flag = u(1)   // 0=8×8, 1=4×4
                if (dct_size_flag == 1) {
                // Four 4×4 sub-blocks
                for (sy = 0; sy < 2; sy++) {
                    for (sx = 0; sx < 2; sx++) {
                        coeff_block(16)   // 4×4 zigzag
                    }
                }
            } else {
                coeff_block(64)      // 8×8 zigzag
                } // end if dct_size_flag
            } // end if block_mode

            // Chroma coefficients
            chroma_qp = clip(qp + 1, 0, 63)
            for (comp = 0; comp < 2; comp++) {
                coeff_block(16)      // 4×4 zigzag (Cb then Cr)
            }
        }
    }
}

coeff_block(n) {
    // Coefficients coded via tANS (see §5.1)
    // tc_tans_enc_coeffs / tc_tans_dec_coeffs
    // Contextless tANS with fixed probability tables
    tans_coeff_block(n)              // n coefficients in zigzag order
}
```

---

## 8. Bitstream Compliance

### 8.1 Decoder Error Handling

Current decoder behavior on malformed bitstreams:

| Condition | Behavior |
|-----------|----------|
| Invalid magic | Return `TC_ERR_BITSTREAM` |
| Invalid version | Accepted (not checked beyond reading) |
| EOF mid-header | Return `TC_ERR_EOF` |
| Invalid intra mode (≥18) | Clamp to DC (mode 1) |
| `last_nz_plus1 > n` | Clamp to `n - 1` |
| Out-of-bounds MV | Fallback to DC(128) prediction — safe degradation |
| Invalid ref_idx (≥4) | Clamp to 0 |
| Truncated coefficient data | Reads past buffer — **no safety check** |

### 8.2 Known Bitstream Deficiencies

1. **No version negotiation**: Decoder cannot reject unknown versions
2. **No CRC or checksum**: Corruption is undetectable except by magic
3. **No random access points**: Must decode from first frame
4. **No error resilience**: A single bit error corrupts all subsequent data
5. **Fixed header size**: No extension mechanism for new fields
6. **WPP entry points**: When TC_FLAG_WPP is set, an entry point table follows the header
7. **Reference frame signaling**: 4 DPB slots, ref_idx coded per block (2 bits)
8. **No bit-rate signaling**: Target bitrate not stored in bitstream
9. **NEON/scalar output divergence**: NEON deblock uses different filter strength than scalar
10. **WPP/sequential bitstream divergence**: WPP bitstreams byte-align per row (sequential do not)

---

## 9. Encoding Example

For a 128×128 frame at QP 32, P-frame:

```
Header (12 bytes):
  54 43 56 00   // Magic + version
  00 80 00 80   // Width=128, Height=128
  00 00 00 00   // flags=0 (P-frame), qp_delta=0 (QP=32), frame_num=0, reserved=0

CTU 0,0 (64×64 = 64 blocks):
  Block (0,0): dct_flag=0, is_intra=1, mode=1(DC), coeffs...
  Block (1,0): dct_flag=0, is_intra=0, mvd_x=0, mvd_y=-4, coeffs...
  Block (2,0): dct_flag=1, is_intra=1, mode=5(vertical), 4×sub-blocks...
  ...

CTU 1,0 (64×64):
  ...

CTU 0,1 (64×64):
  ...

(4 CTUs total for 128×128 frame)
```

---

## 10. Future Bitstream Changes (Not Yet Implemented)

The following changes are needed per the Master Plan:

- **Versioned syntax**: Allow decoders to reject unknown versions
- **Profile/level signaling**: Indicate supported tool subset
- **Tool flags**: Bitfield indicating which optional tools are active
- **Random access points**: Mark frames that can be decoded independently
- **CRC/checksum**: Detect bitstream corruption
- **Tile boundaries**: Explicit tile start/end markers
- **Multiple reference signaling**: Which DPB slots to use
- **Extended header**: Variable-length header with optional sections
- **Bitrate/target metadata**: Store encoding parameters
- **Packetization**: Framing for network transport
