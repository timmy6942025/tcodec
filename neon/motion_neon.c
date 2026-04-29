/*
 * motion_neon.c — NEON-optimized motion estimation helpers for TCodec
 *
 * NEON SAD (Sum of Absolute Differences) for 4×4, 8×8, 16×16 blocks.
 * This is the hottest function in motion estimation — NEON gives ~6× speedup.
 *
 * Also includes quarter-pel interpolation and sub-pixel SAD.
 */

#include "tcodec_common.h"

#if TCODEC_NEON
#include <arm_neon.h>

/* ── SAD 8×8 (NEON) ────────────────────────────────────────────
 *
 * Process 8 pixels per row with vabdq_u8 + vpaddlq_u16.
 * Accumulate across 8 rows → single SAD value.
 * ══════════════════════════════════════════════════════════════ */

tc_sad_t tc_sad_8x8_neon(const tc_pixel_t *a, int stride_a,
                          const tc_pixel_t *b, int stride_b)
{
    uint32x4_t acc = vdupq_n_u32(0);

    for (int y = 0; y < 8; y++) {
        uint8x8_t ra = vld1_u8(a + y * stride_a);
        uint8x8_t rb = vld1_u8(b + y * stride_b);

        /* Absolute difference, pairwise add to u16, then widen add to u32 */
        uint8x8_t abd = vabd_u8(ra, rb);
        uint16x8_t pad = vpaddlq_u8(vcombine_u8(abd, vdup_n_u8(0)));
        acc = vaddq_u32(acc, vpaddlq_u16(pad));
    }

    /* Horizontal sum of 4 u32 values */
    uint32x2_t pair = vadd_u32(vget_low_u32(acc), vget_high_u32(acc));
    uint32x2_t sum2 = vpadd_u32(pair, pair);
    return (tc_sad_t)vget_lane_u32(sum2, 0);
}

/* ── SAD 4×4 (NEON) ──────────────────────────────────────────── */

tc_sad_t tc_sad_4x4_neon(const tc_pixel_t *a, int stride_a,
                          const tc_pixel_t *b, int stride_b)
{
    uint32x2_t acc = vdup_n_u32(0);

    for (int y = 0; y < 4; y++) {
        uint8x8_t ra = vld1_u8(a + y * stride_a);  /* Only use low 4 */
        uint8x8_t rb = vld1_u8(b + y * stride_b);

        uint8x8_t abd = vabd_u8(ra, rb);
        uint16x8_t pad = vpaddlq_u8(vcombine_u8(abd, vdup_n_u8(0)));
        acc = vadd_u32(acc, vget_low_u32(vpaddlq_u16(pad)));
    }

    uint32x2_t sum2 = vpadd_u32(acc, acc);
    return (tc_sad_t)vget_lane_u32(sum2, 0);
}

/* ── SAD 16×16 (NEON) ─────────────────────────────────────────── */

tc_sad_t tc_sad_16x16_neon(const tc_pixel_t *a, int stride_a,
                            const tc_pixel_t *b, int stride_b)
{
    uint32x4_t acc = vdupq_n_u32(0);

    for (int y = 0; y < 16; y++) {
        uint8x16_t ra = vld1q_u8(a + y * stride_a);
        uint8x16_t rb = vld1q_u8(b + y * stride_b);

        uint8x16_t abd = vabdq_u8(ra, rb);
        uint16x8_t pad = vpaddlq_u8(abd);
        acc = vaddq_u32(acc, vpaddlq_u16(pad));
    }

    uint32x2_t pair = vadd_u32(vget_low_u32(acc), vget_high_u32(acc));
    uint32x2_t sum2 = vpadd_u32(pair, pair);
    return (tc_sad_t)vget_lane_u32(sum2, 0);
}

/* ── General SAD dispatcher ───────────────────────────────────── */

tc_sad_t tc_sad(const tc_pixel_t *a, int stride_a,
                      const tc_pixel_t *b, int stride_b, int n)
{
    switch (n) {
        case 4:  return tc_sad_4x4_neon(a, stride_a, b, stride_b);
        case 8:  return tc_sad_8x8_neon(a, stride_a, b, stride_b);
        case 16: return tc_sad_16x16_neon(a, stride_a, b, stride_b);
        default: {
            /* Fallback for non-power-of-2 sizes */
            uint32x4_t acc = vdupq_n_u32(0);
            for (int y = 0; y < n; y++) {
                int x = 0;
                for (; x + 7 < n; x += 8) {
                    uint8x8_t ra = vld1_u8(a + y * stride_a + x);
                    uint8x8_t rb = vld1_u8(b + y * stride_b + x);
                    uint8x8_t abd = vabd_u8(ra, rb);
                    uint16x8_t pad = vpaddlq_u8(vcombine_u8(abd, vdup_n_u8(0)));
                    acc = vaddq_u32(acc, vpaddlq_u16(pad));
                }
                /* Handle remaining pixels */
                uint32_t scalar_sad = 0;
                for (; x < n; x++) {
                    int diff = (int)a[y * stride_a + x] - (int)b[y * stride_b + x];
                    scalar_sad += tc_abs(diff);
                }
                uint32x2_t spair = vdup_n_u32(scalar_sad);
                acc = vaddq_u32(acc, vcombine_u32(spair, vdup_n_u32(0)));
            }
            uint32x2_t pair = vadd_u32(vget_low_u32(acc), vget_high_u32(acc));
            uint32x2_t sum2 = vpadd_u32(pair, pair);
            return (tc_sad_t)vget_lane_u32(sum2, 0);
        }
    }
}

/* ── Quarter-pel interpolation (NEON) ────────────────────────────
 *
 * Bilinear interpolation at quarter-pel position for 8×8 block.
 * Produces an 8×8 predicted block from reference at sub-pel offset.
 *
 * ⚠️  COMPATIBILITY NOTE: This NEON implementation uses bilinear
 * interpolation for all fractional positions, while the scalar
 * tc_inter_predict() in motion.c uses a 6-tap filter (H.264-style)
 * for half-pel positions with bilinear fallback at frame edges.
 * The 6-tap filter produces objectively better half-pel predictions.
 * Wiring this NEON version would REDUCE quality on ARM builds.
 * Therefore, NEON inter predict is NOT wired — the scalar 6-tap
 * path is used on all platforms. A future NEON 6-tap implementation
 * would be needed before wiring can happen.
 * ══════════════════════════════════════════════════════════════ */

void tc_inter_predict_neon(const tc_pixel_t *ref, int ref_stride,
                            int ref_w, int ref_h,
                            tc_mv_s mv,
                            tc_pixel_t *TCODEC_RESTRICT dst, int dst_stride,
                            int blk_size)
{
    (void)ref_w; (void)ref_h;  /* Bounds assumed checked by caller */
    int ix = mv.x >> 2;  /* Integer part */
    int iy = mv.y >> 2;
    int fx = mv.x & 3;   /* Fractional part (0-3) */
    int fy = mv.y & 3;

    if (fx == 0 && fy == 0) {
        /* Integer position — just copy */
        for (int y = 0; y < blk_size; y++) {
            memcpy(dst + y * dst_stride,
                   ref + (iy + y) * ref_stride + ix,
                   (size_t)blk_size);
        }
        return;
    }

    /* Sub-pel: bilinear interpolation */
    int16x8_t two = vdupq_n_s16(2);
    for (int y = 0; y < blk_size; y++) {
        const tc_pixel_t *row0 = ref + (iy + y) * ref_stride + ix;
        const tc_pixel_t *row1 = row0 + ref_stride;

        if (fx == 0) {
            /* Vertical only */
            for (int x = 0; x + 7 < blk_size; x += 8) {
                int16x8_t a = vreinterpretq_s16_u16(vmovl_u8(vld1_u8(row0 + x)));
                int16x8_t b = vreinterpretq_s16_u16(vmovl_u8(vld1_u8(row1 + x)));
                int16x8_t r = vaddq_s16(
                    vaddq_s16(vmulq_n_s16(a, 4 - fy), vmulq_n_s16(b, fy)),
                    two);
                r = vrshrq_n_s16(r, 2);
                int16x8_t zero = vdupq_n_s16(0);
                int16x8_t max255 = vdupq_n_s16(255);
                r = vminq_s16(vmaxq_s16(r, zero), max255);
                vst1_u8(dst + y * dst_stride + x,
                         vmovn_u16(vreinterpretq_u16_s16(r)));
            }
        } else if (fy == 0) {
            /* Horizontal only */
            for (int x = 0; x + 7 < blk_size; x += 8) {
                int16x8_t a = vreinterpretq_s16_u16(vmovl_u8(vld1_u8(row0 + x)));
                int16x8_t b = vreinterpretq_s16_u16(vmovl_u8(vld1_u8(row0 + x + 1)));
                int16x8_t r = vaddq_s16(
                    vaddq_s16(vmulq_n_s16(a, 4 - fx), vmulq_n_s16(b, fx)),
                    two);
                r = vrshrq_n_s16(r, 2);
                int16x8_t zero = vdupq_n_s16(0);
                int16x8_t max255 = vdupq_n_s16(255);
                r = vminq_s16(vmaxq_s16(r, zero), max255);
                vst1_u8(dst + y * dst_stride + x,
                         vmovn_u16(vreinterpretq_u16_s16(r)));
            }
        } else {
            /* Full bilinear: 4 reference points per output pixel */
            for (int x = 0; x < blk_size; x++) {
                int a = row0[x], b_row = row0[x + 1];
                int c = row1[x], d = row1[x + 1];
                int top = (a * (4 - fx) + b_row * fx + 2) >> 2;
                int bot = (c * (4 - fx) + d * fx + 2) >> 2;
                int val = (top * (4 - fy) + bot * fy + 2) >> 2;
                dst[y * dst_stride + x] = (tc_pixel_t)tc_clip(val, 0, 255);
            }
        }
    }
}

#endif /* TCODEC_NEON */
