/*
 * Adapted from https://github.com/cyb70289/utf8
 */
#include <stdint.h>
#include <stdbool.h>
#include <x86intrin.h>

#include "range_scalar.h"

/*
 * Every byte has to be within a range, which the three bytes in front of it
 * decide. Range table, map range index to min value and size; the ranges wrap
 * around and one of size 0 has nothing in it:
 * Index 0    : C2 ~ 7F (First Byte: not a continuation byte, nor C0 or C1)
 * Index 1    : 80 ~ BF (Second, Third, Fourth Byte)
 * Index 2    : A0 ~ BF (Second Byte after E0)
 * Index 3    : 80 ~ 9F (Second Byte after ED)
 * Index 4    : 90 ~ BF (Second Byte after F0)
 * Index 5    : 80 ~ 8F (Second Byte after F4)
 * Index 6    : illegal (Second Byte after F5 ~ FF)
 * Index 7~15 : not used
 *
 * F5 ~ FF pass as First Byte, it is the byte after them that is an error.
 *
 * There is no unsigned comparison of bytes, so the tables are moved by 0x80
 * for the signed one to do: with unsigned bytes
 *   byte - min < size
 * is the same as, with signed bytes
 *   byte - (min + 0x80) < size - 0x80
 */
#define RANGE_MIN(min) (uint8_t)((min) + 0x80)
#define RANGE_SIZE(size) (uint8_t)((size) - 0x80)

static const uint8_t _range_min_tbl[] = {
    RANGE_MIN(0xC2), RANGE_MIN(0x80), RANGE_MIN(0xA0), RANGE_MIN(0x80),
    RANGE_MIN(0x90), RANGE_MIN(0x80), RANGE_MIN(0x00), RANGE_MIN(0x00),
    RANGE_MIN(0x00), RANGE_MIN(0x00), RANGE_MIN(0x00), RANGE_MIN(0x00),
    RANGE_MIN(0x00), RANGE_MIN(0x00), RANGE_MIN(0x00), RANGE_MIN(0x00),
    RANGE_MIN(0xC2), RANGE_MIN(0x80), RANGE_MIN(0xA0), RANGE_MIN(0x80),
    RANGE_MIN(0x90), RANGE_MIN(0x80), RANGE_MIN(0x00), RANGE_MIN(0x00),
    RANGE_MIN(0x00), RANGE_MIN(0x00), RANGE_MIN(0x00), RANGE_MIN(0x00),
    RANGE_MIN(0x00), RANGE_MIN(0x00), RANGE_MIN(0x00), RANGE_MIN(0x00),
};
static const uint8_t _range_size_tbl[] = {
    RANGE_SIZE(0xBE), RANGE_SIZE(0x40), RANGE_SIZE(0x20), RANGE_SIZE(0x20),
    RANGE_SIZE(0x30), RANGE_SIZE(0x10), RANGE_SIZE(0x00), RANGE_SIZE(0x00),
    RANGE_SIZE(0x00), RANGE_SIZE(0x00), RANGE_SIZE(0x00), RANGE_SIZE(0x00),
    RANGE_SIZE(0x00), RANGE_SIZE(0x00), RANGE_SIZE(0x00), RANGE_SIZE(0x00),
    RANGE_SIZE(0xBE), RANGE_SIZE(0x40), RANGE_SIZE(0x20), RANGE_SIZE(0x20),
    RANGE_SIZE(0x30), RANGE_SIZE(0x10), RANGE_SIZE(0x00), RANGE_SIZE(0x00),
    RANGE_SIZE(0x00), RANGE_SIZE(0x00), RANGE_SIZE(0x00), RANGE_SIZE(0x00),
    RANGE_SIZE(0x00), RANGE_SIZE(0x00), RANGE_SIZE(0x00), RANGE_SIZE(0x00),
};

/*
 * Every First Byte (C0 ~ FF) sets the range index of the byte after it to 1.
 * These tables are for the First Bytes after which the Second Byte is not
 * 80 ~ BF. They contain "range index adjustment", by the low nibble of the
 * First Byte.
 * +------------+---------------+------------------+----------------+
 * | First Byte | original range| range adjustment | adjusted range |
 * +------------+---------------+------------------+----------------+
 * | E0         | 1             | 1                | 2              |
 * +------------+---------------+------------------+----------------+
 * | ED         | 1             | 2                | 3              |
 * +------------+---------------+------------------+----------------+
 * | F0         | 1             | 3                | 4              |
 * +------------+---------------+------------------+----------------+
 * | F4         | 1             | 4                | 5              |
 * +------------+---------------+------------------+----------------+
 * | F5 ~ FF    | 1             | 5                | 6 (illegal)    |
 * +------------+---------------+------------------+----------------+
 */
/* index0 -> E0, index13 -> ED */
static const uint8_t _e0_ef_tbl[] = {
    1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 2, 0, 0,
    1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 2, 0, 0,
};
/* index0 -> F0, index4 -> F4 */
static const uint8_t _f0_ff_tbl[] = {
    3, 0, 0, 0, 4, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5,
    3, 0, 0, 0, 4, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5,
};

/* Returns the bytes of input that are within their range */
static inline __m256i utf8_range_process_block(const __m256i input, const __m256i prev_input,
    const __m256i e0_ef_tbl, const __m256i f0_ff_tbl, const __m256i range_min_tbl, const __m256i range_size_tbl)
{
    /*
     * What is 1, 2 and 3 bytes in front of every byte. alignr works on the
     * two halves of the registers apart, so the bytes that are in front of
     * each half of the input are put together first.
     */
    const __m256i prev_halves = _mm256_permute2x128_si256(prev_input, input, 0x21);
    const __m256i prev1 = _mm256_alignr_epi8(input, prev_halves, 15);
    const __m256i prev2 = _mm256_alignr_epi8(input, prev_halves, 14);
    const __m256i prev3 = _mm256_alignr_epi8(input, prev_halves, 13);

    /*
     * Adjust Second Byte range for special First Bytes(E0,ED,F0,F4)
     * See _e0_ef_tbl[] and _f0_ff_tbl[] definition for details
     *
     * prev1:            | 00 ... DF | E0 ... EF | F0 ... FF |
     * prev1 - E0 + 70:  |   >= 90   | 70 ... 7F | 80 ... 8F |
     * prev1 - F0 + 70:  |   >= 80   |   >= 80   | 70 ... 7F |
     *
     * The additions saturate. shuffle takes the low nibble of an index up to
     * 7F, and gives 0 for one from 80.
     */
    const __m256i e0_ef = _mm256_adds_epu8(_mm256_sub_epi8(prev1, _mm256_set1_epi8((char)0xE0)), _mm256_set1_epi8(0x70));
    const __m256i f0_ff = _mm256_adds_epu8(_mm256_sub_epi8(prev1, _mm256_set1_epi8((char)0xF0)), _mm256_set1_epi8(0x70));
    __m256i range = _mm256_add_epi8(_mm256_shuffle_epi8(e0_ef_tbl, e0_ef), _mm256_shuffle_epi8(f0_ff_tbl, f0_ff));

    /*
     * Is a continuation due: Second Byte after C0 ~ FF, Third Byte after
     * E0 ~ FF, Fourth Byte after F0 ~ FF. The subtractions saturate, what is
     * left is not 0 where one is due.
     */
    __m256i continuation = _mm256_subs_epu8(prev1, _mm256_set1_epi8((char)0xBF));
    continuation = _mm256_or_si256(continuation, _mm256_subs_epu8(prev2, _mm256_set1_epi8((char)0xDF)));
    continuation = _mm256_or_si256(continuation, _mm256_subs_epu8(prev3, _mm256_set1_epi8((char)0xEF)));

    /*
     * Range index 1 and up where a continuation is due, 0 for a First Byte.
     * A First Byte where a continuation is due gets the range of that
     * continuation, which makes it an error: no range but 0 has First Bytes
     * in it. That covers the First Bytes that overlap as well, where the
     * range index of the bytes after them can be wrong.
     */
    range = _mm256_add_epi8(range, _mm256_min_epu8(continuation, _mm256_set1_epi8(1)));

    /* Load min value and size per calculated range index */
    const __m256i minv = _mm256_shuffle_epi8(range_min_tbl, range);
    const __m256i sizev = _mm256_shuffle_epi8(range_size_tbl, range);

    /* Check value range */
    return _mm256_cmpgt_epi8(sizev, _mm256_sub_epi8(input, minv));
}

static inline bool utf8_range_is_ascii8(const unsigned char *src)
{
    uint64_t data64;
    __builtin_memcpy(&data64, src, sizeof(data64));

    return (data64 & 0x8080808080808080ull) == 0;
}

static inline bool utf8_range_is_ascii64(const unsigned char *src)
{
    const __m256i input0 = _mm256_loadu_si256((const __m256i *)src);
    const __m256i input1 = _mm256_loadu_si256((const __m256i *)(src + 32));

    return _mm256_movemask_epi8(_mm256_or_si256(input0, input1)) == 0;
}

/* Steps over the ASCII at src, 64 bytes at a time */
static inline void utf8_range_skip_ascii(const unsigned char **src, int64_t *src_len)
{
    /* The last place 64 bytes can be read at, there is none in a short input */
    const int64_t last64 = *src_len - 64;
    int64_t at = 0;

    while (at <= last64 && utf8_range_is_ascii64(*src + at))
        at += 64;

    *src += at;
    *src_len -= at;
}

// gocc: utf8_valid_range_avx2(src string) bool
bool utf8_valid_range_avx2(const unsigned char *src, int64_t src_len)
{
    /*
     * The ASCII at the start, nothing is due there either: 64 and then 8
     * bytes at a time, so that the scalar code has no more than 7 bytes of
     * an input that is ASCII alone.
     */
    utf8_range_skip_ascii(&src, &src_len);
    while (src_len >= 8 && utf8_range_is_ascii8(src))
    {
        src += 8;
        src_len -= 8;
    }

    if (src_len >= 32)
    {
        __m256i prev_input = _mm256_set1_epi8(0);
        __m256i valid = _mm256_set1_epi8(-1);

        /* Cached tables */
        const __m256i e0_ef_tbl = _mm256_loadu_si256((const __m256i *)_e0_ef_tbl);
        const __m256i f0_ff_tbl = _mm256_loadu_si256((const __m256i *)_f0_ff_tbl);
        const __m256i range_min_tbl = _mm256_loadu_si256((const __m256i *)_range_min_tbl);
        const __m256i range_size_tbl = _mm256_loadu_si256((const __m256i *)_range_size_tbl);

        /* How many blocks of nothing but ASCII there were in a row */
        int ascii_blocks = 0;

        while (src_len >= 64)
        {
            const __m256i input0 = _mm256_loadu_si256((const __m256i *)src);
            const __m256i input1 = _mm256_loadu_si256((const __m256i *)(src + 32));

            const __m256i valid0 = utf8_range_process_block(input0, prev_input,
                                       e0_ef_tbl, f0_ff_tbl, range_min_tbl, range_size_tbl);
            const __m256i valid1 = utf8_range_process_block(input1, input0,
                                       e0_ef_tbl, f0_ff_tbl, range_min_tbl, range_size_tbl);
            prev_input = input1;

            valid = _mm256_and_si256(valid0, valid1);
            if (_mm256_movemask_epi8(valid) != -1)
                return false;

            src += 64;
            src_len -= 64;

            /*
             * A block of ASCII leaves nothing due after it, and the ASCII
             * that follows is valid as it is, so that can be skipped. The
             * branch to do so is cheap only if the processor guesses it,
             * which it does not in text that has a few bytes of ASCII here
             * and there: skip after 8 blocks of nothing but ASCII in a row.
             * The last bytes of prev_input are ASCII as well, which is all
             * the next block needs of it.
             */
            const bool is_ascii = _mm256_movemask_epi8(_mm256_or_si256(input0, input1)) == 0;
            ascii_blocks = is_ascii ? ascii_blocks + 1 : 0;
            if (ascii_blocks >= 8)
                utf8_range_skip_ascii(&src, &src_len);
        }

        while (src_len >= 32)
        {
            const __m256i input = _mm256_loadu_si256((const __m256i *)src);

            valid = _mm256_and_si256(valid, utf8_range_process_block(input, prev_input,
                                                e0_ef_tbl, f0_ff_tbl, range_min_tbl, range_size_tbl));
            prev_input = input;

            src += 32;
            src_len -= 32;
        }

        if (_mm256_movemask_epi8(valid) != -1)
            return false;

        /* Find previous token (not 80~BF) */
        int lookahead = 0;
        if ((int8_t)src[-1] > (int8_t)0xBF)
            lookahead = 1;
        else if ((int8_t)src[-2] > (int8_t)0xBF)
            lookahead = 2;
        else if ((int8_t)src[-3] > (int8_t)0xBF)
            lookahead = 3;

        src -= lookahead;
        src_len += lookahead;
    }

    return utf8_valid_scalar(src, src_len);
}
