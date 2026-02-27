#include <stdint.h>
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

#define hasbetween(x, m, n) ((~0ull / 255 * (127 + (n)) - ((x) & ~0ull / 255 * 127) & ~(x) & ((x) & ~0ull / 255 * 127) + ~0ull / 255 * (127 - (m))) & ~0ull / 255 * 128)

// Scalar fallback: compare 8 bytes at a time using lookup table
static inline bool equal_fold_scalar(const uint8_t *a, const uint8_t *b, size_t len)
{
    size_t i = 0;

    // Process 8 bytes at a time
    for (; i + 8 <= len; i += 8)
    {
        uint64_t a64, b64;
        __builtin_memcpy(&a64, a + i, sizeof(a64));
        __builtin_memcpy(&b64, b + i, sizeof(a64));
        if (a64 == b64) continue;

        uint64_t aMask = hasbetween(a64, 'a' - 1, 'z' + 1);
        uint64_t bMask = hasbetween(b64, 'a' - 1, 'z' + 1);

        uint64_t aFolded = a64 - (aMask >> 2);
        uint64_t bFolded = b64 - (bMask >> 2);

        if (aFolded != bFolded) return false;
    }

    // Handle remaining bytes
    for (; i < len; i++)
    {
        uint8_t aCh = a[i];
        uint8_t bCh = b[i];

        if (aCh >= 'a' && aCh <= 'z') aCh -= 0x20;
        if (bCh >= 'a' && bCh <= 'z') bCh -= 0x20;

        if (aCh != bCh) return false;
    }

    return true;
}

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
