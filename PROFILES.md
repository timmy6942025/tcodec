# TCodec Profiles and Levels — Version 0

**Status**: Working prototype. No profiles or levels are enforced yet.  
**Bitstream Version**: 0  
**Last Updated**: Based on code audit of current implementation.

---

## 1. Overview

Profiles define **which coding tools a decoder must support**. Levels define
**performance and memory constraints**. Together they allow a decoder to
announce its capabilities and for encoders to produce bitstreams that will
decode successfully on a given class of device.

**Current state**: No profiles or levels are defined, signaled, or enforced.
The bitstream header has no profile/level fields. All coding tools are always
active (or inactive, as the case may be — see tool status below).

This document serves as:
1. A record of the **current tool inventory** and its activation status
2. A **proposal** for the profile/level structure to be introduced in
   bitstream version 1
3. A **preset reference** for the existing encoder presets

---

## 2. Current Tool Inventory

### 2.1 Coding Tools Active in Pipeline

| Tool | Code Location | Notes |
|------|--------------|-------|
| 9-mode intra prediction | `predict.c` | Planar, DC, 7 angular |
| WHT 4×4 / 8×8 transform | `transform.c` / `transform_neon.c` | Self-inverse, NEON dispatched |
| Variance-based transform size | `encoder.c` | Threshold = 512 |
| tANS coefficient coding | `entropy.c` | Replaces Exp-Golomb in pipeline |
| Hierarchical hex motion search | `motion.c` | ±16/32/64 range by preset |
| 6-tap luma interpolation | `motion.c` | H.264-style half-pel + bilinear quarter-pel |
| Multi-reference inter prediction | `encoder.c` | 4 DPB slots; SLOW preset searches all |
| Skip/merge inter modes | `encoder.c` / `decoder.c` | 2-bit mode field: skip/inter/intra/merge |
| Median MV predictor | `encoder.c` / `decoder.c` | Search center + MVD predictor + merge MV |
| Deblocking filter | `filter.c` / `filter_neon.c` | Scalar guarded; NEON replaces on ARM |
| ρ-domain rate control | `ratectl.c` | CQP, CBR, VBR modes |
| CfL chroma prediction | `encoder.c` / `decoder.c` | Chroma-from-luma for intra; DC(128) for inter |
| JND band weighting | `encoder.c` / `decoder.c` | Applied inline per-coefficient for luma |
| Scene cut detection | `encoder.c` | Chi-squared histogram, forces keyframe |
| NEON SAD | `motion_neon.c` | Replaces scalar via `#if TCODEC_NEON` |
| NEON color conversion | `color_neon.c` | 32-bit accumulation luma; scalar chroma/inverse |

### 2.2 Coding Tools Present but Inactive

| Tool | Code Location | Status | Reason |
|------|--------------|--------|--------|
| DCT-II 4×4 / 8×8 (pixel-mode) | `transform.c` / `transform_neon.c` | **Unused** | Requires position-dependent scaling that dequantizer doesn't provide |
| Thread pool / WPP | `threadpool.c` | **Not dispatched** | Allocated but encode/decode loops are sequential `for` |
| NEON inter predict | `motion_neon.c` | **Not wired** | Uses bilinear, not 6-tap; would reduce quality |

### 2.3 Coding Tools Not Implemented

| Tool | Master Plan Phase | Priority |
|------|------------------|----------|
| Context modeling for entropy coding | Phase 3 | Critical |
| Deringing / SAO / CDEF | Phase 7 | Medium |
| Loop restoration | Phase 7 | Low |
| Film grain synthesis | Phase 7 | Medium (film content) |
| Real lookahead | Phase 8 | High |
| VBV-validated rate control | Phase 8 | Medium |
| Bi-prediction / B-frames | Phase 6 | Low (ARM cost) |
| Affine / global motion | Phase 6+ | Low |
| Quadtree + binary partition | Phase 5 | Medium |
| Extended chroma intra modes (DM, planar, angular) | Phase 5 | Medium |
| Content-adaptive transform selection | Phase 4 | Medium |
| Scaling matrices / freq-dependent quant | Phase 4 | Medium |
| 8-tap interpolation filters | Phase 6 | Medium |

---

## 3. Proposed Profiles

These profiles are **not yet implemented**. They represent the planned
tool subsets for different deployment targets.

### 3.1 Profile 0: `baseline-mobile`

**Target**: Raspberry Pi, mid-range Android phones, low-power embedded  
**Decoder complexity**: Minimal — no tools that require significant
per-block decision logic or memory.

| Tool | Supported |
|------|-----------|
| Intra modes 0–8 | ✅ |
| WHT 4×4 / 8×8 | ✅ |
| Exp-Golomb coding | ✅ (or range coder if upgrade path works) |
| Single-reference inter | ✅ |
| Quarter-pel bilinear | ✅ |
| Deblocking filter | ✅ (simplified) |
| DC chroma prediction | ✅ |
| Skip / merge | ✅ (when implemented) |
| Multi-reference | ❌ |
| Bi-prediction | ❌ |
| 8-tap interpolation | ❌ |
| Deringing / SAO | ❌ |
| Loop restoration | ❌ |
| Film grain synthesis | ❌ |
| Affine motion | ❌ |
| Extended partition types | ❌ |

**Rationale**: This is the deployment profile for the ARM decoder
optimization work (Phase 9). Every tool included must be measurable
in NEON cycle count and must not push decode FPS below target on
Raspberry Pi 4.

### 3.2 Profile 1: `streaming-main`

**Target**: Streaming delivery to smart TVs, tablets, laptops  
**Decoder complexity**: Moderate — adds tools that improve streaming
quality at modest decode cost.

| Tool | Supported |
|------|-----------|
| All baseline-mobile tools | ✅ |
| Context-adaptive entropy coding | ✅ |
| Real chroma intra prediction | ✅ |
| Multiple reference frames (2–4) | ✅ |
| 6-tap interpolation | ✅ |
| Skip / merge modes | ✅ |
| Deringing filter | ✅ |
| SAO (if implemented) | ✅ |
| Rate control: CBR + capped VBR | ✅ |
| Scene cut detection | ✅ |
| Film grain synthesis | ✅ |
| Bi-prediction | ❌ |
| Loop restoration | ❌ |
| Affine motion | ❌ |

**Rationale**: This is the primary commercial profile. It must
beat practical H.264 streaming encodes on bitrate at matched VMAF,
and approach HEVC-class compression on many content types.

### 3.3 Profile 2: `archive-high`

**Target**: Offline encoding, studio archive, post-production  
**Decoder complexity**: High — no real-time decode requirement on ARM.
Used on workstations and servers.

| Tool | Supported |
|------|-----------|
| All streaming-main tools | ✅ |
| Bi-prediction (B-frames) | ✅ |
| 8-tap interpolation | ✅ |
| Loop restoration | ✅ |
| Affine motion | ✅ |
| Extended partition types | ✅ |
| Scaling matrices | ✅ |
| Encoder-side learned guidance | ✅ (encoder only) |
| Deep lookahead | ✅ (encoder only) |

**Rationale**: Offline encoding can afford much higher encoder
complexity. The decoder runs on capable hardware. The goal is
maximum compression quality, not real-time ARM decode.

### 3.4 Profile 3: `grain-cinema`

**Target**: Movie and prestige TV streaming with film grain  
**Decoder complexity**: Similar to streaming-main plus grain synthesis.

| Tool | Supported |
|------|-----------|
| All streaming-main tools | ✅ |
| Film grain synthesis | ✅ (required) |
| Grain-aware quantization | ✅ |
| Per-scene perceptual targeting | ✅ (encoder only) |

**Rationale**: Film grain is one of the highest-leverage areas for
bandwidth savings in movie streaming. Coding grain literally is
extremely expensive. This profile mandates grain synthesis support
and encoder-side grain detection/removal.

---

## 4. Proposed Levels

Levels constrain **memory, throughput, and resolution** to ensure
decoders can handle the bitstream within their resource budget.

**Not yet implemented.** The following is a proposal.

### 4.1 Level Parameters

Each level defines limits on:

| Parameter | Description |
|-----------|-------------|
| Max picture width | Pixels |
| Max picture height | Pixels |
| Max CTU columns | `ceil(width / 64)` |
| Max CTU rows | `ceil(height / 64)` |
| Max DPB slots | Reference frames stored simultaneously |
| Max decode MB/s | Throughput requirement (luma samples/sec) |
| Max bitrate | Coded bits per second |
| Max tile columns | Parallel decode limit |

### 4.2 Proposed Level Table

| Level | Max Width | Max Height | Max DPB | Max Samples/s | Max Bitrate | Target Device |
|-------|-----------|------------|---------|---------------|-------------|---------------|
| 1.0 | 320 | 240 | 1 | 2,304,000 | 500 kbps | IoT / very low power |
| 1.1 | 640 | 480 | 2 | 9,216,000 | 2 Mbps | Raspberry Pi 2 |
| 2.0 | 1280 | 720 | 2 | 16,588,800 | 5 Mbps | Raspberry Pi 4 |
| 2.1 | 1280 | 720 | 4 | 33,177,600 | 10 Mbps | Mid-range Android |
| 3.0 | 1920 | 1080 | 4 | 124,416,000 | 20 Mbps | Raspberry Pi 5, good phone |
| 3.1 | 1920 | 1080 | 4 | 124,416,000 | 40 Mbps | High-end phone, tablet |
| 4.0 | 3840 | 2160 | 4 | 497,664,000 | 80 Mbps | Smart TV, laptop |
| 4.1 | 3840 | 2160 | 8 | 497,664,000 | 160 Mbps | Desktop, server |

**Notes**:
- Max Samples/s = `width × height × fps_max`, assuming 30 fps for levels ≤ 2.1
  and 60 fps for levels ≥ 3.0
- Level 3.0 and 3.1 share the same sample rate but differ in bitrate ceiling
- DPB slots may be further constrained by profile (baseline-mobile: 1–2)
- These levels are provisional and must be validated with real decode
  measurements on target hardware (Phase 9)

---

## 5. Encoder Presets

Presets control **encoder search effort and tool exploration depth**.
They do not affect which tools the decoder must support (that's the
profile's job). Presets are **already implemented** in the codebase.

### 5.1 Defined Presets

| Preset | Value | Search Range | Description |
|--------|-------|-------------|-------------|
| `TC_PRESET_ULTRAFAST` | 0 | 16 px | Minimal search, fastest encode |
| `TC_PRESET_FAST` | 1 | 32 px | Moderate search, good speed |
| `TC_PRESET_MEDIUM` | 2 | 32 px | Default balance |
| `TC_PRESET_SLOW` | 3 | 64 px | Deep search, best quality |
| `TC_PRESET_VERYSLOW` | — | 128 px | **NOT YET IMPLEMENTED.** Research/offline mode from Master Plan. |

### 5.2 What Presets Currently Affect

The only encoder behavior that currently varies by preset is **motion
search range**:

```c
int search_range = 32;                              // default (medium/fast)
if (enc->cfg.preset == TC_PRESET_ULTRAFAST) search_range = 16;
if (enc->cfg.preset == TC_PRESET_SLOW)      search_range = 64;
```

**NOTE**: The Master Plan defines a 5th preset, `veryslow-research`,
which is not yet implemented in the codebase. It will be added as
`TC_PRESET_VERYSLOW` (value 4) when the encoder gains RDO, lookahead,
and deep search capabilities.

### 5.3 What Presets Should Affect (Future)

| Behavior | Ultrafast | Fast | Medium | Slow | Veryslow |
|----------|-----------|------|---------|------|----------|
| Motion search range | 16 px | 32 px | 32 px | 64 px | 128 px |
| Sub-pel refinement | None | Quarter-pel | Quarter-pel | Eighth-pel |
| Intra mode search | DC only | Top-3 modes | All 9 modes | All 9 + RDO |
| Transform size search | 8×8 always | Variance-based | Variance-based | Full RD select |
| Rate-distortion optimize | No | No | Fast prune | Full search |
| Lookahead frames | 0 | 0 | 5 | 15+ |
| Scene cut detection | Off | On | On | On + adaptive |
| Keyframe placement | Fixed interval | Fixed interval | Content-adaptive | Optimal |
| Quantization granularity | Frame-level | Frame-level | CTU-level | Block-level |
| AQ / perceptual masking | Off | Off | Basic | Full |
| Thread count | 1 | Auto | Auto | Auto |
| B-frame decision | Off | Off | Off | On (if profile allows) | On (if profile allows) |

### 5.4 Preset–Profile Interaction

- Encoder presets choose **how hard to search**
- Decoder profiles choose **which tools are available to search over**
- A slow preset with baseline-mobile profile will search deeply within
  the mobile tool subset
- A medium preset with archive-high profile will use all tools but
  with moderate search effort

This separation is essential: it means mobile decoders never need to
handle tools outside their profile, regardless of what preset the
encoder used.

---

## 6. Profile Signaling (Proposed for Bitstream v1)

The frame header currently has no profile/level fields. The proposed
addition for bitstream version 1:

### 6.1 Header Extension

```
Bit Offset  Width  Field           Description
──────────  ─────  ──────────────  ──────────────────────
24–31       8      version         0x01 (version 1)
...
72–79       8      qp_delta        (unchanged)
80–87       8      frame_num       (unchanged)
88–91       4      profile         0=baseline-mobile, 1=streaming-main,
                                    2=archive-high, 3=grain-cinema
92–95       4      level_idx       Level index (see level table)
96          1      tool_flags_ext  1 = extended tool flags follow
97–103      7      reserved        Must be 0
```

### 6.2 Extended Tool Flags (Optional)

If `tool_flags_ext = 1`, a variable-length tool flag section follows,
indicating which optional tools are active in this frame:

```
Bit   Field                  Profile Required
───   ─────────────────────  ───────────────
0     entropy_coded           streaming-main+
1     multi_ref               streaming-main+
2     skip_merge              baseline-mobile+
3     deringing               streaming-main+
4     sao                     streaming-main+
5     grain_synthesis         grain-cinema
6     bipred                  archive-high
7     loop_restoration        archive-high
8     affine_motion           archive-high
9     extended_partition      archive-high
10    six_tap_interpolate     streaming-main+
11    real_chroma_intra       streaming-main+
12–15 reserved                Must be 0
```

A decoder for profile N silently ignores tools that are not in its
profile (it must not encounter them if the encoder is compliant).

---

## 7. Backward Compatibility Strategy

When new tools or profiles are introduced:

1. **Bitstream version increments** — old decoders reject the new version
2. **New profile values** — old decoders that only know profile 0–3
   reject unknown profiles
3. **Tool flags** — decoders only parse tools their profile supports;
   unknown flag bits are ignored
4. **Optional syntax sections** — guarded by flag bits; decoders that
   don't support a tool skip its syntax section entirely

This ensures that:
- A baseline-mobile decoder never needs to understand archive-high syntax
- A new tool can be added without breaking old decoders
- Bitstream conformance can be tested per-profile

---

## 8. Conformance Testing (Proposed)

For each profile, a conformance test suite should verify:

1. **Decode correctness**: All conformance bitstreams decode to
   reference outputs within tolerance
2. **Error handling**: Malformed bitstreams are rejected cleanly
3. **Resource limits**: Level constraints are respected (memory,
   throughput)
4. **Tool subset**: No tools outside the profile are exercised
5. **Interoperability**: Bitstreams from different encoder versions
   produce identical decoder output (deterministic)

Conformance bitstreams should be organized as:

```
conformance/
  baseline-mobile/
    level-1.0/
    level-2.0/
    ...
  streaming-main/
    level-2.0/
    level-3.0/
    ...
  archive-high/
    level-4.0/
    ...
  grain-cinema/
    level-3.0/
    ...
```

Each directory should contain:
- Source YUV files
- Encoded bitstreams
- Expected decoded output (MD5 hashes or binary frames)
- Encoding parameters used
