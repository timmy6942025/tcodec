/*
 * tcodec_types.h — Core type definitions for TCodec
 *
 * All internal types used across the codec. Designed for
 * cache-line alignment and NEON-friendly layout.
 */

#ifndef TCODEC_TYPES_H
#define TCODEC_TYPES_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ── Architecture detection ──────────────────────────────────── */

#if defined(__aarch64__) || defined(__ARM_NEON) || defined(__ARM_NEON__)
  #define TCODEC_NEON 1
#else
  #define TCODEC_NEON 0
#endif

#if defined(__GNUC__) || defined(__clang__)
  #define TCODEC_ALIGN(x) __attribute__((aligned(x)))
  #define TCODEC_LIKELY(x)   __builtin_expect(!!(x), 1)
  #define TCODEC_UNLIKELY(x) __builtin_expect(!!(x), 0)
  #define TCODEC_INLINE static inline __attribute__((always_inline))
  #define TCODEC_RESTRICT __restrict__
  #define TCODEC_FORCEINLINE __attribute__((always_inline)) inline
#else
  #define TCODEC_ALIGN(x)
  #define TCODEC_LIKELY(x)   (x)
  #define TCODEC_UNLIKELY(x) (x)
  #define TCODEC_INLINE static inline
  #define TCODEC_RESTRICT
  #define TCODEC_FORCEINLINE inline
#endif

/* ── Constants ───────────────────────────────────────────────── */

#define TC_CTU_SIZE         64          /* CTU = 64×64 luma pixels */
#define TC_BLOCK_4x4        4
#define TC_BLOCK_8x8        8
#define TC_MAX_WIDTH        4096
#define TC_MAX_HEIGHT       4096
#define TC_MIN_BLOCK        4
#define TC_INTRA_MODES      18          /* Planar + DC + 7 vertical angular + 9 horizontal angular */
#define TC_REF_FRAMES       4           /* DPB size */
#define TC_MV_PREC          4           /* Quarter-pel motion */
#define TC_QP_MIN           0
#define TC_QP_MAX           63
#define TC_QP_DEFAULT       32
#define TC_TANS_TABLE_BITS  10          /* tANS table = 1024 entries */
#define TC_TANS_TABLE_SIZE  (1 << TC_TANS_TABLE_BITS)
#define TC_NUM_CONTEXTS     16          /* Reserved for future ANS context coding */
#define TC_TILE_MAX         8

/* Magic bytes */
#define TC_MAGIC_0  0x54  /* 'T' */
#define TC_MAGIC_1  0x43  /* 'C' */
#define TC_MAGIC_2  0x56  /* 'V' */
#define TC_VERSION  0

/* Frame header size */
#define TC_FRAME_HEADER_SIZE 12

/* ── Pixel types ─────────────────────────────────────────────── */

typedef uint8_t  tc_pixel_t;           /* 8-bit luma/chroma */
typedef int16_t  tc_coeff_t;           /* Transform coefficient */
typedef int16_t  tc_pred_t;            /* Prediction sample (extends 8-bit) */
typedef int32_t  tc_mv_t;              /* Motion vector in quarter-pel */
typedef int32_t  tc_sad_t;             /* Sum of absolute differences */

/* ── Enumerations ────────────────────────────────────────────── */

typedef enum {
    TC_FRAME_KEY     = 0,              /* Intra-only (I-frame) */
    TC_FRAME_INTER   = 1,              /* P-frame with 1 reference */
    TC_FRAME_BIDIR   = 2,              /* B-frame with 2 references (future) */
} tc_frame_type_t;

typedef enum {
    TC_BLOCK_4x4_ID  = 0,
    TC_BLOCK_8x8_ID  = 1,
} tc_block_size_t;

typedef enum {
    TC_INTRA_PLANAR  = 0,
    TC_INTRA_DC      = 1,
    TC_INTRA_ANGULAR_START = 2,
    /* Vertical angular modes 2..8 (project onto above row):
     * 2=NE(45°), 3=NNE(26°), 4=NNW(11°), 5=N(0°), 6=NWW(-11°), 7=NW(-26°), 8=WN(-45°) */
    TC_INTRA_ANGULAR_VERT_END = 9,
    /* Horizontal angular modes 9..17 (project onto left column):
     * 9=EN(45°), 10=EEN(26°), 11=EE(N)(11°), 12=E(0°=horizontal),
     * 13=WW(N)(-11°), 14=WWN(-26°), 15=WN(horiz)(-45°),
     * 16=NNW(horiz, -56°), 17=NNE(horiz, 56°) */
    TC_INTRA_ANGULAR_HORIZ_START = 9,
    TC_INTRA_MAX     = 18,
} tc_intra_mode_t;

typedef enum {
    TC_PRESET_ULTRAFAST = 0,
    TC_PRESET_FAST      = 1,
    TC_PRESET_MEDIUM    = 2,
    TC_PRESET_SLOW      = 3,
} tc_preset_t;

typedef enum {
    TC_RC_CQP  = 0,                    /* Constant QP */
    TC_RC_CBR  = 1,                    /* Constant bitrate */
    TC_RC_VBR  = 2,                    /* Variable bitrate */
} tc_ratectrl_t;

/* ── Error codes ─────────────────────────────────────────────── */

typedef enum {
    TC_OK              =  0,
    TC_ERR_MEMORY      = -1,
    TC_ERR_PARAM       = -2,
    TC_ERR_BITSTREAM   = -3,
    TC_ERR_EOF         = -4,
    TC_ERR_INTERNAL    = -5,
} tc_error_t;

/* ── Motion vector ────────────────────────────────────────────── */

typedef struct tc_mv {
    tc_mv_t x;                         /* Quarter-pel horizontal */
    tc_mv_t y;                         /* Quarter-pel vertical */
} tc_mv_s;

/* ── Reference picture entry ────────────────────────────────── */

typedef struct tc_ref_entry {
    struct tc_frame_buf *frame;        /* Reference frame buffer */
    int32_t             poc;           /* Picture order count */
    int32_t             qp_avg;        /* Average QP of this ref */
} tc_ref_entry_t;

/* ── tANS context state (reserved for future ANS optimization) ─ */

typedef struct TCODEC_ALIGN(64) tc_tans_ctx {
    uint16_t freq[256];                /* Symbol frequencies (scaled) */
    uint16_t cum_freq[257];            /* Cumulative frequencies */
    uint32_t total_freq;               /* Sum of all frequencies */
    uint8_t  adapt_counter;            /* Adaptation countdown */
} tc_tans_ctx_t;
/* Note: tc_tans_ctx_t is currently unused by the Exp-Golomb coding path.
 * It is kept for future tANS/ANS integration. Contexts are NOT allocated
 * in tc_tans_enc_t / tc_tans_dec_t to avoid ~40KB waste per instance. */

/* ── Block coding info ───────────────────────────────────────── */

typedef struct tc_block_info {
    tc_block_size_t  dct_size;         /* 4×4 or 8×8 */
    uint8_t          is_intra;         /* 1=intra, 0=inter */
    tc_intra_mode_t  intra_mode;       /* If intra */
    tc_mv_s          mv;               /* If inter */
    uint8_t          ref_idx;          /* Reference index (0..3) */
} tc_block_info_t;

/* ── CTU coding info ─────────────────────────────────────────── */

#define TC_BLOCKS_PER_CTU 256         /* 64×64 / 4×4 = 16×16 = 256 */
#define TC_8x8_PER_CTU    64          /* 64×64 / 8×8 = 8×8 = 64 */

typedef struct TCODEC_ALIGN(128) tc_ctu_info {
    tc_block_info_t blocks[TC_8x8_PER_CTU];  /* Per-8×8 block info */
    uint8_t         dct_size_map[TC_8x8_PER_CTU]; /* 4×4 or 8×8 per block */
    tc_coeff_t      coeffs[TC_CTU_SIZE * TC_CTU_SIZE]; /* Residual buffer */
    int32_t         row;              /* CTU row in frame */
    int32_t         col;              /* CTU column in frame */
} tc_ctu_info_t;

/* ── Frame header ────────────────────────────────────────────── */

typedef struct tc_frame_header {
    uint8_t         magic[3];         /* 0x54, 0x43, 0x56 */
    uint8_t         version;          /* 0 */
    uint16_t        width;            /* Frame width */
    uint16_t        height;           /* Frame height */
    uint8_t         flags;            /* key_frame(1), tile bits */
    uint8_t         qp_delta;         /* QP delta from previous */
    uint8_t         frame_num;        /* Frame counter low 8 bits */
    uint8_t         reserved;
    /* Derived fields (not in bitstream) */
    tc_frame_type_t frame_type;
    uint8_t         qp;               /* Absolute QP for this frame */
    uint8_t         tile_cols_log2;
    uint8_t         tile_rows_log2;
} tc_frame_header_t;

/* Frame header flags */
#define TC_FLAG_KEY_FRAME   0x80
#define TC_FLAG_WPP          0x40      /* WPP row entry points present */
#define TC_FLAG_TILE_C_MASK 0x0C      /* tile_cols_log2 in bits 2-3 */
#define TC_FLAG_TILE_R_MASK 0x03      /* tile_rows_log2 in bits 0-1 */

/* ── Frame buffer ────────────────────────────────────────────── */

typedef struct tc_frame_buf {
    tc_pixel_t *y;                    /* Luma plane (width × height) */
    tc_pixel_t *cb;                   /* Cb plane (width/2 × height/2) */
    tc_pixel_t *cr;                   /* Cr plane (width/2 × height/2) */
    int32_t     stride_y;             /* Luma stride (bytes per row) */
    int32_t     stride_c;             /* Chroma stride */
    int32_t     width;                /* Frame width */
    int32_t     height;               /* Frame height */
    int32_t     poc;                  /* Picture order count */
    uint8_t     qp_avg;               /* Average QP */
    int         owned;                /* 1 if we allocated the memory */
} tc_frame_buf_t;

/* ── Packet (encoded frame data) ─────────────────────────────── */

typedef struct tc_packet {
    uint8_t  *data;                   /* Encoded bitstream data */
    size_t    size;                   /* Size in bytes */
    int64_t   pts;                    /* Presentation timestamp */
    int       key_frame;              /* 1 if key frame */
} tc_packet_t;

/* ── Encoder/Decoder config ──────────────────────────────────── */

typedef struct tc_config {
    int32_t         width;
    int32_t         height;
    tc_preset_t     preset;
    tc_ratectrl_t   rc_method;
    int32_t         qp;               /* Initial QP (CQP) or target (VBR/CBR) */
    int32_t         target_bitrate;   /* Bits per second (CBR/VBR) */
    int32_t         fps_num;          /* Frame rate numerator */
    int32_t         fps_den;          /* Frame rate denominator */
    int32_t         keyframe_interval;/* Key frame interval (0 = auto) */
    int32_t         threads;          /* Number of WPP threads (0 = auto) */
    int32_t         tile_cols;        /* Number of tile columns (0 = auto) */
    int32_t         tile_rows;        /* Number of tile rows (0 = auto) */
} tc_config_t;

#ifdef __cplusplus
}
#endif

#endif /* TCODEC_TYPES_H */
