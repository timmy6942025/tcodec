# TCodec Specification — Version 0

**Status**: Working prototype, not production-grade.  
**Bitstream Version**: 0 (not yet frozen)  
**Last Updated**: Based on code audit of current implementation.

This document specifies what TCodec **actually implements**. Where the
README makes claims not backed by code, this spec marks them as
**PLANNED** or **STUB**.

---

## 1. Terminology

| Term | Definition |
|------|-----------|
| **CTU** (Coding Tree Unit) | 64×64 luma pixel region. The largest coding structure. |
| **Block** | 8×8 luma pixel region. The fundamental processing unit for prediction and residual coding. |
| **Sub-block** | 4×4 luma pixel region. Used for WHT when an 8×8 block is in 4×4 mode. |
| **Transform Unit (TU)** | The region a single transform covers: either 4×4 or 8×8. |
| **Prediction Unit (PU)** | Same region as the block (8×8). Intra or inter decision is per-block. |
| **Frame** | One complete image: luma (Y) at full resolution, Cb and Cr at half resolution (4:2:0). |
| **DPB** (Decoded Picture Buffer) | Stores reference frames for inter prediction. Current size: 4 slots. |
| **Profile** | A subset of coding tools. No profiles defined yet. |
| **Level** | Performance constraints. No levels defined yet. |
| **QP** (Quantization Parameter) | Integer 0–63 controlling quantization step size. |
| **MVD** (Motion Vector Difference) | Signed displacement from the median MV predictor, in quarter-pel units. |

---

## 2. Color Space and Subsampling

- **Input/Output**: Planar YCbCr 4:2:0
- **Luma (Y)**: Full resolution (width × height), 8-bit unsigned
- **Chroma (Cb, Cr)**: Half resolution (width/2 × height/2), 8-bit unsigned
- **Strides**: 64-byte aligned for NEON cache-line efficiency
- **RGB conversion**: BT.601 coefficients, 14-bit fixed-point arithmetic

---

## 3. Frame Structure

### 3.1 Frame Types

| Type | Value | Description |
|------|-------|-------------|
| KEY (I-frame) | 0 | Intra-only. All blocks use intra prediction. |
| INTER (P-frame) | 1 | Blocks may use skip, inter, intra, or merge modes. Up to 4 reference frames. |
| BIDIR (B-frame) | 2 | **NOT IMPLEMENTED.** Defined in types but unused. |

### 3.2 Frame Dimensions

- **Maximum**: 4096 × 4096
- **Minimum**: Must be ≥ 8 × 8 (at least one block)
- **Width/Height must be even** (for 4:2:0 chroma subsampling)

### 3.3 CTU Grid

Frame is divided into a grid of CTUs:

- `num_ctu_cols = ceil(width / 64)`
- `num_ctu_rows = ceil(height / 64)`

Partial CTUs at frame boundaries are handled by bounds-checking
individual blocks rather than padding.

---

## 4. Block-Level Coding Tools

### 4.1 Transform Size Selection

Each 8×8 block selects its transform size based on **variance** of the
original luma pixels:

- **Variance > 512** → 4×4 WHT (four sub-blocks per block)
- **Variance ≤ 512** → 8×8 WHT (single transform per block)

The transform size flag is coded in the bitstream (1 bit per block).

**NOTE**: The README calls this "variance → DCT size" but the actual
transform is the **Walsh-Hadamard Transform (WHT)**, not DCT. DCT
functions (`tc_fdct4x4`, `tc_idct4x4`, etc.) exist in the codebase but
are **not used** by the encode/decode pipeline. The WHT is used because
it is self-inverse (H×H = n×I), which guarantees perfect reconstruction
with uniform quantize/dequantize — the H.264 integer DCT requires
position-dependent scaling that the current dequantizer doesn't provide.

### 4.2 Intra Prediction

**18 modes** for luma (5-bit signaling):

| Mode | Name | Direction | Displacement |
|------|------|-----------|-------------|
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

Angular modes use 1/32 pixel precision (5-bit fractional part) for
reference projection, with bilinear interpolation from above or left
reference samples. Vertical angular modes (2–8) project onto the above
reference row; horizontal angular modes (9–17) project onto the left
reference column. When the projection falls outside the available
reference samples, the alternate reference is used (e.g., steep vertical
modes use left column, steep horizontal modes use above row).

**Mode decision**: SAD (Sum of Absolute Differences) between original
and predicted block. Lowest-SAD mode wins. No rate-distortion cost
consideration for mode selection.

**Reference samples**: Collected from reconstructed pixels above and to
the left of the current block. If unavailable (frame boundary), default
value 128 is used. Up to 2×block_size samples are collected in each
direction.

### 4.3 Chroma Prediction

**Intra blocks**: CfL (Chroma-from-Luma) prediction — uses reconstructed
luma pixels to predict chroma. A simple linear model blends the luma
correlation with a DC prediction derived from neighboring chroma samples:
```
c_pred = c_dc + ((luma_val - luma_avg) >> 3)   // alpha ≈ 0.125
```
This provides modest chroma quality improvement over fixed DC(128).

**Inter/skip/merge blocks**: Fixed DC value of 128.

**PLANNED**: Full chroma intra mode set (DM, planar, angular),
stronger CfL alpha estimation.

### 4.4 Inter Prediction

Available only in P-frames (TC_FRAME_INTER).

**Motion estimation**: Hierarchical hexagonal search:
1. Large hex (±16 pixels scaled), 7 points, up to 2 iterations
2. Small hex (±4 pixels), 7 points, up to 3 iterations
3. Diamond refinement (±1 pixel), 5 points, up to 4 iterations
4. Quarter-pel refinement, 9 points, single pass (with bounds checking)

**Search center**: Median of spatial neighbors (left, above, above-right).
If no inter neighbors available, defaults to collocated block position.
This produces significantly smaller MVDs than the old (0,0) start.

**Search range**: Depends on preset:
- Ultrafast: 16 pixels
- Fast/Medium: 32 pixels
- Slow: 64 pixels

**Sub-pel interpolation**: 6-tap luma filter (H.264-style) for half-pel
positions, with bilinear fallback for quarter-pel. Chroma uses bilinear.

**Motion vector coding**: MVD coded relative to the median MV predictor
(spatial neighbors), not collocated position. Signed Exp-Golomb. MV is
in quarter-pel units.

**Reference frames**: Up to 4 DPB slots. Only the SLOW preset searches
all reference frames; other presets use only the most recent (slot 0).
Ref_idx is signaled as 2 bits per block.

**Mode decision**: 2-bit mode field: skip(0), inter(1), intra(2), merge(3).
- **Skip**: Zero residual, MV signaled via MVD
- **Inter**: Non-zero residual, MV signaled via MVD + ref_idx
- **Intra**: Intra prediction, no MV
- **Merge**: Zero residual, MV derived from median of spatial neighbors
  (no ref_idx or MVD signaled — significant bitrate savings)

No rate-distortion optimization. Mode chosen by SAD comparison +
residual zero-check + merge availability.

### 4.5 Skip/Merge Modes

**Implemented**. 2-bit mode field in P-frames:

| Mode | Value | Residual | MV Source | Signaled Bits |
|------|-------|----------|-----------|---------------|
| Skip | 0 | Zero | MVD + ref_idx | mode(2) + ref_idx(2) + MVD(se+se) |
| Inter | 1 | Non-zero | MVD + ref_idx | mode(2) + ref_idx(2) + MVD(se+se) + dct_flag(1) + coeffs |
| Intra | 2 | Non-zero | None (intra pred) | mode(2) + intra_mode(5) + dct_flag(1) + coeffs |
| Merge | 3 | Zero | Median of neighbors | mode(2) only |

**Merge mode** derives the MV from the median of spatial neighbors'
MVs (left, above, above-right), same as the median MV predictor.
No ref_idx or MVD is signaled — this saves 2 + 5-15+ bits per block
when the predictor is accurate, which is common in low-motion content.

**Skip mode** signals the MV explicitly (relative to the predictor)
but carries zero residual. Useful when ME finds a good match but the
residual happens to quantize to all zeros.

---

## 5. Transform and Quantization

### 5.1 Walsh-Hadamard Transform (WHT)

The **actual** transform used in the pipeline (not DCT as README claims).

Properties:
- Self-inverse: H × H = n × I, so forward and inverse use the same butterfly
- No level shift (operates on signed residuals directly)
- 4×4: Two-pass butterfly + right-shift 2 (divide by n=4)
- 8×8: Recursive H8 = |H4 H4; H4 -H4| + right-shift 3 (divide by n=8)

Forward:  Y = H × X × H / n  
Inverse:  X = H × Y × H / n  (IDENTICAL operation)

This guarantees mathematically perfect reconstruction with uniform
quantize/dequantize, at the cost of slightly worse energy compaction
than DCT for high-frequency content.

### 5.2 DCT (Pixel-Mode)

**EXISTS BUT UNUSED** in the pipeline. Available functions:
- `tc_fdct4x4` / `tc_idct4x4` — H.264-style integer transform, with ±128 level shift
- `tc_fdct8x8` / `tc_idct8x8` — AAN fast DCT-II, 14-bit fixed-point rotation constants

These are excluded on NEON builds (`#if !TCODEC_NEON`). The NEON
versions in `transform_neon.c` also exist but are similarly unused by
the pipeline.

### 5.3 Quantization

- **QP range**: 0–63
- **Scale table**: HEVC-style power-of-2 structure (doubles every 6 QP steps)
- **Dead-zone**: Offset = effective_scale / 3 (produces sparser output)
- **Dequantize**: Mid-point reconstruction (adds half-scale to reduce bias)

**JND weighting**: **Active** in the luma encode/decode pipeline:
- `tc_freq_band()` classifies zigzag positions into DC/low/mid/high bands
- `tc_jnd_weight()` returns per-band weights (DC=0.75×, low=0.875×, mid=1.0×, high=1.25×)
- Encoder applies JND weighting inline per-coefficient (not via `tc_quantize()`)
- Decoder applies matching JND weighting inline per-coefficient (not via `tc_dequantize()`)
- Chroma still uses `tc_quantize()`/`tc_dequantize()` with `band=0` (no JND)

### 5.4 Coefficient Coding

**Current method**: tANS (tabled Asymmetric Numeral Systems).

The pipeline uses `tc_tans_enc_coeffs()` / `tc_tans_dec_coeffs()` for all
coefficient coding, replacing the earlier Exp-Golomb path. tANS provides
better compression than universal codes by adapting to the actual
coefficient distribution.

**Note**: The tANS implementation currently uses default/fixed probability
tables — no context modeling is applied. The context structures are
allocated in the encoder/decoder but the modeling logic is not yet
implemented. This is a significant remaining compression gap.

**Zigzag scan**: Standard diagonal scan tables for 4×4 and 8×8.

**Coding structure per block**:
- 4×4 mode: Four separate 16-coefficient sub-blocks are coded
- 8×8 mode: One 64-coefficient block is coded
- Skip/merge blocks: No coefficient data is coded (zero residual)

**PLANNED**: Context modeling for tANS — significance maps,
last-nonzero position, magnitude classes, and separate DC/AC models.
This is expected to provide ~15-30% BD-rate improvement over the
current contextless tANS path.

---

## 6. In-Loop Filtering

### 6.1 Deblocking Filter

Applied after reconstruction, per CTU.

**Edge strength** (0–3) determined by pixel difference at boundary:
- 0: No filter (diff < QP/4)
- 1: Weak filter (diff < QP/2)
- 2: Medium filter (diff < QP)
- 3: Strong filter (diff ≥ QP)

**Luma**:
- Vertical and horizontal edges at every 4-pixel boundary within CTU
- Weak: 4-tap filter on 1 pixel each side, clipped to tc = QP/3
- Strong: 6-tap filter on 3 pixels each side, conditional on flatness

**Chroma**:
- Edges at 8-pixel boundaries only
- Simple averaging with tc-based clipping
- Chroma deblock QP = clip(QP - 1, 0, 63) [scalar] or clip(QP + 1, 0, 63) [NEON]

**NOTE**: The deblock filter uses QP-1 (scalar) for chroma, but the
quantization/dequantization step uses QP+1 for chroma. These are
different operations with different QP offsets. See BITSTREAM.md §4.2.5
for the chroma coefficient coding QP.

**NOTE**: NEON deblock (`tc_deblock_ctu`) is **wired** into the pipeline
via `#if TCODEC_NEON` compilation guards. On NEON builds, the NEON
version replaces the scalar version automatically. The NEON version
uses weak-only filtering (no strong mode) and 8px boundary spacing
(vs 4px in scalar), producing slightly different output.

⚠️ The NEON deblock has **different filtering behavior** than the
scalar version — it only applies weak filter (no strong/medium edge
strength), uses 8-pixel boundary spacing (vs 4-pixel), and has
different chroma logic. This means NEON and scalar builds produce
different decoded output for the same bitstream.

### 6.2 Other Restoration

**NOT IMPLEMENTED**: No deringing, SAO, CDEF, or loop restoration.
Only deblocking exists.

---

## 7. Rate Control

### 7.1 Methods

| Method | Value | Description |
|--------|-------|-------------|
| CQP | 0 | Constant QP — uses configured QP directly |
| CBR | 1 | Constant bitrate — adjusts QP via ρ-domain model |
| VBR | 2 | Variable bitrate — adjusts QP within buffer constraints |

### 7.2 ρ-Domain Model

The fraction of zero coefficients (ρ) correlates with bitrate:
- ρ(QP) = 1 / (1 + exp(-(QP - 28) / 8))  [sigmoid model]
- QP(ρ) = 28 - 8 × ln(ρ / (1 - ρ))  [inverse sigmoid]

CBR/VBR adjust QP based on VBV buffer fullness:
- Buffer > 80% → increase QP by 2
- Buffer > 60% → increase QP by 1
- Buffer < 20% → decrease QP by 2
- Buffer < 40% → decrease QP by 1

**VBV buffer**: 1 second at target bitrate.

**Limitation**: The ρ model is not updated from actual zero-fraction
measurements. The sigmoid parameters are fixed.

---

## 8. Threading and Parallelism

### 8.1 Thread Pool

Infrastructure exists in `threadpool.c`:
- Worker threads pull CTU rows from a shared queue
- WPP dependency: row N waits for row N-1 to complete
- `tc_threadpool_run()` dispatches and waits for completion

### 8.2 Current Status

**ACTIVE**. Both encoder and decoder use WPP (Wavefront Parallel Processing)
via the thread pool when multiple CTU rows exist and threading is enabled.

- **Encoder**: When `use_wpp=1`, each CTU row gets its own bitstream buffer
  and tANS encoder. Rows execute in parallel via `tc_threadpool_run()`,
  then per-row bitstreams are byte-aligned and merged with an entry point
  table. The `TC_FLAG_WPP` flag is set in the frame header.
- **Decoder**: When `TC_FLAG_WPP` is set, the decoder parses the entry point
  table to locate each row's data, initializes per-row bitstream readers,
  and dispatches rows via `tc_threadpool_run()`. Falls back to sequential
  decoding with inter-row byte-alignment skips when threading is unavailable.
- **Bitstream**: WPP frames have an entry point table between header and row data,
  containing byte offsets to each row. Non-WPP frames have no such table.

The `TCODEC_NO_THREADS` compile flag disables all threading code.
WPP bitstreams can still be decoded sequentially when threads are disabled.

---

## 9. NEON Optimization Status

| Module | Scalar | NEON | Wired In |
|--------|--------|------|----------|
| Transform (WHT) | ✅ | ✅ | ✅ (via #if TCODEC_NEON) |
| Transform (DCT) | ✅ | ✅ | ❌ Not used in pipeline |
| SAD | ✅ (guarded) | ✅ | ✅ Scalar guarded by `#if !TCODEC_NEON`; NEON replaces |
| Inter predict | ✅ | ✅ | ❌ NEON uses bilinear not 6-tap; quality mismatch |
| Deblock filter | ✅ (guarded) | ✅ | ✅ Scalar guarded by `#if !TCODEC_NEON`; NEON replaces |
| Color convert | ✅ (guarded) | ✅ | ✅ Luma NEON with 32-bit accum; chroma/inverse scalar |

All active NEON functions are dispatched via the `#if TCODEC_NEON` /
`#if !TCODEC_NEON` compilation guards pattern: the scalar `_internal`
function is replaced by an `extern` declaration on NEON builds, and the
NEON implementation defines the same function name. Public API wrappers
are always compiled and dispatch to whichever `_internal` is linked.

**ACT-4 (NEON inter predict) is deferred** because the NEON version
uses bilinear interpolation while the scalar version uses a proper
6-tap luma filter. Wiring the NEON version would reduce quality.

---

## 10. Implementation Status Summary

### Working (Verified by Tests — 19 tests pass)

- Encode/decode roundtrip for I-frames and P-frames
- Intra prediction (18 modes, SAD mode decision)
- Inter prediction (hex search centered on median predictor, 6-tap luma + bilinear quarter-pel)
- WHT forward/inverse with JND-weighted quantize/dequantize
- tANS coefficient coding (replaces Exp-Golomb in pipeline)
- Skip/merge inter modes (2-bit mode field: skip/inter/intra/merge)
- CfL chroma prediction for intra blocks (DC for inter/skip/merge)
- Multiple reference frames (4 DPB slots; SLOW preset searches all)
- Median MV predictor (used as ME search center AND MVD predictor)
- Scene cut detection (chi-squared histogram distance, forces keyframe)
- Deblocking filter (scalar + NEON via compilation guards)
- Color conversion (RGB ↔ YCbCr, scalar + NEON with 32-bit accumulation)
- NEON SAD (replaces scalar via compilation guards)
- MV bounds checking in decoder (out-of-bounds → DC(128) fallback)
- Sub-pel bounds checking in motion estimation
- QP < 32 correct encoding (int8_t cast for qp_delta)
- ρ-domain rate control (CQP, CBR, VBR modes)
- Frame buffer management, PSNR computation
- Non-CTU-aligned resolution support (96×80 tested)

### Exists But Inactive

- DCT pixel-mode functions (not used in pipeline)
- Thread pool / WPP (allocated but encode/decode loops are sequential)
- NEON inter predict (exists but uses bilinear, not 6-tap; quality mismatch)

### Not Implemented

- Golden corpus of encoded samples and decoded outputs with hash manifest
  (Phase 0 deliverable — see `golden/` directory, to be created)
- Context modeling for entropy coding (contexts allocated, no modeling logic)
- Deringing / SAO / loop restoration
- Film grain synthesis
- Real lookahead
- VBV-constrained rate control (model exists, not validated)
- Profiles and levels
- Bitstream versioning / extensibility
- Container format integration
- Bi-prediction / B-frames
- Quadtree partitioning
- More intra mode RDO (rate-distortion optimized mode decision)
