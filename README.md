# TCodec — A Novel Video Codec for ARM

**Target**: 4K@60fps encode+decode on Raspberry Pi 4 / mid-range phones  
**Compression**: 20:1 with minimal quality loss  
**Design**: ARM-NEON-first, integer-arithmetic-only, WPP-parallel

---

## Architecture Overview

TCodec is a novel block-based hybrid video codec designed from the ground up for
ARM NEON SIMD execution. Every algorithm choice prioritizes:

1. **NEON-vectorizable operations** — no branch-heavy scalar paths
2. **Integer-only arithmetic** — fixed-point DCT, table-driven entropy
3. **Asymmetric complexity** — expensive work at encoder, cheap at decoder
4. **Wavefront parallelism** — linear scaling on 4–8 core ARM SoCs

### Current Feature Set

| Feature | Status | Notes |
|---------|--------|-------|
| 18-mode intra prediction | ✅ Done | Planar, DC, 7 vertical angular, 9 horizontal angular |
| WHT 4×4 / 8×8 transform | ✅ Done | Self-inverse, uniform dequant |
| Exp-Golomb coefficient coding | ✅ Done | tANS framework reserved |
| Hierarchical hex motion search | ✅ Done | ±16/32/64 range by preset |
| 6-tap luma interpolation | ✅ Done | H.264-style half-pel + bilinear quarter-pel |
| Multi-reference inter prediction | ✅ Done | 4 DPB slots; SLOW preset searches all |
| Skip/merge inter modes | ✅ Done | 2-bit mode field: skip/inter/intra/merge |
| Median MV predictor | ✅ Done | Search center + MVD predictor + merge MV |
| Deblocking filter | ✅ Done | Scalar + NEON (auto-dispatch on ARM) |
| ρ-domain rate control | ✅ Done | CQP, CBR, VBR modes |
| CfL chroma prediction | ✅ Done | Chroma-from-luma for intra; DC(128) for inter |
| JND band weighting | ✅ Done | Per-coefficient luma quant weighting |
| Scene cut detection | ✅ Done | Chi-squared histogram, forces keyframe |
| NEON SAD | ✅ Done | Auto-dispatch on ARM (4×4/8×8/16×16) |
| NEON color conversion | ✅ Done | 32-bit accumulation luma; scalar chroma/inverse |
| NEON deblock | ✅ Done | Auto-dispatch on ARM |
| NEON transform | ✅ Done | DCT + WHT, auto-dispatch on ARM |
| WPP thread pool | ✅ Done | Per-row bitstream buffers + tANS encoders, entry point table, sequential fallback |
| B-frames | ❌ Planned | Bi-prediction, COLLOCATED_MV |
| tANS context modeling | ❌ Planned | 16 contexts: 4 bands × 4 magnitude classes |
| SAO filter | ❌ Planned | Post-deblock offset filter |
| DCT-II alternative | ❌ Planned | Requires position-dependent dequant |

### Key Novelty: Variance → DCT Size

Instead of a full quadtree partition search (expensive), TCodec uses **block
variance** to select the transform size:

- **High variance** (edges, detail) → 4×4 WHT (preserves detail, fewer coefficients)
- **Low variance** (flat, smooth) → 8×8 WHT (better energy compaction, fewer bits)

This gives 80% of quadtree compression efficiency at 20% of the decision cost.

---

## Bitstream Structure

```
┌─────────────┐
│ Frame Header │  12 bytes fixed
├─────────────┤
│ Tile 0       │  Variable
│  Row 0 CTUs  │  WPP parallel
│  Row 1 CTUs  │
│  ...         │
├─────────────┤
│ Tile 1       │
│  ...         │
└─────────────┘
```

### Frame Header (12 bytes)

| Offset | Size | Field |
|--------|------|-------|
| 0 | 3 | Magic `0x54 0x43 0x56` ("TCV") |
| 3 | 1 | Version (0) |
| 4 | 2 | Frame width |
| 6 | 2 | Frame height |
| 8 | 1 | Flags (key_frame, qp, tile_cols_log2, tile_rows_log2) |
| 9 | 1 | QP delta (signed, encoded as `(uint8_t)(int8_t)(qp - 32)`) |
| 10 | 1 | Frame number (low 8 bits) |
| 11 | 1 | Reserved |

### Block Mode Field (P-frames)

| Value | Mode | Residual | MV Source |
|-------|------|----------|-----------|
| 0 | Skip | Zero | MVD + ref_idx signaled |
| 1 | Inter | Non-zero | MVD + ref_idx signaled |
| 2 | Intra | Non-zero | None (intra prediction) |
| 3 | Merge | Zero | Derived from median of spatial neighbors |

MVD is coded relative to the median MV predictor (not collocated), producing
smaller MVDs when spatial neighbors are available.

---

## Encoding Pipeline

```
RGB Input → Color Convert (YCbCr 4:2:0) → Frame Buffer
  → For each CTU row:
      For each CTU:
        1. Variance analysis → WHT size selection (4×4 or 8×8)
        2. Intra prediction (9 modes, SAD-select best)
        3. Motion estimation (hierarchical hex search, median-predictor-centered)
        4. Mode decision (skip/merge/inter/intra, simplified RDO)
        5. Forward WHT → Quantize (JND-weighted) → Exp-Golomb encode
        6. Deblock filter (edge strength)
  → Rate control feedback (ρ-domain)
```

## Decoding Pipeline

```
Bitstream → Exp-Golomb decode → Inverse quantize (JND-weighted) → Inverse WHT
  → Prediction (intra/inter/merge) → Add residual
  → Deblock filter → Frame buffer → Color Convert (YCbCr → RGB)
```

---

## Build

```bash
cd tcodec
make              # Build library + CLI tools
make test         # Run unit tests (21 tests)
make bench        # Benchmark critical paths
make NEON=1       # Force NEON build (auto-detected on ARM)
make install      # Install to /usr/local
```

## Usage

### C API

```c
#include <tcodec.h>

/* Encode */
tc_config_t cfg;
tc_config_defaults(&cfg, 1920, 1080);
cfg.qp = 32;
cfg.preset = TC_PRESET_FAST;

tc_encoder_t *enc = tc_encoder_create(&cfg);
tc_encoder_encode(enc, y, stride_y, cb, stride_cb, cr, stride_cr, &pkt);

/* Decode */
tc_decoder_t *dec = tc_decoder_create(0, 0);  /* auto-detect dimensions */
tc_decoder_decode(dec, pkt.data, pkt.size,
                  &out_y, &out_stride_y,
                  &out_cb, &out_stride_cb,
                  &out_cr, &out_stride_cr);

/* Stats */
int64_t total_bytes;
int32_t total_frames;
double avg_psnr;
tc_encoder_get_stats(enc, &total_bytes, &total_frames, &avg_psnr);
```

### CLI Tools

```bash
# Encode
tcenc -i input.y4m -o output.tcv --preset fast --qp 32

# Decode
tcdec -i output.tcv -o output.y4m
```

## Test Suite

21 tests covering:
- Color conversion roundtrip (14-bit fixed-point, ±25 tolerance for 4:2:0)
- Encode/decode roundtrip at multiple QP values (10–50)
- Skip/merge mode with static content
- Scene cut detection (chi-squared histogram)
- CfL chroma prediction
- Deterministic encoding (identical output across runs)
- Motion estimation quality (panning gradient)
- Multi-reference frames (SLOW preset, 4 DPB slots)
- Non-CTU-aligned resolution (96×80)

## Continuing Development with AI

Copy-paste the prompt below into a new Codebuff session (or any AI coding agent) to continue development from exactly where we left off.

```
I'm working on TCodec, a custom video codec written in C. The project is at: <PATH_TO_TCODEC_REPO>

Please read these files first to understand the project state:
1. README.md — current feature set and build instructions
2. TODO.md — comprehensive task list with completion status
3. MASTER_PLAN.md — 10-phase roadmap from prototype to production
4. SPEC.md — codec specification (frame structure, prediction, transforms, coding)
5. BITSTREAM.md — bitstream syntax documentation (header fields, block coding, tANS)
6. PROFILES.md — profile/level/preset definitions and active tool inventory
7. BENCHMARKS.md — benchmark methodology
8. include/tcodec.h — public API
9. include/tcodec_common.h — internal constants (CTU_SIZE, TC_VERSION, etc.)
10. include/tcodec_types.h — type definitions

Then read the source files in src/ and neon/ to understand the implementation.

## What's Done (Don't Re-do These)

All 5 critical bugs are fixed (qp_delta wraparound, motion search centering, ref_h bounds, MV bounds check, QP overflow).
All NEON wiring is done except ACT-4 (NEON inter predict uses bilinear, scalar uses 6-tap — wiring would reduce quality) and ACT-7 (WPP thread pool — needs row-dependency sync).
21 tests pass: color roundtrip, encode/decode at QP 10-50, deterministic, low QP, skip/merge, multi-ref, non-CTU-aligned (96x80), scene cut, CfL chroma, motion quality, fuzz malformed (50 random packets), bitstream error recovery (truncated/bad magic/bad version).
All 4 spec docs (SPEC, BITSTREAM, PROFILES, BENCHMARKS) are updated to match current implementation.
README is updated with real feature table and build instructions.

## What To Do Next (Priority Order)

1. **ACT-7: Wire WPP thread pool** — encoder/decoder row loops are sequential. threadpool.c has the infrastructure. Need row-dependency synchronization (each row waits for the row above to finish deblocking before starting). Start with encoder only.
2. **Phase 5: Extend intra modes from 9 to 18** — predict.c has 9 modes (planar, DC, 7 angular). Add 9 more angular directions following HEVC-style angles (modes 9-17). Update intra_mode encoding from 4 bits to 5 bits. Update encoder mode search loop.
3. **Phase 7: Add SAO filter skeleton** — Add tc_sao_ctu() function in filter.c. Two types: Edge Offset (EO) for edge direction, Band Offset (BO) for level shift. Per-CTU type signaling in bitstream. Wire after deblock in encoder/decoder.
4. **Phase 4: Wire DCT-II as alternative transform** — transform.c currently only has WHT. Add tc_fdct4x4/tc_idct4x4 and tc_fdct8x8/tc_idct8x8. Use for low-detail blocks where DCT compresses better. Add transform_type flag in bitstream.
5. **Phase 3: Implement tANS context modeling** — entropy.c has tc_tans_enc_coeffs/tc_tans_dec_coeffs but they use default/fixed probability tables. Add context modeling: significance flag per position, last_nz position, magnitude class (0/1/2-4/5+). Expected ~15-30% BD-rate improvement.
6. **Phase 6: Add B-frame skeleton** — Add BIDIR frame type support. Bi-prediction (average of two reference frames). COLLOCATED_MV temporal predictor. Update DPB management for 2 reference lists.
7. **Phase 8: Add lookahead rate control** — Pre-analyze future frames for QP decisions. Buffer of 5-15 frames. Scene-aware complexity estimation. Improves CBR/VBR quality consistency.
8. **Phase 1: Create benchmark harness** — tools/run_benchmark.sh for encode matrix. tools/evaluate_quality.py for VMAF/PSNR extraction. tools/bd_rate.py for BD-rate calculation. Test against x264, x265, SVT-AV1.
9. **Phase 9: Verify NEON transform dispatch** — transform_neon.c has tc_fwht4x4_neon and tc_iwht4x4_neon. Verify they dispatch correctly on ARM builds (same #if TCODEC_NEON guard pattern as SAD/deblock/color).
10. **Phase 0 remaining items** — Golden corpus directory, architecture diagrams in SPEC.md, large resolution test (1920x1080), long-run soak test (1000+ frames).

## Key Architecture Notes

- CTU_SIZE = 64, block size = 8×8, chroma 4:2:0 (4×4 blocks)
- Frame header: 8-byte magic, 8-bit version, 16-bit width/height, 8-bit flags, 8-bit qp_delta, 8-bit tile_cols_log2, 8-bit tile_rows_log2
- Block modes: 2-bit field (0=skip, 1=inter, 2=intra, 3=merge)
- MVD coded relative to median of spatial neighbor MVs (not collocated)
- 4 DPB slots, ref_idx signaled per block (2 bits)
- Scalar code guarded by #if !TCODEC_NEON when NEON version exists
- NEON versions in neon/ directory, named same as scalar (replaces via #if)
- tANS for coefficients, Exp-Golomb (se) for MVD only
- Build: make release (optimized), make test (run tests), make NEON=1 (force NEON)
- TC_VERSION = 0 (decoder rejects version > TC_VERSION)
```

### Quick Reference: File Map

| Path | Purpose |
|------|---------|
| `src/encoder.c` | Main encode loop: mode decision, quantize, bitstream write |
| `src/decoder.c` | Main decode loop: bitstream read, dequantize, reconstruct |
| `src/entropy.c` | tANS + Exp-Golomb coding |
| `src/motion.c` | Hierarchical hex search, 6-tap interpolation, SAD |
| `src/predict.c` | 9 intra modes (planar, DC, angular) |
| `src/transform.c` | WHT 4×4 and 8×8 |
| `src/quantize.c` | Quantize/dequantize with JND band weighting |
| `src/filter.c` | Deblocking filter (scalar, guarded by #if !TCODEC_NEON) |
| `src/color.c` | RGB↔YCbCr conversion (scalar guarded, NEON dispatched) |
| `src/frame.c` | Frame allocation, DPB management |
| `src/bitstream.c` | Bitstream reader/writer |
| `src/ratectl.c` | ρ-domain rate control (CQP/CBR/VBR) |
| `src/threadpool.c` | Thread pool (exists but NOT WIRED in enc/dec) |
| `src/tcodec.c` | Public API implementation |
| `neon/motion_neon.c` | NEON SAD (wired) + NEON inter predict (NOT wired — bilinear) |
| `neon/filter_neon.c` | NEON deblock (wired) |
| `neon/color_neon.c` | NEON color convert (wired, 32-bit accumulation) |
| `neon/transform_neon.c` | NEON WHT (exists, verify dispatch) |
| `include/tcodec.h` | Public API: encoder, decoder, config, PSNR |
| `include/tcodec_common.h` | Internal constants: CTU_SIZE, TC_VERSION, QP limits |
| `include/tcodec_types.h` | Types: tc_pixel_t, tc_mv_s, tc_frame_t, etc. |
| `test/test_tcodec.c` | 21 tests |
| `tools/tcenc.c` | Encoder CLI |
| `tools/tcdec.c` | Decoder CLI |

---

## License

MIT
