/*
 * Adapted from https://github.com/cyb70289/utf8
 */
#include <stdint.h>
#include <stdbool.h>
#include <x86intrin.h>

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
 */
static const uint8_t _range_min_tbl[] = {
    0xC2, 0x80, 0xA0, 0x80, 0x90, 0x80, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
};
static const uint8_t _range_size_tbl[] = {
    0xBE, 0x40, 0x20, 0x20, 0x30, 0x10, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
};

/*
 * The range index of the Second Byte, by the First Byte less C0. Index 0 is
 * every byte up to C0: none of them has a Second Byte, and C0 is an error by
 * itself.
 */
static const uint8_t _second_range_tbl[] = {
    /* C0 ~ DF */
    0, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,
    1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,
    /* E0 ~ EF: A0 ~ BF after E0, 80 ~ 9F after ED */
    2, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 3, 1, 1,
    /* F0 ~ FF: 90 ~ BF after F0, 80 ~ 8F after F4, nothing after F5 ~ FF */
    4, 1, 1, 1, 5, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6,
};

/* The place of every byte in a block */
static const uint8_t _byte_index_tbl[] = {
     0,  1,  2,  3,  4,  5,  6,  7,  8,  9, 10, 11, 12, 13, 14, 15,
    16, 17, 18, 19, 20, 21, 22, 23, 24, 25, 26, 27, 28, 29, 30, 31,
    32, 33, 34, 35, 36, 37, 38, 39, 40, 41, 42, 43, 44, 45, 46, 47,
    48, 49, 50, 51, 52, 53, 54, 55, 56, 57, 58, 59, 60, 61, 62, 63,
};

/*
 * Returns the bytes of input that are within their range. prev1, prev2 and
 * prev3 are what is 1, 2 and 3 bytes in front of every byte.
 */
static inline __mmask64 utf8_range_check(const __m512i input, const __m512i prev1, const __m512i prev2, const __m512i prev3,
    const __m512i second_range_tbl, const __m512i range_min_tbl, const __m512i range_size_tbl)
{
    /*
     * Second Byte after C0 ~ FF: its range is in the table. The subtraction
     * saturates, as the one below.
     */
    __m512i range = _mm512_permutexvar_epi8(_mm512_subs_epu8(prev1, _mm512_set1_epi8((char)0xC0)), second_range_tbl);

    /*
     * Third Byte after E0 ~ FF, Fourth Byte after F0 ~ FF: range index 1.
     * One comparison does for both, prev3 less 10 is E0 and up where prev3
     * is F0 and up.
     *
     * A First Byte where a continuation is due gets the range of that
     * continuation, which makes it an error: no range but 0 has First Bytes
     * in it. That covers the First Bytes that overlap as well, where the
     * range index of the bytes after them can be wrong.
     */
    const __m512i third_fourth = _mm512_max_epu8(prev2, _mm512_subs_epu8(prev3, _mm512_set1_epi8(0x10)));
    const __mmask64 continuation = _mm512_cmpge_epu8_mask(third_fourth, _mm512_set1_epi8((char)0xE0));
    range = _mm512_mask_max_epu8(range, continuation, range, _mm512_set1_epi8(1));

    /* Load min value and size per calculated range index */
    const __m512i minv = _mm512_shuffle_epi8(range_min_tbl, range);
    const __m512i sizev = _mm512_shuffle_epi8(range_size_tbl, range);

    /* Check value range */
    return _mm512_cmplt_epu8_mask(_mm512_sub_epi8(input, minv), sizev);
}

/*
 * The same for a block that has the 64 bytes in front of it in a register,
 * and not in memory: the first block and the last one.
 */
static inline __mmask64 utf8_range_check_after(const __m512i input, const __m512i prev_input,
    const __m512i second_range_tbl, const __m512i range_min_tbl, const __m512i range_size_tbl)
{
    /*
     * alignr_epi8 works on the four quarters of the registers apart, so the
     * 16 bytes in front of each quarter of the input are put together first.
     */
    const __m512i prev16 = _mm512_alignr_epi64(input, prev_input, 6);
    const __m512i prev1 = _mm512_alignr_epi8(input, prev16, 15);
    const __m512i prev2 = _mm512_alignr_epi8(input, prev16, 14);
    const __m512i prev3 = _mm512_alignr_epi8(input, prev16, 13);

    return utf8_range_check(input, prev1, prev2, prev3, second_range_tbl, range_min_tbl, range_size_tbl);
}

/*
 * Returns the src_len bytes at src, 1 to 63 of them, with zeros after them.
 * It reads 64 bytes: the ones from src on, or the ones up to the end of the
 * input where the former go into the next page. That page may not be there;
 * the page src is in is, and it has those 64 bytes, as src is near its end.
 */
static inline __m512i utf8_range_load_tail(const unsigned char *src, int64_t src_len)
{
    const __mmask64 left = ((uint64_t)1 << src_len) - 1;

    if (((uintptr_t)src & 4095) <= 4096 - 64)
        return _mm512_maskz_mov_epi8(left, _mm512_loadu_si512(src));

    /* The bytes are at the end of what is read: move them down to the start */
    const __m512i from = _mm512_add_epi8(_mm512_loadu_si512(_byte_index_tbl), _mm512_set1_epi8((char)(64 - src_len)));
    return _mm512_maskz_permutexvar_epi8(left, from, _mm512_loadu_si512(src + src_len - 64));
}

// gocc: utf8_valid_range_avx512(src string) bool
// gocc-variant: utf8_valid_range_avx512_bytes(src []byte) bool
bool utf8_valid_range_avx512(const unsigned char *src, int64_t src_len)
{
    /*
     * The ASCII at the start: nothing is due there. last64 is the last place
     * 64 bytes can be read at, there is none in a short input.
     */
    const int64_t last64 = src_len - 64;
    int64_t ascii_len = 0;

    while (ascii_len <= last64 && _mm512_movepi8_mask(_mm512_loadu_si512(src + ascii_len)) == 0)
        ascii_len += 64;

    src += ascii_len;
    src_len -= ascii_len;

    /* Nothing left: the input was ASCII, or empty */
    if (src_len == 0)
        return true;

    /* Cached tables */
    const __m512i second_range_tbl = _mm512_loadu_si512(_second_range_tbl);
    const __m512i range_min_tbl = _mm512_broadcast_i32x4(_mm_loadu_si128((const __m128i *)_range_min_tbl));
    const __m512i range_size_tbl = _mm512_broadcast_i32x4(_mm_loadu_si128((const __m128i *)_range_size_tbl));

    /* In front of the first block there is ASCII or nothing: zeros do for both */
    __m512i prev_input = _mm512_setzero_si512();

    if (src_len >= 64)
    {
        if (utf8_range_check_after(_mm512_loadu_si512(src), prev_input,
                                   second_range_tbl, range_min_tbl, range_size_tbl) != UINT64_MAX)
            return false;

        src += 64;
        src_len -= 64;

        /* The blocks after it have the bytes in front of them in memory */
        while (src_len >= 64)
        {
            if (utf8_range_check(_mm512_loadu_si512(src), _mm512_loadu_si512(src - 1),
                                 _mm512_loadu_si512(src - 2), _mm512_loadu_si512(src - 3),
                                 second_range_tbl, range_min_tbl, range_size_tbl) != UINT64_MAX)
                return false;

            src += 64;
            src_len -= 64;
        }

        prev_input = _mm512_loadu_si512(src - 64);
    }

    /*
     * What is left, with zeros after it. A zero is an error where a
     * continuation is due, so this block is also what finds a character cut
     * short at the end of the input, and it has to be looked at when nothing
     * is left as well.
     */
    const __m512i input = src_len ? utf8_range_load_tail(src, src_len) : _mm512_setzero_si512();

    return utf8_range_check_after(input, prev_input, second_range_tbl, range_min_tbl, range_size_tbl) == UINT64_MAX;
}
