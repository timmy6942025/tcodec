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

## ⚠️ Phase Order Note — We're Doing Phases Out of Order

The Master Plan's suggested execution order is:
Phase 0 → Phase 1 → Phase 2 → Phase 3 → Phase 4 → ...

We have skipped Phases 1 (Benchmark Harness) and 2 (Bitstream Redesign). We are
doing phases in the following order instead (prioritizing compression wins first):
Phase 0 → Phase 5 → Phase 3 → Phase 4 → Phase 6 → Phase 7 → Phase 8 → Phase 9 → Phase 10 → Phase 1 → Phase 2

This is NOT the recommended order — benchmarking should come before feature work,
and bitstream versioning should come before entropy coding changes. However, we are
proceeding this way to get compression wins early.

When continuing on a new computer, you should know:
- Phases 1 and 2 are skipped for now
- Phase 3 (entropy coding) comes after Phase 5 (intra modes)
- The AI_AGENT_PROMPT.md and TODO.md reflect this non-standard order

## What's Done (Don't Re-do These)

**Completed features (verified by tests):**
- All 5 critical bugs fixed (qp_delta wraparound, motion search centering, ref_h bounds, MV bounds check, QP overflow)
- ACT-7: WPP thread pool wired — encoder uses per-row bitstream buffers + tANS encoders, dispatches via tc_threadpool_run(), merges byte-aligned row bitstreams with entry point table. Decoder reads entry point table when TC_FLAG_WPP is set, initializes per-row readers, dispatches WPP. Sequential fallback with inter-row byte-alignment skips. TCODEC_NO_THREADS compatible.
- Phase 5: 18 intra modes — 7 vertical angular (2-8), 9 horizontal angular (9-17), planar + DC. 5-bit encoding. horizontal angular uses left column reference projection.
- All NEON wiring done except ACT-4 (NEON inter predict uses bilinear, scalar uses 6-tap — wiring would reduce quality)
- 21 tests pass: color roundtrip, encode/decode at QP 10-50, deterministic, low QP, skip/merge, multi-ref, non-CTU-aligned (96x80), scene cut, CfL chroma, motion quality, fuzz malformed (50 random packets), bitstream error recovery (truncated/bad magic/bad version)

**Updated documentation:**
- All 4 spec docs (SPEC, BITSTREAM, PROFILES, BENCHMARKS) updated
- TODO.md and README.md updated with current feature table

## What To Do Next (Priority Order)

The remaining work from TODO.md, in priority order (skipping Phases 1 and 2 for now):

1. **Phase 3: Implement range coder** — entropy.c currently uses tANS framework but with Exp-Golomb. Implement proper range coder (range_coder.c) with encode/decode + renormalization. Wire into coefficient coding path replacing Exp-Golomb. This is the single biggest BD-rate win (~15-30%).

2. **Phase 7: Add SAO filter skeleton** — Add tc_sao_ctu() function in filter.c. Two types: Edge Offset (EO) for edge direction, Band Offset (BO) for level shift. Per-CTU type signaling in bitstream. Wire after deblock in encoder/decoder.

3. **Phase 4: Wire DCT-II as alternative transform** — transform.c currently only has WHT. Add tc_fdct4x4/tc_idct4x4 and tc_fdct8x8/tc_idct8x8. Use for low-detail blocks where DCT compresses better. Add transform_type flag in bitstream.

4. **Phase 6: Add B-frame skeleton** — Add BIDIR frame type support. Bi-prediction (average of two reference frames). COLLOCATED_MV temporal predictor. Update DPB management for 2 reference lists.

5. **Phase 8: Add lookahead rate control** — Pre-analyze future frames for QP decisions. Buffer of 5-15 frames. Scene-aware complexity estimation. Improves CBR/VBR quality consistency.

6. **Phase 9: ARM/Mobile Decoder Optimization** — Full NEON kernel optimization, WPP row-dependency sync, memory management, Raspberry Pi benchmark.

7. **Phase 10: Ecosystem & Deployment** — Container format, FFmpeg integration, CLI improvements, conformance bitstreams.

8. **Phase 1: Create benchmark harness** — **SKIPPED FOR NOW** — Should be done before Phase 3 ideally. tools/run_benchmark.sh for encode matrix. tools/evaluate_quality.py for VMAF/PSNR extraction. tools/bd_rate.py for BD-rate calculation. Test against x264, x265, SVT-AV1.

9. **Phase 2: Rebuild Bitstream for Longevity** — **SKIPPED FOR NOW** — Should be done before Phase 3 ideally. Versioned bitstream syntax, profiles/levels, tool flags, random access points, tiles/slices.

## Key Architecture Notes

- CTU_SIZE = 64, block size = 8×8, chroma 4:2:0 (4×4 blocks)
- Frame header: 8-byte magic, 8-bit version, 16-bit width/height, 8-bit flags, 8-bit qp_delta, 8-bit tile_cols_log2, 8-bit tile_rows_log2
- Block modes: 2-bit field (0=skip, 1=inter, 2=intra, 3=merge)
- Intra modes: 5 bits (0..17), modes 0=planar, 1=DC, 2-8=vertical angular, 9-17=horizontal angular
- MVD coded relative to median of spatial neighbor MVs (not collocated)
- 4 DPB slots, ref_idx signaled per block (2 bits)
- WPP: entry point table (16 bits num_offsets + N×16 bits offsets), row byte-alignment, sequential fallback skips inter-row padding
- TC_FLAG_WPP = 0x40 in frame header flags byte
- Scalar code guarded by #if !TCODEC_NEON when NEON version exists
- NEON versions in neon/ directory, named same as scalar (replaces via #if)
- tANS for coefficients, Exp-Golomb (se) for MVD only
- Build: make release (optimized), make test (run tests), make nothreads (TCODEC_NO_THREADS=1), make NEON=1 (force NEON)
- TC_VERSION = 0 (decoder rejects version > TC_VERSION)

## Bitstream Changes (Breaking)

As of this session, the bitstream format has changed (version still 0 but encoder produces different output):
- Intra mode encoding: 4 bits → 5 bits (18 modes now, was 9)
- WPP bitstreams: entry point table added between header and row data
- Non-WPP bitstreams: unchanged (sequential row-by-row)

## Phase Order (Non-Standard — See Note Above)

We're doing phases out of order. Correct Master Plan order is:
Phase 0 → Phase 1 → Phase 2 → Phase 3 → Phase 4 → ...

Our actual order:
Phase 0 ✅ → Phase 5 ✅ → Phase 3 ⏸️ → Phase 4 ⏸️ → Phase 6 ⏸️ → Phase 7 ⏸️ → Phase 8 ⏸️ → Phase 9 ⏸️ → Phase 10 ⏸️ → Phase 1 ⚠️ → Phase 2 ⚠️

Phases 1 and 2 are deferred until after the main codec features are done.
```

---

## Quick Reference: File Map

| Path | Purpose |
|------|---------|
| `src/encoder.c` | Main encode loop: mode decision, quantize, bitstream write, WPP dispatch |
| `src/decoder.c` | Main decode loop: bitstream read, dequantize, reconstruct, WPP dispatch |
| `src/entropy.c` | tANS + Exp-Golomb coding |
| `src/motion.c` | Hierarchical hex search, 6-tap interpolation, SAD |
| `src/predict.c` | 18 intra modes (planar, DC, 7 vertical angular, 9 horizontal angular) |
| `src/transform.c` | WHT 4×4 and 8×8 |
| `src/quantize.c` | Quantize/dequantize with JND band weighting |
| `src/filter.c` | Deblocking filter (scalar, guarded by #if !TCODEC_NEON) |
| `src/color.c` | RGB↔YCbCr conversion (scalar guarded, NEON dispatched) |
| `src/frame.c` | Frame allocation, DPB management |
| `src/bitstream.c` | Bitstream reader/writer |
| `src/ratectl.c` | ρ-domain rate control (CQP/CBR/VBR) |
| `src/threadpool.c` | Thread pool (wired for WPP in encoder/decoder) |
| `src/tcodec.c` | Public API implementation |
| `neon/motion_neon.c` | NEON SAD (wired) + NEON inter predict (NOT wired — bilinear) |
| `neon/filter_neon.c` | NEON deblock (wired) |
| `neon/color_neon.c` | NEON color convert (wired, 32-bit accumulation) |
| `neon/transform_neon.c` | NEON WHT (exists, verify dispatch) |
| `include/tcodec.h` | Public API: encoder, decoder, config, PSNR |
| `include/tcodec_common.h` | Internal constants: CTU_SIZE, TC_VERSION, QP limits |
| `include/tcodec_types.h` | Types: tc_pixel_t, tc_mv_s, tc_frame_t, TC_FLAG_WPP |
| `test/test_tcodec.c` | 21 tests |
| `tools/tcenc.c` | Encoder CLI |
| `tools/tcdec.c` | Decoder CLI |