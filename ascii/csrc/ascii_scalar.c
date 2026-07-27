// Shared scalar helpers for the SIMD translation units.
// Included (not compiled standalone) from ascii_sse.c / ascii_avx2.c.
#ifndef VELOZ_ASCII_SCALAR_C
#define VELOZ_ASCII_SCALAR_C

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#define hasbetween(x, m, n) ((~0ull / 255 * (127 + (n)) - ((x) & ~0ull / 255 * 127) & ~(x) & ((x) & ~0ull / 255 * 127) + ~0ull / 255 * (127 - (m))) & ~0ull / 255 * 128)

// Scalar case-insensitive compare, 8 bytes at a time.
static inline bool equal_fold_scalar(const uint8_t *a, const uint8_t *b, size_t len)
{
    size_t i = 0;

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

#define PRIME_RK 16777619

// Maps 'a'..'z' and 'A'..'Z' onto the same value so the rolling hash below
// ignores case. The wraparound on non-letters is harmless as long as both the
// needle and the haystack go through it.
static inline uint8_t rabin_karp_fold_byte(uint8_t c)
{
    return (c >= 'a' && c <= 'z') ? c - 0x80 : c - 0x60;
}

static inline uint32_t rabin_karp_hash_fold(const unsigned char *data, int64_t data_len,
                                           uint32_t *pow_ret)
{
    uint32_t hash = 0;
    for (int64_t i = 0; i < data_len; i++)
    {
        hash = hash * PRIME_RK + rabin_karp_fold_byte(data[i]);
    }

    uint32_t sq = PRIME_RK;
    uint32_t pow = 1;
    for (uint64_t i = data_len; i > 0; i >>= 1)
    {
        if (i & 1) pow *= sq;
        sq *= sq;
    }

    *pow_ret = pow;
    return hash;
}

// Rolling-hash search. Unlike the first2/last2 prefilter this touches every
// haystack byte a fixed number of times, so it can't degenerate. Used as the
// bailout when the SIMD prefilter produces too many false candidates.
//
// Verification is deliberately scalar: the callers inline this whole function,
// and any extra vector constant would spill an XMM register. Spilled XMM slots
// need a 16-byte aligned frame, which makes clang emit a real "and rsp, -16"
// that gocc cannot reconcile with a Go stack frame.
static inline int64_t index_fold_rabin_karp_core(const unsigned char *haystack, int64_t haystack_len,
                                                const unsigned char *needle, int64_t needle_len)
{
    uint32_t pow;
    const uint32_t hash_needle = rabin_karp_hash_fold(needle, needle_len, &pow);

    uint32_t hash = 0;
    for (int64_t i = 0; i < needle_len; i++)
    {
        hash = hash * PRIME_RK + rabin_karp_fold_byte(haystack[i]);
    }

    if (hash == hash_needle && equal_fold_scalar(haystack, needle, needle_len))
    {
        return 0;
    }

    for (int64_t i = needle_len; i < haystack_len; i++)
    {
        hash = hash * PRIME_RK + rabin_karp_fold_byte(haystack[i]);
        hash -= pow * rabin_karp_fold_byte(haystack[i - needle_len]);

        const int64_t pos = i - needle_len + 1;
        if (hash == hash_needle && equal_fold_scalar(haystack + pos, needle, needle_len))
        {
            return pos;
        }
    }

    return -1;
}

#endif // VELOZ_ASCII_SCALAR_C
