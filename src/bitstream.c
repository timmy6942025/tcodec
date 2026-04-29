/*
 * bitstream.c — Bitstream reader/writer for TCodec
 *
 * Supports:
 *  - Raw bits (fixed-length)
 *  - Unsigned Exp-Golomb (ue)
 *  - Signed Exp-Golomb (se)
 *  - Byte alignment
 */

#include "tcodec_common.h"
#include <assert.h>

/* ── Writer ──────────────────────────────────────────────────── */

void tc_bs_writer_init(tc_bs_writer_t *w, uint8_t *buf, size_t capacity)
{
    w->buf      = buf;
    w->capacity = capacity;
    w->byte_pos = 0;
    w->bit_pos  = 0;
    if (capacity > 0) w->buf[0] = 0;
}

static void bs_ensure_capacity(tc_bs_writer_t *w, size_t need)
{
    TCODEC_UNUSED(w);
    TCODEC_UNUSED(need);
    /* In production, reallocate. For now, caller ensures capacity. */
}

void tc_bs_writer_write_bits(tc_bs_writer_t *w, uint32_t val, int nbits)
{
    assert(nbits >= 0 && nbits <= 32);
    if (nbits == 0) return;

    /* Mask to nbits */
    if (nbits < 32) val &= (1u << nbits) - 1;

    while (nbits > 0) {
        bs_ensure_capacity(w, w->byte_pos + 1);

        int avail = 8 - w->bit_pos;
        int write = tc_min(nbits, avail);

        /* Shift val so the top 'write' bits align with available slots */
        int shift = avail - write;
        uint8_t bits = (uint8_t)((val >> (nbits - write)) << shift);

        w->buf[w->byte_pos] |= bits;

        w->bit_pos += write;
        nbits -= write;

        if (w->bit_pos == 8) {
            w->bit_pos = 0;
            w->byte_pos++;
            if (w->byte_pos < w->capacity) w->buf[w->byte_pos] = 0;
        }
    }
}

void tc_bs_writer_write_ue(tc_bs_writer_t *w, uint32_t val)
{
    /* Exp-Golomb: val+1 encoded as (zeros)(1)(suffix)
     * val+1 in binary has floor(log2(val+1))+1 bits.
     * Prefix zeros = that count - 1. */
    uint32_t code = val + 1;
    int      bits = 0;
    uint32_t tmp  = code;
    while (tmp > 0) { bits++; tmp >>= 1; }
    int leading_zeros = bits - 1;

    tc_bs_writer_write_bits(w, 0, leading_zeros);    /* prefix zeros */
    tc_bs_writer_write_bits(w, code, bits);           /* 1 + suffix */
}

void tc_bs_writer_write_se(tc_bs_writer_t *w, int32_t val)
{
    /* Signed Exp-Golomb: map 0→0, 1→1, -1→2, 2→3, -2→4, ... */
    uint32_t mapped;
    if (val > 0)       mapped = (uint32_t)(2 * val - 1);
    else if (val < 0)  mapped = (uint32_t)(-2 * val);
    else               mapped = 0;
    tc_bs_writer_write_ue(w, mapped);
}

void tc_bs_writer_byte_align(tc_bs_writer_t *w)
{
    if (w->bit_pos > 0) {
        w->bit_pos = 0;
        w->byte_pos++;
        if (w->byte_pos < w->capacity) w->buf[w->byte_pos] = 0;
    }
}

size_t tc_bs_writer_bytes(tc_bs_writer_t *w)
{
    return w->byte_pos + (w->bit_pos > 0 ? 1 : 0);
}

/* ── Reader ──────────────────────────────────────────────────── */

void tc_bs_reader_init(tc_bs_reader_t *r, const uint8_t *buf, size_t size)
{
    r->buf      = buf;
    r->size     = size;
    r->byte_pos = 0;
    r->bit_pos  = 0;
}

uint32_t tc_bs_reader_read_bits(tc_bs_reader_t *r, int nbits)
{
    assert(nbits >= 0 && nbits <= 32);
    uint32_t val = 0;

    while (nbits > 0) {
        if (r->byte_pos >= r->size) return val;

        int avail = 8 - r->bit_pos;
        int read  = tc_min(nbits, avail);

        uint8_t byte = r->buf[r->byte_pos];
        int     shift = avail - read;
        uint8_t bits = (byte >> shift) & ((1u << read) - 1);

        val = (val << read) | bits;

        r->bit_pos += read;
        nbits -= read;

        if (r->bit_pos == 8) {
            r->bit_pos = 0;
            r->byte_pos++;
        }
    }
    return val;
}

uint32_t tc_bs_reader_read_ue(tc_bs_reader_t *r)
{
    /* Count leading zeros */
    int leading_zeros = 0;
    while (r->byte_pos < r->size) {
        uint32_t bit = tc_bs_reader_read_bits(r, 1);
        if (bit == 1) break;
        leading_zeros++;
        if (leading_zeros > 31) return 0; /* error protection */
    }

    /* Read suffix (leading_zeros bits after the 1) */
    uint32_t suffix = 0;
    if (leading_zeros > 0) {
        suffix = tc_bs_reader_read_bits(r, leading_zeros);
    }

    return (1u << leading_zeros) - 1 + suffix;
}

int32_t tc_bs_reader_read_se(tc_bs_reader_t *r)
{
    uint32_t mapped = tc_bs_reader_read_ue(r);
    if (mapped == 0) return 0;
    if (mapped & 1)  return (int32_t)((mapped + 1) / 2);   /* positive */
    return -(int32_t)(mapped / 2);                           /* negative */
}

int tc_bs_reader_eof(tc_bs_reader_t *r)
{
    return r->byte_pos >= r->size;
}
