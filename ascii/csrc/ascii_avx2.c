#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <x86intrin.h>

// gocc: isAsciiAvx(src string) bool
bool is_ascii_avx(unsigned char *src, uint64_t src_len)
{
    // ASCII chars have MSB = 0, so we use movemask to detect any set MSBs
    if (src_len >= 16)
    {
        // Mask with MSB set in each byte position
        const __m256i hi_mask_v = _mm256_set1_epi8((char)0x80);

        // Process 4 vectors at once for better ILP (128 bytes per iteration)
        for (const unsigned char *data128_end = (src + src_len) - (src_len % 128); src < data128_end; src += 128)
        {
            __m256i v0 = _mm256_loadu_si256((const __m256i *)(src));
            __m256i v1 = _mm256_loadu_si256((const __m256i *)(src + 32));
            __m256i v2 = _mm256_loadu_si256((const __m256i *)(src + 64));
            __m256i v3 = _mm256_loadu_si256((const __m256i *)(src + 96));

            // OR all vectors together - if any byte has MSB set, result will too
            __m256i combined = _mm256_or_si256(_mm256_or_si256(v0, v1),
                                               _mm256_or_si256(v2, v3));

            // VPTEST: returns 1 if (combined & hi_mask_v) == 0 (all ASCII)
            if (!_mm256_testz_si256(combined, hi_mask_v))
            {
                return false;
            }
        }
        src_len %= 128;

        // Process remaining 32-byte chunks
        for (const unsigned char *data32_end = (src + src_len) - (src_len % 32); src < data32_end; src += 32)
        {
            __m256i chunk = _mm256_loadu_si256((const __m256i *)(src));
            if (!_mm256_testz_si256(chunk, hi_mask_v))
            {
                return false;
            }
        }
        src_len %= 32;

        if (src_len >= 16)
        {
            __m128i chunk = _mm_loadu_si128((const __m128i *)(src));
            if (!_mm_testz_si128(chunk, _mm256_castsi256_si128(hi_mask_v)))
            {
                return false;
            }
            src += 16;
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

static inline __m256i broadcast_word_bytes(uint64_t word)
{
    __m128i lo = _mm_cvtsi64_si128((long long)word);
    __m128i table = _mm_unpacklo_epi64(lo, lo);
    return _mm256_broadcastsi128_si256(table);
}

// gocc: indexAnyAvxBitset(data string, bitset0 uint64, bitset1 uint64, bitset2 uint64, bitset3 uint64) int
int64_t index_any_avx_bitset(unsigned char *data, uint64_t data_len,
    uint64_t bitset0, uint64_t bitset1, uint64_t bitset2, uint64_t bitset3)
{
    if (data_len == 0) {
        return -1;
    }

    const unsigned char *data_start = data;
    const __m256i zero = _mm256_setzero_si256();
    const __m256i all_ones = _mm256_set1_epi8((char)0xFF);

    const __m256i mask01 = _mm256_set1_epi8(0x01);
    const __m256i mask02 = _mm256_set1_epi8(0x02);
    const __m256i mask04 = _mm256_set1_epi8(0x04);
    const __m256i mask07 = _mm256_set1_epi8(0x07);
    const __m256i mask08 = _mm256_set1_epi8(0x08);
    const __m256i mask10 = _mm256_set1_epi8(0x10);
    const __m256i mask20 = _mm256_set1_epi8(0x20);
    const __m256i mask40 = _mm256_set1_epi8(0x40);
    const __m256i mask80 = _mm256_set1_epi8((char)0x80);
    const __m256i bit_lut = broadcast_word_bytes(0x8040201008040201ULL);

    const __m256i table0 = broadcast_word_bytes(bitset0);
    const __m256i table1 = broadcast_word_bytes(bitset1);
    const __m256i table2 = broadcast_word_bytes(bitset2);
    const __m256i table3 = broadcast_word_bytes(bitset3);

    for (const unsigned char *data32_end = (data + data_len) - (data_len % 32); data < data32_end; data += 32)
    {
        __m256i d = _mm256_loadu_si256((const __m256i *)data);
        __m256i low6 = _mm256_and_si256(d, _mm256_set1_epi8(0x3F));

        // byte_idx = (c & 0x38) >> 3, computed branchlessly per byte
        __m256i idx0 = _mm256_and_si256(_mm256_cmpgt_epi8(_mm256_and_si256(low6, mask08), zero), mask01);
        __m256i idx1 = _mm256_and_si256(_mm256_cmpgt_epi8(_mm256_and_si256(low6, mask10), zero), mask02);
        __m256i idx2 = _mm256_and_si256(_mm256_cmpgt_epi8(_mm256_and_si256(low6, mask20), zero), mask04);
        __m256i byte_idx = _mm256_or_si256(_mm256_or_si256(idx0, idx1), idx2);

        // bit_mask = 1 << (c & 7) via 8-byte LUT
        __m256i bit_idx = _mm256_and_si256(low6, mask07);
        __m256i bit_mask = _mm256_shuffle_epi8(bit_lut, bit_idx);

        // bucket masks from the top 2 bits: [00, 01, 10, 11]
        __m256i bit6 = _mm256_cmpgt_epi8(_mm256_and_si256(d, mask40), zero);
        __m256i bit7 = _mm256_cmpgt_epi8(zero, _mm256_and_si256(d, mask80));
        __m256i not6 = _mm256_xor_si256(bit6, all_ones);
        __m256i not7 = _mm256_xor_si256(bit7, all_ones);

        __m256i bucket0 = _mm256_and_si256(not6, not7);
        __m256i bucket1 = _mm256_and_si256(bit6, not7);
        __m256i bucket2 = _mm256_and_si256(not6, bit7);
        __m256i bucket3 = _mm256_and_si256(bit6, bit7);

        __m256i word_bytes0 = _mm256_shuffle_epi8(table0, byte_idx);
        __m256i word_bytes1 = _mm256_shuffle_epi8(table1, byte_idx);
        __m256i word_bytes2 = _mm256_shuffle_epi8(table2, byte_idx);
        __m256i word_bytes3 = _mm256_shuffle_epi8(table3, byte_idx);

        __m256i set0 = _mm256_and_si256(word_bytes0, bit_mask);
        __m256i set1 = _mm256_and_si256(word_bytes1, bit_mask);
        __m256i set2 = _mm256_and_si256(word_bytes2, bit_mask);
        __m256i set3 = _mm256_and_si256(word_bytes3, bit_mask);

        __m256i is_set0 = _mm256_xor_si256(_mm256_cmpeq_epi8(set0, zero), all_ones);
        __m256i is_set1 = _mm256_xor_si256(_mm256_cmpeq_epi8(set1, zero), all_ones);
        __m256i is_set2 = _mm256_xor_si256(_mm256_cmpeq_epi8(set2, zero), all_ones);
        __m256i is_set3 = _mm256_xor_si256(_mm256_cmpeq_epi8(set3, zero), all_ones);

        __m256i hit0 = _mm256_and_si256(is_set0, bucket0);
        __m256i hit1 = _mm256_and_si256(is_set1, bucket1);
        __m256i hit2 = _mm256_and_si256(is_set2, bucket2);
        __m256i hit3 = _mm256_and_si256(is_set3, bucket3);

        __m256i hits = _mm256_or_si256(_mm256_or_si256(hit0, hit1), _mm256_or_si256(hit2, hit3));
        int hit_mask = _mm256_movemask_epi8(hits);
        if (hit_mask != 0) {
            return (data - data_start) + __builtin_ctz(hit_mask);
        }
    }

    data_len %= 32;

    // Scalar tail for remaining bytes
    for (uint64_t i = 0; i < data_len; i++) {
        unsigned char c = data[i];
        uint64_t word;
        switch (c >> 6) {
            case 0: word = bitset0; break;
            case 1: word = bitset1; break;
            case 2: word = bitset2; break;
            default: word = bitset3; break;
        }
        if (word & (1ULL << (c & 63))) {
            return (data - data_start) + i;
        }
    }

    return -1;
}


#include "ascii_scalar.c"

// AVX2 helper: check if 32 bytes are equal (case-insensitive)
// Returns mask where 0xFF = match, 0x00 = mismatch
static inline __m256i equal_fold_vec(__m256i va, __m256i vb,
                                     __m256i v_0x20, __m256i v_0x1f,
                                     __m256i v_0x9a, __m256i v_0x01) {
    // diff = a ^ b (0x00 if equal, 0x20 if case differs, other if mismatch)
    __m256i diff = _mm256_xor_si256(va, vb);

    // mask_0x20 = (diff == 0x20) - potential case difference
    __m256i mask_0x20 = _mm256_cmpeq_epi8(diff, v_0x20);

    // Check if character is ASCII letter [A-Za-z]
    // Force to lowercase: tmp = a | 0x20
    __m256i tmp = _mm256_or_si256(va, v_0x20);
    // Shift range: tmp = tmp + 0x1f  (now 'a'=0x80, 'z'=0x99)
    tmp = _mm256_add_epi8(tmp, v_0x1f);
    // is_alpha = (0x9a > tmp) signed - true for 0x80-0x99
    __m256i is_alpha = _mm256_cmpgt_epi8(v_0x9a, tmp);

    // acceptable_diff = is_alpha & mask_0x20 & 0x01
    __m256i acceptable = _mm256_and_si256(is_alpha, mask_0x20);
    acceptable = _mm256_and_si256(acceptable, v_0x01);
    // Shift 0x01 -> 0x20 to match the diff value
    acceptable = _mm256_slli_epi16(acceptable, 5);

    // Match if diff == acceptable (either both 0, or both 0x20 for valid case diff)
    return _mm256_cmpeq_epi8(diff, acceptable);
}

// ASCII case-insensitive string comparison using AVX2
// gocc: equalFoldAvx(a, b string) bool
bool equal_fold_avx2(const char *a, uint64_t a_len, const char *b, uint64_t b_len) {
    if (a_len != b_len)
        return false;

    size_t len = a_len;

    if (len < 32) return equal_fold_scalar((const uint8_t *)a, (const uint8_t *)b, len);

    // Broadcast constants
    const __m256i v_0x20 = _mm256_set1_epi8(0x20);
    const __m256i v_0x1f = _mm256_set1_epi8(0x1f);
    const __m256i v_0x9a = _mm256_set1_epi8((char)0x9a);
    const __m256i v_0x01 = _mm256_set1_epi8(0x01);
    const __m256i all_ones = _mm256_set1_epi8((char)0xFF);

    // Process 64 bytes at a time
    for (const char *end = (a + len) - (len % 64); a < end; a += 64, b += 64) {
        __m256i a0 = _mm256_loadu_si256((const __m256i *)a);
        __m256i a1 = _mm256_loadu_si256((const __m256i *)(a + 32));
        __m256i b0 = _mm256_loadu_si256((const __m256i *)b);
        __m256i b1 = _mm256_loadu_si256((const __m256i *)(b + 32));

        __m256i eq0 = equal_fold_vec(a0, b0, v_0x20, v_0x1f, v_0x9a, v_0x01);
        __m256i eq1 = equal_fold_vec(a1, b1, v_0x20, v_0x1f, v_0x9a, v_0x01);
        __m256i combined = _mm256_and_si256(eq0, eq1);

        // VPTEST: testc returns 1 if (~combined & all_ones) == 0, i.e. combined is all ones
        if (!_mm256_testc_si256(combined, all_ones)) {
            return false;
        }
    }
    len %= 64;

    // Process 32 bytes
    for (const char *end = (a + len) - (len % 32); a < end; a += 32, b += 32) {
        __m256i va = _mm256_loadu_si256((const __m256i *)a);
        __m256i vb = _mm256_loadu_si256((const __m256i *)b);
        __m256i eq = equal_fold_vec(va, vb, v_0x20, v_0x1f, v_0x9a, v_0x01);

        if (!_mm256_testc_si256(eq, all_ones)) {
            return false;
        }
    }
    len %= 32;

    if (len == 0)
        return true;

    // Overlapped tail load for final 1-31 bytes
    const char *aEnd = (a + len) - 32;
    const char *bEnd = (b + len) - 32;

    __m256i va = _mm256_loadu_si256((const __m256i *)aEnd);
    __m256i vb = _mm256_loadu_si256((const __m256i *)bEnd);
    __m256i eq = equal_fold_vec(va, vb, v_0x20, v_0x1f, v_0x9a, v_0x01);

    return _mm256_testc_si256(eq, all_ones);
}

// gocc: indexMaskAvx(data string, mask byte) int
int64_t index_mask_avx(unsigned char *data, uint64_t length, uint8_t mask)
{
    const unsigned char *data_start = data;

    if (length >= 16)
    {
        const __m256i mask_vec = _mm256_set1_epi8(mask);
        const __m256i zero = _mm256_setzero_si256();

        // Process 128 bytes at a time (4 x 32 bytes)
        for (const unsigned char *data128_end = (data + length) - (length % 128); data < data128_end; data += 128)
        {
            __m256i v0 = _mm256_loadu_si256((const __m256i *)(data));
            __m256i v1 = _mm256_loadu_si256((const __m256i *)(data + 32));
            __m256i v2 = _mm256_loadu_si256((const __m256i *)(data + 64));
            __m256i v3 = _mm256_loadu_si256((const __m256i *)(data + 96));

            __m256i combined = _mm256_or_si256(_mm256_or_si256(v0, v1),
                                               _mm256_or_si256(v2, v3));

            // If no bytes have any bits set that match the mask, continue
            if (_mm256_testz_si256(combined, mask_vec))
            {
                continue;
            }

            // Find which byte has the match
            __m256i t0 = _mm256_and_si256(v0, mask_vec);
            int cmp0 = _mm256_movemask_epi8(_mm256_cmpeq_epi8(t0, zero));
            int match0 = ~cmp0;
            if (match0) {
                return (data - data_start) + __builtin_ctz(match0);
            }

            __m256i t1 = _mm256_and_si256(v1, mask_vec);
            int cmp1 = _mm256_movemask_epi8(_mm256_cmpeq_epi8(t1, zero));
            int match1 = ~cmp1;
            if (match1) {
                return (data - data_start) + 32 + __builtin_ctz(match1);
            }

            __m256i t2 = _mm256_and_si256(v2, mask_vec);
            int cmp2 = _mm256_movemask_epi8(_mm256_cmpeq_epi8(t2, zero));
            int match2 = ~cmp2;
            if (match2) {
                return (data - data_start) + 64 + __builtin_ctz(match2);
            }

            __m256i t3 = _mm256_and_si256(v3, mask_vec);
            int cmp3 = _mm256_movemask_epi8(_mm256_cmpeq_epi8(t3, zero));
            int match3 = ~cmp3;
            // This must have a match since combined had one
            return (data - data_start) + 96 + __builtin_ctz(match3);
        }
        length %= 128;

        // Process 32 bytes at a time
        for (const unsigned char *data32_end = (data + length) - (length % 32); data < data32_end; data += 32)
        {
            __m256i chunk = _mm256_loadu_si256((const __m256i *)(data));

            if (_mm256_testz_si256(chunk, mask_vec))
            {
                continue;
            }

            __m256i result = _mm256_and_si256(chunk, mask_vec);
            int cmp_mask = _mm256_movemask_epi8(_mm256_cmpeq_epi8(result, zero));
            int match_mask = ~cmp_mask;
            return (data - data_start) + __builtin_ctz(match_mask);
        }
        length %= 32;

        // Process remaining 16 bytes
        if (length >= 16)
        {
            __m128i mask_vec_128 = _mm256_castsi256_si128(mask_vec);
            __m128i zero_128 = _mm256_castsi256_si128(zero);
            __m128i chunk = _mm_loadu_si128((const __m128i *)(data));

            if (!_mm_testz_si128(chunk, mask_vec_128))
            {
                __m128i result = _mm_and_si128(chunk, mask_vec_128);
                int cmp_mask = _mm_movemask_epi8(_mm_cmpeq_epi8(result, zero_128));
                int match_mask = (~cmp_mask) & 0xFFFF;
                return (data - data_start) + __builtin_ctz(match_mask);
            }
            data += 16;
            length -= 16;
        }
    }

    // Scalar fallback for remaining bytes (0-15 bytes)
    uint32_t mask32 = mask;
    mask32 |= mask32 << 8;
    mask32 |= mask32 << 16;

    if (length >= 8)
    {
        uint64_t mask64 = mask32;
        mask64 |= mask64 << 32;

        uint64_t data64 = *(uint64_t *)(data);
        data64 &= mask64;
        if (data64 != 0)
        {
            return (data - data_start) + __builtin_ctzll(data64) / 8;
        }
        data += 8;
        length -= 8;
    }

    uint32_t data32;

    if (length >= 4)
    {
        data32 = *(uint32_t *)(data);
        data32 &= mask32;
        if (data32 != 0)
        {
            return (data - data_start) + __builtin_ctz(data32) / 8;
        }
        data += 4;
        length -= 4;
    }

    // Handle the remaining bytes (if any)
    switch (length)
    {
    case 3:
        data32 = *(uint16_t *)(data);
        data32 |= data[2] << 16;
        break;
    case 2:
        data32 = *(uint16_t *)(data);
        break;
    case 1:
        data32 = (uint32_t)*data;
        break;
    default:
        data32 = 0;
        break;
    }

    data32 &= mask32;
    if (data32)
    {
        return (data - data_start) + __builtin_ctz(data32) / 8;
    }

    return -1;
}

// AVX2 helper: fold a vector to uppercase (a-z -> A-Z)
// Returns the uppercased vector
static inline __m256i fold_to_upper_vec(__m256i v,
                                        __m256i v_0x20, __m256i v_0x1f,
                                        __m256i v_0x9a) {
    // Check if character is lowercase letter [a-z]
    // Shift range: tmp = v + 0x1f  (now 'a'=0x80, 'z'=0x99)
    // Note: we check the ORIGINAL value, not (v | 0x20), to avoid
    // incorrectly detecting uppercase letters as lowercase
    __m256i tmp = _mm256_add_epi8(v, v_0x1f);
    // is_lower = (0x9a > tmp) signed - true for 0x80-0x99 (lowercase letters only)
    __m256i is_lower = _mm256_cmpgt_epi8(v_0x9a, tmp);

    // Mask to subtract 0x20 only from lowercase letters
    __m256i sub_mask = _mm256_and_si256(is_lower, v_0x20);

    // Subtract 0x20 from lowercase letters to convert to uppercase
    return _mm256_sub_epi8(v, sub_mask);
}

// SSE2 helper: fold a 128-bit vector to uppercase (a-z -> A-Z)
static inline __m128i fold_to_upper_vec_128(__m128i v,
                                            __m128i v_0x20, __m128i v_0x1f,
                                            __m128i v_0x9a) {
    __m128i tmp = _mm_add_epi8(v, v_0x1f);
    __m128i is_lower = _mm_cmpgt_epi8(v_0x9a, tmp);
    __m128i sub_mask = _mm_and_si128(is_lower, v_0x20);
    return _mm_sub_epi8(v, sub_mask);
}

// Helper to load 1-31 bytes into a 256-bit register (zero-padded)
// Uses only registers, no stack buffer, no overlapping loads
static inline __m256i load_data32_avx2(const unsigned char *src, int64_t len) {
    if (len >= 32) {
        return _mm256_loadu_si256((const __m256i *)src);
    } else if (len <= 0) {
        return _mm256_setzero_si256();
    }

    uint64_t d0 = 0, d1 = 0, d2 = 0, d3 = 0;
    int64_t pos;

    // Load d0 (bytes 0-7)
    if (len >= 8) {
        __builtin_memcpy(&d0, src, 8);
    } else {
        // Partial load into d0 (1-7 bytes)
        pos = 0;
        if (len & 4) { uint32_t t; __builtin_memcpy(&t, src, 4); d0 = t; pos = 4; }
        if (len & 2) { uint16_t t; __builtin_memcpy(&t, src + pos, 2); d0 |= (uint64_t)t << (pos * 8); pos += 2; }
        if (len & 1) { d0 |= (uint64_t)src[pos] << (pos * 8); }
        return _mm256_set_epi64x(0, 0, 0, d0);
    }

    // Load d1 (bytes 8-15)
    if (len >= 16) {
        __builtin_memcpy(&d1, src + 8, 8);
    } else {
        // Partial load into d1 (1-7 bytes remaining)
        int64_t rem = len - 8;
        pos = 0;
        if (rem & 4) { uint32_t t; __builtin_memcpy(&t, src + 8, 4); d1 = t; pos = 4; }
        if (rem & 2) { uint16_t t; __builtin_memcpy(&t, src + 8 + pos, 2); d1 |= (uint64_t)t << (pos * 8); pos += 2; }
        if (rem & 1) { d1 |= (uint64_t)src[8 + pos] << (pos * 8); }
        return _mm256_set_epi64x(0, 0, d1, d0);
    }

    // Load d2 (bytes 16-23)
    if (len >= 24) {
        __builtin_memcpy(&d2, src + 16, 8);
    } else {
        // Partial load into d2 (1-7 bytes remaining)
        int64_t rem = len - 16;
        pos = 0;
        if (rem & 4) { uint32_t t; __builtin_memcpy(&t, src + 16, 4); d2 = t; pos = 4; }
        if (rem & 2) { uint16_t t; __builtin_memcpy(&t, src + 16 + pos, 2); d2 |= (uint64_t)t << (pos * 8); pos += 2; }
        if (rem & 1) { d2 |= (uint64_t)src[16 + pos] << (pos * 8); }
        return _mm256_set_epi64x(0, d2, d1, d0);
    }

    // Load d3 (bytes 24-31, partial: 1-7 bytes remaining)
    {
        int64_t rem = len - 24;
        pos = 0;
        if (rem & 4) { uint32_t t; __builtin_memcpy(&t, src + 24, 4); d3 = t; pos = 4; }
        if (rem & 2) { uint16_t t; __builtin_memcpy(&t, src + 24 + pos, 2); d3 |= (uint64_t)t << (pos * 8); pos += 2; }
        if (rem & 1) { d3 |= (uint64_t)src[24 + pos] << (pos * 8); }
    }

    return _mm256_set_epi64x(d3, d2, d1, d0);
}

// Special case: search for a single byte case-insensitively
static inline int64_t index_fold_1_byte_avx2(const unsigned char *haystack, int64_t haystack_len,
                                              uint8_t needle) {
    const unsigned char *haystack_start = haystack;

    // Uppercase the needle if it's lowercase
    if (needle >= 'a' && needle <= 'z') needle -= 0x20;

    // Constants for case folding
    const __m256i v_0x20 = _mm256_set1_epi8(0x20);
    const __m256i v_0x1f = _mm256_set1_epi8(0x1f);
    const __m256i v_0x9a = _mm256_set1_epi8((char)0x9a);
    const __m256i needle_vec = _mm256_set1_epi8(needle);

    // Process 32 bytes at a time
    for (const unsigned char *data_bound = haystack + haystack_len - (haystack_len % 32);
         haystack < data_bound; haystack += 32) {
        __m256i data = _mm256_loadu_si256((const __m256i *)haystack);
        __m256i folded = fold_to_upper_vec(data, v_0x20, v_0x1f, v_0x9a);
        __m256i cmp = _mm256_cmpeq_epi8(folded, needle_vec);
        int mask = _mm256_movemask_epi8(cmp);
        if (mask) {
            return (haystack - haystack_start) + __builtin_ctz(mask);
        }
    }
    haystack_len %= 32;

    // Handle remaining bytes with SSE
    if (haystack_len >= 16) {
        __m128i v_0x20_128 = _mm256_castsi256_si128(v_0x20);
        __m128i v_0x1f_128 = _mm256_castsi256_si128(v_0x1f);
        __m128i v_0x9a_128 = _mm256_castsi256_si128(v_0x9a);
        __m128i needle_vec_128 = _mm256_castsi256_si128(needle_vec);

        __m128i data = _mm_loadu_si128((const __m128i *)haystack);
        __m128i folded = fold_to_upper_vec_128(data, v_0x20_128, v_0x1f_128, v_0x9a_128);
        __m128i cmp = _mm_cmpeq_epi8(folded, needle_vec_128);
        int mask = _mm_movemask_epi8(cmp);
        if (mask) {
            return (haystack - haystack_start) + __builtin_ctz(mask);
        }
        haystack += 16;
        haystack_len -= 16;
    }

    // Scalar fallback for remaining bytes
    for (int64_t i = 0; i < haystack_len; i++) {
        uint8_t c = haystack[i];
        if (c >= 'a' && c <= 'z') c -= 0x20;
        if (c == needle) {
            return (haystack - haystack_start) + i;
        }
    }

    return -1;
}

// Broadcast a 2-byte needle and uppercase it for 256-bit compares.
static inline __m256i prepare_needle16(const uint16_t *needle,
                                       __m256i v_0x20, __m256i v_0x1f,
                                       __m256i v_0x9a) {
    __m256i needle_vec = _mm256_set1_epi16(*needle);
    return fold_to_upper_vec(needle_vec, v_0x20, v_0x1f, v_0x9a);
}

// AVX2 vpalignr is lane-local, so a true 1-byte shift across 32 bytes needs a
// permute to feed each lane the right neighbour: result =
//   [prev[31], curr[0..14] | curr[15], curr[16..30]]
static inline __m256i shift_right_1(__m256i curr, __m256i prev) {
    // [prev.high, curr.low]
    __m256i bridge = _mm256_permute2x128_si256(prev, curr, 0x21);
    return _mm256_alignr_epi8(curr, bridge, 15);
}

// Process a 32-byte block for 2-byte needle search. Bit N in the result is a
// candidate starting at position N-1 (bit 0 is the cross-block odd position).
static inline uint32_t index_fold_2byte_block(
    __m256i folded, __m256i prev_folded,
    __m256i needle_vec) {

    __m256i cmp_even = _mm256_cmpeq_epi16(folded, needle_vec);
    __m256i cmp_odd = _mm256_cmpeq_epi16(shift_right_1(folded, prev_folded), needle_vec);

    uint32_t mask_even = (uint32_t)_mm256_movemask_epi8(cmp_even);
    uint32_t mask_odd = (uint32_t)_mm256_movemask_epi8(cmp_odd);

    // Both bytes of a matching 16-bit word are 0xFF
    uint32_t valid_even = mask_even & (mask_even >> 1) & 0x55555555u;
    uint32_t valid_odd = mask_odd & (mask_odd >> 1) & 0x55555555u;

    return (valid_even << 1) | valid_odd;
}

// Special case: search for a 2-byte needle case-insensitively
static inline int64_t index_fold_2_byte_avx2(const unsigned char *haystack, int64_t haystack_len,
                                              const uint16_t *needle) {
    const int64_t checked_len = haystack_len - 2;
    if (checked_len < 0) return -1;

    const __m256i v_0x20 = _mm256_set1_epi8(0x20);
    const __m256i v_0x1f = _mm256_set1_epi8(0x1f);
    const __m256i v_0x9a = _mm256_set1_epi8((char)0x9a);
    const __m256i needle_vec = prepare_needle16(needle, v_0x20, v_0x1f, v_0x9a);

    __m256i prev_folded = _mm256_setzero_si256();

    for (int64_t i = 0; i <= checked_len + 32; i += 32) {
        int64_t remaining = haystack_len - i;
        if (remaining <= 0) break;

        __m256i data = (remaining >= 32) ? _mm256_loadu_si256((const __m256i *)(haystack + i))
                                          : load_data32_avx2(haystack + i, remaining);
        __m256i folded = fold_to_upper_vec(data, v_0x20, v_0x1f, v_0x9a);

        uint32_t matches = index_fold_2byte_block(folded, prev_folded, needle_vec);
        prev_folded = folded;

        // On first iteration, clear bit 0 (represents position -1 which is invalid)
        if (i == 0) {
            matches &= ~1u;
        }

        while (matches) {
            int pos = __builtin_ctz(matches);
            matches &= matches - 1;  // Clear lowest bit

            int64_t match_pos = i + pos - 1;
            if (match_pos >= 0 && match_pos <= checked_len) {
                return match_pos;
            }
        }
    }

    return -1;
}

#define MIN(a, b) ((a) > (b) ? (b) : (a))
#define MAX(a, b) ((a) < (b) ? (b) : (a))

// Process a 32-byte block for first2+last2 matching.
static inline uint32_t index_fold_block(
    __m256i data, __m256i data_end,
    __m256i prev_data, __m256i prev_data_end,
    __m256i first2, __m256i last2,
    __m256i v_0x20, __m256i v_0x1f, __m256i v_0x9a) {

    __m256i folded = fold_to_upper_vec(data, v_0x20, v_0x1f, v_0x9a);
    __m256i folded_end = fold_to_upper_vec(data_end, v_0x20, v_0x1f, v_0x9a);
    __m256i prev_folded = fold_to_upper_vec(prev_data, v_0x20, v_0x1f, v_0x9a);
    __m256i prev_folded_end = fold_to_upper_vec(prev_data_end, v_0x20, v_0x1f, v_0x9a);

    __m256i cmp_first_even = _mm256_cmpeq_epi16(folded, first2);
    __m256i cmp_last_even = _mm256_cmpeq_epi16(folded_end, last2);
    __m256i cmp_even = _mm256_and_si256(cmp_first_even, cmp_last_even);

    __m256i shifted = shift_right_1(folded, prev_folded);
    __m256i shifted_end = shift_right_1(folded_end, prev_folded_end);
    __m256i cmp_first_odd = _mm256_cmpeq_epi16(shifted, first2);
    __m256i cmp_last_odd = _mm256_cmpeq_epi16(shifted_end, last2);
    __m256i cmp_odd = _mm256_and_si256(cmp_first_odd, cmp_last_odd);

    uint32_t mask_even = (uint32_t)_mm256_movemask_epi8(cmp_even);
    uint32_t mask_odd = (uint32_t)_mm256_movemask_epi8(cmp_odd);

    uint32_t valid_even = mask_even & (mask_even >> 1) & 0x55555555u;
    uint32_t valid_odd = mask_odd & (mask_odd >> 1) & 0x55555555u;

    return (valid_even << 1) | valid_odd;
}

// gocc: indexFoldRabinKarpAvx(a, b string) int
int64_t index_fold_rabin_karp_avx2(unsigned char *haystack, int64_t haystack_len,
                                   unsigned char *needle, int64_t needle_len) {
    if (haystack_len < needle_len) return -1;
    if (needle_len <= 0) return 0;
    if (haystack_len == needle_len) {
        return equal_fold_scalar(haystack, needle, needle_len) ? 0 : -1;
    }

    switch (needle_len) {
    case 1:
        return index_fold_1_byte_avx2(haystack, haystack_len, needle[0]);
    case 2:
        return index_fold_2_byte_avx2(haystack, haystack_len, (const uint16_t *)needle);
    }

    return index_fold_rabin_karp_core(haystack, haystack_len, needle, needle_len);
}

// gocc: indexFoldAvx(a, b string) int
int64_t index_fold_avx2(unsigned char *haystack, int64_t haystack_len,
                        unsigned char *needle, int64_t needle_len) {
    // Edge cases
    if (haystack_len < needle_len) return -1;
    if (needle_len <= 0) return 0;
    if (haystack_len == needle_len) {
        return equal_fold_scalar((const uint8_t *)haystack, (const uint8_t *)needle, needle_len) ? 0 : -1;
    }

    // Special cases for short needles
    switch (needle_len) {
    case 1:
        return index_fold_1_byte_avx2(haystack, haystack_len, needle[0]);
    case 2:
        return index_fold_2_byte_avx2(haystack, haystack_len, (const uint16_t *)needle);
    }

    const __m256i v_0x20 = _mm256_set1_epi8(0x20);
    const __m256i v_0x1f = _mm256_set1_epi8(0x1f);
    const __m256i v_0x9a = _mm256_set1_epi8((char)0x9a);

    const __m256i first2 = prepare_needle16((const uint16_t *)needle, v_0x20, v_0x1f, v_0x9a);
    const __m256i last2 = prepare_needle16((const uint16_t *)(needle + needle_len - 2), v_0x20, v_0x1f, v_0x9a);

    const int64_t checked_len = haystack_len - needle_len;
    __m256i prev_data = _mm256_setzero_si256();
    __m256i prev_data_end = _mm256_setzero_si256();
    int64_t failures = 0;

    // Process 32 bytes at a time. The shifted comparison can report a match at
    // i + 30, so keep iterating while i <= checked_len + 32.
    for (int64_t i = 0; i <= checked_len + 32; i += 32) {
        int64_t remaining = haystack_len - i;
        if (remaining <= 0) break;

        __m256i data = (remaining >= 32) ? _mm256_loadu_si256((const __m256i *)(haystack + i))
                                          : load_data32_avx2(haystack + i, remaining);
        int64_t end_remaining = remaining - needle_len + 2;
        __m256i data_end = (end_remaining >= 32) ? _mm256_loadu_si256((const __m256i *)(haystack + i + needle_len - 2))
                                                  : (end_remaining > 0) ? load_data32_avx2(haystack + i + needle_len - 2, end_remaining)
                                                                        : _mm256_setzero_si256();

        uint32_t candidates = index_fold_block(data, data_end, prev_data, prev_data_end,
                                                first2, last2, v_0x20, v_0x1f, v_0x9a);
        prev_data = data;
        prev_data_end = data_end;

        // On first iteration, clear bit 0 (represents position -1)
        if (i == 0) {
            candidates &= ~1u;
        }

        // Blocks without a candidate are the common case, so the bailout below
        // stays out of their way.
        if (candidates) {
            while (candidates) {
                int pos = __builtin_ctz(candidates);
                candidates &= candidates - 1;

                int64_t match_pos = i + pos - 1;
                if (match_pos < 0 || match_pos > checked_len) continue;

                // Verify the middle part of the needle (skip first2 and last2)
                if (needle_len <= 4 ||
                    equal_fold_scalar((const uint8_t *)(haystack + match_pos + 2),
                                      (const uint8_t *)(needle + 2),
                                      needle_len - 4)) {
                    return match_pos;
                }
                failures++;
            }

            // Periodic inputs can make first2/last2 match at nearly every
            // position while every verification runs the length of the needle.
            // Once the failures outweigh the ground covered, hand the rest to
            // Rabin-Karp. Bit 31 of the block reported position i + 30, so
            // i + 31 is the first position still unchecked.
            const int64_t rk_start = i + 31;
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