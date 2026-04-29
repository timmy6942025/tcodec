/*
 * color_neon.c — NEON-optimized RGB ↔ YCbCr 4:2:0 conversion for TCodec
 *
 * Uses int32x4_t (32-bit) accumulation for luma to avoid overflow with
 * 14-bit fixed-point coefficients: max intermediate 255 × 9617 = 2,452,335
 * which exceeds int16_t range (32,767).
 *
 * Forward luma: NEON, 8 pixels/iteration with vmull_n_s16 (widening multiply).
 * Forward chroma + inverse: scalar (matches non-NEON exactly for roundtrip
 * correctness; chroma uses area-average which is hard to vectorize for 4:2:0).
 */

#include "tcodec_common.h"

#if TCODEC_NEON
#include <arm_neon.h>

/* Fixed-point BT.601 coefficients (14-bit precision) — must match scalar */
#define FP_YR  4899
#define FP_YG  9617
#define FP_YB  1868

#define FP_CBR (-2763)
#define FP_CBG (-5425)
#define FP_CBB 8192

#define FP_CRR 8192
#define FP_CRG (-6855)
#define FP_CRB (-1327)

/* Inverse coefficients (14-bit) — must match scalar */
#define FP_IR  22970
#define FP_IGC (-5638)
#define FP_IGR (-11696)
#define FP_IB  29032

/* Clamp two int32x4_t halves to [0,255] and narrow to uint8x8_t */
static inline uint8x8_t clamp32_to_u8(int32x4_t lo, int32x4_t hi)
{
    int32x4_t zero   = vdupq_n_s32(0);
    int32x4_t max255 = vdupq_n_s32(255);
    int32x4_t cl_lo  = vminq_s32(vmaxq_s32(lo, zero), max255);
    int32x4_t cl_hi  = vminq_s32(vmaxq_s32(hi, zero), max255);
    int16x4_t lo16   = vmovn_s32(cl_lo);
    int16x4_t hi16   = vmovn_s32(cl_hi);
    int16x8_t comb   = vcombine_s16(lo16, hi16);
    return vmovn_u16(vreinterpretq_u16_s16(comb));
}

void tc_rgb_to_ycbcr_internal(const uint8_t *rgb, int rgb_stride,
                           tc_pixel_t *y,  int stride_y,
                           tc_pixel_t *cb, int stride_cb,
                           tc_pixel_t *cr, int stride_cr,
                           int width, int height)
{
    int cw = width / 2;
    int ch = height / 2;

    /* ── Luma: 8 pixels/iteration with 32-bit accumulation ──── */
    for (int yy = 0; yy < height; yy++) {
        const uint8_t *row = rgb + yy * rgb_stride;
        tc_pixel_t *y_row  = y + yy * stride_y;
        int x;

        for (x = 0; x + 7 < width; x += 8) {
            /* Load 8 RGB triplets (24 bytes) and deinterleave */
            uint8x8x3_t rgb_vec = vld3_u8(row + x * 3);
            int16x8_t r16 = vreinterpretq_s16_u16(vmovl_u8(rgb_vec.val[0]));
            int16x8_t g16 = vreinterpretq_s16_u16(vmovl_u8(rgb_vec.val[1]));
            int16x8_t b16 = vreinterpretq_s16_u16(vmovl_u8(rgb_vec.val[2]));

            /* Low 4: Y = (YR*R + YG*G + YB*B + 8192) >> 14 */
            int32x4_t y_lo = vmull_n_s16(vget_low_s16(r16), FP_YR);
            y_lo = vmlal_n_s16(y_lo, vget_low_s16(g16), FP_YG);
            y_lo = vmlal_n_s16(y_lo, vget_low_s16(b16), FP_YB);
            y_lo = vrshrq_n_s32(y_lo, 14);

            /* High 4 */
            int32x4_t y_hi = vmull_n_s16(vget_high_s16(r16), FP_YR);
            y_hi = vmlal_n_s16(y_hi, vget_high_s16(g16), FP_YG);
            y_hi = vmlal_n_s16(y_hi, vget_high_s16(b16), FP_YB);
            y_hi = vrshrq_n_s32(y_hi, 14);

            vst1_u8(y_row + x, clamp32_to_u8(y_lo, y_hi));
        }
        /* Scalar tail */
        for (; x < width; x++) {
            int rv = row[x*3], gv = row[x*3+1], bv = row[x*3+2];
            y_row[x] = (uint8_t)tc_clip(
                (FP_YR*rv + FP_YG*gv + FP_YB*bv + 8192) >> 14, 0, 255);
        }
    }

    /* ── Chroma: scalar with 2×2 area-average (4:2:0) ────────
     * Identical to non-NEON scalar path for roundtrip correctness.
     * Per-pixel Cb/Cr computation then averaging is equivalent to
     * averaging RGB then computing Cb/Cr (linear transform), but
     * rounding differences can cause up to ±1 error which is
     * acceptable.  The scalar path is also fast enough for chroma
     * since it processes width/2 × height/2 pixels.  */
    for (int yy = 0; yy < ch; yy++) {
        tc_pixel_t *cb_row = cb + yy * stride_cb;
        tc_pixel_t *cr_row = cr + yy * stride_cr;
        for (int x = 0; x < cw; x++) {
            int sx = x * 2, sy = yy * 2;
            const uint8_t *p0 = rgb + sy       * rgb_stride + sx * 3;
            const uint8_t *p1 = rgb + (sy + 1) * rgb_stride + sx * 3;
            int r = (p0[0] + p0[3] + p1[0] + p1[3] + 2) / 4;
            int g = (p0[1] + p0[4] + p1[1] + p1[4] + 2) / 4;
            int b = (p0[2] + p0[5] + p1[2] + p1[5] + 2) / 4;

            cb_row[x] = (uint8_t)tc_clip(
                ((FP_CBR*r + FP_CBG*g + FP_CBB*b) >> 14) + 128, 0, 255);
            cr_row[x] = (uint8_t)tc_clip(
                ((FP_CRR*r + FP_CRG*g + FP_CRB*b) >> 14) + 128, 0, 255);
        }
    }
}

void tc_ycbcr_to_rgb_internal(const tc_pixel_t *y,  int stride_y,
                           const tc_pixel_t *cb, int stride_cb,
                           const tc_pixel_t *cr, int stride_cr,
                           uint8_t *rgb, int rgb_stride,
                           int width, int height)
{
    /* Scalar implementation with 14-bit precision — matches the non-NEON
     * scalar path exactly for roundtrip correctness.  The NEON version
     * previously used 13-bit precision which caused larger roundtrip
     * errors, and the int16 overflow in chroma multiplication produced
     * completely wrong results for saturated colors.  */
    for (int yy = 0; yy < height; yy++) {
        const tc_pixel_t *y_row  = y  + yy * stride_y;
        const tc_pixel_t *cb_row = cb + (yy / 2) * stride_cb;
        const tc_pixel_t *cr_row = cr + (yy / 2) * stride_cr;
        uint8_t *rgb_row = rgb + yy * rgb_stride;

        for (int x = 0; x < width; x++) {
            int Y  = y_row[x];
            int Cb = cb_row[x / 2] - 128;
            int Cr = cr_row[x / 2] - 128;

            /* 14-bit fixed-point, matching scalar exactly */
            int R = Y + ((FP_IR  * Cr + (1 << 13)) >> 14);
            int G = Y + ((FP_IGC * Cb + FP_IGR * Cr + (1 << 13)) >> 14);
            int B = Y + ((FP_IB  * Cb + (1 << 13)) >> 14);

            rgb_row[x * 3 + 0] = (uint8_t)tc_clip(R, 0, 255);
            rgb_row[x * 3 + 1] = (uint8_t)tc_clip(G, 0, 255);
            rgb_row[x * 3 + 2] = (uint8_t)tc_clip(B, 0, 255);
        }
    }
}

#endif /* TCODEC_NEON */
