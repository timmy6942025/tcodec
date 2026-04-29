/*
 * color.c — RGB ↔ YCbCr 4:2:0 conversion for TCodec
 *
 * Uses ITU-R BT.601 coefficients (SDR video standard):
 *   Y  =  0.299 R + 0.587 G + 0.114 B
 *   Cb = -0.169 R - 0.331 G + 0.500 B  + 128
 *   Cr =  0.500 R - 0.419 G - 0.081 B  + 128
 *
 * All arithmetic is integer with 16-bit fixed-point (14 fractional bits).
 * Chroma subsampling: 4×4 luma blocks → 2×2 chroma blocks (area average).
 */

#include "tcodec_common.h"

/* Fixed-point BT.601 coefficients (14-bit precision) */
#define FP_YR  4899    /* 0.299 × 16384 ≈ 4899  */
#define FP_YG  9617    /* 0.587 × 16384 ≈ 9617  */
#define FP_YB  1868    /* 0.114 × 16384 ≈ 1868  */

#define FP_CBR (-2763) /* -0.169 × 16384 ≈ -2763 */
#define FP_CBG (-5425) /* -0.331 × 16384 ≈ -5425 */
#define FP_CBB 8192    /*  0.500 × 16384 = 8192  */

#define FP_CRR 8192    /*  0.500 × 16384 = 8192  */
#define FP_CRG (-6855) /* -0.419 × 16384 ≈ -6855 */
#define FP_CRB (-1327) /* -0.081 × 16384 ≈ -1327 */

/* Inverse coefficients for YCbCr → RGB (scaled by 16384, then >> 14) */
/* R = Y + 1.402 (Cr-128)   →  1.402 × 16384 ≈ 22970 */
/* G = Y - 0.344 (Cb-128) - 0.714 (Cr-128) */
/* B = Y + 1.772 (Cb-128)   →  1.772 × 16384 ≈ 29032 */

#define FP_IR  22970
#define FP_IGC (-5638)   /* -0.344 × 16384 ≈ -5638 */
#define FP_IGR (-11696)  /* -0.714 × 16384 ≈ -11696 */
#define FP_IB  29032     /*  1.772 × 16384 ≈ 29032 */

/* ── RGB → YCbCr ────────────────────────────────────────────── */

#if TCODEC_NEON  /* NEON version in color_neon.c replaces this */
extern void tc_rgb_to_ycbcr_internal(const uint8_t *rgb, int rgb_stride,
                               tc_pixel_t *y,  int stride_y,
                               tc_pixel_t *cb, int stride_cb,
                               tc_pixel_t *cr, int stride_cr,
                               int width, int height);
extern void tc_ycbcr_to_rgb_internal(const tc_pixel_t *y,  int stride_y,
                               const tc_pixel_t *cb, int stride_cb,
                               const tc_pixel_t *cr, int stride_cr,
                               uint8_t *rgb, int rgb_stride,
                               int width, int height);
#else
/* Scalar helpers — only needed when NEON is not available */
TCODEC_INLINE uint8_t clip8(int v) {
    return (uint8_t)(v < 0 ? 0 : (v > 255 ? 255 : v));
}
void tc_rgb_to_ycbcr_internal(const uint8_t *rgb, int rgb_stride,
                               tc_pixel_t *y,  int stride_y,
                               tc_pixel_t *cb, int stride_cb,
                               tc_pixel_t *cr, int stride_cr,
                               int width, int height)
{
    int cw = width  / 2;
    int ch = height / 2;

    /* First pass: compute full-resolution Y and accumulate chroma */
    /* We use a temporary buffer for chroma accumulation (16-bit) */
    int16_t *cb_acc = (int16_t *)calloc((size_t)cw * ch * 2, sizeof(int16_t));
    int16_t *cr_acc = (int16_t *)calloc((size_t)cw * ch * 2, sizeof(int16_t));

    for (int row = 0; row < height; row++) {
        const uint8_t *rgb_row = rgb + row * rgb_stride;
        tc_pixel_t *y_row = y + row * stride_y;

        for (int col = 0; col < width; col++) {
            int R = rgb_row[col * 3 + 0];
            int G = rgb_row[col * 3 + 1];
            int B = rgb_row[col * 3 + 2];

            /* Luma */
            int Y = (FP_YR * R + FP_YG * G + FP_YB * B + (1 << 13)) >> 14;
            y_row[col] = (tc_pixel_t)clip8(Y);

            /* Chroma accumulation: 2×2 box filter → 4:2:0 */
            int crow = row / 2;
            int ccol = col / 2;
            if (crow < ch && ccol < cw) {
                int cb_val = (FP_CBR * R + FP_CBG * G + FP_CBB * B + (1 << 13)) >> 14;
                int cr_val = (FP_CRR * R + FP_CRG * G + FP_CRB * B + (1 << 13)) >> 14;
                cb_acc[crow * cw + ccol] += cb_val + 128;
                cr_acc[crow * cw + ccol] += cr_val + 128;
            }
        }
    }

    /* Second pass: average chroma and write output */
    for (int row = 0; row < ch; row++) {
        tc_pixel_t *cb_row = cb + row * stride_cb;
        tc_pixel_t *cr_row = cr + row * stride_cr;
        for (int col = 0; col < cw; col++) {
            cb_row[col] = (tc_pixel_t)clip8(cb_acc[row * cw + col] / 4);
            cr_row[col] = (tc_pixel_t)clip8(cr_acc[row * cw + col] / 4);
        }
    }

    free(cb_acc);
    free(cr_acc);
}

/* ── YCbCr → RGB ────────────────────────────────────────────── */

void tc_ycbcr_to_rgb_internal(const tc_pixel_t *y,  int stride_y,
                               const tc_pixel_t *cb, int stride_cb,
                               const tc_pixel_t *cr, int stride_cr,
                               uint8_t *rgb, int rgb_stride,
                               int width, int height)
{
    for (int row = 0; row < height; row++) {
        const tc_pixel_t *y_row  = y  + row * stride_y;
        const tc_pixel_t *cb_row = cb + (row / 2) * stride_cb;
        const tc_pixel_t *cr_row = cr + (row / 2) * stride_cr;
        uint8_t *rgb_row = rgb + row * rgb_stride;

        for (int col = 0; col < width; col++) {
            int Y  = y_row[col];
            int Cb = cb_row[col / 2] - 128;
            int Cr = cr_row[col / 2] - 128;

            /* Coefficients are 14-bit (×16384), so >>14 to get result.
             * Rounding: + (1<<13) for round-half-up. */
            int R = Y + ((FP_IR * Cr + (1 << 13)) >> 14);
            int G = Y + ((FP_IGC * Cb + FP_IGR * Cr + (1 << 13)) >> 14);
            int B = Y + ((FP_IB * Cb + (1 << 13)) >> 14);

            rgb_row[col * 3 + 0] = clip8(R);
            rgb_row[col * 3 + 1] = clip8(G);
            rgb_row[col * 3 + 2] = clip8(B);
        }
    }
}
#endif /* TCODEC_NEON — scalar color convert fallback */

/* Public API wrappers — always compiled, dispatch to _internal (scalar or NEON) */
tc_error_t tc_rgb_to_ycbcr(const uint8_t *rgb, int rgb_stride,
                            tc_pixel_t *y,  int stride_y,
                            tc_pixel_t *cb, int stride_cb,
                            tc_pixel_t *cr, int stride_cr,
                            int width, int height)
{
    if (!rgb || !y || !cb || !cr || width <= 0 || height <= 0)
        return TC_ERR_PARAM;
    tc_rgb_to_ycbcr_internal(rgb, rgb_stride, y, stride_y,
                              cb, stride_cb, cr, stride_cr, width, height);
    return TC_OK;
}

tc_error_t tc_ycbcr_to_rgb(const tc_pixel_t *y,  int stride_y,
                            const tc_pixel_t *cb, int stride_cb,
                            const tc_pixel_t *cr, int stride_cr,
                            uint8_t *rgb, int rgb_stride,
                            int width, int height)
{
    if (!y || !cb || !cr || !rgb || width <= 0 || height <= 0)
        return TC_ERR_PARAM;
    tc_ycbcr_to_rgb_internal(y, stride_y, cb, stride_cb, cr, stride_cr,
                              rgb, rgb_stride, width, height);
    return TC_OK;
}
