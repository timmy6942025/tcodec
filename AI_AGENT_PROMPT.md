# TCodec — AI Agent Continuation Prompt

Copy-paste the prompt below into a new Codebuff session (or any AI coding agent) to continue development from exactly where we left off.

---

## Prompt

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

The remaining work from TODO.md, in priority order:

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

---

## Quick Reference: File Map

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
