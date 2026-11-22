/*
 * Adapted from https://github.com/cyb70289/utf8
 */
#include <stdint.h>
#include <stdbool.h>
#include <arm_neon.h>

#include "range_naive.h"

/*
 * Every byte has to be within a range, which the three bytes in front of it
 * decide. Range table, map range index to min value and size; the ranges wrap
 * around and one of size 0 has nothing in it:
 * Index 0    : 80 ~ BF (Second, Third, Fourth Byte)
 * Index 1    : A0 ~ BF (Second Byte after E0)
 * Index 2    : 80 ~ 9F (Second Byte after ED)
 * Index 3    : 90 ~ BF (Second Byte after F0)
 * Index 4    : 80 ~ 8F (Second Byte after F4)
 * Index 5    : illegal (Second Byte after F5 ~ FF)
 * Index 6    : C2 ~ 7F (First Byte: not a continuation byte, nor C0 or C1)
 * Index 7~15 : not used
 *
 * F5 ~ FF pass as First Byte, it is the byte after them that is an error.
 */
#define RANGE_FIRST_BYTE 6

static const uint8_t _range_min_tbl[] = {
    0x80, 0xA0, 0x80, 0x90, 0x80, 0x00, 0xC2, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
};
static const uint8_t _range_size_tbl[] = {
    0x40, 0x20, 0x20, 0x30, 0x10, 0x00, 0xBE, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
};

/*
 * This table is for the First Bytes after which the Second Byte is not
 * 80 ~ BF. It contains the range index of the Second Byte.
 * - The idea is to minus byte with E0, use the result(0~31) as the index to
 *   lookup the range index. Everything below E0 is outside of the table,
 *   which gives 0.
 */
static const uint8_t _second_range_tbl[] = {
    /* E0 */ 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 2, 0, 0,
    /* F0 */ 3, 0, 0, 0, 4, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5,
};

/* Returns the bytes of input that are within their range */
static inline uint8x16_t utf8_range_process_block(const uint8x16_t input, const uint8x16_t prev_input,
    const uint8x16_t const_c0, const uint8x16_t const_e0, const uint8x16_t const_f0,
    const uint8x16_t const_first_byte,
    const uint8x16x2_t second_range_tbl, const uint8x16_t range_min_tbl, const uint8x16_t range_size_tbl)
{
    /* What is 1, 2 and 3 bytes in front of every byte */
    const uint8x16_t prev1 = vextq_u8(prev_input, input, 15);
    const uint8x16_t prev2 = vextq_u8(prev_input, input, 14);
    const uint8x16_t prev3 = vextq_u8(prev_input, input, 13);

    /* Second Byte: range index by the First Byte */
    /* See _second_range_tbl[] definition for details */
    const uint8x16_t second_range = vqtbl2q_u8(second_range_tbl, vsubq_u8(prev1, const_e0));

    /* Is a continuation due: Second Byte after C0 ~ FF, */
    /* Third Byte after E0 ~ FF, Fourth Byte after F0 ~ FF */
    uint8x16_t continuation = vcgeq_u8(prev1, const_c0);
    continuation = vorrq_u8(continuation, vcgeq_u8(prev2, const_e0));
    continuation = vorrq_u8(continuation, vcgeq_u8(prev3, const_f0));

    /*
     * A First Byte where a continuation is due gets the range of that
     * continuation, which makes it an error: no range but the one of First
     * Bytes has First Bytes in it. That covers the First Bytes that overlap
     * as well, where the range index of the bytes after them can be wrong.
     */
    const uint8x16_t range = vbslq_u8(continuation, second_range, const_first_byte);

    /* Load min value and size per calculated range index */
    const uint8x16_t minv = vqtbl1q_u8(range_min_tbl, range);
    const uint8x16_t sizev = vqtbl1q_u8(range_size_tbl, range);

    /* Check value range */
    return vcltq_u8(vsubq_u8(input, minv), sizev);
}

static inline bool utf8_range_is_ascii8(const unsigned char *src)
{
    uint64_t data64;
    __builtin_memcpy(&data64, src, sizeof(data64));

    return (data64 & 0x8080808080808080ull) == 0;
}

// gocc: utf8_valid_range(src string) bool
bool utf8_valid_range(const unsigned char *src, int64_t src_len)
{
    if (src_len > 16)
    {
        uint8x16_t prev_input = vdupq_n_u8(0);
        uint8x16_t valid = vdupq_n_u8(0xFF);

        /* Cached tables */
        const uint8x16_t range_min_tbl = vld1q_u8(_range_min_tbl);
        const uint8x16_t range_size_tbl = vld1q_u8(_range_size_tbl);
        const uint8x16x2_t second_range_tbl = vld1q_u8_x2(_second_range_tbl);

        /* Cached values */
        const uint8x16_t const_c0 = vdupq_n_u8(0xC0);
        const uint8x16_t const_e0 = vdupq_n_u8(0xE0);
        const uint8x16_t const_f0 = vdupq_n_u8(0xF0);
        const uint8x16_t const_first_byte = vdupq_n_u8(RANGE_FIRST_BYTE);

        while (src_len >= 64)
        {
            const uint8x16x4_t input = vld1q_u8_x4(src);

            const uint8x16_t valid0 = utf8_range_process_block(input.val[0], prev_input,
                                          const_c0, const_e0, const_f0, const_first_byte,
                                          second_range_tbl, range_min_tbl, range_size_tbl);
            const uint8x16_t valid1 = utf8_range_process_block(input.val[1], input.val[0],
                                          const_c0, const_e0, const_f0, const_first_byte,
                                          second_range_tbl, range_min_tbl, range_size_tbl);
            const uint8x16_t valid2 = utf8_range_process_block(input.val[2], input.val[1],
                                          const_c0, const_e0, const_f0, const_first_byte,
                                          second_range_tbl, range_min_tbl, range_size_tbl);
            const uint8x16_t valid3 = utf8_range_process_block(input.val[3], input.val[2],
                                          const_c0, const_e0, const_f0, const_first_byte,
                                          second_range_tbl, range_min_tbl, range_size_tbl);
            prev_input = input.val[3];

            valid = vandq_u8(vandq_u8(valid0, valid1), vandq_u8(valid2, valid3));
            if (vminvq_u32(vreinterpretq_u32_u8(valid)) != UINT32_MAX)
                return false;

            src += 64;
            src_len -= 64;

            /*
             * ASCII at the end of the block leaves nothing due after it, and
             * the ASCII that follows is valid as it is: skip it, 8 bytes at a
             * time. The last bytes of prev_input are ASCII as well, which is
             * all the next block needs of it.
             */
            if (utf8_range_is_ascii8(src - 8))
            {
                /* The last place 8 bytes can be read at */
                const unsigned char *const last8 = src + src_len - 8;
                while (src <= last8 && utf8_range_is_ascii8(src))
                    src += 8;
                src_len = last8 + 8 - src;
            }
        }

        while (src_len >= 16)
        {
            const uint8x16_t input = vld1q_u8(src);

            valid = vandq_u8(valid, utf8_range_process_block(input, prev_input,
                                        const_c0, const_e0, const_f0, const_first_byte,
                                        second_range_tbl, range_min_tbl, range_size_tbl));
            prev_input = input;

            src += 16;
            src_len -= 16;
        }

        if (vminvq_u32(vreinterpretq_u32_u8(valid)) != UINT32_MAX)
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

    return utf8_valid_naive(src, src_len);
}
