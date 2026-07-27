#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <x86intrin.h>

// The function returns true (1) if all chars passed in src are
// 7-bit values (0x00..0x7F). Otherwise, it returns false (0).
// gocc: isAsciiSse(src string) bool
bool is_ascii_sse(unsigned char *src, uint64_t src_len)
{
    // ASCII chars have MSB = 0, so we use ptest to detect any set MSBs
    if (src_len >= 16)
    {
        // Mask with MSB set in each byte position
        const __m128i hi_mask_v = _mm_set1_epi8((char)0x80);

        // Process 4 vectors at once for better ILP (64 bytes per iteration)
        for (const unsigned char *data64_end = (src + src_len) - (src_len % 64); src < data64_end; src += 64)
        {
            __m128i v0 = _mm_loadu_si128((const __m128i *)(src));
            __m128i v1 = _mm_loadu_si128((const __m128i *)(src + 16));
            __m128i v2 = _mm_loadu_si128((const __m128i *)(src + 32));
            __m128i v3 = _mm_loadu_si128((const __m128i *)(src + 48));

            // OR all vectors together - if any byte has MSB set, result will too
            __m128i combined = _mm_or_si128(_mm_or_si128(v0, v1),
                                            _mm_or_si128(v2, v3));

            // PTEST: returns 1 if (combined & hi_mask_v) == 0 (all ASCII)
            if (!_mm_testz_si128(combined, hi_mask_v))
            {
                return false;
            }
        }
        src_len %= 64;

        // Process remaining 16-byte chunks
        for (const unsigned char *data16_end = (src + src_len) - (src_len % 16); src < data16_end; src += 16)
        {
            __m128i chunk = _mm_loadu_si128((const __m128i *)(src));
            if (!_mm_testz_si128(chunk, hi_mask_v))
            {
                return false;
            }
        }
        src_len %= 16;
    }

    // Scalar fallback for remaining bytes (0-15 bytes)
    if (src_len == 0)
        return true;

    if (src_len & 8)
    {
        uint64_t lo64, hi64;
        uint8_t *data_end = src + src_len;
        __builtin_memcpy(&lo64, src, sizeof(lo64));
        __builtin_memcpy(&hi64, data_end - 8, sizeof(hi64));

        uint64_t data64 = lo64 | hi64;
        return (data64 & 0x8080808080808080ull) ? false : true;
    }

    if (src_len & 4)
    {
        uint32_t lo32, hi32;
        uint8_t *data_end = src + src_len;
        __builtin_memcpy(&lo32, src, sizeof(lo32));
        __builtin_memcpy(&hi32, data_end - 4, sizeof(hi32));

        uint32_t data32 = lo32 | hi32;
        return (data32 & 0x80808080) ? false : true;
    }

    uint32_t data32 = 0;

    // branchless check for 1-3 bytes
    uint8_t *data_end = src + src_len;
    int idx = src_len >> 1;
    data32 |= src[0];
    data32 |= src[idx];
    data32 |= data_end[-1];

    return (data32 & 0x80808080) ? false : true;
}

#include "ascii_scalar.c"

// Returns mask where 0xFF = match, 0x00 = mismatch under ASCII case folding.
static inline __m128i equal_fold_vec(__m128i va, __m128i vb,
                                     __m128i v_0x20, __m128i v_0x1f,
                                     __m128i v_0x9a, __m128i v_0x01) {
    // diff = a ^ b (0x00 if equal, 0x20 if case differs, other if mismatch)
    __m128i diff = _mm_xor_si128(va, vb);

    // mask_0x20 = (diff == 0x20) - potential case difference
    __m128i mask_0x20 = _mm_cmpeq_epi8(diff, v_0x20);

    // Check if character is ASCII letter [A-Za-z]
    // Force to lowercase: tmp = a | 0x20
    __m128i tmp = _mm_or_si128(va, v_0x20);
    // Shift range: tmp = tmp + 0x1f  (now 'a'=0x80, 'z'=0x99)
    tmp = _mm_add_epi8(tmp, v_0x1f);
    // is_alpha = (0x9a > tmp) signed - true for 0x80-0x99
    __m128i is_alpha = _mm_cmpgt_epi8(v_0x9a, tmp);

    // acceptable_diff = is_alpha & mask_0x20 & 0x01, then 0x01 -> 0x20
    __m128i acceptable = _mm_and_si128(is_alpha, mask_0x20);
    acceptable = _mm_and_si128(acceptable, v_0x01);
    acceptable = _mm_slli_epi16(acceptable, 5);

    // Match if diff == acceptable (both 0, or both 0x20 for a valid case bit)
    return _mm_cmpeq_epi8(diff, acceptable);
}

// ASCII case-insensitive string comparison using SSE4.1
// gocc: equalFoldSse(a, b string) bool
bool equal_fold_sse(const char *a, uint64_t a_len, const char *b, uint64_t b_len) {
    if (a_len != b_len)
        return false;

    size_t len = a_len;

    if (len < 16) return equal_fold_scalar((const uint8_t *)a, (const uint8_t *)b, len);

    const __m128i v_0x20 = _mm_set1_epi8(0x20);
    const __m128i v_0x1f = _mm_set1_epi8(0x1f);
    const __m128i v_0x9a = _mm_set1_epi8((char)0x9a);
    const __m128i v_0x01 = _mm_set1_epi8(0x01);
    const __m128i all_ones = _mm_set1_epi8((char)0xFF);

    // Process 64 bytes at a time (4 x 16) for better ILP
    for (const char *end = (a + len) - (len % 64); a < end; a += 64, b += 64) {
        __m128i a0 = _mm_loadu_si128((const __m128i *)a);
        __m128i a1 = _mm_loadu_si128((const __m128i *)(a + 16));
        __m128i a2 = _mm_loadu_si128((const __m128i *)(a + 32));
        __m128i a3 = _mm_loadu_si128((const __m128i *)(a + 48));
        __m128i b0 = _mm_loadu_si128((const __m128i *)b);
        __m128i b1 = _mm_loadu_si128((const __m128i *)(b + 16));
        __m128i b2 = _mm_loadu_si128((const __m128i *)(b + 32));
        __m128i b3 = _mm_loadu_si128((const __m128i *)(b + 48));

        __m128i eq0 = equal_fold_vec(a0, b0, v_0x20, v_0x1f, v_0x9a, v_0x01);
        __m128i eq1 = equal_fold_vec(a1, b1, v_0x20, v_0x1f, v_0x9a, v_0x01);
        __m128i eq2 = equal_fold_vec(a2, b2, v_0x20, v_0x1f, v_0x9a, v_0x01);
        __m128i eq3 = equal_fold_vec(a3, b3, v_0x20, v_0x1f, v_0x9a, v_0x01);
        __m128i combined = _mm_and_si128(_mm_and_si128(eq0, eq1),
                                         _mm_and_si128(eq2, eq3));

        // PTEST: testc returns 1 if (~combined & all_ones) == 0
        if (!_mm_testc_si128(combined, all_ones)) {
            return false;
        }
    }
    len %= 64;

    // Process remaining 16-byte chunks
    for (const char *end = (a + len) - (len % 16); a < end; a += 16, b += 16) {
        __m128i va = _mm_loadu_si128((const __m128i *)a);
        __m128i vb = _mm_loadu_si128((const __m128i *)b);
        __m128i eq = equal_fold_vec(va, vb, v_0x20, v_0x1f, v_0x9a, v_0x01);

        if (!_mm_testc_si128(eq, all_ones)) {
            return false;
        }
    }
    len %= 16;

    if (len == 0)
        return true;

    // Overlapped tail load for final 1-15 bytes (safe: caller ensured len >= 16)
    const char *aEnd = (a + len) - 16;
    const char *bEnd = (b + len) - 16;

    __m128i va = _mm_loadu_si128((const __m128i *)aEnd);
    __m128i vb = _mm_loadu_si128((const __m128i *)bEnd);
    __m128i eq = equal_fold_vec(va, vb, v_0x20, v_0x1f, v_0x9a, v_0x01);

    return _mm_testc_si128(eq, all_ones);
}

// Fold a 128-bit vector to uppercase (a-z -> A-Z)
static inline __m128i fold_to_upper_vec(__m128i v,
                                        __m128i v_0x20, __m128i v_0x1f,
                                        __m128i v_0x9a) {
    // Shift range: tmp = v + 0x1f  (now 'a'=0x80, 'z'=0x99)
    __m128i tmp = _mm_add_epi8(v, v_0x1f);
    // is_lower = (0x9a > tmp) signed - true for lowercase letters only
    __m128i is_lower = _mm_cmpgt_epi8(v_0x9a, tmp);
    __m128i sub_mask = _mm_and_si128(is_lower, v_0x20);
    return _mm_sub_epi8(v, sub_mask);
}

// Load 1-15 bytes into a 128-bit register (zero-padded)
static inline __m128i load_data16(const unsigned char *src, int64_t len) {
    if (len >= 16) {
        return _mm_loadu_si128((const __m128i *)src);
    } else if (len <= 0) {
        return _mm_setzero_si128();
    }

    uint64_t d0 = 0, d1 = 0;
    int64_t pos;

    if (len >= 8) {
        __builtin_memcpy(&d0, src, 8);
        int64_t rem = len - 8;
        pos = 0;
        if (rem & 4) { uint32_t t; __builtin_memcpy(&t, src + 8, 4); d1 = t; pos = 4; }
        if (rem & 2) { uint16_t t; __builtin_memcpy(&t, src + 8 + pos, 2); d1 |= (uint64_t)t << (pos * 8); pos += 2; }
        if (rem & 1) { d1 |= (uint64_t)src[8 + pos] << (pos * 8); }
    } else {
        pos = 0;
        if (len & 4) { uint32_t t; __builtin_memcpy(&t, src, 4); d0 = t; pos = 4; }
        if (len & 2) { uint16_t t; __builtin_memcpy(&t, src + pos, 2); d0 |= (uint64_t)t << (pos * 8); pos += 2; }
        if (len & 1) { d0 |= (uint64_t)src[pos] << (pos * 8); }
    }

    return _mm_set_epi64x(d1, d0);
}

static inline __m128i prepare_needle16(const uint16_t *needle,
                                       __m128i v_0x20, __m128i v_0x1f,
                                       __m128i v_0x9a) {
    __m128i needle_vec = _mm_set1_epi16(*needle);
    return fold_to_upper_vec(needle_vec, v_0x20, v_0x1f, v_0x9a);
}

// Special case: search for a single byte case-insensitively
static inline int64_t index_fold_1_byte_sse(const unsigned char *haystack, int64_t haystack_len,
                                            uint8_t needle) {
    const unsigned char *haystack_start = haystack;

    if (needle >= 'a' && needle <= 'z') needle -= 0x20;

    const __m128i v_0x20 = _mm_set1_epi8(0x20);
    const __m128i v_0x1f = _mm_set1_epi8(0x1f);
    const __m128i v_0x9a = _mm_set1_epi8((char)0x9a);
    const __m128i needle_vec = _mm_set1_epi8(needle);

    for (const unsigned char *data_bound = haystack + haystack_len - (haystack_len % 16);
         haystack < data_bound; haystack += 16) {
        __m128i data = _mm_loadu_si128((const __m128i *)haystack);
        __m128i folded = fold_to_upper_vec(data, v_0x20, v_0x1f, v_0x9a);
        __m128i cmp = _mm_cmpeq_epi8(folded, needle_vec);
        int mask = _mm_movemask_epi8(cmp);
        if (mask) {
            return (haystack - haystack_start) + __builtin_ctz(mask);
        }
    }
    haystack_len %= 16;

    for (int64_t i = 0; i < haystack_len; i++) {
        uint8_t c = haystack[i];
        if (c >= 'a' && c <= 'z') c -= 0x20;
        if (c == needle) {
            return (haystack - haystack_start) + i;
        }
    }

    return -1;
}

// Process a 16-byte block for 2-byte needle search, returns match mask.
// Bit N represents a match starting at position N-1 (caller adjusts).
static inline uint32_t index_fold_2byte_block(
    __m128i folded, __m128i prev_folded,
    __m128i needle_vec) {

    __m128i cmp_even = _mm_cmpeq_epi16(folded, needle_vec);

    // Odd positions: palignr shifts by 1 byte across the previous block
    __m128i shifted = _mm_alignr_epi8(folded, prev_folded, 15);
    __m128i cmp_odd = _mm_cmpeq_epi16(shifted, needle_vec);

    int mask_even = _mm_movemask_epi8(cmp_even);
    int mask_odd = _mm_movemask_epi8(cmp_odd);

    int valid_even = mask_even & (mask_even >> 1) & 0x5555;
    int valid_odd = mask_odd & (mask_odd >> 1) & 0x5555;

    return (uint32_t)((valid_even << 1) | valid_odd);
}

static inline int64_t index_fold_2_byte_sse(const unsigned char *haystack, int64_t haystack_len,
                                            const uint16_t *needle) {
    const int64_t checked_len = haystack_len - 2;
    if (checked_len < 0) return -1;

    const __m128i v_0x20 = _mm_set1_epi8(0x20);
    const __m128i v_0x1f = _mm_set1_epi8(0x1f);
    const __m128i v_0x9a = _mm_set1_epi8((char)0x9a);
    const __m128i needle_vec = prepare_needle16(needle, v_0x20, v_0x1f, v_0x9a);

    __m128i prev_folded = _mm_setzero_si128();

    for (int64_t i = 0; i <= checked_len + 16; i += 16) {
        int64_t remaining = haystack_len - i;
        if (remaining <= 0) break;

        __m128i data = (remaining >= 16) ? _mm_loadu_si128((const __m128i *)(haystack + i))
                                          : load_data16(haystack + i, remaining);
        __m128i folded = fold_to_upper_vec(data, v_0x20, v_0x1f, v_0x9a);

        uint32_t matches = index_fold_2byte_block(folded, prev_folded, needle_vec);
        prev_folded = folded;

        if (i == 0) {
            matches &= ~1u;
        }

        while (matches) {
            int pos = __builtin_ctz(matches);
            matches &= matches - 1;

            int64_t match_pos = i + pos - 1;
            if (match_pos >= 0 && match_pos <= checked_len) {
                return match_pos;
            }
        }
    }

    return -1;
}

// Process a 16-byte block for first2+last2 matching
static inline uint32_t index_fold_block(
    __m128i data, __m128i data_end,
    __m128i prev_data, __m128i prev_data_end,
    __m128i first2, __m128i last2,
    __m128i v_0x20, __m128i v_0x1f, __m128i v_0x9a) {

    __m128i folded = fold_to_upper_vec(data, v_0x20, v_0x1f, v_0x9a);
    __m128i folded_end = fold_to_upper_vec(data_end, v_0x20, v_0x1f, v_0x9a);
    __m128i prev_folded = fold_to_upper_vec(prev_data, v_0x20, v_0x1f, v_0x9a);
    __m128i prev_folded_end = fold_to_upper_vec(prev_data_end, v_0x20, v_0x1f, v_0x9a);

    __m128i cmp_first_even = _mm_cmpeq_epi16(folded, first2);
    __m128i cmp_last_even = _mm_cmpeq_epi16(folded_end, last2);
    __m128i cmp_even = _mm_and_si128(cmp_first_even, cmp_last_even);

    __m128i shifted = _mm_alignr_epi8(folded, prev_folded, 15);
    __m128i shifted_end = _mm_alignr_epi8(folded_end, prev_folded_end, 15);
    __m128i cmp_first_odd = _mm_cmpeq_epi16(shifted, first2);
    __m128i cmp_last_odd = _mm_cmpeq_epi16(shifted_end, last2);
    __m128i cmp_odd = _mm_and_si128(cmp_first_odd, cmp_last_odd);

    int mask_even = _mm_movemask_epi8(cmp_even);
    int mask_odd = _mm_movemask_epi8(cmp_odd);

    int valid_even = mask_even & (mask_even >> 1) & 0x5555;
    int valid_odd = mask_odd & (mask_odd >> 1) & 0x5555;

    return (uint32_t)((valid_even << 1) | valid_odd);
}

// gocc: indexFoldRabinKarpSse(a, b string) int
int64_t index_fold_rabin_karp_sse(unsigned char *haystack, int64_t haystack_len,
                                  unsigned char *needle, int64_t needle_len) {
    if (haystack_len < needle_len) return -1;
    if (needle_len <= 0) return 0;
    if (haystack_len == needle_len) {
        return equal_fold_scalar(haystack, needle, needle_len) ? 0 : -1;
    }

    switch (needle_len) {
    case 1:
        return index_fold_1_byte_sse(haystack, haystack_len, needle[0]);
    case 2:
        return index_fold_2_byte_sse(haystack, haystack_len, (const uint16_t *)needle);
    }

    return index_fold_rabin_karp_core(haystack, haystack_len, needle, needle_len);
}

// gocc: indexFoldSse(a, b string) int
int64_t index_fold_sse(unsigned char *haystack, int64_t haystack_len,
                       unsigned char *needle, int64_t needle_len) {
    if (haystack_len < needle_len) return -1;
    if (needle_len <= 0) return 0;
    if (haystack_len == needle_len) {
        return equal_fold_scalar(haystack, needle, needle_len) ? 0 : -1;
    }

    switch (needle_len) {
    case 1:
        return index_fold_1_byte_sse(haystack, haystack_len, needle[0]);
    case 2:
        return index_fold_2_byte_sse(haystack, haystack_len, (const uint16_t *)needle);
    }

    const __m128i v_0x20 = _mm_set1_epi8(0x20);
    const __m128i v_0x1f = _mm_set1_epi8(0x1f);
    const __m128i v_0x9a = _mm_set1_epi8((char)0x9a);

    const __m128i first2 = prepare_needle16((const uint16_t *)needle, v_0x20, v_0x1f, v_0x9a);
    const __m128i last2 = prepare_needle16((const uint16_t *)(needle + needle_len - 2), v_0x20, v_0x1f, v_0x9a);

    const int64_t checked_len = haystack_len - needle_len;
    __m128i prev_data = _mm_setzero_si128();
    __m128i prev_data_end = _mm_setzero_si128();
    int64_t failures = 0;

    for (int64_t i = 0; i <= checked_len + 16; i += 16) {
        int64_t remaining = haystack_len - i;
        if (remaining <= 0) break;

        __m128i data = (remaining >= 16) ? _mm_loadu_si128((const __m128i *)(haystack + i))
                                          : load_data16(haystack + i, remaining);
        int64_t end_remaining = remaining - needle_len + 2;
        __m128i data_end = (end_remaining >= 16) ? _mm_loadu_si128((const __m128i *)(haystack + i + needle_len - 2))
                                                  : (end_remaining > 0) ? load_data16(haystack + i + needle_len - 2, end_remaining)
                                                                        : _mm_setzero_si128();

        uint32_t candidates = index_fold_block(data, data_end, prev_data, prev_data_end,
                                                first2, last2, v_0x20, v_0x1f, v_0x9a);
        prev_data = data;
        prev_data_end = data_end;

        if (i == 0) {
            candidates &= ~1u;
        }

        if (candidates) {
            while (candidates) {
                int pos = __builtin_ctz(candidates);
                candidates &= candidates - 1;

                int64_t match_pos = i + pos - 1;
                if (match_pos < 0 || match_pos > checked_len) continue;

                if (needle_len <= 4 ||
                    equal_fold_scalar((const uint8_t *)(haystack + match_pos + 2),
                                      (const uint8_t *)(needle + 2),
                                      needle_len - 4)) {
                    return match_pos;
                }
                failures++;
            }

            // Bail out to Rabin-Karp once false candidates dominate. Bit 15 of
            // the block reported position i + 14, so i + 15 is still unchecked.
            const int64_t rk_start = i + 15;
            if (failures > 4 + (rk_start >> 4) && rk_start < checked_len) {
                const int64_t rk_pos = index_fold_rabin_karp_core(haystack + rk_start,
                                                                 haystack_len - rk_start,
                                                                 needle, needle_len);
                return rk_pos < 0 ? -1 : rk_start + rk_pos;
            }
        }
    }

    return -1;
}