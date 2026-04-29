/*
 * predict.c — Intra prediction for TCodec
 *
 * 9 intra prediction modes:
 *   0: Planar  (bilinear interpolation from boundaries)
 *   1: DC      (average of reference samples)
 *   2..8: Angular (directional prediction along 7 angles)
 *
 * Reference samples come from:
 *   - Above: reconstructed pixels directly above the block
 *   - Left:  reconstructed pixels to the left of the block
 *   - If unavailable (frame boundary), use DC substitution
 *
 * All operations are integer-only, NEON-friendly.
 */

#include "tcodec_common.h"
#include <string.h>

/* ── Reference sample collection ─────────────────────────────── */

void tc_intra_get_ref(const tc_pixel_t *recon, int stride,
                      int x, int y, int blk_size, int frame_w, int frame_h,
                      tc_pixel_t *ref_above, tc_pixel_t *ref_left)
{
    /* Default value when no reference available */
    tc_pixel_t default_val = 128;

    /* Above row: positions (x-1, y-1) .. (x+2*blk_size-1, y-1)
     * But we need x-1 to x+2*blk_size-2 (2*blk_size - 1 samples above + top-right)
     * Simplified: we get blk_size*2 samples from above */
    int have_above = (y > 0);
    int have_left  = (x > 0);

    if (have_above) {
        const tc_pixel_t *above_row = recon + (y - 1) * stride;
        /* Top-left corner */
        if (have_left) {
            ref_left[blk_size] = above_row[x - 1];  /* Store above-left in ref_left[n] */
        }
        /* Above samples */
        for (int i = 0; i < 2 * blk_size; i++) {
            int px = x + i;
            if (px < frame_w) {
                ref_above[i] = above_row[px];
            } else {
                /* Repeat last available */
                ref_above[i] = above_row[frame_w - 1];
            }
        }
        /* Top-left */
        if (have_left) {
            ref_above[-1] = above_row[x - 1];
        } else {
            ref_above[-1] = ref_above[0];
        }
    } else {
        for (int i = 0; i < 2 * blk_size; i++) {
            ref_above[i] = default_val;
        }
        ref_above[-1] = default_val;
    }

    /* Left column: positions (x-1, y-1) .. (x-1, y+2*blk_size-2) */
    if (have_left) {
        for (int i = 0; i < 2 * blk_size; i++) {
            int py = y + i;
            if (py < frame_h) {
                ref_left[i] = recon[py * stride + (x - 1)];
            } else {
                ref_left[i] = recon[(frame_h - 1) * stride + (x - 1)];
            }
        }
        /* Above-left is shared with ref_above[-1] */
        if (have_above) {
            ref_left[-1] = recon[(y - 1) * stride + (x - 1)];
        } else {
            ref_left[-1] = ref_left[0];
        }
    } else {
        for (int i = 0; i < 2 * blk_size; i++) {
            ref_left[i] = default_val;
        }
        ref_left[-1] = default_val;
    }

    /* If neither available, use DC */
    if (!have_above && !have_left) {
        for (int i = 0; i < 2 * blk_size; i++) {
            ref_above[i] = default_val;
            ref_left[i]  = default_val;
        }
        ref_above[-1] = default_val;
        ref_left[-1]  = default_val;
    }
}

/* ── Planar prediction (Mode 0) ────────────────────────────────
 *
 * Bilinear interpolation from top and left boundaries:
 *   pred[x][y] = (top[x] * (N-y) + left[y] * (N-x) + top_left * x * y + N) / (2*N)
 * where N = block_size.
 *
 * This smoothly blends horizontal and vertical predictions.
 * ══════════════════════════════════════════════════════════════ */

static void intra_planar(tc_pixel_t *TCODEC_RESTRICT dst, int dst_stride,
                         const tc_pixel_t *ref_above,
                         const tc_pixel_t *ref_left,
                         int blk_size)
{
    int N = blk_size;
    (void)N;  /* Used implicitly in the angular prediction modes */

    for (int y = 0; y < N; y++) {
        for (int x = 0; x < N; x++) {
            int v_left  = ref_left[y + 1];      /* Left column */
            int v_top   = ref_above[x + 1];     /* Top row */
            int v_tl    = ref_above[0];         /* Top-left */

            int pred = (v_top * (N - 1 - y) + v_left * (N - 1 - x)
                        + v_tl * (x + y) + N) / (2 * N);
            dst[y * dst_stride + x] = (tc_pixel_t)tc_clip(pred, 0, 255);
        }
    }
}

/* ── DC prediction (Mode 1) ──────────────────────────────────── */

static void intra_dc(tc_pixel_t *TCODEC_RESTRICT dst, int dst_stride,
                     const tc_pixel_t *ref_above,
                     const tc_pixel_t *ref_left,
                     int blk_size)
{
    int sum = 0;
    int count = 0;

    for (int i = 0; i < blk_size; i++) {
        sum += ref_above[i + 1];
        sum += ref_left[i + 1];
        count += 2;
    }

    int dc_val = (sum + (count >> 1)) / count;

    for (int y = 0; y < blk_size; y++) {
        for (int x = 0; x < blk_size; x++) {
            dst[y * dst_stride + x] = (tc_pixel_t)dc_val;
        }
    }
}

/* ── Angular prediction (Modes 2..8) ────────────────────────────
 *
 * Directional prediction along angles:
 *   Mode 2: angle ≈ 45° NE   (intraRightUp)
 *   Mode 3: angle ≈ 26° NE   (intraUpRight)
 *   Mode 4: angle ≈ 11° NNE
 *   Mode 5: angle =  0°  N   (vertical)
 *   Mode 6: angle ≈ -11° NNW
 *   Mode 7: angle ≈ -26° NW  (intraUpLeft)
 *   Mode 8: angle ≈ -45° WN  (intraLeftUp)
 *
 * Simplified: we project each pixel (x,y) back along the
 * prediction direction to find the reference position, then
 * interpolate from the reference samples.
 *
 * For performance, we use integer math with 1/32 precision.
 * ══════════════════════════════════════════════════════════════ */

/* Angle table: inverse displacement per row, in 1/32 pixel units.
 * Positive = projects to above, negative = projects to left. */
static const int angular_disp[7] = {
     32,    /* Mode 2:  1.0  px/row (45°)    */
     16,    /* Mode 3:  0.5  px/row (26°)    */
      6,    /* Mode 4:  0.19 px/row (11°)    */
      0,    /* Mode 5:  0    px/row (0° = vertical) */
     -6,    /* Mode 6: -0.19 px/row (-11°)   */
    -16,    /* Mode 7: -0.5  px/row (-26°)   */
    -32,    /* Mode 8: -1.0  px/row (-45°)   */
};

static void intra_angular(tc_pixel_t *TCODEC_RESTRICT dst, int dst_stride,
                          const tc_pixel_t *ref_above,
                          const tc_pixel_t *ref_left,
                          int blk_size, int mode_idx)
{
    int disp = angular_disp[mode_idx - 2];  /* Index into displacement table */
    int frac_bits = 5;                       /* 1/32 pixel precision */

    for (int y = 0; y < blk_size; y++) {
        for (int x = 0; x < blk_size; x++) {
            /* Project (x,y) along direction to find reference position */
            int ref_pos_x = (x << frac_bits) + y * disp;

            if (disp >= 0 || ref_pos_x >= 0) {
                /* Project onto above row */
                int abs_pos = ref_pos_x >> frac_bits;
                int frac    = ref_pos_x & ((1 << frac_bits) - 1);
                int idx0 = abs_pos + 1;  /* +1 because ref_above[0] = above-left */

                /* Bilinear interpolation from above */
                int p0 = ref_above[tc_clip(idx0, 0, 2 * blk_size)];
                int p1 = ref_above[tc_clip(idx0 + 1, 0, 2 * blk_size)];

                int pred = p0 + (((p1 - p0) * frac + (1 << (frac_bits - 1))) >> frac_bits);
                dst[y * dst_stride + x] = (tc_pixel_t)tc_clip(pred, 0, 255);
            } else {
                /* Project onto left column */
                int abs_pos = (-ref_pos_x) >> frac_bits;
                int frac    = (-ref_pos_x) & ((1 << frac_bits) - 1);
                int idx0 = abs_pos + 1;

                int p0 = ref_left[tc_clip(idx0, 0, 2 * blk_size)];
                int p1 = ref_left[tc_clip(idx0 + 1, 0, 2 * blk_size)];

                int pred = p0 + (((p1 - p0) * frac + (1 << (frac_bits - 1))) >> frac_bits);
                dst[y * dst_stride + x] = (tc_pixel_t)tc_clip(pred, 0, 255);
            }
        }
    }
}

/* ── Top-level intra prediction dispatcher ───────────────────── */

void tc_intra_predict(tc_pixel_t *TCODEC_RESTRICT dst, int dst_stride,
                      const tc_pixel_t *ref_above,
                      const tc_pixel_t *ref_left,
                      int blk_size, tc_intra_mode_t mode)
{
    switch (mode) {
    case TC_INTRA_PLANAR:
        intra_planar(dst, dst_stride, ref_above, ref_left, blk_size);
        break;
    case TC_INTRA_DC:
        intra_dc(dst, dst_stride, ref_above, ref_left, blk_size);
        break;
    default:
        if (mode >= TC_INTRA_ANGULAR_START && mode < TC_INTRA_MAX) {
            intra_angular(dst, dst_stride, ref_above, ref_left, blk_size, mode);
        } else {
            /* Fallback to DC */
            intra_dc(dst, dst_stride, ref_above, ref_left, blk_size);
        }
        break;
    }
}

/* ── SAD cost for mode decision ──────────────────────────────── */

tc_sad_t tc_intra_cost(const tc_pixel_t *orig, int orig_stride,
                        const tc_pixel_t *pred, int blk_size)
{
    tc_sad_t sad = 0;
    for (int y = 0; y < blk_size; y++) {
        for (int x = 0; x < blk_size; x++) {
            int diff = (int)orig[y * orig_stride + x] - (int)pred[y * blk_size + x];
            sad += tc_abs(diff);
        }
    }
    return sad;
}
