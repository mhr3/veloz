//go:build ignore

/**
 * Standalone Arm NEON UTF-8 case-insensitive substring search.
 *
 * Extracted from StringZilla 5.0.5. The implementation performs full,
 * locale-independent Unicode case folding, including one-to-many folds, and
 * treats malformed UTF-8 bytes losslessly. It requires AArch64 with NEON.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 *  @brief Uncased UTF-8 substring search, comparison & case-invariance checks.
 *  @file include/stringzilla/utf8_uncased.h
 *  @author Ash Vardanian
 *  @sa include/stringzilla/utf8_uncased_fold.h
 *
 *  Public Core API:
 *
 *  - `sz_utf8_uncased_search` - uncased substring search in UTF-8 strings
 *  - `sz_utf8_uncased_order` - uncased lexicographical comparison of UTF-8 strings
 *  - `sz_utf8_find_cased` - pointer to the first cased (foldable) codepoint, or NULL if fully caseless
 *
 *  All comparison and matching uses full Unicode Case Folding (UAX #21 / CaseFolding.txt), including
 *  one-to-many expansions (e.g., 'ß' (U+00DF, C3 9F) → "ss" (U+0073 U+0073, 73 73)). Folding is
 *  locale-independent and deterministic across platforms.
 *
 *  On fast vectorized paths there may be significant algorithmic differences between ISA versions;
 *  the per-script SIMD kernels (Ice Lake, ...) consume needle metadata produced by the ISA-agnostic
 *  classifier in `utf8_uncased/serial.h`.
 */


/**
 *  @brief Serial backend for UTF-8 codepoint mechanics (count, find-nth, unpack) and shared decode helpers.
 *  @file include/stringzilla/utf8_runes/serial.h
 *  @author Ash Vardanian
 */


/* Minimal StringZilla type and helper subset used by this extraction. */
#include <stddef.h>
#include <stdint.h>
#include <arm_neon.h>

typedef uint8_t sz_u8_t;
typedef uint16_t sz_u16_t;
typedef uint32_t sz_u32_t;
typedef uint64_t sz_u64_t;
typedef size_t sz_size_t;
typedef char const *sz_cptr_t;
typedef uint32_t sz_rune_t;

typedef enum { sz_false_k = 0, sz_true_k = 1 } sz_bool_t;
typedef enum {
    sz_rune_invalid_k = 0,
    sz_rune_1byte_k = 1,
    sz_rune_2bytes_k = 2,
    sz_rune_3bytes_k = 3,
    sz_rune_4bytes_k = 4,
} sz_rune_length_t;

typedef union {
    sz_u32_t u32;
    sz_u8_t u8s[4];
} sz_u32_vec_t;
typedef union {
    sz_u64_t u64;
    sz_u8_t u8s[8];
} sz_u64_vec_t;
typedef union {
    uint8x16_t u8x16;
} sz_u128_vec_t;
typedef sz_cptr_t (*sz_find_t)(sz_cptr_t, sz_size_t, sz_cptr_t, sz_size_t);

#define SZ_NULL ((void *)0)
#define SZ_NULL_CHAR ((char *)0)
#define SZ_SWAR_THRESHOLD 24u
#define SZ_API_COMPTIME static inline __attribute__((unused))
#define SZ_HELPER_AUTO static inline __attribute__((unused))
#define SZ_HELPER_INLINE static inline __attribute__((always_inline, unused))
#define SZ_HELPER_NOINLINE static inline __attribute__((noinline, unused))
#define sz_unused_(x) ((void)(x))
#define sz_assert_(condition) ((void)(condition))
#define sz_min_of_two(x, y) ((x) < (y) ? (x) : (y))

SZ_HELPER_AUTO int sz_u64_ctz(sz_u64_t value) { return __builtin_ctzll(value); }
SZ_HELPER_AUTO int sz_u32_ctz(sz_u32_t value) { return __builtin_ctz(value); }
SZ_HELPER_AUTO sz_u64_vec_t sz_u64_load(void const *pointer) {
    sz_u64_vec_t result;
    __builtin_memcpy(&result.u64, pointer, sizeof(result.u64));
    return result;
}
SZ_HELPER_AUTO sz_u64_vec_t sz_u64_each_byte_equal_(sz_u64_vec_t a, sz_u64_vec_t b) {
    sz_u64_vec_t result;
    result.u64 = ~(a.u64 ^ b.u64);
    result.u64 = ((result.u64 & 0x7F7F7F7F7F7F7F7Full) + 0x0101010101010101ull) &
                 (result.u64 & 0x8080808080808080ull);
    return result;
}



/** @brief Whether @p byte is a UTF-8 continuation byte (`0x80..0xBF`). The single low-level predicate every decode
 *         path shares, so `sz_rune_decode` and `sz_utf8_maximal_subpart_` can never disagree on validity. */
SZ_HELPER_INLINE sz_bool_t sz_utf8_is_continuation_(sz_u8_t byte) { return (sz_bool_t)((byte & 0xC0) == 0x80); }

/** @brief Whether @p second is a valid @b first continuation for lead byte @p lead: a continuation byte that also
 *         satisfies the E0/ED/F0/F4 overlong/surrogate/range constraint (Unicode Table 3-7). For C2..DF and the
 *         unconstrained 3/4-byte leads it is just the continuation test. Shared by `sz_rune_decode` and
 *         `sz_utf8_maximal_subpart_` so the "is byte 2 in range" rule lives in exactly one place. */
SZ_HELPER_AUTO sz_bool_t sz_utf8_first_continuation_ok_(sz_u8_t lead, sz_u8_t second) {
    if ((second & 0xC0) != 0x80) return sz_false_k;
    if (lead == 0xE0) return (sz_bool_t)(second >= 0xA0); // overlong 3-byte
    if (lead == 0xED) return (sz_bool_t)(second < 0xA0);  // surrogate U+D800..U+DFFF
    if (lead == 0xF0) return (sz_bool_t)(second >= 0x90); // overlong 4-byte
    if (lead == 0xF4) return (sz_bool_t)(second < 0x90);  // > U+10FFFF
    return sz_true_k;
}

/** @brief Bounds-checked, validating UTF-8 decode. Returns the codepoint's 1-4 byte length in @p rune, or
 *         `sz_rune_invalid_k` (0) when the bytes at @p utf8 do not begin a well-formed, shortest-form,
 *         non-surrogate, in-range codepoint (a truncated trailing sequence before @p utf8_end included). The
 *         single authority for "is this a foldable/normalizable rune"; the decode-side mirror of `sz_rune_encode`.
 *         On failure use `sz_utf8_maximal_subpart_` for how many bytes the resulting U+FFFD consumes. */
SZ_HELPER_AUTO sz_rune_length_t sz_rune_decode(sz_cptr_t utf8, sz_cptr_t utf8_end, sz_rune_t *rune) {
    sz_u8_t const *u = (sz_u8_t const *)utf8;
    sz_size_t const available = (sz_size_t)((sz_u8_t const *)utf8_end - u);
    sz_u8_t const lead = u[0];
    if (lead < 0x80) {
        *rune = lead;
        return sz_rune_1byte_k;
    }
    if (lead < 0xC2) return sz_rune_invalid_k; // continuation byte, or C0/C1 (overlong 2-byte)
    if (lead < 0xE0) {                         // C2..DF
        if (available < 2 || !sz_utf8_first_continuation_ok_(lead, u[1])) return sz_rune_invalid_k;
        *rune = (sz_rune_t)(lead & 0x1F) << 6 | (u[1] & 0x3F);
        return sz_rune_2bytes_k;
    }
    if (lead < 0xF0) { // E0..EF
        if (available < 3 || !sz_utf8_first_continuation_ok_(lead, u[1]) || !sz_utf8_is_continuation_(u[2]))
            return sz_rune_invalid_k;
        *rune = (sz_rune_t)(lead & 0x0F) << 12 | (sz_rune_t)(u[1] & 0x3F) << 6 | (u[2] & 0x3F);
        return sz_rune_3bytes_k;
    }
    if (lead <= 0xF4) { // F0..F4
        if (available < 4 || !sz_utf8_first_continuation_ok_(lead, u[1]) || !sz_utf8_is_continuation_(u[2]) ||
            !sz_utf8_is_continuation_(u[3]))
            return sz_rune_invalid_k;
        *rune = (sz_rune_t)(lead & 0x07) << 18 | (sz_rune_t)(u[1] & 0x3F) << 12 | (sz_rune_t)(u[2] & 0x3F) << 6 |
                (u[3] & 0x3F);
        return sz_rune_4bytes_k;
    }
    return sz_rune_invalid_k; // F5..FF
}

/** @brief Byte length (1..3) of the maximal ill-formed subpart starting at @p utf8, per Unicode 17.0 §3.9 and the
 *         W3C Encoding Standard: the longest prefix that matches the start of @b some well-formed sequence, so a
 *         single U+FFFD replaces it and decoding resyncs immediately after. A bad lead (stray continuation, C0/C1,
 *         F5..FF) is length 1; a good 3/4-byte lead whose continuation chain breaks at byte 2 or 3 yields 2 or 3.
 *         @pre `[utf8, utf8_end)` does not begin a well-formed rune (`sz_rune_decode` returned `sz_rune_invalid_k`).
 *         Shares `sz_utf8_first_continuation_ok_`/`sz_utf8_is_continuation_` with `sz_rune_decode` so the two never
 *         disagree on where a sequence stops being well-formed. */
/** @brief  Emit one `(start, length)` segment per set boundary lane of @p boundary (ascending), honoring
 *          @p capacity and the carried previous boundary in @p previous_io - the portable scalar twin of the
 *          per-ISA `drain_forward` leaves, shared by back-ends that carry their window state as `sz_u64_t` masks. */


/** @brief Decode the 1-4 byte sequence the lead byte declares, with NO bounds check and NO validation; returns
 *         the byte length and stores the codepoint in @p rune.
 *  @warning Assumes valid, complete UTF-8 (a truncated trailing sequence over-reads). Use `sz_rune_decode` for the
 *           bounds-checked + validating variant, or `sz_utf8_find_malformed()` first. */
SZ_HELPER_AUTO sz_rune_length_t sz_rune_decode_unchecked(sz_cptr_t utf8, sz_rune_t *rune) {
    sz_u8_t const *u8s = (sz_u8_t const *)utf8;
    sz_u8_t lead = *u8s++;
    sz_rune_length_t length = (sz_rune_length_t)(1 + (lead >= 0xC0U) + (lead >= 0xE0U) + (lead >= 0xF0U));
    switch (length) {
    case 1: *rune = lead; break;
    case 2: *rune = (lead & 0x1FU) << 6 | (u8s[0] & 0x3FU); break;
    case 3: *rune = (lead & 0x0FU) << 12 | (u8s[0] & 0x3FU) << 6 | (u8s[1] & 0x3FU); break;
    default:
        *rune = (sz_rune_t)(lead & 0x07U) << 18 | (u8s[0] & 0x3FU) << 12 | (u8s[1] & 0x3FU) << 6 | (u8s[2] & 0x3FU);
        break;
    }
    return length;
}

/** @brief Encode a UTF-32 codepoint to UTF-8 (1-4 bytes). @return byte count, or `sz_rune_invalid_k` if invalid. */
SZ_HELPER_AUTO sz_rune_length_t sz_rune_encode(sz_rune_t rune, sz_u8_t *utf8s) {
    if (rune <= 0x7F) {
        utf8s[0] = (sz_u8_t)rune;
        return sz_rune_1byte_k;
    }
    else if (rune <= 0x7FF) {
        utf8s[0] = (sz_u8_t)(0xC0 | (rune >> 6));
        utf8s[1] = (sz_u8_t)(0x80 | (rune & 0x3F));
        return sz_rune_2bytes_k;
    }
    else if (rune <= 0xFFFF) {
        if (rune >= 0xD800 && rune <= 0xDFFF) return sz_rune_invalid_k; // reject surrogates
        utf8s[0] = (sz_u8_t)(0xE0 | (rune >> 12));
        utf8s[1] = (sz_u8_t)(0x80 | ((rune >> 6) & 0x3F));
        utf8s[2] = (sz_u8_t)(0x80 | (rune & 0x3F));
        return sz_rune_3bytes_k;
    }
    else if (rune <= 0x10FFFF) {
        utf8s[0] = (sz_u8_t)(0xF0 | (rune >> 18));
        utf8s[1] = (sz_u8_t)(0x80 | ((rune >> 12) & 0x3F));
        utf8s[2] = (sz_u8_t)(0x80 | ((rune >> 6) & 0x3F));
        utf8s[3] = (sz_u8_t)(0x80 | (rune & 0x3F));
        return sz_rune_4bytes_k;
    }
    return sz_rune_invalid_k;
}

/** @brief Locate the first ill-formed byte in `[text, text+length)`; `SZ_NULL_CHAR` if entirely well-formed UTF-8. */
SZ_API_COMPTIME sz_cptr_t sz_utf8_find_malformed(sz_cptr_t text, sz_size_t length) {
    sz_u8_t const *text_u8 = (sz_u8_t const *)text;
    sz_u8_t const *end_u8 = text_u8 + length;
    while (text_u8 < end_u8) {
        sz_rune_t rune;
        sz_rune_length_t const consumed = sz_rune_decode((sz_cptr_t)text_u8, (sz_cptr_t)end_u8, &rune);
        if (consumed == sz_rune_invalid_k) return (sz_cptr_t)text_u8;
        text_u8 += consumed;
    }
    return SZ_NULL_CHAR;
}

/** @brief Whether `[text, end)` is a well-formed but @b truncated multi-byte prefix: a valid lead followed only by
 *         valid (so far) continuation bytes, with fewer bytes present than the lead declares. Such a tail is not
 *         ill-formed - a streaming decoder stops on it and resumes once more bytes arrive, rather than substituting
 *         U+FFFD. Genuinely ill-formed bytes (a bad lead, a malformed present continuation, or an overlong/surrogate/
 *         out-of-range prefix) return false so the caller emits the replacement character. */





/** @brief Decode the UTF-8 codepoint at `*position`, advancing it. Ill-formed bytes (lone continuation, overlong,
 *         surrogate, out-of-range, truncated) decode to U+FFFD consuming one byte — the segmentation-side
 *         substitution the SIMD `sz_utf8_rune_decode_window_` classifiers mirror byte-for-byte. (The
 *         rune-unpack path uses the ICU maximal-subpart resync instead; segmentation keeps the 1-byte contract.) */

/**
 *  @brief Get the UTF-8 sequence length from a lead byte, branchlessly.
 *
 *  The length is fully determined by the lead byte's high nibble: 0x0-0xB map to 1 (ASCII and, for robustness,
 *  stray continuation bytes treated as single bytes), 0xC-0xD to 2, 0xE to 3, 0xF to 4. A single 16-entry
 *  table resolves it without a four-way `if`-ladder on the codepoint advance.
 */

/** @brief Returns the start offset of the codepoint preceding `position` (a codepoint start), or `position` if none. */


/*  ISA-independent `sz_u64_t` boundary-mask algebra shared by every backend (serial / haswell / icelake / neon).
 *  It lives here so each `utf8_runes/<isa>.h` inherits it through its `#include "utf8_runes/serial.h"` and
 *  the per-family rule code is written once and reused across ISAs. */

/** @brief  Smear set bits rightward (toward higher lanes) for @p steps, gated by the @p reach mask each step. */

/**
 *  @brief  Unbounded segmented flood of @p seed bits rightward (toward higher lanes) through every lane where
 *          @p gate is set, in log-depth Kogge-Stone doubling steps. Reaches the full 64-lane span without a
 *          per-lane loop, so runs of arbitrary length resolve in six iterations (sentence SB8 / line SP-runs).
 *  @note   A bit at lane `i` ends up set when `seed[i]` or there is a contiguous `gate`-true run from some
 *          seeded lane up to and including `i`. The gate is contracted alongside the flood so each doubling
 *          step still only crosses lanes that are themselves gated.
 */

/**
 *  @brief  Unbounded segmented flood of @p seed bits leftward (toward lower lanes) through every lane where
 *          @p gate is set, in log-depth Kogge-Stone doubling steps. Mirror of @ref sz_u64_fill_right_.
 */

/**
 *  @brief  Segmented exclusive prefix-XOR parity over each maximal @p gate run: lane i carries the XOR of @p seed
 *          across the contiguous run of @p gate ending at i. The dual of @ref sz_u64_fill_right_ (OR ->
 *          XOR), in log-depth Kogge-Stone doubling. Used by the grapheme GB12/13 Regional_Indicator parity scan.
 */

/** @brief  Low @p count bits set (`[0, count)`), 0 for `count==0`, all-ones for `count>=64`. Portable, branch-light
 *          replacement for the BMI2 `sz_u64_mask_until_` so the shared boundary algebra compiles on every backend. */



typedef struct {
    sz_size_t offset_in_unfolded;
    sz_size_t length_in_unfolded;
    sz_u8_t folded_slice[16];
    sz_u8_t folded_slice_length;
    sz_u8_t probe_second;
    sz_u8_t probe_third;
    sz_u8_t kernel_id;
} sz_utf8_uncased_needle_metadata_t;

typedef enum {
    sz_utf8_uncased_rune_unknown_k = 0,
    sz_utf8_uncased_rune_ascii_invariant_k = 1,
    sz_utf8_uncased_rune_safe_western_europe_k = 2,
    sz_utf8_uncased_rune_safe_central_europe_k = 3,
    sz_utf8_uncased_rune_safe_cyrillic_k = 4,
    sz_utf8_uncased_rune_safe_greek_k = 5,
    sz_utf8_uncased_rune_safe_armenian_k = 6,
    sz_utf8_uncased_rune_safe_vietnamese_k = 7,
    sz_utf8_uncased_rune_safe_georgian_k = 8,
    sz_utf8_uncased_rune_invariant_k = 9,
    sz_utf8_uncased_rune_fallback_serial_k = 255,
} sz_utf8_uncased_rune_safety_profile_t;

SZ_API_COMPTIME sz_cptr_t sz_utf8_find_cased_neon(sz_cptr_t str, sz_size_t length);


/**
 *  @brief NEON (Arm) uncased UTF-8 search, comparison & invariance backend.
 *  @file include/stringzilla/utf8_uncased/neon.h
 *  @author Ash Vardanian
 *  @sa include/stringzilla/utf8_uncased.h
 *
 *  Ports the validated AVX2 architecture to 32-byte NEON chunks held as `uint8x16x2_t` (two
 *  16-byte registers, mirroring a 32-byte YMM). The needle classification, match verification,
 *  and danger-zone scanning stay ISA-independent in `serial.h`; per-script `fold`/`alarm`
 *  helpers become `uint8x16x2_t` functions; and the shared force-inlined `scripted_` driver walks
 *  the haystack in 32-byte steps. AVX2's two compare domains map directly: byte-mask vectors (for
 *  folds, which feed masked adds) and a 32-bit movemask integer (for alarms and probe filters,
 *  where scalar shifts replace `VPMOVMSKB`-shift algebra bit-for-bit). The 256-bit register has no
 *  NEON equivalent, so the fold callbacks splice the internal 16-byte boundary themselves with
 *  `vextq_u8`: the predecessor of register 1's lane 0 is register 0's lane 15, exactly as AVX2's
 *  `VPERM2I128` carries the low lane across the 128-bit edge.
 */


/**
 *  @brief NEON backend for substring & byte-set search.
 *  @file include/stringzilla/find/neon.h
 *  @author Ash Vardanian
 *  @sa include/stringzilla/find.h
 */


/**
 *  @brief Serial backend for string comparison utilities.
 *  @file include/stringzilla/compare/serial.h
 *  @author Ash Vardanian
 *  @sa include/stringzilla/compare.h
 */


SZ_API_COMPTIME sz_bool_t sz_equal_serial(sz_cptr_t a, sz_cptr_t b, sz_size_t length) {
    sz_cptr_t const a_end = a + length;
    if (length >= SZ_SWAR_THRESHOLD) {
        sz_u64_vec_t a_vec, b_vec;
        for (; a + 8 <= a_end; a += 8, b += 8) {
            a_vec = sz_u64_load(a);
            b_vec = sz_u64_load(b);
            if (a_vec.u64 != b_vec.u64) return sz_false_k;
        }
    }
    while (a != a_end && *a == *b) a++, b++;
    return (sz_bool_t)(a_end == a);
}



/**
 *  @brief Serial backend for substring & byte-set search.
 *  @file include/stringzilla/find/serial.h
 *  @author Ash Vardanian
 *  @sa include/stringzilla/find.h
 */


/**
 *  @brief Chooses the offsets of the most interesting characters in a search needle.
 *
 *  Search throughput can significantly deteriorate if we are matching the wrong characters.
 *  Say the needle is "aXaYa", and we are comparing the first, second, and last character.
 *  If we use SIMD and compare many offsets at a time, comparing against "a" in every register is a waste.
 *
 *  Similarly, dealing with UTF-8 inputs, we know that the lower bits of each character code carry more information.
 *  Cyrillic alphabet, for example, falls into [0x0410, 0x042F] code range for uppercase [А, Я], and
 *  into [0x0430, 0x044F] for lowercase [а, я]. Scanning through a text written in Russian, half of the
 *  bytes will carry absolutely no value and will be equal to 0x04.
 *
 *  @param start Pointer to the needle bytes.
 *  @param length Length of the needle in bytes.
 *  @param first Output offset of the first anomalous byte.
 *  @param second Output offset of the second anomalous byte.
 *  @param third Output offset of the third anomalous byte.
 */
SZ_HELPER_AUTO void sz_locate_needle_anomalies_( //
    sz_cptr_t start, sz_size_t length,           //
    sz_size_t *first, sz_size_t *second, sz_size_t *third) {

    *first = 0;
    *second = length / 2;
    *third = length - 1;

    //
    int has_duplicates =                   //
        start[*first] == start[*second] || //
        start[*first] == start[*third] ||  //
        start[*second] == start[*third];

    // Loop through letters to find non-colliding variants.
    if (length > 3 && has_duplicates) {
        // Pivot the middle point right, until we find a character different from the first one.
        while (start[*second] == start[*first] && *second + 1 < *third) ++(*second);
        // Pivot the third (last) point left, until we find a different character.
        while ((start[*third] == start[*second] || start[*third] == start[*first]) && *third > (*second + 1))
            --(*third);
    }

    // TODO: Investigate alternative strategies for long needles.
    // On very long needles we have the luxury to choose!
    // Often dealing with UTF-8, we will likely benefit from shifting the first and second characters
    // further to the right, to achieve not only uniqueness within the needle, but also avoid common
    // rune prefixes of 2-, 3-, and 4-byte codes.
    if (length > 8) {
        // Pivot the first and second points right, until we find a character, that:
        // > is different from others.
        // > doesn't start with 0b'110x'xxxx - only 5 bits of relevant info.
        // > doesn't start with 0b'1110'xxxx - only 4 bits of relevant info.
        // > doesn't start with 0b'1111'0xxx - only 3 bits of relevant info.
        //
        // So we are practically searching for byte values that start with 0b0xxx'xxxx or 0b'10xx'xxxx.
        // Meaning they fall in the range [0, 127] and [128, 191], in other words any unsigned int up to 191.
        sz_u8_t const *start_u8 = (sz_u8_t const *)start;
        sz_size_t vibrant_first = *first, vibrant_second = *second, vibrant_third = *third;

        // Let's begin with the second character, as the termination criteria there is more obvious
        // and we may end up with more variants to check for the first candidate.
        while ((start_u8[vibrant_second] > 191 || start_u8[vibrant_second] == start_u8[vibrant_third]) &&
               (vibrant_second + 1 < vibrant_third))
            ++vibrant_second;

        // Now check if we've indeed found a good candidate or should revert the `vibrant_second` to `second`.
        if (start_u8[vibrant_second] < 191) { *second = vibrant_second; }
        else { vibrant_second = *second; }

        // Now check the first character.
        while ((start_u8[vibrant_first] > 191 || start_u8[vibrant_first] == start_u8[vibrant_second] ||
                start_u8[vibrant_first] == start_u8[vibrant_third]) &&
               (vibrant_first + 1 < vibrant_second))
            ++vibrant_first;

        // Now check if we've indeed found a good candidate or should revert the `vibrant_first` to `first`.
        // We don't need to shift the third one when dealing with texts as the last byte of the text is
        // also the last byte of a rune and contains the most information.
        if (start_u8[vibrant_first] < 191) { *first = vibrant_first; }
    }
}

/** @brief  Number of byte values present in @p set - four branchless word popcounts. */

/** @brief  Unpack the member byte values of @p set into @p members in ascending order; the caller has already
 *          sized the destination from @ref sz_byteset_population_serial_. Shared by the ISA back-ends whose
 *          small-set fast paths broadcast the members as a needle segment. */



SZ_API_COMPTIME sz_cptr_t sz_find_byte_serial(sz_cptr_t haystack, sz_size_t haystack_length, sz_cptr_t needle) {

    if (!haystack_length) return SZ_NULL_CHAR;
    // Reinterpret as unsigned bytes so the SWAR broadcast below cannot sign-extend
    // on platforms where `char` is signed (e.g. `-fsigned-char`). See issue #306.
    sz_u8_t const *haystack_cursor = (sz_u8_t const *)haystack;
    sz_u8_t const *const needle_u8 = (sz_u8_t const *)needle;
    sz_u8_t const *const haystack_end = haystack_cursor + haystack_length;

    // Broadcast the needle into every byte of a 64-bit integer to use SWAR
    // techniques and process eight characters at a time.
    sz_u64_vec_t haystack_vec, needle_vec, match_vec;
    match_vec.u64 = 0;
    needle_vec.u64 = (sz_u64_t)*needle_u8 * 0x0101010101010101ull;
    for (; haystack_cursor + 8 <= haystack_end; haystack_cursor += 8) {
        haystack_vec.u64 = *(sz_u64_t const *)haystack_cursor;
        match_vec = sz_u64_each_byte_equal_(haystack_vec, needle_vec);
        if (match_vec.u64) return (sz_cptr_t)(haystack_cursor + sz_u64_ctz(match_vec.u64) / 8);
    }

    // Handle the misaligned tail.
    for (; haystack_cursor < haystack_end; ++haystack_cursor)
        if (*haystack_cursor == *needle_u8) return (sz_cptr_t)haystack_cursor;
    return SZ_NULL_CHAR;
}


/**
 *  @brief 2-byte-level equality comparison between two 64-bit integers.
 *  @return 64-bit integer, where every top bit in each 2-byte group signifies a match.
 */
SZ_HELPER_INLINE sz_u64_vec_t sz_u64_each_2byte_equal_(sz_u64_vec_t a, sz_u64_vec_t b) {
    sz_u64_vec_t vec;
    vec.u64 = ~(a.u64 ^ b.u64);
    // The match is valid, if every bit within each 2-byte group is set.
    // For that take the bottom 15 bits of each 2-byte group, add one to them,
    // and if this sets the top bit to one, then all the 15 bits are ones as well.
    vec.u64 = ((vec.u64 & 0x7FFF7FFF7FFF7FFFull) + 0x0001000100010001ull) & ((vec.u64 & 0x8000800080008000ull));
    return vec;
}

SZ_HELPER_NOINLINE sz_cptr_t sz_find_1byte_serial_(sz_cptr_t haystack, sz_size_t haystack_length, sz_cptr_t needle,
                                                   sz_size_t needle_length) {
    sz_unused_(needle_length); //? We keep this argument only for `sz_find_t` signature compatibility.
    return sz_find_byte_serial(haystack, haystack_length, needle);
}


/**
 *  @brief Find the first occurrence of a @b two-character needle in an arbitrary length haystack.
 *         This implementation uses hardware-agnostic SWAR technique, to process 8 possible offsets at a time.
 */
SZ_HELPER_NOINLINE sz_cptr_t sz_find_2byte_serial_(sz_cptr_t haystack, sz_size_t haystack_length, sz_cptr_t needle,
                                                   sz_size_t needle_length) {

    // This is an internal method, and the haystack is guaranteed to be at least 2 bytes long.
    sz_assert_(haystack_length >= 2 && "The haystack is too short.");
    sz_unused_(needle_length); //? We keep this argument only for `sz_find_t` signature compatibility.
    sz_cptr_t const haystack_end = haystack + haystack_length;

    // On big-endian systems, skip SWAR and use simple serial search

    // Process the misaligned head, to void UB on unaligned 64-bit loads.

    sz_u64_vec_t haystack_even_vec, haystack_odd_vec, needle_vec, matches_even_vec, matches_odd_vec;
    needle_vec.u64 = 0;
    needle_vec.u8s[0] = needle[0], needle_vec.u8s[1] = needle[1];
    needle_vec.u64 *= 0x0001000100010001ull; // broadcast

    // This code simulates hyper-scalar execution, analyzing 8 offsets at a time.
    for (; haystack + 9 <= haystack_end; haystack += 8) {
        haystack_even_vec.u64 = *(sz_u64_t *)haystack;
        haystack_odd_vec.u64 = (haystack_even_vec.u64 >> 8) | ((sz_u64_t)haystack[8] << 56);
        matches_even_vec = sz_u64_each_2byte_equal_(haystack_even_vec, needle_vec);
        matches_odd_vec = sz_u64_each_2byte_equal_(haystack_odd_vec, needle_vec);
        matches_even_vec.u64 >>= 8;
        if (matches_even_vec.u64 + matches_odd_vec.u64) {
            sz_u64_t match_indicators = matches_even_vec.u64 | matches_odd_vec.u64;
            return haystack + sz_u64_ctz(match_indicators) / 8;
        }
    }

    for (; haystack + 2 <= haystack_end; ++haystack)
        if ((haystack[0] == needle[0]) + (haystack[1] == needle[1]) == 2) return haystack;
    return SZ_NULL_CHAR;
}

/**
 *  @brief 4-byte-level equality comparison between two 64-bit integers.
 *  @return 64-bit integer, where every top bit in each 4-byte group signifies a match.
 */
SZ_HELPER_INLINE sz_u64_vec_t sz_u64_each_4byte_equal_(sz_u64_vec_t a, sz_u64_vec_t b) {
    sz_u64_vec_t vec;
    vec.u64 = ~(a.u64 ^ b.u64);
    // The match is valid, if every bit within each 4-byte group is set.
    // For that take the bottom 31 bits of each 4-byte group, add one to them,
    // and if this sets the top bit to one, then all the 31 bits are ones as well.
    vec.u64 = ((vec.u64 & 0x7FFFFFFF7FFFFFFFull) + 0x0000000100000001ull) & ((vec.u64 & 0x8000000080000000ull));
    return vec;
}

/**
 *  @brief Find the first occurrence of a @b four-character needle in an arbitrary length haystack.
 *         This implementation uses hardware-agnostic SWAR technique, to process 8 possible offsets at a time.
 */
SZ_HELPER_NOINLINE sz_cptr_t sz_find_4byte_serial_(sz_cptr_t haystack, sz_size_t haystack_length, sz_cptr_t needle,
                                                   sz_size_t needle_length) {

    // This is an internal method, and the haystack is guaranteed to be at least 4 bytes long.
    sz_assert_(haystack_length >= 4 && "The haystack is too short.");
    sz_unused_(needle_length); //? We keep this argument only for `sz_find_t` signature compatibility.
    sz_cptr_t const haystack_end = haystack + haystack_length;

    // On big-endian systems, skip SWAR and use simple serial search

    // Process the misaligned head, to void UB on unaligned 64-bit loads.

    sz_u64_vec_t haystack0_vec, haystack1_vec, haystack2_vec, haystack3_vec, needle_vec;
    sz_u64_vec_t matches0_vec, matches1_vec, matches2_vec, matches3_vec;
    needle_vec.u64 = 0;
    needle_vec.u8s[0] = needle[0], needle_vec.u8s[1] = needle[1], needle_vec.u8s[2] = needle[2],
    needle_vec.u8s[3] = needle[3];
    needle_vec.u64 *= 0x0000000100000001ull; // broadcast

    // This code simulates hyper-scalar execution, analyzing 8 offsets at a time using four 64-bit words.
    // We load the subsequent four-byte word as well, taking its first bytes. Think of it as a glorified prefetch :)
    sz_u64_t haystack_page_current, haystack_page_next;
    for (; haystack + sizeof(sz_u64_t) + sizeof(sz_u32_t) <= haystack_end; haystack += sizeof(sz_u64_t)) {
        haystack_page_current = *(sz_u64_t *)haystack;
        haystack_page_next = *(sz_u32_t *)(haystack + 8);
        haystack0_vec.u64 = (haystack_page_current);
        haystack1_vec.u64 = (haystack_page_current >> 8) | (haystack_page_next << 56);
        haystack2_vec.u64 = (haystack_page_current >> 16) | (haystack_page_next << 48);
        haystack3_vec.u64 = (haystack_page_current >> 24) | (haystack_page_next << 40);
        matches0_vec = sz_u64_each_4byte_equal_(haystack0_vec, needle_vec);
        matches1_vec = sz_u64_each_4byte_equal_(haystack1_vec, needle_vec);
        matches2_vec = sz_u64_each_4byte_equal_(haystack2_vec, needle_vec);
        matches3_vec = sz_u64_each_4byte_equal_(haystack3_vec, needle_vec);

        if (matches0_vec.u64 | matches1_vec.u64 | matches2_vec.u64 | matches3_vec.u64) {
            matches0_vec.u64 >>= 24;
            matches1_vec.u64 >>= 16;
            matches2_vec.u64 >>= 8;
            sz_u64_t match_indicators = matches0_vec.u64 | matches1_vec.u64 | matches2_vec.u64 | matches3_vec.u64;
            return haystack + sz_u64_ctz(match_indicators) / 8;
        }
    }

    for (; haystack + 4 <= haystack_end; ++haystack)
        if ((haystack[0] == needle[0]) + (haystack[1] == needle[1]) + (haystack[2] == needle[2]) +
                (haystack[3] == needle[3]) ==
            4)
            return haystack;
    return SZ_NULL_CHAR;
}

/**
 *  @brief 3-byte-level equality comparison between two 64-bit integers.
 *  @return 64-bit integer, where every top bit in each 3-byte group signifies a match.
 */
SZ_HELPER_INLINE sz_u64_vec_t sz_u64_each_3byte_equal_(sz_u64_vec_t a, sz_u64_vec_t b) {
    sz_u64_vec_t vec;
    vec.u64 = ~(a.u64 ^ b.u64);
    // The match is valid, if every bit within each 4-byte group is set.
    // For that take the bottom 31 bits of each 4-byte group, add one to them,
    // and if this sets the top bit to one, then all the 31 bits are ones as well.
    vec.u64 = ((vec.u64 & 0xFFFF7FFFFF7FFFFFull) + 0x0000000001000001ull) & ((vec.u64 & 0x0000800000800000ull));
    return vec;
}

/**
 *  @brief Find the first occurrence of a @b three-character needle in an arbitrary length haystack.
 *         This implementation uses hardware-agnostic SWAR technique, to process 8 possible offsets at a time.
 */
SZ_HELPER_NOINLINE sz_cptr_t sz_find_3byte_serial_(sz_cptr_t haystack, sz_size_t haystack_length, sz_cptr_t needle,
                                                   sz_size_t needle_length) {

    // This is an internal method, and the haystack is guaranteed to be at least 4 bytes long.
    sz_assert_(haystack_length >= 3 && "The haystack is too short.");
    sz_unused_(needle_length); //? We keep this argument only for `sz_find_t` signature compatibility.
    sz_cptr_t const haystack_end = haystack + haystack_length;

    // On big-endian systems, skip SWAR and use simple serial search

    // Process the misaligned head, to void UB on unaligned 64-bit loads.

    // We fetch 12
    sz_u64_vec_t haystack0_vec, haystack1_vec, haystack2_vec, haystack3_vec, haystack4_vec;
    sz_u64_vec_t matches0_vec, matches1_vec, matches2_vec, matches3_vec, matches4_vec;
    sz_u64_vec_t needle_vec;
    needle_vec.u64 = 0;
    needle_vec.u8s[0] = needle[0], needle_vec.u8s[1] = needle[1], needle_vec.u8s[2] = needle[2];
    needle_vec.u64 *= 0x0000000001000001ull; // broadcast

    // This code simulates hyper-scalar execution, analyzing 8 offsets at a time using three 64-bit words.
    // We load the subsequent two-byte word as well.
    sz_u64_t haystack_page_current, haystack_page_next;
    for (; haystack + sizeof(sz_u64_t) + sizeof(sz_u16_t) <= haystack_end; haystack += sizeof(sz_u64_t)) {
        haystack_page_current = *(sz_u64_t *)haystack;
        haystack_page_next = *(sz_u16_t *)(haystack + 8);
        haystack0_vec.u64 = (haystack_page_current);
        haystack1_vec.u64 = (haystack_page_current >> 8) | (haystack_page_next << 56);
        haystack2_vec.u64 = (haystack_page_current >> 16) | (haystack_page_next << 48);
        haystack3_vec.u64 = (haystack_page_current >> 24) | (haystack_page_next << 40);
        haystack4_vec.u64 = (haystack_page_current >> 32) | (haystack_page_next << 32);
        matches0_vec = sz_u64_each_3byte_equal_(haystack0_vec, needle_vec);
        matches1_vec = sz_u64_each_3byte_equal_(haystack1_vec, needle_vec);
        matches2_vec = sz_u64_each_3byte_equal_(haystack2_vec, needle_vec);
        matches3_vec = sz_u64_each_3byte_equal_(haystack3_vec, needle_vec);
        matches4_vec = sz_u64_each_3byte_equal_(haystack4_vec, needle_vec);

        if (matches0_vec.u64 | matches1_vec.u64 | matches2_vec.u64 | matches3_vec.u64 | matches4_vec.u64) {
            matches0_vec.u64 >>= 16;
            matches1_vec.u64 >>= 8;
            matches3_vec.u64 <<= 8;
            matches4_vec.u64 <<= 16;
            sz_u64_t match_indicators = matches0_vec.u64 | matches1_vec.u64 | matches2_vec.u64 | matches3_vec.u64 |
                                        matches4_vec.u64;
            return haystack + sz_u64_ctz(match_indicators) / 8;
        }
    }

    for (; haystack + 3 <= haystack_end; ++haystack)
        if ((haystack[0] == needle[0]) + (haystack[1] == needle[1]) + (haystack[2] == needle[2]) == 3) return haystack;
    return SZ_NULL_CHAR;
}

/**
 *  @brief Boyer-Moore-Horspool algorithm for exact matching of patterns up to @b 256-bytes long.
 *         Uses the Raita heuristic to match the first two, the last, and the middle character of the pattern.
 *
 *  @param haystack The haystack bytes.
 *  @param haystack_length Length of the haystack in bytes.
 *  @param needle The needle bytes.
 *  @param needle_length Length of the needle in bytes (must be <= 256).
 *  @return Pointer to first match, or SZ_NULL_CHAR if none.
 */
SZ_HELPER_NOINLINE sz_cptr_t sz_find_horspool_upto_256bytes_serial_( //
    sz_cptr_t haystack, sz_size_t haystack_length,                   //
    sz_cptr_t needle, sz_size_t needle_length) {
    sz_assert_(needle_length <= 256 && "The pattern is too long.");
    // Several popular string matching algorithms are using a bad-character shift table.
    // Boyer Moore: https://www-igm.univ-mlv.fr/~lecroq/string/node14.html
    // Quick Search: https://www-igm.univ-mlv.fr/~lecroq/string/node19.html
    // Smith: https://www-igm.univ-mlv.fr/~lecroq/string/node21.html
    union {
        sz_u8_t jumps[256];
        sz_u64_vec_t vecs[64];
    } bad_shift_table;

    // Let's initialize the table using SWAR to the total length of the string.
    sz_u8_t const *haystack_u8 = (sz_u8_t const *)haystack;
    sz_u8_t const *needle_u8 = (sz_u8_t const *)needle;
    {
        sz_u64_vec_t needle_length_vec;
        needle_length_vec.u64 = ((sz_u8_t)(needle_length - 1)) * 0x0101010101010101ull; // broadcast
        for (sz_size_t byte_index = 0; byte_index != 64; ++byte_index)
            bad_shift_table.vecs[byte_index].u64 = needle_length_vec.u64;
        for (sz_size_t byte_index = 0; byte_index + 1 < needle_length; ++byte_index)
            bad_shift_table.jumps[needle_u8[byte_index]] = (sz_u8_t)(needle_length - byte_index - 1);
    }

    // Another common heuristic is to match a few characters from different parts of a string.
    // Raita suggests to use the first two, the last, and the middle character of the pattern.
    sz_u32_vec_t haystack_vec, needle_vec;

    // Pick the parts of the needle that are worth comparing.
    sz_size_t offset_first, offset_mid, offset_last;
    sz_locate_needle_anomalies_(needle, needle_length, &offset_first, &offset_mid, &offset_last);

    // Broadcast those characters into an unsigned integer.
    needle_vec.u8s[0] = needle_u8[offset_first];
    needle_vec.u8s[1] = needle_u8[offset_first + 1];
    needle_vec.u8s[2] = needle_u8[offset_mid];
    needle_vec.u8s[3] = needle_u8[offset_last];

    // Scan through the whole haystack, skipping the last `needle_length - 1` bytes.
    for (sz_size_t byte_index = 0; byte_index <= haystack_length - needle_length;) {
        haystack_vec.u8s[0] = haystack_u8[byte_index + offset_first];
        haystack_vec.u8s[1] = haystack_u8[byte_index + offset_first + 1];
        haystack_vec.u8s[2] = haystack_u8[byte_index + offset_mid];
        haystack_vec.u8s[3] = haystack_u8[byte_index + offset_last];
        if (haystack_vec.u32 == needle_vec.u32 &&
            sz_equal_serial((sz_cptr_t)haystack_u8 + byte_index, needle, needle_length))
            return (sz_cptr_t)haystack_u8 + byte_index;
        byte_index += bad_shift_table.jumps[haystack_u8[byte_index + needle_length - 1]];
    }
    return SZ_NULL_CHAR;
}

/**
 *  @brief Boyer-Moore-Horspool algorithm for @b reverse-order exact matching of patterns up to @b 256-bytes long.
 *         Uses the Raita heuristic to match the first two, the last, and the middle character of the pattern.
 *
 *  @param haystack The haystack bytes.
 *  @param haystack_length Length of the haystack in bytes.
 *  @param needle The needle bytes.
 *  @param needle_length Length of the needle in bytes (must be <= 256).
 *  @return Pointer to last match, or SZ_NULL_CHAR if none.
 */

/**
 *  @brief Exact substring search helper function, that finds the first occurrence of a prefix of the needle
 *         using a given search function, and then verifies the remaining part of the needle.
 *
 *  @param haystack Pointer to the haystack.
 *  @param haystack_length Length of the haystack in bytes.
 *  @param needle Pointer to the needle.
 *  @param needle_length Length of the needle in bytes.
 *  @param find_prefix Function used to search for the prefix.
 *  @param prefix_length Length of the prefix to search for.
 *  @return Pointer to first match, or SZ_NULL_CHAR if none.
 */
SZ_HELPER_AUTO sz_cptr_t sz_find_with_prefix_( //
    sz_cptr_t haystack, sz_size_t haystack_length, sz_cptr_t needle, sz_size_t needle_length, sz_find_t find_prefix,
    sz_size_t prefix_length) {

    sz_size_t suffix_length = needle_length - prefix_length;
    while (1) {
        sz_cptr_t found = find_prefix(haystack, haystack_length, needle, prefix_length);
        if (!found) return SZ_NULL_CHAR;

        // Verify the remaining part of the needle
        sz_size_t remaining = haystack_length - (found - haystack);
        if (remaining < needle_length) return SZ_NULL_CHAR;
        if (sz_equal_serial(found + prefix_length, needle + prefix_length, suffix_length)) return found;

        // Adjust the position.
        haystack = found + 1;
        haystack_length = remaining - 1;
    }

    // Unreachable, but helps silence compiler warnings:
    return SZ_NULL_CHAR;
}

/**
 *  @brief Exact reverse-order substring search helper function, that finds the last occurrence of a suffix of the
 *         needle using a given search function, and then verifies the remaining part of the needle.
 *
 *  @param haystack Pointer to the haystack.
 *  @param haystack_length Length of the haystack in bytes.
 *  @param needle Pointer to the needle.
 *  @param needle_length Length of the needle in bytes.
 *  @param find_suffix Function used to search for the suffix.
 *  @param suffix_length Length of the suffix to search for.
 *  @return Pointer to last match start, or SZ_NULL_CHAR if none.
 */

SZ_HELPER_NOINLINE sz_cptr_t sz_find_over_4bytes_serial_(sz_cptr_t haystack, sz_size_t haystack_length,
                                                         sz_cptr_t needle, sz_size_t needle_length) {
    return sz_find_with_prefix_(haystack, haystack_length, needle, needle_length, (sz_find_t)sz_find_4byte_serial_, 4);
}

SZ_HELPER_NOINLINE sz_cptr_t sz_find_horspool_over_256bytes_serial_( //
    sz_cptr_t haystack, sz_size_t haystack_length, sz_cptr_t needle, sz_size_t needle_length) {
    return sz_find_with_prefix_(haystack, haystack_length, needle, needle_length,
                                sz_find_horspool_upto_256bytes_serial_, 256);
}


SZ_API_COMPTIME sz_cptr_t sz_find_serial(sz_cptr_t haystack, sz_size_t haystack_length, sz_cptr_t needle,
                                         sz_size_t needle_length) {
    // Empty needle matches at the start, like `strstr`.
    if (!needle_length) return haystack;
    if (haystack_length < needle_length) return SZ_NULL_CHAR;

    // Direct dispatch keeps the compiler-emitted call graph representable in
    // Plan 9 assembly; gocc cannot translate an indirect BLR target.
    if (needle_length == 1) return sz_find_1byte_serial_(haystack, haystack_length, needle, needle_length);
    if (needle_length == 2) return sz_find_2byte_serial_(haystack, haystack_length, needle, needle_length);
    if (needle_length == 3) return sz_find_3byte_serial_(haystack, haystack_length, needle, needle_length);
    if (needle_length == 4) return sz_find_4byte_serial_(haystack, haystack_length, needle, needle_length);
    if (needle_length <= 8) return sz_find_over_4bytes_serial_(haystack, haystack_length, needle, needle_length);
    if (needle_length <= 256)
        return sz_find_horspool_upto_256bytes_serial_(haystack, haystack_length, needle, needle_length);
    return sz_find_horspool_over_256bytes_serial_(haystack, haystack_length, needle, needle_length);
}



/*  Implementation of the string search algorithms using the Arm NEON instruction set, available on 64-bit
 *  Arm processors. Covers billions of mobile CPUs worldwide, including Apple's A-series, and Qualcomm's Snapdragon.
 */
#if defined(__clang__)
#pragma clang attribute push(__attribute__((target("+simd"))), apply_to = function)
#elif defined(__GNUC__)
#pragma GCC push_options
#pragma GCC target("+simd")
#endif

/**
 *  @brief Produce a movemask-style 64-bit value from a NEON comparison result.
 *      Each matching byte sets one bit in the result (bit spacing is 4 bits per byte).
 *
 *  @param vec A 16-byte NEON comparison vector (0xFF where matched, 0x00 otherwise).
 *  @return 64-bit mask with one set bit per matching byte (at bit positions 0, 4, 8, ..., 60).
 */
SZ_HELPER_INLINE sz_u64_t sz_find_vreinterpretq_u8_u4_(uint8x16_t vec) {
    // Use `vshrn` to produce a bitmask, similar to `movemask` in SSE.
    // https://community.arm.com/arm-community-blogs/b/infrastructure-solutions-blog/posts/porting-x86-vector-bitmask-optimizations-to-arm-neon
    return vget_lane_u64(vreinterpret_u64_u8(vshrn_n_u16(vreinterpretq_u16_u8(vec), 4)), 0) & 0x8888888888888888ull;
}

SZ_API_COMPTIME sz_cptr_t sz_find_byte_neon(sz_cptr_t haystack, sz_size_t haystack_length, sz_cptr_t needle) {
    sz_u64_t matches;
    sz_u128_vec_t haystack_vec, needle_vec, matches_vec;
    needle_vec.u8x16 = vld1q_dup_u8((sz_u8_t const *)needle);

    while (haystack_length >= 16) {
        haystack_vec.u8x16 = vld1q_u8((sz_u8_t const *)haystack);
        matches_vec.u8x16 = vceqq_u8(haystack_vec.u8x16, needle_vec.u8x16);
        // In Arm NEON we don't have a `movemask` to combine it with `ctz` and get the offset of the match.
        // But assuming the `vmaxvq` is cheap, we can use it to find the first match, by blending (bitwise
        // selecting) the vector with a relative offsets array.
        matches = sz_find_vreinterpretq_u8_u4_(matches_vec.u8x16);
        if (matches) return haystack + sz_u64_ctz(matches) / 4;

        haystack += 16, haystack_length -= 16;
    }

    return sz_find_byte_serial(haystack, haystack_length, needle);
}


/**
 *  @brief Compute a movemask-style presence bitmask for a 16-byte register against a byteset.
 *
 *  @param haystack_vec The 16-byte input register.
 *  @param set_top_vec_u8x16 Top half of the 32-byte byteset (bytes 0..15).
 *  @param set_bottom_vec_u8x16 Bottom half of the 32-byte byteset (bytes 16..31).
 *  @return 64-bit mask with 4-bit-spaced bits set for matching positions.
 */

/**
 *  @brief Branch-light substring verify, bit-identical to `sz_equal_neon`, inlined into the match loop
 *         to avoid the per-candidate call + length re-dispatch. Loops over 16-byte `vceqq_u8` chunks with
 *         a `vminvq_u8` all-match reduction and closes with one overlapping tail window.
 */
SZ_HELPER_AUTO sz_bool_t sz_find_verify_neon_(sz_cptr_t a, sz_cptr_t b, sz_size_t length) {
    if (length < 16) return sz_equal_serial(a, b, length);

    sz_size_t offset = 0;
    do {
        uint8x16_t a_u8x16 = vld1q_u8((sz_u8_t const *)(a + offset));
        uint8x16_t b_u8x16 = vld1q_u8((sz_u8_t const *)(b + offset));
        if (vminvq_u8(vceqq_u8(a_u8x16, b_u8x16)) != 255) return sz_false_k; // Check if all bytes match.
        offset += 16;
    } while (offset + 16 <= length);

    // Final check - load the last register-long window from the end.
    uint8x16_t a_tail_u8x16 = vld1q_u8((sz_u8_t const *)(a + length - 16));
    uint8x16_t b_tail_u8x16 = vld1q_u8((sz_u8_t const *)(b + length - 16));
    if (vminvq_u8(vceqq_u8(a_tail_u8x16, b_tail_u8x16)) != 255) return sz_false_k;
    return sz_true_k;
}

SZ_API_COMPTIME sz_cptr_t sz_find_neon(sz_cptr_t haystack, sz_size_t haystack_length, sz_cptr_t needle,
                                       sz_size_t needle_length) {

    // Empty needle matches at the start, like `strstr`.
    if (!needle_length) return haystack;
    if (haystack_length < needle_length) return SZ_NULL_CHAR;
    if (needle_length == 1) return sz_find_byte_neon(haystack, haystack_length, needle);

    // Scan through the string.
    // Assuming how tiny the Arm NEON registers are, we should avoid internal branches at all costs.
    // That's why, for smaller needles, we use different loops.
    if (needle_length == 2) {
        // Broadcast needle characters into SIMD registers.
        sz_u64_t matches;
        sz_u128_vec_t h_first_vec, h_last_vec, n_first_vec, n_last_vec, matches_vec;
        // Dealing with 16-bit values, we can load 2 registers at a time and compare 31 possible offsets
        // in a single loop iteration.
        n_first_vec.u8x16 = vld1q_dup_u8((sz_u8_t const *)&needle[0]);
        n_last_vec.u8x16 = vld1q_dup_u8((sz_u8_t const *)&needle[1]);
        for (; haystack_length >= 17; haystack += 16, haystack_length -= 16) {
            h_first_vec.u8x16 = vld1q_u8((sz_u8_t const *)(haystack + 0));
            h_last_vec.u8x16 = vld1q_u8((sz_u8_t const *)(haystack + 1));
            matches_vec.u8x16 = vandq_u8(vceqq_u8(h_first_vec.u8x16, n_first_vec.u8x16),
                                         vceqq_u8(h_last_vec.u8x16, n_last_vec.u8x16));
            matches = sz_find_vreinterpretq_u8_u4_(matches_vec.u8x16);
            if (matches) return haystack + sz_u64_ctz(matches) / 4;
        }
    }
    else if (needle_length == 3) {
        // Broadcast needle characters into SIMD registers.
        sz_u64_t matches;
        sz_u128_vec_t h_first_vec, h_mid_vec, h_last_vec, n_first_vec, n_mid_vec, n_last_vec, matches_vec;
        // Comparing 24-bit values is a bumer. Being lazy, I went with the same approach
        // as when searching for string over 4 characters long. I only avoid the last comparison.
        n_first_vec.u8x16 = vld1q_dup_u8((sz_u8_t const *)&needle[0]);
        n_mid_vec.u8x16 = vld1q_dup_u8((sz_u8_t const *)&needle[1]);
        n_last_vec.u8x16 = vld1q_dup_u8((sz_u8_t const *)&needle[2]);
        for (; haystack_length >= 18; haystack += 16, haystack_length -= 16) {
            h_first_vec.u8x16 = vld1q_u8((sz_u8_t const *)(haystack + 0));
            h_mid_vec.u8x16 = vld1q_u8((sz_u8_t const *)(haystack + 1));
            h_last_vec.u8x16 = vld1q_u8((sz_u8_t const *)(haystack + 2));
            matches_vec.u8x16 = vandq_u8(                           //
                vandq_u8(                                           //
                    vceqq_u8(h_first_vec.u8x16, n_first_vec.u8x16), //
                    vceqq_u8(h_mid_vec.u8x16, n_mid_vec.u8x16)),
                vceqq_u8(h_last_vec.u8x16, n_last_vec.u8x16));
            matches = sz_find_vreinterpretq_u8_u4_(matches_vec.u8x16);
            if (matches) return haystack + sz_u64_ctz(matches) / 4;
        }
    }
    else {
        // Pick the parts of the needle that are worth comparing.
        sz_size_t offset_first, offset_mid, offset_last;
        sz_locate_needle_anomalies_(needle, needle_length, &offset_first, &offset_mid, &offset_last);
        // Broadcast those characters into SIMD registers.
        sz_u64_t matches;
        sz_u128_vec_t h_first_vec, h_mid_vec, h_last_vec, n_first_vec, n_mid_vec, n_last_vec, matches_vec;
        n_first_vec.u8x16 = vld1q_dup_u8((sz_u8_t const *)&needle[offset_first]);
        n_mid_vec.u8x16 = vld1q_dup_u8((sz_u8_t const *)&needle[offset_mid]);
        n_last_vec.u8x16 = vld1q_dup_u8((sz_u8_t const *)&needle[offset_last]);
        // Walk through the string.
        for (; haystack_length >= needle_length + 16; haystack += 16, haystack_length -= 16) {
            h_first_vec.u8x16 = vld1q_u8((sz_u8_t const *)(haystack + offset_first));
            h_mid_vec.u8x16 = vld1q_u8((sz_u8_t const *)(haystack + offset_mid));
            h_last_vec.u8x16 = vld1q_u8((sz_u8_t const *)(haystack + offset_last));
            matches_vec.u8x16 = vandq_u8(                           //
                vandq_u8(                                           //
                    vceqq_u8(h_first_vec.u8x16, n_first_vec.u8x16), //
                    vceqq_u8(h_mid_vec.u8x16, n_mid_vec.u8x16)),
                vceqq_u8(h_last_vec.u8x16, n_last_vec.u8x16));
            matches = sz_find_vreinterpretq_u8_u4_(matches_vec.u8x16);
            while (matches) {
                int potential_offset = sz_u64_ctz(matches) / 4;
                if (sz_find_verify_neon_(haystack + potential_offset, needle, needle_length))
                    return haystack + potential_offset;
                matches &= matches - 1;
            }
        }
    }

    return sz_find_serial(haystack, haystack_length, needle, needle_length);
}




#if defined(__clang__)
#pragma clang attribute pop
#elif defined(__GNUC__)
#pragma GCC pop_options
#endif


/**
 *  @brief Uncased UTF-8 search, comparison & invariance checks: serial scaffolding.
 *  @file include/stringzilla/utf8_uncased/serial.h
 *  @author Ash Vardanian
 *  @sa include/stringzilla/utf8_uncased.h
 */


/**
 *  @brief Serial backend for UTF-8 case folding.
 *  @file include/stringzilla/utf8_uncased_fold/serial.h
 *  @author Ash Vardanian
 *  @sa include/stringzilla/utf8_uncased_fold.h
 */


/**
 *  @brief Branchless ASCII case fold - converts A-Z to a-z.
 *  Uses unsigned subtraction trick: (c - 'A') <= 25 is true only for uppercase letters.
 */
SZ_HELPER_AUTO sz_u8_t sz_ascii_fold_(sz_u8_t c) { return c + (((sz_u8_t)(c - 'A') <= 25u) * 0x20); }

/**
 *  @brief Folded-rune representation of a byte that does not begin a well-formed codepoint.
 *
 *  A malformed byte folds to itself and is matched/compared byte-for-byte, never as a Unicode codepoint.
 *  Tagging it above the valid Unicode range (0x10FFFF) keeps it distinct from every real folded rune, so a
 *  lone malformed byte 0xFC can only match another malformed 0xFC - never the valid rune U+00FC ('ü'). Two
 *  equal malformed bytes still produce equal tagged runes, preserving byte-for-byte matching.
 */
SZ_HELPER_INLINE sz_rune_t sz_rune_malformed_byte_(sz_u8_t byte) { return 0x80000000u | (sz_rune_t)byte; }

/**
 *  Bit flags describing which UTF-8 lead-byte families occur in a chunk, shared by every back-end.
 *  Each family shares one folding strategy, so the union of flags picks the chunk handler in a
 *  single dispatch - instead of sequentially probing per-script fast paths, which degrades on
 *  mixed-script text. The flags themselves are ISA-neutral; only the way a back-end derives them
 *  differs - a `VPERMB` lookup on Ice Lake, `vqtbl4q_u8` on NEON, a compare tree on Haswell.
 */
enum sz_utf8_fold_lead_family_t {
    sz_utf8_fold_lead_caseless_flag_k = 1 << 0,       // D7-DF, E0, E3-E9, EB-EE: scripts with no case
    sz_utf8_fold_lead_latin_flag_k = 1 << 1,          // C2-C3: Latin-1 Supplement
    sz_utf8_fold_lead_latin_extended_flag_k = 1 << 2, // C4-C6: Latin Extended-A and the Ext-B +1 pairs
    sz_utf8_fold_lead_cyrillic_flag_k = 1 << 3,       // D0-D1: basic Cyrillic
    sz_utf8_fold_lead_greek_flag_k = 1 << 4,          // CE-CF: basic Greek
    sz_utf8_fold_lead_e1_flag_k = 1 << 5,             // E1: Latin Ext Additional, Georgian, Greek Extended
    sz_utf8_fold_lead_guarded_flag_k = 1 << 6,        // E2, EA, EF: case-awareness depends on the second byte
    sz_utf8_fold_lead_complex_flag_k = 1 << 7,        // C0-C1, C7-CD, D2-D6, F0-FF: decode or serial paths
};

/**
 *  Lead-byte family table indexed by the low 6 bits of the lead byte: leads 0xC0-0xFF map onto
 *  indices 0x00-0x3F injectively, and ASCII/continuation bytes are masked out before the lookup.
 *  Byte-for-byte the same 64 values as the Ice Lake `lead_families_lut`, but laid out in
 *  ascending index order: `vqtbl4q_u8` reads its `uint8x16x4_t` table in memory order, whereas
 *  `_mm512_set_epi8` lists lanes 0x3F → 0x00.
 */

/**
 *  Per-codepoint deltas for 2-byte Latin Extended sequences, indexed by the continuation byte's
 *  low 6 bits: 0x00 = identity, 0x01 = fold by +1, 0x80 = irregular (serial fallback). The same
 *  64 values as the Ice Lake `c4_deltas_lut`, but in ascending index order: `vqtbl4q_u8` reads
 *  its table in memory order, whereas `_mm512_set_epi8` lists lanes 0x3F → 0x00.
 *  Generated from Unicode full case folding; verified against the serial reference in tests.
 */

/** @brief  Character boundary of the first stop lane in a 64-byte superchunk: takes the lowest set bit of
 *          @p stop_lanes and walks back over continuation bytes so the consumed prefix ends on a boundary.
 *          Shared u64 mask math for the windowed ISA fold handlers; landing on byte 0 routes one rune to the
 *          serial fallback. */
/**  Helper macro for readable assertions - use for SIMD implementation reference */
#define sz_is_in_range_(x, low, high) ((x) >= (low) && (x) <= (high))

/**
 *  @brief Fold a Unicode codepoint to its case-folded form (Unicode 17.0).
 *
 *  Optimization strategy:
 *  - Single-comparison range checks: `(sz_u32_t)(rune - base) <= size` instead of two comparisons
 *  - Combined upper+lower ranges: check both cases, apply offset only for uppercase (branchless)
 *  - Combined even/odd ranges: check full range, apply +1 only for uppercase parity
 *  - Hierarchical by UTF-8 byte width for early exit on common cases
 *  - Per-section switches for irregular mappings (better compiler optimization)
 *
 *  Each range check includes an assertion with traditional bounds for SIMD implementation reference.
 */
SZ_HELPER_AUTO sz_size_t sz_unicode_fold_codepoint_(sz_rune_t rune, sz_rune_t *folded) {

    // 1-byte UTF-8 (U+0000-007F): ASCII - only A-Z needs folding
    if (rune <= 0x7F) {
        if ((sz_u32_t)(rune - 0x41) <= 25) { // A-Z: 0x41-0x5A (26 chars)
            sz_assert_(sz_is_in_range_(rune, 0x0041, 0x005A));
            folded[0] = rune + 0x20;
            return 1;
        }
        folded[0] = rune;
        return 1; // digits, punctuation, control chars unchanged
    }

    // 2-byte UTF-8 (U+0080-07FF): Latin, Greek, Cyrillic, Armenian
    if (rune <= 0x7FF) {
        // Cyrillic А-я: 0x0410-0x044F (upper 0x0410-0x042F, lower 0x0430-0x044F)
        if ((sz_u32_t)(rune - 0x0410) <= 0x3F) {
            sz_assert_(sz_is_in_range_(rune, 0x0410, 0x044F));
            folded[0] = rune + ((rune <= 0x042F) * 0x20);
            return 1;
        } // +32 if upper, +0 if lower

        // Latin-1 À-þ: 0x00C0-0x00FE (upper 0x00C0-0x00DE, lower 0x00E0-0x00FE)
        if ((sz_u32_t)(rune - 0x00C0) <= 0x3E) {
            sz_assert_(sz_is_in_range_(rune, 0x00C0, 0x00FE));
            if ((rune | 0x20) == 0xF7) {
                folded[0] = rune;
                return 1;
            } // × (D7) and ÷ (F7) unchanged
            // 'ß' (U+00DF, C3 9F) → "ss" (U+0073 U+0073, 73 73)
            if (rune == 0x00DF) {
                folded[0] = 0x0073;
                folded[1] = 0x0073;
                return 2;
            }
            folded[0] = rune + ((rune <= 0x00DE) * 0x20);
            return 1;
        }

        // Greek Α-Ρ: 0x0391-0x03A1 → α-ρ (+32)
        if ((sz_u32_t)(rune - 0x0391) <= 0x10) {
            sz_assert_(sz_is_in_range_(rune, 0x0391, 0x03A1));
            folded[0] = rune + 0x20;
            return 1;
        }

        // Greek Σ-Ϋ: 0x03A3-0x03AB → σ-ϋ (+32)
        if ((sz_u32_t)(rune - 0x03A3) <= 0x08) {
            sz_assert_(sz_is_in_range_(rune, 0x03A3, 0x03AB));
            folded[0] = rune + 0x20;
            return 1;
        }

        // Cyrillic Ѐ-Џ: 0x0400-0x040F → ѐ-џ (+80)
        if ((sz_u32_t)(rune - 0x0400) <= 0x0F) {
            sz_assert_(sz_is_in_range_(rune, 0x0400, 0x040F));
            folded[0] = rune + 0x50;
            return 1;
        }

        // Armenian Ա-Ֆ: 0x0531-0x0556 → ա-ֆ (+48)
        if ((sz_u32_t)(rune - 0x0531) <= 0x25) {
            sz_assert_(sz_is_in_range_(rune, 0x0531, 0x0556));
            folded[0] = rune + 0x30;
            return 1;
        }

        // Greek Έ-Ί: 0x0388-0x038A (+37)
        if ((sz_u32_t)(rune - 0x0388) <= 0x02) {
            sz_assert_(sz_is_in_range_(rune, 0x0388, 0x038A));
            folded[0] = rune + 0x25;
            return 1;
        }

        // Greek Ͻ-Ͽ: 0x03FD-0x03FF → ͻ-Ϳ (-130)
        if ((sz_u32_t)(rune - 0x03FD) <= 0x02) {
            sz_assert_(sz_is_in_range_(rune, 0x03FD, 0x03FF));
            folded[0] = rune - 130;
            return 1;
        }

        // Next let's handle the even/odd parity-based ranges
        sz_u32_t is_even = ((rune & 1) == 0);

        // Latin Extended-A: Ā-Į (0x0100-0x012E, even → +1)
        if ((sz_u32_t)(rune - 0x0100) <= 0x2E && is_even) {
            sz_assert_(sz_is_in_range_(rune, 0x0100, 0x012E));
            folded[0] = rune + 1;
            return 1;
        }

        // Latin Extended-A: Ĳ-Ķ (0x0132-0x0136, even → +1)
        if ((sz_u32_t)(rune - 0x0132) <= 0x04 && is_even) {
            sz_assert_(sz_is_in_range_(rune, 0x0132, 0x0136));
            folded[0] = rune + 1;
            return 1;
        }

        // Latin Extended-A: Ĺ-Ň (0x0139-0x0147, odd → +1)
        if ((sz_u32_t)(rune - 0x0139) <= 0x0E && !is_even) {
            sz_assert_(sz_is_in_range_(rune, 0x0139, 0x0147));
            folded[0] = rune + 1;
            return 1;
        }

        // Latin Extended-A: Ŋ-Ŷ (0x014A-0x0176, even → +1)
        if ((sz_u32_t)(rune - 0x014A) <= 0x2C && is_even) {
            sz_assert_(sz_is_in_range_(rune, 0x014A, 0x0176));
            folded[0] = rune + 1;
            return 1;
        }

        // Latin Extended-A: Ź-Ž (0x0179-0x017D, odd → +1)
        if ((sz_u32_t)(rune - 0x0179) <= 0x04 && !is_even) {
            sz_assert_(sz_is_in_range_(rune, 0x0179, 0x017D));
            folded[0] = rune + 1;
            return 1;
        }

        // Latin Extended-B: Ǎ-Ǜ (0x01CD-0x01DB, odd → +1)
        if ((sz_u32_t)(rune - 0x01CD) <= 0x0E && !is_even) {
            sz_assert_(sz_is_in_range_(rune, 0x01CD, 0x01DB));
            folded[0] = rune + 1;
            return 1;
        }

        // Latin Extended-B: Ǟ-Ǯ (0x01DE-0x01EE, even → +1)
        if ((sz_u32_t)(rune - 0x01DE) <= 0x10 && is_even) {
            sz_assert_(sz_is_in_range_(rune, 0x01DE, 0x01EE));
            folded[0] = rune + 1;
            return 1;
        }

        // Latin Extended-B: Ǹ-Ǿ (0x01F8-0x01FE, even → +1)
        if ((sz_u32_t)(rune - 0x01F8) <= 0x06 && is_even) {
            sz_assert_(sz_is_in_range_(rune, 0x01F8, 0x01FE));
            folded[0] = rune + 1;
            return 1;
        }

        // Latin Extended-B: Ȁ-Ȟ (0x0200-0x021E, even → +1)
        if ((sz_u32_t)(rune - 0x0200) <= 0x1E && is_even) {
            sz_assert_(sz_is_in_range_(rune, 0x0200, 0x021E));
            folded[0] = rune + 1;
            return 1;
        }

        // Latin Extended-B: Ȣ-Ȳ (0x0222-0x0232, even → +1)
        if ((sz_u32_t)(rune - 0x0222) <= 0x10 && is_even) {
            sz_assert_(sz_is_in_range_(rune, 0x0222, 0x0232));
            folded[0] = rune + 1;
            return 1;
        }

        // Latin Extended-B: Ɇ-Ɏ (0x0246-0x024E, even → +1)
        if ((sz_u32_t)(rune - 0x0246) <= 0x08 && is_even) {
            sz_assert_(sz_is_in_range_(rune, 0x0246, 0x024E));
            folded[0] = rune + 1;
            return 1;
        }

        // Greek archaic: Ͱ-Ͳ (0x0370-0x0372, even → +1)
        if ((sz_u32_t)(rune - 0x0370) <= 0x02 && is_even) {
            sz_assert_(sz_is_in_range_(rune, 0x0370, 0x0372));
            folded[0] = rune + 1;
            return 1;
        }

        // Greek archaic: Ϙ-Ϯ (0x03D8-0x03EE, even → +1)
        if ((sz_u32_t)(rune - 0x03D8) <= 0x16 && is_even) {
            sz_assert_(sz_is_in_range_(rune, 0x03D8, 0x03EE));
            folded[0] = rune + 1;
            return 1;
        }

        // Cyrillic extended: Ѡ-Ҁ (0x0460-0x0480, even → +1)
        if ((sz_u32_t)(rune - 0x0460) <= 0x20 && is_even) {
            sz_assert_(sz_is_in_range_(rune, 0x0460, 0x0480));
            folded[0] = rune + 1;
            return 1;
        }

        // Cyrillic extended: Ҋ-Ҿ (0x048A-0x04BE, even → +1)
        if ((sz_u32_t)(rune - 0x048A) <= 0x34 && is_even) {
            sz_assert_(sz_is_in_range_(rune, 0x048A, 0x04BE));
            folded[0] = rune + 1;
            return 1;
        }

        // Cyrillic extended: Ӂ-Ӎ (0x04C1-0x04CD, odd → +1)
        if ((sz_u32_t)(rune - 0x04C1) <= 0x0C && !is_even) {
            sz_assert_(sz_is_in_range_(rune, 0x04C1, 0x04CD));
            folded[0] = rune + 1;
            return 1;
        }

        // Cyrillic extended: Ӑ-Ӿ (0x04D0-0x04FE, even → +1)
        if ((sz_u32_t)(rune - 0x04D0) <= 0x2E && is_even) {
            sz_assert_(sz_is_in_range_(rune, 0x04D0, 0x04FE));
            folded[0] = rune + 1;
            return 1;
        }

        // Cyrillic extended: Ԁ-Ԯ (0x0500-0x052E, even → +1)
        if ((sz_u32_t)(rune - 0x0500) <= 0x2E && is_even) {
            sz_assert_(sz_is_in_range_(rune, 0x0500, 0x052E));
            folded[0] = rune + 1;
            return 1;
        }

        // Next let's handle the 2-byte irregular one-to-one mappings
        switch (rune) {
        // Latin-1 Supplement specials
        case 0x00B5: folded[0] = 0x03BC; return 1; // 'µ' (U+00B5, C2 B5) → 'μ' (U+03BC, CE BC)
        case 0x0178: folded[0] = 0x00FF; return 1; // 'Ÿ' (U+0178, C5 B8) → 'ÿ' (U+00FF, C3 BF)
        case 0x017F:
            folded[0] = 0x0073;
            return 1; // 'ſ' (U+017F, C5 BF) → 's' (U+0073, 73)
        // Latin Extended-B: African/IPA letters (0x0181-0x01BF)
        case 0x0181: folded[0] = 0x0253; return 1; // 'Ɓ' (U+0181, C6 81) → 'ɓ' (U+0253, C9 93)
        case 0x0182: folded[0] = 0x0183; return 1; // 'Ƃ' (U+0182, C6 82) → 'ƃ' (U+0183, C6 83)
        case 0x0184: folded[0] = 0x0185; return 1; // 'Ƅ' (U+0184, C6 84) → 'ƅ' (U+0185, C6 85)
        case 0x0186: folded[0] = 0x0254; return 1; // 'Ɔ' (U+0186, C6 86) → 'ɔ' (U+0254, C9 94)
        case 0x0187: folded[0] = 0x0188; return 1; // 'Ƈ' (U+0187, C6 87) → 'ƈ' (U+0188, C6 88)
        case 0x0189: folded[0] = 0x0256; return 1; // 'Ɖ' (U+0189, C6 89) → 'ɖ' (U+0256, C9 96)
        case 0x018A: folded[0] = 0x0257; return 1; // 'Ɗ' (U+018A, C6 8A) → 'ɗ' (U+0257, C9 97)
        case 0x018B: folded[0] = 0x018C; return 1; // 'Ƌ' (U+018B, C6 8B) → 'ƌ' (U+018C, C6 8C)
        case 0x018E: folded[0] = 0x01DD; return 1; // 'Ǝ' (U+018E, C6 8E) → 'ǝ' (U+01DD, C7 9D)
        case 0x018F: folded[0] = 0x0259; return 1; // 'Ə' (U+018F, C6 8F) → 'ə' (U+0259, C9 99)
        case 0x0190: folded[0] = 0x025B; return 1; // 'Ɛ' (U+0190, C6 90) → 'ɛ' (U+025B, C9 9B)
        case 0x0191: folded[0] = 0x0192; return 1; // 'Ƒ' (U+0191, C6 91) → 'ƒ' (U+0192, C6 92)
        case 0x0193: folded[0] = 0x0260; return 1; // 'Ɠ' (U+0193, C6 93) → 'ɠ' (U+0260, C9 A0)
        case 0x0194: folded[0] = 0x0263; return 1; // 'Ɣ' (U+0194, C6 94) → 'ɣ' (U+0263, C9 A3)
        case 0x0196: folded[0] = 0x0269; return 1; // 'Ɩ' (U+0196, C6 96) → 'ɩ' (U+0269, C9 A9)
        case 0x0197: folded[0] = 0x0268; return 1; // 'Ɨ' (U+0197, C6 97) → 'ɨ' (U+0268, C9 A8)
        case 0x0198: folded[0] = 0x0199; return 1; // 'Ƙ' (U+0198, C6 98) → 'ƙ' (U+0199, C6 99)
        case 0x019C: folded[0] = 0x026F; return 1; // 'Ɯ' (U+019C, C6 9C) → 'ɯ' (U+026F, C9 AF)
        case 0x019D: folded[0] = 0x0272; return 1; // 'Ɲ' (U+019D, C6 9D) → 'ɲ' (U+0272, C9 B2)
        case 0x019F: folded[0] = 0x0275; return 1; // 'Ɵ' (U+019F, C6 9F) → 'ɵ' (U+0275, C9 B5)
        case 0x01A0: folded[0] = 0x01A1; return 1; // 'Ơ' (U+01A0, C6 A0) → 'ơ' (U+01A1, C6 A1)
        case 0x01A2: folded[0] = 0x01A3; return 1; // 'Ƣ' (U+01A2, C6 A2) → 'ƣ' (U+01A3, C6 A3)
        case 0x01A4: folded[0] = 0x01A5; return 1; // 'Ƥ' (U+01A4, C6 A4) → 'ƥ' (U+01A5, C6 A5)
        case 0x01A6: folded[0] = 0x0280; return 1; // 'Ʀ' (U+01A6, C6 A6) → 'ʀ' (U+0280, CA 80)
        case 0x01A7: folded[0] = 0x01A8; return 1; // 'Ƨ' (U+01A7, C6 A7) → 'ƨ' (U+01A8, C6 A8)
        case 0x01A9: folded[0] = 0x0283; return 1; // 'Ʃ' (U+01A9, C6 A9) → 'ʃ' (U+0283, CA 83)
        case 0x01AC: folded[0] = 0x01AD; return 1; // 'Ƭ' (U+01AC, C6 AC) → 'ƭ' (U+01AD, C6 AD)
        case 0x01AE: folded[0] = 0x0288; return 1; // 'Ʈ' (U+01AE, C6 AE) → 'ʈ' (U+0288, CA 88)
        case 0x01AF: folded[0] = 0x01B0; return 1; // 'Ư' (U+01AF, C6 AF) → 'ư' (U+01B0, C6 B0)
        case 0x01B1: folded[0] = 0x028A; return 1; // 'Ʊ' (U+01B1, C6 B1) → 'ʊ' (U+028A, CA 8A)
        case 0x01B2: folded[0] = 0x028B; return 1; // 'Ʋ' (U+01B2, C6 B2) → 'ʋ' (U+028B, CA 8B)
        case 0x01B3: folded[0] = 0x01B4; return 1; // 'Ƴ' (U+01B3, C6 B3) → 'ƴ' (U+01B4, C6 B4)
        case 0x01B5: folded[0] = 0x01B6; return 1; // 'Ƶ' (U+01B5, C6 B5) → 'ƶ' (U+01B6, C6 B6)
        case 0x01B7: folded[0] = 0x0292; return 1; // 'Ʒ' (U+01B7, C6 B7) → 'ʒ' (U+0292, CA 92)
        case 0x01B8: folded[0] = 0x01B9; return 1; // 'Ƹ' (U+01B8, C6 B8) → 'ƹ' (U+01B9, C6 B9)
        case 0x01BC:
            folded[0] = 0x01BD;
            return 1; // 'Ƽ' (U+01BC, C6 BC) → 'ƽ' (U+01BD, C6 BD)
        // Digraphs: Serbian/Croatian DŽ, LJ, NJ and DZ
        case 0x01C4: folded[0] = 0x01C6; return 1; // 'Ǆ' (U+01C4, C7 84) → 'ǆ' (U+01C6, C7 86)
        case 0x01C5: folded[0] = 0x01C6; return 1; // 'ǅ' (U+01C5, C7 85) → 'ǆ' (U+01C6, C7 86)
        case 0x01C7: folded[0] = 0x01C9; return 1; // 'Ǉ' (U+01C7, C7 87) → 'ǉ' (U+01C9, C7 89)
        case 0x01C8: folded[0] = 0x01C9; return 1; // 'ǈ' (U+01C8, C7 88) → 'ǉ' (U+01C9, C7 89)
        case 0x01CA: folded[0] = 0x01CC; return 1; // 'Ǌ' (U+01CA, C7 8A) → 'ǌ' (U+01CC, C7 8C)
        case 0x01CB: folded[0] = 0x01CC; return 1; // 'ǋ' (U+01CB, C7 8B) → 'ǌ' (U+01CC, C7 8C)
        case 0x01F1: folded[0] = 0x01F3; return 1; // 'Ǳ' (U+01F1, C7 B1) → 'ǳ' (U+01F3, C7 B3)
        case 0x01F2:
            folded[0] = 0x01F3;
            return 1; // 'ǲ' (U+01F2, C7 B2) → 'ǳ' (U+01F3, C7 B3)
        // Latin Extended-B: isolated irregulars
        case 0x01F4: folded[0] = 0x01F5; return 1; // 'Ǵ' (U+01F4, C7 B4) → 'ǵ' (U+01F5, C7 B5)
        case 0x01F6: folded[0] = 0x0195; return 1; // 'Ƕ' (U+01F6, C7 B6) → 'ƕ' (U+0195, C6 95)
        case 0x01F7: folded[0] = 0x01BF; return 1; // 'Ƿ' (U+01F7, C7 B7) → 'ƿ' (U+01BF, C6 BF)
        case 0x0220: folded[0] = 0x019E; return 1; // 'Ƞ' (U+0220, C8 A0) → 'ƞ' (U+019E, C6 9E)
        case 0x023A: folded[0] = 0x2C65; return 1; // 'Ⱥ' (U+023A, C8 BA) → 'ⱥ' (U+2C65, E2 B1 A5)
        case 0x023B: folded[0] = 0x023C; return 1; // 'Ȼ' (U+023B, C8 BB) → 'ȼ' (U+023C, C8 BC)
        case 0x023D: folded[0] = 0x019A; return 1; // 'Ƚ' (U+023D, C8 BD) → 'ƚ' (U+019A, C6 9A)
        case 0x023E: folded[0] = 0x2C66; return 1; // 'Ⱦ' (U+023E, C8 BE) → 'ⱦ' (U+2C66, E2 B1 A6)
        case 0x0241: folded[0] = 0x0242; return 1; // 'Ɂ' (U+0241, C9 81) → 'ɂ' (U+0242, C9 82)
        case 0x0243: folded[0] = 0x0180; return 1; // 'Ƀ' (U+0243, C9 83) → 'ƀ' (U+0180, C6 80)
        case 0x0244: folded[0] = 0x0289; return 1; // 'Ʉ' (U+0244, C9 84) → 'ʉ' (U+0289, CA 89)
        case 0x0245:
            folded[0] = 0x028C;
            return 1; // 'Ʌ' (U+0245, C9 85) → 'ʌ' (U+028C, CA 8C)
        // Greek: combining iota, accented vowels, variant forms
        case 0x0345: folded[0] = 0x03B9; return 1; // 'ͅ' (U+0345, CD 85) → 'ι' (U+03B9, CE B9)
        case 0x0376: folded[0] = 0x0377; return 1; // 'Ͷ' (U+0376, CD B6) → 'ͷ' (U+0377, CD B7)
        case 0x037F: folded[0] = 0x03F3; return 1; // 'Ϳ' (U+037F, CD BF) → 'ϳ' (U+03F3, CF B3)
        case 0x0386: folded[0] = 0x03AC; return 1; // 'Ά' (U+0386, CE 86) → 'ά' (U+03AC, CE AC)
        case 0x038C: folded[0] = 0x03CC; return 1; // 'Ό' (U+038C, CE 8C) → 'ό' (U+03CC, CF 8C)
        case 0x038E: folded[0] = 0x03CD; return 1; // 'Ύ' (U+038E, CE 8E) → 'ύ' (U+03CD, CF 8D)
        case 0x038F: folded[0] = 0x03CE; return 1; // 'Ώ' (U+038F, CE 8F) → 'ώ' (U+03CE, CF 8E)
        case 0x03C2: folded[0] = 0x03C3; return 1; // 'ς' (U+03C2, CF 82) → 'σ' (U+03C3, CF 83)
        case 0x03CF: folded[0] = 0x03D7; return 1; // 'Ϗ' (U+03CF, CF 8F) → 'ϗ' (U+03D7, CF 97)
        case 0x03D0: folded[0] = 0x03B2; return 1; // 'ϐ' (U+03D0, CF 90) → 'β' (U+03B2, CE B2)
        case 0x03D1: folded[0] = 0x03B8; return 1; // 'ϑ' (U+03D1, CF 91) → 'θ' (U+03B8, CE B8)
        case 0x03D5: folded[0] = 0x03C6; return 1; // 'ϕ' (U+03D5, CF 95) → 'φ' (U+03C6, CF 86)
        case 0x03D6: folded[0] = 0x03C0; return 1; // 'ϖ' (U+03D6, CF 96) → 'π' (U+03C0, CF 80)
        case 0x03F0: folded[0] = 0x03BA; return 1; // 'ϰ' (U+03F0, CF B0) → 'κ' (U+03BA, CE BA)
        case 0x03F1: folded[0] = 0x03C1; return 1; // 'ϱ' (U+03F1, CF B1) → 'ρ' (U+03C1, CF 81)
        case 0x03F4: folded[0] = 0x03B8; return 1; // 'ϴ' (U+03F4, CF B4) → 'θ' (U+03B8, CE B8)
        case 0x03F5: folded[0] = 0x03B5; return 1; // 'ϵ' (U+03F5, CF B5) → 'ε' (U+03B5, CE B5)
        case 0x03F7: folded[0] = 0x03F8; return 1; // 'Ϸ' (U+03F7, CF B7) → 'ϸ' (U+03F8, CF B8)
        case 0x03F9: folded[0] = 0x03F2; return 1; // 'Ϲ' (U+03F9, CF B9) → 'ϲ' (U+03F2, CF B2)
        case 0x03FA:
            folded[0] = 0x03FB;
            return 1; // 'Ϻ' (U+03FA, CF BA) → 'ϻ' (U+03FB, CF BB)
        // Cyrillic: palochka
        case 0x04C0: folded[0] = 0x04CF; return 1; // 'Ӏ' (U+04C0, D3 80) → 'ӏ' (U+04CF, D3 8F)
        }

        // 2-byte one-to-many expansions
        switch (rune) {
        // ß handled inline in Latin-1 range above... interestingly the capital Eszett is in the 3-byte range!
        // case 0x00DF: folded[0] = 0x0073; folded[1] = 0x0073; return 2;

        // 'İ' (U+0130, C4 B0) → "i̇" (U+0069 U+0307, 69 CC 87)
        case 0x0130:
            folded[0] = 0x0069;
            folded[1] = 0x0307;
            return 2;
        // 'ŉ' (U+0149, C5 89) → "ʼn" (U+02BC U+006E, CA BC 6E)
        case 0x0149:
            folded[0] = 0x02BC;
            folded[1] = 0x006E;
            return 2;
        // 'ǰ' (U+01F0, C7 B0) → "ǰ" (U+006A U+030C, 6A CC 8C)
        case 0x01F0:
            folded[0] = 0x006A;
            folded[1] = 0x030C;
            return 2;
        // 'ΐ' (U+0390, CE 90) → "ΐ" (U+03B9 U+0308 U+0301, CE B9 CC 88 CC 81)
        case 0x0390:
            folded[0] = 0x03B9;
            folded[1] = 0x0308;
            folded[2] = 0x0301;
            return 3;
        // 'ΰ' (U+03B0, CE B0) → "ΰ" (U+03C5 U+0308 U+0301, CF 85 CC 88 CC 81)
        case 0x03B0:
            folded[0] = 0x03C5;
            folded[1] = 0x0308;
            folded[2] = 0x0301;
            return 3;
        // 'և' (U+0587, D6 87) → "եւ" (U+0565 U+0582, D5 A5 D6 82)
        case 0x0587:
            folded[0] = 0x0565;
            folded[1] = 0x0582;
            return 2;
        }

        folded[0] = rune;
        return 1; // 2-byte: no folding needed
    }

    // 3-byte UTF-8 (U+0800-FFFF): Georgian, Cherokee, Greek Extended, etc.
    if (rune <= 0xFFFF) {
        // Georgian Ⴀ-Ⴥ: 0x10A0-0x10C5 (+7264)
        if ((sz_u32_t)(rune - 0x10A0) <= 0x25) {
            sz_assert_(sz_is_in_range_(rune, 0x10A0, 0x10C5));
            folded[0] = rune + 0x1C60;
            return 1;
        }

        // Georgian Mtavruli Ა-Ჺ: 0x1C90-0x1CBA (-3008)
        if ((sz_u32_t)(rune - 0x1C90) <= 0x2A) {
            sz_assert_(sz_is_in_range_(rune, 0x1C90, 0x1CBA));
            folded[0] = rune - 0xBC0;
            return 1;
        }

        // Georgian Mtavruli Ჽ-Ჿ: 0x1CBD-0x1CBF (-3008)
        if ((sz_u32_t)(rune - 0x1CBD) <= 0x02) {
            sz_assert_(sz_is_in_range_(rune, 0x1CBD, 0x1CBF));
            folded[0] = rune - 0xBC0;
            return 1;
        }

        // Cherokee Ᏸ-Ᏽ: 0x13F8-0x13FD (-8)
        if ((sz_u32_t)(rune - 0x13F8) <= 0x05) {
            sz_assert_(sz_is_in_range_(rune, 0x13F8, 0x13FD));
            folded[0] = rune - 8;
            return 1;
        }

        // Cherokee Ꭰ-Ᏼ: 0xAB70-0xABBF → Ꭰ-Ᏼ: 0x13A0-0x13EF (-38864)
        if ((sz_u32_t)(rune - 0xAB70) <= 0x4F) {
            sz_assert_(sz_is_in_range_(rune, 0xAB70, 0xABBF));
            folded[0] = rune - 0x97D0;
            return 1;
        }

        // Greek Extended: multiple -8 offset ranges
        if ((sz_u32_t)(rune - 0x1F08) <= 0x07) { // Ἀ-Ἇ
            sz_assert_(sz_is_in_range_(rune, 0x1F08, 0x1F0F));
            folded[0] = rune - 8;
            return 1;
        }
        if ((sz_u32_t)(rune - 0x1F18) <= 0x05) { // Ἐ-Ἕ
            sz_assert_(sz_is_in_range_(rune, 0x1F18, 0x1F1D));
            folded[0] = rune - 8;
            return 1;
        }
        if ((sz_u32_t)(rune - 0x1F28) <= 0x07) { // Ἠ-Ἧ
            sz_assert_(sz_is_in_range_(rune, 0x1F28, 0x1F2F));
            folded[0] = rune - 8;
            return 1;
        }
        if ((sz_u32_t)(rune - 0x1F38) <= 0x07) { // Ἰ-Ἷ
            sz_assert_(sz_is_in_range_(rune, 0x1F38, 0x1F3F));
            folded[0] = rune - 8;
            return 1;
        }
        if ((sz_u32_t)(rune - 0x1F48) <= 0x05) { // Ὀ-Ὅ
            sz_assert_(sz_is_in_range_(rune, 0x1F48, 0x1F4D));
            folded[0] = rune - 8;
            return 1;
        }
        if ((sz_u32_t)(rune - 0x1F68) <= 0x07) { // Ὠ-Ὧ
            sz_assert_(sz_is_in_range_(rune, 0x1F68, 0x1F6F));
            folded[0] = rune - 8;
            return 1;
        }

        // Greek Extended Ὲ-Ή: 0x1FC8-0x1FCB (-86)
        if ((sz_u32_t)(rune - 0x1FC8) <= 0x03) {
            sz_assert_(sz_is_in_range_(rune, 0x1FC8, 0x1FCB));
            folded[0] = rune - 86;
            return 1;
        }

        // Roman numerals Ⅰ-Ⅿ: 0x2160-0x216F (+16)
        if ((sz_u32_t)(rune - 0x2160) <= 0x0F) {
            sz_assert_(sz_is_in_range_(rune, 0x2160, 0x216F));
            folded[0] = rune + 0x10;
            return 1;
        }

        // Circled letters Ⓐ-Ⓩ: 0x24B6-0x24CF (+26)
        if ((sz_u32_t)(rune - 0x24B6) <= 0x19) {
            sz_assert_(sz_is_in_range_(rune, 0x24B6, 0x24CF));
            folded[0] = rune + 0x1A;
            return 1;
        }

        // Glagolitic Ⰰ-Ⱟ: 0x2C00-0x2C2F (+48)
        if ((sz_u32_t)(rune - 0x2C00) <= 0x2F) {
            sz_assert_(sz_is_in_range_(rune, 0x2C00, 0x2C2F));
            folded[0] = rune + 0x30;
            return 1;
        }

        // Fullwidth Ａ-Ｚ: 0xFF21-0xFF3A (+32)
        if ((sz_u32_t)(rune - 0xFF21) <= 0x19) {
            sz_assert_(sz_is_in_range_(rune, 0xFF21, 0xFF3A));
            folded[0] = rune + 0x20;
            return 1;
        }

        // Next let's handle the even/odd parity-based ranges
        sz_u32_t is_even = ((rune & 1) == 0);

        // Latin Extended Additional Ḁ-Ẕ: 0x1E00-0x1E94
        if ((sz_u32_t)(rune - 0x1E00) <= 0x94 && is_even) {
            sz_assert_(sz_is_in_range_(rune, 0x1E00, 0x1E94));
            folded[0] = rune + 1;
            return 1;
        }

        // Latin Extended Additional (Vietnamese) Ạ-Ỿ: 0x1EA0-0x1EFE
        if ((sz_u32_t)(rune - 0x1EA0) <= 0x5E && is_even) {
            sz_assert_(sz_is_in_range_(rune, 0x1EA0, 0x1EFE));
            folded[0] = rune + 1;
            return 1;
        }

        // Coptic Ⲁ-Ⳣ: 0x2C80-0x2CE2
        if ((sz_u32_t)(rune - 0x2C80) <= 0x62 && is_even) {
            sz_assert_(sz_is_in_range_(rune, 0x2C80, 0x2CE2));
            folded[0] = rune + 1;
            return 1;
        }

        // Cyrillic Extended-B Ꙁ-Ꙭ: 0xA640-0xA66C
        if ((sz_u32_t)(rune - 0xA640) <= 0x2C && is_even) {
            sz_assert_(sz_is_in_range_(rune, 0xA640, 0xA66C));
            folded[0] = rune + 1;
            return 1;
        }

        // Cyrillic Extended-B Ꚁ-Ꚛ: 0xA680-0xA69A
        if ((sz_u32_t)(rune - 0xA680) <= 0x1A && is_even) {
            sz_assert_(sz_is_in_range_(rune, 0xA680, 0xA69A));
            folded[0] = rune + 1;
            return 1;
        }

        // Latin Extended-D ranges
        if ((sz_u32_t)(rune - 0xA722) <= 0x0C && is_even) { // Ꜣ-Ꜯ
            sz_assert_(sz_is_in_range_(rune, 0xA722, 0xA72E));
            folded[0] = rune + 1;
            return 1;
        }
        if ((sz_u32_t)(rune - 0xA732) <= 0x3C && is_even) { // Ꜳ-Ꝯ
            sz_assert_(sz_is_in_range_(rune, 0xA732, 0xA76E));
            folded[0] = rune + 1;
            return 1;
        }
        if ((sz_u32_t)(rune - 0xA77E) <= 0x08 && is_even) { // Ꝿ-Ꞇ
            sz_assert_(sz_is_in_range_(rune, 0xA77E, 0xA786));
            folded[0] = rune + 1;
            return 1;
        }
        if ((sz_u32_t)(rune - 0xA790) <= 0x02 && is_even) { // Ꞑ-Ꞓ
            sz_assert_(sz_is_in_range_(rune, 0xA790, 0xA792));
            folded[0] = rune + 1;
            return 1;
        }
        if ((sz_u32_t)(rune - 0xA796) <= 0x12 && is_even) { // Ꞗ-Ꞩ
            sz_assert_(sz_is_in_range_(rune, 0xA796, 0xA7A8));
            folded[0] = rune + 1;
            return 1;
        }
        if ((sz_u32_t)(rune - 0xA7B4) <= 0x0E && is_even) { // Ꞵ-Ꟃ
            sz_assert_(sz_is_in_range_(rune, 0xA7B4, 0xA7C2));
            folded[0] = rune + 1;
            return 1;
        }

        // Next let's handle the 3-byte irregular one-to-one mappings
        switch (rune) {
        // Georgian irregular
        case 0x10C7: folded[0] = 0x2D27; return 1; // 'Ⴧ' (U+10C7, E1 83 87) → 'ⴧ' (U+2D27, E2 B4 A7)
        case 0x10CD:
            folded[0] = 0x2D2D;
            return 1; // 'Ⴭ' (U+10CD, E1 83 8D) → 'ⴭ' (U+2D2D, E2 B4 AD)
        // Cyrillic Extended-C: Old Slavonic variant forms
        case 0x1C80: folded[0] = 0x0432; return 1; // 'ᲀ' (U+1C80, E1 B2 80) → 'в' (U+0432, D0 B2)
        case 0x1C81: folded[0] = 0x0434; return 1; // 'ᲁ' (U+1C81, E1 B2 81) → 'д' (U+0434, D0 B4)
        case 0x1C82: folded[0] = 0x043E; return 1; // 'ᲂ' (U+1C82, E1 B2 82) → 'о' (U+043E, D0 BE)
        case 0x1C83: folded[0] = 0x0441; return 1; // 'ᲃ' (U+1C83, E1 B2 83) → 'с' (U+0441, D1 81)
        case 0x1C84: folded[0] = 0x0442; return 1; // 'ᲄ' (U+1C84, E1 B2 84) → 'т' (U+0442, D1 82)
        case 0x1C85: folded[0] = 0x0442; return 1; // 'ᲅ' (U+1C85, E1 B2 85) → 'т' (U+0442, D1 82)
        case 0x1C86: folded[0] = 0x044A; return 1; // 'ᲆ' (U+1C86, E1 B2 86) → 'ъ' (U+044A, D1 8A)
        case 0x1C87: folded[0] = 0x0463; return 1; // 'ᲇ' (U+1C87, E1 B2 87) → 'ѣ' (U+0463, D1 A3)
        case 0x1C88: folded[0] = 0xA64B; return 1; // 'ᲈ' (U+1C88, E1 B2 88) → 'ꙋ' (U+A64B, EA 99 8B)
        case 0x1C89:
            folded[0] = 0x1C8A;
            return 1; // 'Ᲊ' (U+1C89, E1 B2 89) → 'ᲊ' (U+1C8A, E1 B2 8A)
        // Latin Extended Additional: long s with dot
        case 0x1E9B:
            folded[0] = 0x1E61;
            return 1; // 'ẛ' (U+1E9B, E1 BA 9B) → 'ṡ' (U+1E61, E1 B9 A1)
        // Greek Extended: vowels with breathing marks (irregular offsets)
        case 0x1F59: folded[0] = 0x1F51; return 1; // 'Ὑ' (U+1F59, E1 BD 99) → 'ὑ' (U+1F51, E1 BD 91)
        case 0x1F5B: folded[0] = 0x1F53; return 1; // 'Ὓ' (U+1F5B, E1 BD 9B) → 'ὓ' (U+1F53, E1 BD 93)
        case 0x1F5D: folded[0] = 0x1F55; return 1; // 'Ὕ' (U+1F5D, E1 BD 9D) → 'ὕ' (U+1F55, E1 BD 95)
        case 0x1F5F: folded[0] = 0x1F57; return 1; // 'Ὗ' (U+1F5F, E1 BD 9F) → 'ὗ' (U+1F57, E1 BD 97)
        case 0x1FB8: folded[0] = 0x1FB0; return 1; // 'Ᾰ' (U+1FB8, E1 BE B8) → 'ᾰ' (U+1FB0, E1 BE B0)
        case 0x1FB9: folded[0] = 0x1FB1; return 1; // 'Ᾱ' (U+1FB9, E1 BE B9) → 'ᾱ' (U+1FB1, E1 BE B1)
        case 0x1FBA: folded[0] = 0x1F70; return 1; // 'Ὰ' (U+1FBA, E1 BE BA) → 'ὰ' (U+1F70, E1 BD B0)
        case 0x1FBB: folded[0] = 0x1F71; return 1; // 'Ά' (U+1FBB, E1 BE BB) → 'ά' (U+1F71, E1 BD B1)
        case 0x1FBE: folded[0] = 0x03B9; return 1; // 'ι' (U+1FBE, E1 BE BE) → 'ι' (U+03B9, CE B9)
        case 0x1FD8: folded[0] = 0x1FD0; return 1; // 'Ῐ' (U+1FD8, E1 BF 98) → 'ῐ' (U+1FD0, E1 BF 90)
        case 0x1FD9: folded[0] = 0x1FD1; return 1; // 'Ῑ' (U+1FD9, E1 BF 99) → 'ῑ' (U+1FD1, E1 BF 91)
        case 0x1FDA: folded[0] = 0x1F76; return 1; // 'Ὶ' (U+1FDA, E1 BF 9A) → 'ὶ' (U+1F76, E1 BD B6)
        case 0x1FDB: folded[0] = 0x1F77; return 1; // 'Ί' (U+1FDB, E1 BF 9B) → 'ί' (U+1F77, E1 BD B7)
        case 0x1FE8: folded[0] = 0x1FE0; return 1; // 'Ῠ' (U+1FE8, E1 BF A8) → 'ῠ' (U+1FE0, E1 BF A0)
        case 0x1FE9: folded[0] = 0x1FE1; return 1; // 'Ῡ' (U+1FE9, E1 BF A9) → 'ῡ' (U+1FE1, E1 BF A1)
        case 0x1FEA: folded[0] = 0x1F7A; return 1; // 'Ὺ' (U+1FEA, E1 BF AA) → 'ὺ' (U+1F7A, E1 BD BA)
        case 0x1FEB: folded[0] = 0x1F7B; return 1; // 'Ύ' (U+1FEB, E1 BF AB) → 'ύ' (U+1F7B, E1 BD BB)
        case 0x1FEC: folded[0] = 0x1FE5; return 1; // 'Ῥ' (U+1FEC, E1 BF AC) → 'ῥ' (U+1FE5, E1 BF A5)
        case 0x1FF8: folded[0] = 0x1F78; return 1; // 'Ὸ' (U+1FF8, E1 BF B8) → 'ὸ' (U+1F78, E1 BD B8)
        case 0x1FF9: folded[0] = 0x1F79; return 1; // 'Ό' (U+1FF9, E1 BF B9) → 'ό' (U+1F79, E1 BD B9)
        case 0x1FFA: folded[0] = 0x1F7C; return 1; // 'Ὼ' (U+1FFA, E1 BF BA) → 'ὼ' (U+1F7C, E1 BD BC)
        case 0x1FFB:
            folded[0] = 0x1F7D;
            return 1; // 'Ώ' (U+1FFB, E1 BF BB) → 'ώ' (U+1F7D, E1 BD BD)
        // Letterlike Symbols: compatibility mappings
        case 0x2126: folded[0] = 0x03C9; return 1; // 'Ω' (U+2126, E2 84 A6) → 'ω' (U+03C9, CF 89)
        case 0x212A: folded[0] = 0x006B; return 1; // 'K' (U+212A, E2 84 AA) → 'k' (U+006B, 6B)
        case 0x212B: folded[0] = 0x00E5; return 1; // 'Å' (U+212B, E2 84 AB) → 'å' (U+00E5, C3 A5)
        case 0x2132: folded[0] = 0x214E; return 1; // 'Ⅎ' (U+2132, E2 84 B2) → 'ⅎ' (U+214E, E2 85 8E)
        case 0x2183:
            folded[0] = 0x2184;
            return 1; // 'Ↄ' (U+2183, E2 86 83) → 'ↄ' (U+2184, E2 86 84)
        // Latin Extended-C: irregular mappings to IPA/other blocks
        case 0x2C60: folded[0] = 0x2C61; return 1; // 'Ⱡ' (U+2C60, E2 B1 A0) → 'ⱡ' (U+2C61, E2 B1 A1)
        case 0x2C62: folded[0] = 0x026B; return 1; // 'Ɫ' (U+2C62, E2 B1 A2) → 'ɫ' (U+026B, C9 AB)
        case 0x2C63: folded[0] = 0x1D7D; return 1; // 'Ᵽ' (U+2C63, E2 B1 A3) → 'ᵽ' (U+1D7D, E1 B5 BD)
        case 0x2C64: folded[0] = 0x027D; return 1; // 'Ɽ' (U+2C64, E2 B1 A4) → 'ɽ' (U+027D, C9 BD)
        case 0x2C67: folded[0] = 0x2C68; return 1; // 'Ⱨ' (U+2C67, E2 B1 A7) → 'ⱨ' (U+2C68, E2 B1 A8)
        case 0x2C69: folded[0] = 0x2C6A; return 1; // 'Ⱪ' (U+2C69, E2 B1 A9) → 'ⱪ' (U+2C6A, E2 B1 AA)
        case 0x2C6B: folded[0] = 0x2C6C; return 1; // 'Ⱬ' (U+2C6B, E2 B1 AB) → 'ⱬ' (U+2C6C, E2 B1 AC)
        case 0x2C6D: folded[0] = 0x0251; return 1; // 'Ɑ' (U+2C6D, E2 B1 AD) → 'ɑ' (U+0251, C9 91)
        case 0x2C6E: folded[0] = 0x0271; return 1; // 'Ɱ' (U+2C6E, E2 B1 AE) → 'ɱ' (U+0271, C9 B1)
        case 0x2C6F: folded[0] = 0x0250; return 1; // 'Ɐ' (U+2C6F, E2 B1 AF) → 'ɐ' (U+0250, C9 90)
        case 0x2C70: folded[0] = 0x0252; return 1; // 'Ɒ' (U+2C70, E2 B1 B0) → 'ɒ' (U+0252, C9 92)
        case 0x2C72: folded[0] = 0x2C73; return 1; // 'Ⱳ' (U+2C72, E2 B1 B2) → 'ⱳ' (U+2C73, E2 B1 B3)
        case 0x2C75: folded[0] = 0x2C76; return 1; // 'Ⱶ' (U+2C75, E2 B1 B5) → 'ⱶ' (U+2C76, E2 B1 B6)
        case 0x2C7E: folded[0] = 0x023F; return 1; // 'Ȿ' (U+2C7E, E2 B1 BE) → 'ȿ' (U+023F, C8 BF)
        case 0x2C7F:
            folded[0] = 0x0240;
            return 1; // 'Ɀ' (U+2C7F, E2 B1 BF) → 'ɀ' (U+0240, C9 80)
        // Coptic: irregular cases outside even/odd range
        case 0x2CEB: folded[0] = 0x2CEC; return 1; // 'Ⳬ' (U+2CEB, E2 B3 AB) → 'ⳬ' (U+2CEC, E2 B3 AC)
        case 0x2CED: folded[0] = 0x2CEE; return 1; // 'Ⳮ' (U+2CED, E2 B3 AD) → 'ⳮ' (U+2CEE, E2 B3 AE)
        case 0x2CF2:
            folded[0] = 0x2CF3;
            return 1; // 'Ⳳ' (U+2CF2, E2 B3 B2) → 'ⳳ' (U+2CF3, E2 B3 B3)
        // Latin Extended-D: isolated irregulars
        case 0xA779: folded[0] = 0xA77A; return 1; // 'Ꝺ' (U+A779, EA 9D B9) → 'ꝺ' (U+A77A, EA 9D BA)
        case 0xA77B: folded[0] = 0xA77C; return 1; // 'Ꝼ' (U+A77B, EA 9D BB) → 'ꝼ' (U+A77C, EA 9D BC)
        case 0xA77D: folded[0] = 0x1D79; return 1; // 'Ᵹ' (U+A77D, EA 9D BD) → 'ᵹ' (U+1D79, E1 B5 B9)
        case 0xA78B: folded[0] = 0xA78C; return 1; // 'Ꞌ' (U+A78B, EA 9E 8B) → 'ꞌ' (U+A78C, EA 9E 8C)
        case 0xA78D: folded[0] = 0x0265; return 1; // 'Ɥ' (U+A78D, EA 9E 8D) → 'ɥ' (U+0265, C9 A5)
        case 0xA7AA: folded[0] = 0x0266; return 1; // 'Ɦ' (U+A7AA, EA 9E AA) → 'ɦ' (U+0266, C9 A6)
        case 0xA7AB: folded[0] = 0x025C; return 1; // 'Ɜ' (U+A7AB, EA 9E AB) → 'ɜ' (U+025C, C9 9C)
        case 0xA7AC: folded[0] = 0x0261; return 1; // 'Ɡ' (U+A7AC, EA 9E AC) → 'ɡ' (U+0261, C9 A1)
        case 0xA7AD: folded[0] = 0x026C; return 1; // 'Ɬ' (U+A7AD, EA 9E AD) → 'ɬ' (U+026C, C9 AC)
        case 0xA7AE: folded[0] = 0x026A; return 1; // 'Ɪ' (U+A7AE, EA 9E AE) → 'ɪ' (U+026A, C9 AA)
        case 0xA7B0: folded[0] = 0x029E; return 1; // 'Ʞ' (U+A7B0, EA 9E B0) → 'ʞ' (U+029E, CA 9E)
        case 0xA7B1: folded[0] = 0x0287; return 1; // 'Ʇ' (U+A7B1, EA 9E B1) → 'ʇ' (U+0287, CA 87)
        case 0xA7B2: folded[0] = 0x029D; return 1; // 'Ʝ' (U+A7B2, EA 9E B2) → 'ʝ' (U+029D, CA 9D)
        case 0xA7B3: folded[0] = 0xAB53; return 1; // 'Ꭓ' (U+A7B3, EA 9E B3) → 'ꭓ' (U+AB53, EA AD 93)
        case 0xA7C4: folded[0] = 0xA794; return 1; // 'Ꞔ' (U+A7C4, EA 9F 84) → 'ꞔ' (U+A794, EA 9E 94)
        case 0xA7C5: folded[0] = 0x0282; return 1; // 'Ʂ' (U+A7C5, EA 9F 85) → 'ʂ' (U+0282, CA 82)
        case 0xA7C6: folded[0] = 0x1D8E; return 1; // 'Ᶎ' (U+A7C6, EA 9F 86) → 'ᶎ' (U+1D8E, E1 B6 8E)
        case 0xA7C7: folded[0] = 0xA7C8; return 1; // 'Ꟈ' (U+A7C7, EA 9F 87) → 'ꟈ' (U+A7C8, EA 9F 88)
        case 0xA7C9: folded[0] = 0xA7CA; return 1; // 'Ꟊ' (U+A7C9, EA 9F 89) → 'ꟊ' (U+A7CA, EA 9F 8A)
        case 0xA7CB: folded[0] = 0x0264; return 1; // 'Ɤ' (U+A7CB, EA 9F 8B) → 'ɤ' (U+0264, C9 A4)
        case 0xA7CC: folded[0] = 0xA7CD; return 1; // 'Ꟍ' (U+A7CC, EA 9F 8C) → 'ꟍ' (U+A7CD, EA 9F 8D)
        case 0xA7CE: folded[0] = 0xA7CF; return 1; // '꟎' (U+A7CE, EA 9F 8E) → '꟏' (U+A7CF, EA 9F 8F)
        case 0xA7D0: folded[0] = 0xA7D1; return 1; // 'Ꟑ' (U+A7D0, EA 9F 90) → 'ꟑ' (U+A7D1, EA 9F 91)
        case 0xA7D2: folded[0] = 0xA7D3; return 1; // '꟒' (U+A7D2, EA 9F 92) → 'ꟓ' (U+A7D3, EA 9F 93)
        case 0xA7D4: folded[0] = 0xA7D5; return 1; // '꟔' (U+A7D4, EA 9F 94) → 'ꟕ' (U+A7D5, EA 9F 95)
        case 0xA7D6: folded[0] = 0xA7D7; return 1; // 'Ꟗ' (U+A7D6, EA 9F 96) → 'ꟗ' (U+A7D7, EA 9F 97)
        case 0xA7D8: folded[0] = 0xA7D9; return 1; // 'Ꟙ' (U+A7D8, EA 9F 98) → 'ꟙ' (U+A7D9, EA 9F 99)
        case 0xA7DA: folded[0] = 0xA7DB; return 1; // 'Ꟛ' (U+A7DA, EA 9F 9A) → 'ꟛ' (U+A7DB, EA 9F 9B)
        case 0xA7DC: folded[0] = 0x019B; return 1; // 'Ƛ' (U+A7DC, EA 9F 9C) → 'ƛ' (U+019B, C6 9B)
        case 0xA7F5: folded[0] = 0xA7F6; return 1; // 'Ꟶ' (U+A7F5, EA 9F B5) → 'ꟶ' (U+A7F6, EA 9F B6)
        }

        // Next let's handle the 3-byte one-to-many expansions
        switch (rune) {
        // Latin Extended Additional
        // 'ẖ' (U+1E96, E1 BA 96) → "ẖ" (U+0068 U+0331, 68 CC B1)
        case 0x1E96:
            folded[0] = 0x0068;
            folded[1] = 0x0331;
            return 2;
        // 'ẗ' (U+1E97, E1 BA 97) → "ẗ" (U+0074 U+0308, 74 CC 88)
        case 0x1E97:
            folded[0] = 0x0074;
            folded[1] = 0x0308;
            return 2;
        // 'ẘ' (U+1E98, E1 BA 98) → "ẘ" (U+0077 U+030A, 77 CC 8A)
        case 0x1E98:
            folded[0] = 0x0077;
            folded[1] = 0x030A;
            return 2;
        // 'ẙ' (U+1E99, E1 BA 99) → "ẙ" (U+0079 U+030A, 79 CC 8A)
        case 0x1E99:
            folded[0] = 0x0079;
            folded[1] = 0x030A;
            return 2;
        // 'ẚ' (U+1E9A, E1 BA 9A) → "aʾ" (U+0061 U+02BE, 61 CA BE)
        case 0x1E9A:
            folded[0] = 0x0061;
            folded[1] = 0x02BE;
            return 2;
        // 'ẞ' (U+1E9E, E1 BA 9E) → "ss" (U+0073 U+0073, 73 73)
        case 0x1E9E:
            folded[0] = 0x0073;
            folded[1] = 0x0073;
            return 2;
        // Greek Extended: breathing marks
        // 'ὐ' (U+1F50, E1 BD 90) → "ὐ" (U+03C5 U+0313, CF 85 CC 93)
        case 0x1F50:
            folded[0] = 0x03C5;
            folded[1] = 0x0313;
            return 2;
        // 'ὒ' (U+1F52, E1 BD 92) → "ὒ" (U+03C5 U+0313 U+0300, CF 85 CC 93 CC 80)
        case 0x1F52:
            folded[0] = 0x03C5;
            folded[1] = 0x0313;
            folded[2] = 0x0300;
            return 3;
        // 'ὔ' (U+1F54, E1 BD 94) → "ὔ" (U+03C5 U+0313 U+0301, CF 85 CC 93 CC 81)
        case 0x1F54:
            folded[0] = 0x03C5;
            folded[1] = 0x0313;
            folded[2] = 0x0301;
            return 3;
        // 'ὖ' (U+1F56, E1 BD 96) → "ὖ" (U+03C5 U+0313 U+0342, CF 85 CC 93 CD 82)
        case 0x1F56:
            folded[0] = 0x03C5;
            folded[1] = 0x0313;
            folded[2] = 0x0342;
            return 3;
        // Greek Extended: iota subscript combinations (0x1F80-0x1FAF)
        // 'ᾀ' (U+1F80, E1 BE 80) → "ἀι" (U+1F00 U+03B9, E1 BC 80 CE B9)
        case 0x1F80:
            folded[0] = 0x1F00;
            folded[1] = 0x03B9;
            return 2;
        // 'ᾁ' (U+1F81, E1 BE 81) → "ἁι" (U+1F01 U+03B9, E1 BC 81 CE B9)
        case 0x1F81:
            folded[0] = 0x1F01;
            folded[1] = 0x03B9;
            return 2;
        // 'ᾂ' (U+1F82, E1 BE 82) → "ἂι" (U+1F02 U+03B9, E1 BC 82 CE B9)
        case 0x1F82:
            folded[0] = 0x1F02;
            folded[1] = 0x03B9;
            return 2;
        // 'ᾃ' (U+1F83, E1 BE 83) → "ἃι" (U+1F03 U+03B9, E1 BC 83 CE B9)
        case 0x1F83:
            folded[0] = 0x1F03;
            folded[1] = 0x03B9;
            return 2;
        // 'ᾄ' (U+1F84, E1 BE 84) → "ἄι" (U+1F04 U+03B9, E1 BC 84 CE B9)
        case 0x1F84:
            folded[0] = 0x1F04;
            folded[1] = 0x03B9;
            return 2;
        // 'ᾅ' (U+1F85, E1 BE 85) → "ἅι" (U+1F05 U+03B9, E1 BC 85 CE B9)
        case 0x1F85:
            folded[0] = 0x1F05;
            folded[1] = 0x03B9;
            return 2;
        // 'ᾆ' (U+1F86, E1 BE 86) → "ἆι" (U+1F06 U+03B9, E1 BC 86 CE B9)
        case 0x1F86:
            folded[0] = 0x1F06;
            folded[1] = 0x03B9;
            return 2;
        // 'ᾇ' (U+1F87, E1 BE 87) → "ἇι" (U+1F07 U+03B9, E1 BC 87 CE B9)
        case 0x1F87:
            folded[0] = 0x1F07;
            folded[1] = 0x03B9;
            return 2;
        // 'ᾈ' (U+1F88, E1 BE 88) → "ἀι" (U+1F00 U+03B9, E1 BC 80 CE B9)
        case 0x1F88:
            folded[0] = 0x1F00;
            folded[1] = 0x03B9;
            return 2;
        // 'ᾉ' (U+1F89, E1 BE 89) → "ἁι" (U+1F01 U+03B9, E1 BC 81 CE B9)
        case 0x1F89:
            folded[0] = 0x1F01;
            folded[1] = 0x03B9;
            return 2;
        // 'ᾊ' (U+1F8A, E1 BE 8A) → "ἂι" (U+1F02 U+03B9, E1 BC 82 CE B9)
        case 0x1F8A:
            folded[0] = 0x1F02;
            folded[1] = 0x03B9;
            return 2;
        // 'ᾋ' (U+1F8B, E1 BE 8B) → "ἃι" (U+1F03 U+03B9, E1 BC 83 CE B9)
        case 0x1F8B:
            folded[0] = 0x1F03;
            folded[1] = 0x03B9;
            return 2;
        // 'ᾌ' (U+1F8C, E1 BE 8C) → "ἄι" (U+1F04 U+03B9, E1 BC 84 CE B9)
        case 0x1F8C:
            folded[0] = 0x1F04;
            folded[1] = 0x03B9;
            return 2;
        // 'ᾍ' (U+1F8D, E1 BE 8D) → "ἅι" (U+1F05 U+03B9, E1 BC 85 CE B9)
        case 0x1F8D:
            folded[0] = 0x1F05;
            folded[1] = 0x03B9;
            return 2;
        // 'ᾎ' (U+1F8E, E1 BE 8E) → "ἆι" (U+1F06 U+03B9, E1 BC 86 CE B9)
        case 0x1F8E:
            folded[0] = 0x1F06;
            folded[1] = 0x03B9;
            return 2;
        // 'ᾏ' (U+1F8F, E1 BE 8F) → "ἇι" (U+1F07 U+03B9, E1 BC 87 CE B9)
        case 0x1F8F:
            folded[0] = 0x1F07;
            folded[1] = 0x03B9;
            return 2;
        // 'ᾐ' (U+1F90, E1 BE 90) → "ἠι" (U+1F20 U+03B9, E1 BC A0 CE B9)
        case 0x1F90:
            folded[0] = 0x1F20;
            folded[1] = 0x03B9;
            return 2;
        // 'ᾑ' (U+1F91, E1 BE 91) → "ἡι" (U+1F21 U+03B9, E1 BC A1 CE B9)
        case 0x1F91:
            folded[0] = 0x1F21;
            folded[1] = 0x03B9;
            return 2;
        // 'ᾒ' (U+1F92, E1 BE 92) → "ἢι" (U+1F22 U+03B9, E1 BC A2 CE B9)
        case 0x1F92:
            folded[0] = 0x1F22;
            folded[1] = 0x03B9;
            return 2;
        // 'ᾓ' (U+1F93, E1 BE 93) → "ἣι" (U+1F23 U+03B9, E1 BC A3 CE B9)
        case 0x1F93:
            folded[0] = 0x1F23;
            folded[1] = 0x03B9;
            return 2;
        // 'ᾔ' (U+1F94, E1 BE 94) → "ἤι" (U+1F24 U+03B9, E1 BC A4 CE B9)
        case 0x1F94:
            folded[0] = 0x1F24;
            folded[1] = 0x03B9;
            return 2;
        // 'ᾕ' (U+1F95, E1 BE 95) → "ἥι" (U+1F25 U+03B9, E1 BC A5 CE B9)
        case 0x1F95:
            folded[0] = 0x1F25;
            folded[1] = 0x03B9;
            return 2;
        // 'ᾖ' (U+1F96, E1 BE 96) → "ἦι" (U+1F26 U+03B9, E1 BC A6 CE B9)
        case 0x1F96:
            folded[0] = 0x1F26;
            folded[1] = 0x03B9;
            return 2;
        // 'ᾗ' (U+1F97, E1 BE 97) → "ἧι" (U+1F27 U+03B9, E1 BC A7 CE B9)
        case 0x1F97:
            folded[0] = 0x1F27;
            folded[1] = 0x03B9;
            return 2;
        // 'ᾘ' (U+1F98, E1 BE 98) → "ἠι" (U+1F20 U+03B9, E1 BC A0 CE B9)
        case 0x1F98:
            folded[0] = 0x1F20;
            folded[1] = 0x03B9;
            return 2;
        // 'ᾙ' (U+1F99, E1 BE 99) → "ἡι" (U+1F21 U+03B9, E1 BC A1 CE B9)
        case 0x1F99:
            folded[0] = 0x1F21;
            folded[1] = 0x03B9;
            return 2;
        // 'ᾚ' (U+1F9A, E1 BE 9A) → "ἢι" (U+1F22 U+03B9, E1 BC A2 CE B9)
        case 0x1F9A:
            folded[0] = 0x1F22;
            folded[1] = 0x03B9;
            return 2;
        // 'ᾛ' (U+1F9B, E1 BE 9B) → "ἣι" (U+1F23 U+03B9, E1 BC A3 CE B9)
        case 0x1F9B:
            folded[0] = 0x1F23;
            folded[1] = 0x03B9;
            return 2;
        // 'ᾜ' (U+1F9C, E1 BE 9C) → "ἤι" (U+1F24 U+03B9, E1 BC A4 CE B9)
        case 0x1F9C:
            folded[0] = 0x1F24;
            folded[1] = 0x03B9;
            return 2;
        // 'ᾝ' (U+1F9D, E1 BE 9D) → "ἥι" (U+1F25 U+03B9, E1 BC A5 CE B9)
        case 0x1F9D:
            folded[0] = 0x1F25;
            folded[1] = 0x03B9;
            return 2;
        // 'ᾞ' (U+1F9E, E1 BE 9E) → "ἦι" (U+1F26 U+03B9, E1 BC A6 CE B9)
        case 0x1F9E:
            folded[0] = 0x1F26;
            folded[1] = 0x03B9;
            return 2;
        // 'ᾟ' (U+1F9F, E1 BE 9F) → "ἧι" (U+1F27 U+03B9, E1 BC A7 CE B9)
        case 0x1F9F:
            folded[0] = 0x1F27;
            folded[1] = 0x03B9;
            return 2;
        // 'ᾠ' (U+1FA0, E1 BE A0) → "ὠι" (U+1F60 U+03B9, E1 BD A0 CE B9)
        case 0x1FA0:
            folded[0] = 0x1F60;
            folded[1] = 0x03B9;
            return 2;
        // 'ᾡ' (U+1FA1, E1 BE A1) → "ὡι" (U+1F61 U+03B9, E1 BD A1 CE B9)
        case 0x1FA1:
            folded[0] = 0x1F61;
            folded[1] = 0x03B9;
            return 2;
        // 'ᾢ' (U+1FA2, E1 BE A2) → "ὢι" (U+1F62 U+03B9, E1 BD A2 CE B9)
        case 0x1FA2:
            folded[0] = 0x1F62;
            folded[1] = 0x03B9;
            return 2;
        // 'ᾣ' (U+1FA3, E1 BE A3) → "ὣι" (U+1F63 U+03B9, E1 BD A3 CE B9)
        case 0x1FA3:
            folded[0] = 0x1F63;
            folded[1] = 0x03B9;
            return 2;
        // 'ᾤ' (U+1FA4, E1 BE A4) → "ὤι" (U+1F64 U+03B9, E1 BD A4 CE B9)
        case 0x1FA4:
            folded[0] = 0x1F64;
            folded[1] = 0x03B9;
            return 2;
        // 'ᾥ' (U+1FA5, E1 BE A5) → "ὥι" (U+1F65 U+03B9, E1 BD A5 CE B9)
        case 0x1FA5:
            folded[0] = 0x1F65;
            folded[1] = 0x03B9;
            return 2;
        // 'ᾦ' (U+1FA6, E1 BE A6) → "ὦι" (U+1F66 U+03B9, E1 BD A6 CE B9)
        case 0x1FA6:
            folded[0] = 0x1F66;
            folded[1] = 0x03B9;
            return 2;
        // 'ᾧ' (U+1FA7, E1 BE A7) → "ὧι" (U+1F67 U+03B9, E1 BD A7 CE B9)
        case 0x1FA7:
            folded[0] = 0x1F67;
            folded[1] = 0x03B9;
            return 2;
        // 'ᾨ' (U+1FA8, E1 BE A8) → "ὠι" (U+1F60 U+03B9, E1 BD A0 CE B9)
        case 0x1FA8:
            folded[0] = 0x1F60;
            folded[1] = 0x03B9;
            return 2;
        // 'ᾩ' (U+1FA9, E1 BE A9) → "ὡι" (U+1F61 U+03B9, E1 BD A1 CE B9)
        case 0x1FA9:
            folded[0] = 0x1F61;
            folded[1] = 0x03B9;
            return 2;
        // 'ᾪ' (U+1FAA, E1 BE AA) → "ὢι" (U+1F62 U+03B9, E1 BD A2 CE B9)
        case 0x1FAA:
            folded[0] = 0x1F62;
            folded[1] = 0x03B9;
            return 2;
        // 'ᾫ' (U+1FAB, E1 BE AB) → "ὣι" (U+1F63 U+03B9, E1 BD A3 CE B9)
        case 0x1FAB:
            folded[0] = 0x1F63;
            folded[1] = 0x03B9;
            return 2;
        // 'ᾬ' (U+1FAC, E1 BE AC) → "ὤι" (U+1F64 U+03B9, E1 BD A4 CE B9)
        case 0x1FAC:
            folded[0] = 0x1F64;
            folded[1] = 0x03B9;
            return 2;
        // 'ᾭ' (U+1FAD, E1 BE AD) → "ὥι" (U+1F65 U+03B9, E1 BD A5 CE B9)
        case 0x1FAD:
            folded[0] = 0x1F65;
            folded[1] = 0x03B9;
            return 2;
        // 'ᾮ' (U+1FAE, E1 BE AE) → "ὦι" (U+1F66 U+03B9, E1 BD A6 CE B9)
        case 0x1FAE:
            folded[0] = 0x1F66;
            folded[1] = 0x03B9;
            return 2;
        // 'ᾯ' (U+1FAF, E1 BE AF) → "ὧι" (U+1F67 U+03B9, E1 BD A7 CE B9)
        case 0x1FAF:
            folded[0] = 0x1F67;
            folded[1] = 0x03B9;
            return 2;
        // Greek Extended: vowel + iota subscript (0x1FB2-0x1FFC)
        // 'ᾲ' (U+1FB2, E1 BE B2) → "ὰι" (U+1F70 U+03B9, E1 BD B0 CE B9)
        case 0x1FB2:
            folded[0] = 0x1F70;
            folded[1] = 0x03B9;
            return 2;
        // 'ᾳ' (U+1FB3, E1 BE B3) → "αι" (U+03B1 U+03B9, CE B1 CE B9)
        case 0x1FB3:
            folded[0] = 0x03B1;
            folded[1] = 0x03B9;
            return 2;
        // 'ᾴ' (U+1FB4, E1 BE B4) → "άι" (U+03AC U+03B9, CE AC CE B9)
        case 0x1FB4:
            folded[0] = 0x03AC;
            folded[1] = 0x03B9;
            return 2;
        // 'ᾶ' (U+1FB6, E1 BE B6) → "ᾶ" (U+03B1 U+0342, CE B1 CD 82)
        case 0x1FB6:
            folded[0] = 0x03B1;
            folded[1] = 0x0342;
            return 2;
        // 'ᾷ' (U+1FB7, E1 BE B7) → "ᾶι" (U+03B1 U+0342 U+03B9, CE B1 CD 82 CE B9)
        case 0x1FB7:
            folded[0] = 0x03B1;
            folded[1] = 0x0342;
            folded[2] = 0x03B9;
            return 3;
        // 'ᾼ' (U+1FBC, E1 BE BC) → "αι" (U+03B1 U+03B9, CE B1 CE B9)
        case 0x1FBC:
            folded[0] = 0x03B1;
            folded[1] = 0x03B9;
            return 2;
        // 'ῂ' (U+1FC2, E1 BF 82) → "ὴι" (U+1F74 U+03B9, E1 BD B4 CE B9)
        case 0x1FC2:
            folded[0] = 0x1F74;
            folded[1] = 0x03B9;
            return 2;
        // 'ῃ' (U+1FC3, E1 BF 83) → "ηι" (U+03B7 U+03B9, CE B7 CE B9)
        case 0x1FC3:
            folded[0] = 0x03B7;
            folded[1] = 0x03B9;
            return 2;
        // 'ῄ' (U+1FC4, E1 BF 84) → "ήι" (U+03AE U+03B9, CE AE CE B9)
        case 0x1FC4:
            folded[0] = 0x03AE;
            folded[1] = 0x03B9;
            return 2;
        // 'ῆ' (U+1FC6, E1 BF 86) → "ῆ" (U+03B7 U+0342, CE B7 CD 82)
        case 0x1FC6:
            folded[0] = 0x03B7;
            folded[1] = 0x0342;
            return 2;
        // 'ῇ' (U+1FC7, E1 BF 87) → "ῆι" (U+03B7 U+0342 U+03B9, CE B7 CD 82 CE B9)
        case 0x1FC7:
            folded[0] = 0x03B7;
            folded[1] = 0x0342;
            folded[2] = 0x03B9;
            return 3;
        // 'ῌ' (U+1FCC, E1 BF 8C) → "ηι" (U+03B7 U+03B9, CE B7 CE B9)
        case 0x1FCC:
            folded[0] = 0x03B7;
            folded[1] = 0x03B9;
            return 2;
        // 'ῒ' (U+1FD2, E1 BF 92) → "ῒ" (U+03B9 U+0308 U+0300, CE B9 CC 88 CC 80)
        case 0x1FD2:
            folded[0] = 0x03B9;
            folded[1] = 0x0308;
            folded[2] = 0x0300;
            return 3;
        // 'ΐ' (U+1FD3, E1 BF 93) → "ΐ" (U+03B9 U+0308 U+0301, CE B9 CC 88 CC 81)
        case 0x1FD3:
            folded[0] = 0x03B9;
            folded[1] = 0x0308;
            folded[2] = 0x0301;
            return 3;
        // 'ῖ' (U+1FD6, E1 BF 96) → "ῖ" (U+03B9 U+0342, CE B9 CD 82)
        case 0x1FD6:
            folded[0] = 0x03B9;
            folded[1] = 0x0342;
            return 2;
        // 'ῗ' (U+1FD7, E1 BF 97) → "ῗ" (U+03B9 U+0308 U+0342, CE B9 CC 88 CD 82)
        case 0x1FD7:
            folded[0] = 0x03B9;
            folded[1] = 0x0308;
            folded[2] = 0x0342;
            return 3;
        // 'ῢ' (U+1FE2, E1 BF A2) → "ῢ" (U+03C5 U+0308 U+0300, CF 85 CC 88 CC 80)
        case 0x1FE2:
            folded[0] = 0x03C5;
            folded[1] = 0x0308;
            folded[2] = 0x0300;
            return 3;
        // 'ΰ' (U+1FE3, E1 BF A3) → "ΰ" (U+03C5 U+0308 U+0301, CF 85 CC 88 CC 81)
        case 0x1FE3:
            folded[0] = 0x03C5;
            folded[1] = 0x0308;
            folded[2] = 0x0301;
            return 3;
        // 'ῤ' (U+1FE4, E1 BF A4) → "ῤ" (U+03C1 U+0313, CF 81 CC 93)
        case 0x1FE4:
            folded[0] = 0x03C1;
            folded[1] = 0x0313;
            return 2;
        // 'ῦ' (U+1FE6, E1 BF A6) → "ῦ" (U+03C5 U+0342, CF 85 CD 82)
        case 0x1FE6:
            folded[0] = 0x03C5;
            folded[1] = 0x0342;
            return 2;
        // 'ῧ' (U+1FE7, E1 BF A7) → "ῧ" (U+03C5 U+0308 U+0342, CF 85 CC 88 CD 82)
        case 0x1FE7:
            folded[0] = 0x03C5;
            folded[1] = 0x0308;
            folded[2] = 0x0342;
            return 3;
        // 'ῲ' (U+1FF2, E1 BF B2) → "ὼι" (U+1F7C U+03B9, E1 BD BC CE B9)
        case 0x1FF2:
            folded[0] = 0x1F7C;
            folded[1] = 0x03B9;
            return 2;
        // 'ῳ' (U+1FF3, E1 BF B3) → "ωι" (U+03C9 U+03B9, CF 89 CE B9)
        case 0x1FF3:
            folded[0] = 0x03C9;
            folded[1] = 0x03B9;
            return 2;
        // 'ῴ' (U+1FF4, E1 BF B4) → "ώι" (U+03CE U+03B9, CF 8E CE B9)
        case 0x1FF4:
            folded[0] = 0x03CE;
            folded[1] = 0x03B9;
            return 2;
        // 'ῶ' (U+1FF6, E1 BF B6) → "ῶ" (U+03C9 U+0342, CF 89 CD 82)
        case 0x1FF6:
            folded[0] = 0x03C9;
            folded[1] = 0x0342;
            return 2;
        // 'ῷ' (U+1FF7, E1 BF B7) → "ῶι" (U+03C9 U+0342 U+03B9, CF 89 CD 82 CE B9)
        case 0x1FF7:
            folded[0] = 0x03C9;
            folded[1] = 0x0342;
            folded[2] = 0x03B9;
            return 3;
        // 'ῼ' (U+1FFC, E1 BF BC) → "ωι" (U+03C9 U+03B9, CF 89 CE B9)
        case 0x1FFC:
            folded[0] = 0x03C9;
            folded[1] = 0x03B9;
            return 2;
        // Alphabetic Presentation Forms: ligatures
        // 'ﬀ' (U+FB00, EF AC 80) → "ff" (U+0066 U+0066, 66 66)
        case 0xFB00:
            folded[0] = 0x0066;
            folded[1] = 0x0066;
            return 2;
        // 'ﬁ' (U+FB01, EF AC 81) → "fi" (U+0066 U+0069, 66 69)
        case 0xFB01:
            folded[0] = 0x0066;
            folded[1] = 0x0069;
            return 2;
        // 'ﬂ' (U+FB02, EF AC 82) → "fl" (U+0066 U+006C, 66 6C)
        case 0xFB02:
            folded[0] = 0x0066;
            folded[1] = 0x006C;
            return 2;
        // 'ﬃ' (U+FB03, EF AC 83) → "ffi" (U+0066 U+0066 U+0069, 66 66 69)
        case 0xFB03:
            folded[0] = 0x0066;
            folded[1] = 0x0066;
            folded[2] = 0x0069;
            return 3;
        // 'ﬄ' (U+FB04, EF AC 84) → "ffl" (U+0066 U+0066 U+006C, 66 66 6C)
        case 0xFB04:
            folded[0] = 0x0066;
            folded[1] = 0x0066;
            folded[2] = 0x006C;
            return 3;
        // 'ﬅ' (U+FB05, EF AC 85) → "st" (U+0073 U+0074, 73 74)
        case 0xFB05:
            folded[0] = 0x0073;
            folded[1] = 0x0074;
            return 2;
        // 'ﬆ' (U+FB06, EF AC 86) → "st" (U+0073 U+0074, 73 74)
        case 0xFB06:
            folded[0] = 0x0073;
            folded[1] = 0x0074;
            return 2;
        // Armenian ligatures
        // 'ﬓ' (U+FB13, EF AC 93) → "մն" (U+0574 U+0576, D5 B4 D5 B6)
        case 0xFB13:
            folded[0] = 0x0574;
            folded[1] = 0x0576;
            return 2;
        // 'ﬔ' (U+FB14, EF AC 94) → "մե" (U+0574 U+0565, D5 B4 D5 A5)
        case 0xFB14:
            folded[0] = 0x0574;
            folded[1] = 0x0565;
            return 2;
        // 'ﬕ' (U+FB15, EF AC 95) → "մի" (U+0574 U+056B, D5 B4 D5 AB)
        case 0xFB15:
            folded[0] = 0x0574;
            folded[1] = 0x056B;
            return 2;
        // 'ﬖ' (U+FB16, EF AC 96) → "վն" (U+057E U+0576, D5 BE D5 B6)
        case 0xFB16:
            folded[0] = 0x057E;
            folded[1] = 0x0576;
            return 2;
        // 'ﬗ' (U+FB17, EF AC 97) → "մխ" (U+0574 U+056D, D5 B4 D5 AD)
        case 0xFB17:
            folded[0] = 0x0574;
            folded[1] = 0x056D;
            return 2;
        }

        folded[0] = rune;
        return 1; // 3-byte: no folding needed
    }

    // 4-byte UTF-8 (U+10000-10FFFF): Deseret, Osage, Vithkuqi, etc.

    // Deseret 𐐀-𐐧: 0x10400-0x10427 (+40)
    if ((sz_u32_t)(rune - 0x10400) <= 0x27) {
        sz_assert_(sz_is_in_range_(rune, 0x10400, 0x10427));
        folded[0] = rune + 0x28;
        return 1;
    }

    // Osage 𐒰-𐓓: 0x104B0-0x104D3 (+40)
    if ((sz_u32_t)(rune - 0x104B0) <= 0x23) {
        sz_assert_(sz_is_in_range_(rune, 0x104B0, 0x104D3));
        folded[0] = rune + 0x28;
        return 1;
    }

    // Vithkuqi: 3 ranges with gaps, all +39
    if ((sz_u32_t)(rune - 0x10570) <= 0x0A) { // 0x10570-0x1057A
        sz_assert_(sz_is_in_range_(rune, 0x10570, 0x1057A));
        folded[0] = rune + 0x27;
        return 1;
    }
    if ((sz_u32_t)(rune - 0x1057C) <= 0x0E) { // 0x1057C-0x1058A
        sz_assert_(sz_is_in_range_(rune, 0x1057C, 0x1058A));
        folded[0] = rune + 0x27;
        return 1;
    }
    if ((sz_u32_t)(rune - 0x1058C) <= 0x06) { // 0x1058C-0x10592
        sz_assert_(sz_is_in_range_(rune, 0x1058C, 0x10592));
        folded[0] = rune + 0x27;
        return 1;
    }

    // Old Hungarian: 0x10C80-0x10CB2 (+64)
    if ((sz_u32_t)(rune - 0x10C80) <= 0x32) {
        sz_assert_(sz_is_in_range_(rune, 0x10C80, 0x10CB2));
        folded[0] = rune + 0x40;
        return 1;
    }

    // Garay: 0x10D50-0x10D65 (+32)
    if ((sz_u32_t)(rune - 0x10D50) <= 0x15) {
        sz_assert_(sz_is_in_range_(rune, 0x10D50, 0x10D65));
        folded[0] = rune + 0x20;
        return 1;
    }

    // Warang Citi: 0x118A0-0x118BF (+32)
    if ((sz_u32_t)(rune - 0x118A0) <= 0x1F) {
        sz_assert_(sz_is_in_range_(rune, 0x118A0, 0x118BF));
        folded[0] = rune + 0x20;
        return 1;
    }

    // Medefaidrin: 0x16E40-0x16E5F (+32)
    if ((sz_u32_t)(rune - 0x16E40) <= 0x1F) {
        sz_assert_(sz_is_in_range_(rune, 0x16E40, 0x16E5F));
        folded[0] = rune + 0x20;
        return 1;
    }

    // Beria Erfe: 0x16EA0-0x16EB8 (+27)
    if ((sz_u32_t)(rune - 0x16EA0) <= 0x18) {
        sz_assert_(sz_is_in_range_(rune, 0x16EA0, 0x16EB8));
        folded[0] = rune + 0x1B;
        return 1;
    }

    // Adlam: 0x1E900-0x1E921 (+34)
    if ((sz_u32_t)(rune - 0x1E900) <= 0x21) {
        sz_assert_(sz_is_in_range_(rune, 0x1E900, 0x1E921));
        folded[0] = rune + 0x22;
        return 1;
    }

    // Next let's handle the 4-byte irregular mappings
    switch (rune) {
    // Vithkuqi: Albanian historical script
    case 0x10594: folded[0] = 0x105BB; return 1; // '𐖔' (U+010594, F0 90 96 94) → '𐖻' (U+0105BB, F0 90 96 BB)
    case 0x10595: folded[0] = 0x105BC; return 1; // '𐖕' (U+010595, F0 90 96 95) → '𐖼' (U+0105BC, F0 90 96 BC)
    }

    folded[0] = rune;
    return 1; // No folding needed
}

/**
 *  @brief Helper function performing case-folding under the constraint, that no output may be incomplete.
 *
 *  @param source Pointer to the source UTF-8 data, must be valid UTF-8.
 *  @param source_length Length of the source data in bytes.
 *  @param destination Pointer to the destination buffer.
 *  @param destination_length Length of the destination buffer in bytes.
 *  @param codepoints_consumed Number of codepoints read from source.
 *  @param codepoints_exported Number of codepoints written to destination.
 *  @param bytes_consumed Number of bytes read from source.
 *  @param bytes_exported Number of bytes written to destination.
 */



/**
 *  @brief Iterator state for streaming through folded UTF-8 runes.
 *  Handles one-to-many case folding expansions (e.g., 'ß' (U+00DF, C3 9F) → "ss" (U+0073 U+0073, 73 73)) transparently.
 */
typedef struct {
    sz_cptr_t ptr;           // Current position in UTF-8 string
    sz_cptr_t end;           // End of string
    sz_rune_t pending[4];    // Buffered folded runes from one-to-many expansions
    sz_size_t pending_count; // Number of pending folded runes
    sz_size_t pending_idx;   // Current index into pending buffer
} sz_utf8_folded_iter_t;

/** @brief Initialize a folded rune iterator. */
SZ_HELPER_AUTO void sz_utf8_folded_iter_init_(sz_utf8_folded_iter_t *iterator, sz_cptr_t string, sz_size_t length) {
    iterator->ptr = string;
    iterator->end = string + length;
    iterator->pending_count = 0;
    iterator->pending_idx = 0;
}

/**
 *  @brief Get next folded rune. Returns `sz_false_k` when exhausted.
 *  Malformed UTF-8 is handled losslessly: a byte that does not begin a well-formed codepoint is emitted as a
 *  single literal byte (tagged so it compares byte-for-byte and never collides with a real folded codepoint) and
 *  the iterator resyncs by one byte, never reading past `end`.
 */
SZ_HELPER_AUTO sz_bool_t sz_utf8_folded_iter_next_(sz_utf8_folded_iter_t *it, sz_rune_t *out_rune) {
    // Refill pending buffer if exhausted
    if (it->pending_idx >= it->pending_count) {
        if (it->ptr >= it->end) return sz_false_k;

        // ASCII fast-path: fold inline without buffering
        sz_u8_t lead = *(sz_u8_t const *)it->ptr;
        if (lead < 0x80) {
            *out_rune = sz_ascii_fold_(lead);
            it->ptr++;
            it->pending_count = 0; // Clear pending buffer
            it->pending_idx = 0;   // Signal first rune of new codepoint for source tracking
            return sz_true_k;
        }

        // Multi-byte UTF-8: decode (bounds-checked), fold, and buffer. A byte that does not begin a
        // well-formed codepoint folds to itself (>= 0x80 bytes are unchanged by `sz_ascii_fold_`) and resyncs
        // by one byte, never over-reading past `end`.
        sz_rune_t rune;
        sz_rune_length_t const rune_length = sz_rune_decode(it->ptr, it->end, &rune);
        if (rune_length == sz_rune_invalid_k) {
            *out_rune = sz_rune_malformed_byte_(lead);
            it->ptr++;
            it->pending_count = 0;
            it->pending_idx = 0;
            return sz_true_k;
        }

        it->ptr += rune_length;
        // Pre-fill pending buffer with sentinel values to prevent stale data from causing false matches.
        // The fold function will overwrite positions it uses; unused positions keep the sentinel.
        // This follows the same pattern as sz_utf8_uncased_search_2folded_serial_ and
        // sz_utf8_uncased_search_3folded_serial_.
        it->pending[0] = 0xFFFFFFFFu;
        it->pending[1] = 0xFFFFFFFEu;
        it->pending[2] = 0xFFFFFFFDu;
        it->pending[3] = 0xFFFFFFFCu;
        it->pending_count = sz_unicode_fold_codepoint_(rune, it->pending);
        it->pending_idx = 0;
    }

    *out_rune = it->pending[it->pending_idx++];
    return sz_true_k;
}

/**
 *  @brief Reverse iterator state for streaming through folded UTF-8 runes backwards.
 * Handles one-to-many case folding expansions (e.g., 'ß' (U+00DF, C3 9F) → "ss" (U+0073 U+0073, 73 73)) transparently
 * in reverse order.
 */
typedef struct {
    sz_cptr_t ptr;           // Current position (points to byte AFTER current sequence)
    sz_cptr_t start;         // Start of string (stop when ptr reaches this)
    sz_rune_t pending[4];    // Buffered folded runes from one-to-many expansions (in reverse order)
    sz_size_t pending_count; // Number of pending folded runes
    sz_size_t pending_idx;   // Current index into pending buffer
} sz_utf8_folded_reverse_iter_t;

/** @brief Initialize a reverse folded rune iterator. Iterates from end towards start. */
SZ_HELPER_AUTO void sz_utf8_folded_reverse_iter_init_(sz_utf8_folded_reverse_iter_t *it, sz_cptr_t start,
                                                      sz_cptr_t end) {
    it->ptr = end;
    it->start = start;
    it->pending_count = 0;
    it->pending_idx = 0;
}

/**
 *  @brief Get previous folded rune (walking backwards). Returns `sz_false_k` when exhausted.
 * When a codepoint folds to multiple runes (like 'ß' (U+00DF, C3 9F) → "ss" (U+0073 U+0073, 73 73)), returns them in
 * reverse order ('s', then 's'). Malformed UTF-8 is handled losslessly and byte-identically to the forward
 * iterator: a byte that does not begin/end a well-formed codepoint is emitted as a single tagged literal byte and
 * the iterator resyncs by one byte, so the backward rune stream is exactly the reverse of the forward stream.
 */
SZ_HELPER_AUTO sz_bool_t sz_utf8_folded_reverse_iter_prev_(sz_utf8_folded_reverse_iter_t *it, sz_rune_t *out_rune) {
    // Return pending runes if any (stored in reverse order, consumed in reverse)
    if (it->pending_idx < it->pending_count) {
        *out_rune = it->pending[it->pending_count - 1 - it->pending_idx];
        it->pending_idx++;
        return sz_true_k;
    }

    // Refill: find previous codepoint
    if (it->ptr <= it->start) return sz_false_k;

    // Remember one-past-the-end of the sequence we are about to decode, so the strict decode is bounded
    // and a malformed run resyncs one byte at a time - mirroring the forward iterator byte-for-byte.
    sz_cptr_t const sequence_end = it->ptr;

    // The byte immediately before `sequence_end` is the last byte of whatever codepoint ends here.
    sz_u8_t const last_byte = *(sz_u8_t const *)(sequence_end - 1);

    // ASCII fast-path: a byte < 0x80 is always its own complete 1-byte codepoint.
    if (last_byte < 0x80) {
        it->ptr = sequence_end - 1;
        *out_rune = sz_ascii_fold_(last_byte);
        it->pending_count = 0;
        it->pending_idx = 0;
        return sz_true_k;
    }

    // Otherwise walk backwards over up to 3 continuation bytes (0x80-0xBF) to locate a candidate lead.
    // A well-formed multi-byte rune is at most 4 bytes, so stop after considering 4 positions.
    sz_cptr_t candidate = sequence_end - 1;
    for (sz_size_t back = 0; back < 3 && candidate > it->start && (*(sz_u8_t const *)candidate & 0xC0) == 0x80; ++back)
        candidate--;

    // Multi-byte UTF-8: decode (bounded) and fold only if the bytes from the candidate lead form a well-formed
    // codepoint that ends EXACTLY at `sequence_end`. Otherwise the last byte does not begin/end a valid rune, so
    // treat it as a literal folded-to-itself byte and resync by one - matching the forward iterator byte-for-byte.
    sz_rune_t rune;
    sz_rune_length_t const rune_length = sz_rune_decode(candidate, sequence_end, &rune);
    if (rune_length == sz_rune_invalid_k || candidate + rune_length != sequence_end) {
        it->ptr = sequence_end - 1;
        *out_rune = sz_rune_malformed_byte_(last_byte);
        it->pending_count = 0;
        it->pending_idx = 0;
        return sz_true_k;
    }
    it->ptr = candidate;

    // Store folded runes in pending buffer
    it->pending[0] = 0xFFFFFFFFu;
    it->pending[1] = 0xFFFFFFFEu;
    it->pending[2] = 0xFFFFFFFDu;
    it->pending[3] = 0xFFFFFFFCu;
    it->pending_count = sz_unicode_fold_codepoint_(rune, it->pending);
    it->pending_idx = 1; // We'll return the last one now, then the rest in subsequent calls

    // Return the LAST folded rune first (since we're going backwards)
    *out_rune = it->pending[it->pending_count - 1];
    return sz_true_k;
}


/**
 *  @brief Internal helper: checks if a single Unicode codepoint is case-agnostic.
 *
 *  A codepoint is case-agnostic if ALL of the following are true:
 *  1. It folds to exactly itself (no transformation, no expansion)
 *  2. It does NOT belong to any bicameral (cased) script
 *  3. It does NOT appear in any case fold expansion as a target character
 *
 *  The third condition is critical. Consider 'ʾ' (U+02BE, CA BE):
 *  - It has no case variant and folds to itself
 *  - However, 'ẚ' (U+1E9A, E1 BA 9A) → "aʾ" (U+0061 U+02BE, 61 CA BE)
 *  - A needle containing 'ʾ' must match at position 1 of the folded expansion of 'ẚ'
 *  - Binary search cannot handle this - it only sees 'ẚ' as a 3-byte sequence (E1 BA 9A)
 *  - Therefore 'ʾ' must NOT be treated as case-agnostic
 *
 *  This function implements the check via explicit range exclusions for all bicameral
 *  scripts and all Unicode blocks containing case fold expansion target characters.
 *
 *  @param rune Unicode codepoint to check.
 *  @return sz_true_k if the codepoint is case-agnostic, sz_false_k otherwise.
 *
 *  @warning This is an internal function. Use sz_utf8_find_cased_serial() for string checking.
 *  @see sz_utf8_find_cased_serial
 *  @see sz_unicode_fold_codepoint_
 */
SZ_HELPER_AUTO sz_bool_t sz_rune_is_uncased_(sz_rune_t rune) {

    // Check if this rune participates in case folding
    sz_rune_t folded_runes[3];
    sz_size_t folded_count = sz_unicode_fold_codepoint_(rune, folded_runes);

    // If it expands or changes, it's not caseless
    if (folded_count != 1 || folded_runes[0] != rune) return sz_false_k;

    // Check if this rune is a lowercase target of some uppercase letter.
    // Lowercase letters that don't change when folded still participate in case
    // because uppercase versions fold TO them. We must mark entire bicameral
    // script ranges as "not caseless" to enable proper uncased matching.
    //
    // Important: Combining diacritical marks (U+0300-U+036F) can appear as non-first
    // runes in multi-rune case fold expansions. Example: ǰ (U+01F0) → j + ̌ (U+030C).
    // A needle starting with combining caron could match inside such an expansion,
    // so combining marks must NOT be treated as case-agnostic.
    //
    // Bicameral scripts organized by UTF-8 lead byte for efficient checking:
    //
    // 1-byte sequences with upper and lower case (U+0000-007F): 00-7F
    if (rune >= 0x0041 && rune <= 0x005A) return sz_false_k; // Basic Latin (A-Z)
    if (rune >= 0x0061 && rune <= 0x007A) return sz_false_k; // Basic Latin (a-z)
    //
    // 2-byte sequences (U+0080-07FF): C2-DF lead bytes
    if (rune >= 0x00C0 && rune <= 0x00FF) return sz_false_k; // Latin-1 Supplement (À-ÿ)
    if (rune >= 0x0100 && rune <= 0x024F) return sz_false_k; // Latin Extended-A/B
    if (rune >= 0x0250 && rune <= 0x02AF) return sz_false_k; // IPA Extensions
    if (rune >= 0x02B0 && rune <= 0x02FF) return sz_false_k; // Spacing Modifier Letters (ʾ U+02BE appears in ẚ→aʾ)
    if (rune >= 0x0300 && rune <= 0x036F) return sz_false_k; // Combining Diacritical Marks (can appear in expansions!)
    if (rune >= 0x0370 && rune <= 0x03FF) return sz_false_k; // Greek and Coptic
    if (rune >= 0x0400 && rune <= 0x04FF) return sz_false_k; // Cyrillic
    if (rune >= 0x0500 && rune <= 0x052F) return sz_false_k; // Cyrillic Supplement
    if (rune >= 0x0531 && rune <= 0x0587) return sz_false_k; // Armenian (uppercase + lowercase + ligature)
    //
    // 3-byte sequences (U+0800-FFFF): E0-EF lead bytes
    if (rune >= 0x10A0 && rune <= 0x10FF) return sz_false_k; // Georgian (Asomtavruli + Mkhedruli)
    if (rune >= 0x13A0 && rune <= 0x13FD) return sz_false_k; // Cherokee (folds to uppercase!)
    if (rune >= 0x1C80 && rune <= 0x1C8F) return sz_false_k; // Cyrillic Extended-C
    if (rune >= 0x1C90 && rune <= 0x1CBF) return sz_false_k; // Georgian Extended (Mtavruli)
    if (rune == 0x1D79 || rune == 0x1D7D || rune == 0x1D8E)
        return sz_false_k; // Phonetic Extensions ᵹ ᵽ ᶎ (fold targets of Ᵹ U+A77D, Ᵽ U+2C63, Ᶎ U+A7C6)
    if (rune >= 0x1E00 && rune <= 0x1EFF) return sz_false_k; // Latin Extended Additional
    if (rune >= 0x1F00 && rune <= 0x1FFF) return sz_false_k; // Greek Extended
    if (rune == 0x214E) return sz_false_k;                   // ⅎ (fold target of Ⅎ U+2132)
    if (rune >= 0x2170 && rune <= 0x217F) return sz_false_k; // small Roman numerals (fold targets of U+2160-216F)
    if (rune == 0x2184) return sz_false_k;                   // ↄ (fold target of Ↄ U+2183)
    if (rune >= 0x24D0 && rune <= 0x24E9) return sz_false_k; // circled small Latin (fold targets of U+24B6-24CF)
    if (rune >= 0x2C00 && rune <= 0x2C5F) return sz_false_k; // Glagolitic
    if (rune >= 0x2C60 && rune <= 0x2C7F) return sz_false_k; // Latin Extended-C
    if (rune >= 0x2C80 && rune <= 0x2CFF) return sz_false_k; // Coptic
    if (rune >= 0x2D00 && rune <= 0x2D2F) return sz_false_k; // Georgian Supplement (Nuskhuri)
    if (rune >= 0x2DE0 && rune <= 0x2DFF) return sz_false_k; // Cyrillic Extended-A
    if (rune >= 0xA640 && rune <= 0xA69F) return sz_false_k; // Cyrillic Extended-B
    if (rune >= 0xA720 && rune <= 0xA7FF) return sz_false_k; // Latin Extended-D
    if (rune >= 0xAB30 && rune <= 0xAB6F) return sz_false_k; // Latin Extended-E
    if (rune >= 0xAB70 && rune <= 0xABBF) return sz_false_k; // Cherokee Supplement (lowercase)
    if (rune >= 0xFB00 && rune <= 0xFB06) return sz_false_k; // Alphabetic Presentation (ligatures)
    if (rune >= 0xFB13 && rune <= 0xFB17) return sz_false_k; // Armenian ligatures
    if (rune >= 0xFF21 && rune <= 0xFF5A) return sz_false_k; // Fullwidth Latin
    //
    // 4-byte sequences (U+10000-10FFFF): F0-F4 lead bytes
    if (rune >= 0x10400 && rune <= 0x1044F) return sz_false_k; // Deseret
    if (rune >= 0x104B0 && rune <= 0x104FF) return sz_false_k; // Osage
    if (rune >= 0x10570 && rune <= 0x105BF) return sz_false_k; // Vithkuqi
    if (rune >= 0x10780 && rune <= 0x107BF) return sz_false_k; // Latin Extended-F
    if (rune >= 0x10C80 && rune <= 0x10CFF) return sz_false_k; // Old Hungarian
    if (rune >= 0x10D70 && rune <= 0x10D85) return sz_false_k; // Garay small letters (fold targets of U+10D50-10D65)
    if (rune >= 0x118A0 && rune <= 0x118FF) return sz_false_k; // Warang Citi
    if (rune >= 0x16E40 && rune <= 0x16E9F) return sz_false_k; // Medefaidrin
    if (rune >= 0x16EBB && rune <= 0x16ED3) return sz_false_k; // Beria Erfe small letters (Unicode 17 fold targets)
    if (rune >= 0x1DF00 && rune <= 0x1DFFF) return sz_false_k; // Latin Extended-G
    if (rune >= 0x1E000 && rune <= 0x1E02F) return sz_false_k; // Glagolitic Supplement
    if (rune >= 0x1E030 && rune <= 0x1E08F) return sz_false_k; // Cyrillic Extended-D
    if (rune >= 0x1E900 && rune <= 0x1E95F) return sz_false_k; // Adlam

    return sz_true_k;
}

SZ_API_COMPTIME sz_cptr_t sz_utf8_find_cased_serial(sz_cptr_t str, sz_size_t length) {
    sz_u8_t const *text_cursor = (sz_u8_t const *)str;
    sz_u8_t const *text_end = text_cursor + length;

    while (text_cursor < text_end) {
        sz_u8_t lead = *text_cursor;

        // ASCII fast path: only digits, punctuation, and control chars are caseless
        // A-Z (0x41-0x5A) and a-z (0x61-0x7A) participate in case folding
        if (lead < 0x80) {
            if ((lead >= 'A' && lead <= 'Z') || (lead >= 'a' && lead <= 'z')) return (sz_cptr_t)text_cursor;
            text_cursor++;
            continue;
        }

        // Multi-byte: decode and check. A byte that does not begin a well-formed codepoint is its own
        // 1-byte maximal subpart - it folds to itself, so it is caseless and never a violation; resync by one byte.
        sz_rune_t rune;
        sz_rune_length_t const rune_length = sz_rune_decode((sz_cptr_t)text_cursor, (sz_cptr_t)text_end, &rune);
        if (rune_length == sz_rune_invalid_k) {
            text_cursor++;
            continue;
        }
        if (sz_rune_is_uncased_(rune) == sz_false_k) return (sz_cptr_t)text_cursor;
        text_cursor += rune_length;
    }

    return SZ_NULL_CHAR;
}



/** @brief  Pops the lowest candidate position from @p matches and returns its bit index - the shared scalar
 *          walk behind every ISA probe filter, so the vector kernels never materialize their own bit scans. */

/**
 *  Per-codepoint Latin Extended-A fold deltas after a C4/C5 lead, indexed by the continuation
 *  byte's low 6 bits (`text & 0x3F`). Entry value is the in-place add: 0 = identity, 1 = fold by
 *  +1. The cross-block irregulars that the case-fold tables flag with 0x80 ('İ' C4 B0, 'Ŀ' C4 BF,
 *  'Ÿ' C5 B8, 'ſ' C5 BF) are 0 here because the alarm routes them to the danger-zone handler, so
 *  the fold leaves them untouched. Same parity that the explicit range checks used to compute, but
 *  resolved in one `vqtbl4q_u8` per lead family. Verified against the serial reference in tests.
 */
static sz_u8_t const sz_utf8_uncased_central_c4_deltas_lut_[64] = {
    1, 0, 1, 0, 1, 0, 1, 0, 1, 0, 1, 0, 1, 0, 1, 0, // C4 80-8F: 'Ā'-'ď' even-parity pairs
    1, 0, 1, 0, 1, 0, 1, 0, 1, 0, 1, 0, 1, 0, 1, 0, // C4 90-9F
    1, 0, 1, 0, 1, 0, 1, 0, 1, 0, 1, 0, 1, 0, 1, 0, // C4 A0-AF
    0, 0, 1, 0, 1, 0, 1, 0, 0, 1, 0, 1, 0, 1, 0, 0, // C4 B0-BF: 'İ'/'ĸ'/'Ŀ' caseless or cross-block
};
static sz_u8_t const sz_utf8_uncased_central_c5_deltas_lut_[64] = {
    0, 1, 0, 1, 0, 1, 0, 1, 0, 0, 1, 0, 1, 0, 1, 0, // C5 80-8F: odd head, 'ŉ' (C5 89) irregular -> 0
    1, 0, 1, 0, 1, 0, 1, 0, 1, 0, 1, 0, 1, 0, 1, 0, // C5 90-9F
    1, 0, 1, 0, 1, 0, 1, 0, 1, 0, 1, 0, 1, 0, 1, 0, // C5 A0-AF
    1, 0, 1, 0, 1, 0, 1, 0, 0, 1, 0, 1, 0, 1, 0, 0, // C5 B0-BF: 'Ÿ'/'ſ' cross-block -> 0
};

/**
 *  Monotonic-Greek second-byte fold deltas after a CE lead, indexed by `text & 0x3F`. The deltas
 *  the per-rule range checks used to assemble, resolved in one `vqtbl4q_u8`:
 *  'Ά' (86) +0x26, 'Έ'-'Ί' (88-8A) +0x25, 'Ύ'/'Ώ' (8E-8F) −1, 'Α'-'Ο' (91-9F) +0x20,
 *  'Π'-'Ω' (A0-A9) and 'Ϊ'/'Ϋ' (AA-AB) −0x20. 'Ό' (8C) keeps its byte (lead-only change).
 */
static sz_u8_t const sz_utf8_uncased_greek_ce_deltas_lut_[64] = {
    0,    0,    0,    0,    0,    0,    0x26, 0,
    0x25, 0x25, 0x25, 0,    0,    0,    0xFF, 0xFF, // CE 80-8F
    0,    0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20,
    0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, // CE 90-9F
    0xE0, 0xE0, 0xE0, 0xE0, 0xE0, 0xE0, 0xE0, 0xE0,
    0xE0, 0xE0, 0xE0, 0xE0, 0,    0,    0,    0, // CE A0-AF
    0,    0,    0,    0,    0,    0,    0,    0,
    0,    0,    0,    0,    0,    0,    0,    0, // CE B0-BF (already lowercase)
};

/**
 *  Lead-promotion flags (+1, CE → CF) for the second-byte classes whose lowercase lands in the CF
 *  block: 'Ό' (8C), 'Ύ'/'Ώ' (8E-8F), 'Π'-'Ω' (A0-A9), 'Ϊ'/'Ϋ' (AA-AB). 'Α'-'Ο' (91-9F) stay
 *  under CE, so they do not promote. Propagated one lane back through `next_bytes` onto the lead.
 */
static sz_u8_t const sz_utf8_uncased_greek_ce_promotes_lut_[64] = {
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 0, 1, 1, // CE 80-8F: 8C, 8E, 8F promote
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, // CE 90-9F: stay under CE
    1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 0, 0, 0, 0, // CE A0-AB: promote to CF
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, // CE B0-BF
};


/**
 *  @brief Verify head region uncasedly (backward iteration).
 *
 *  Walks backward from needle_end/haystack_end, comparing folded runes.
 *  Returns true if needle region exhausts (matched), with haystack bytes consumed.
 *
 *  @param needle_start Start of needle head region.
 *  @param needle_end End of needle head region (where safe window begins).
 *  @param haystack_start Start of haystack (lower bound for backward scan).
 *  @param haystack_end End of haystack head region (where safe window was found).
 *  @param match_length Haystack bytes consumed by this match.
 */
SZ_HELPER_AUTO sz_bool_t sz_utf8_uncased_verify_head_(sz_cptr_t needle_start, sz_cptr_t needle_end,
                                                      sz_cptr_t haystack_start, sz_cptr_t haystack_end,
                                                      sz_size_t *match_length) {

    // If needle head is empty, no haystack bytes needed
    if (needle_end <= needle_start) {
        *match_length = 0;
        return sz_true_k;
    }

    sz_utf8_folded_reverse_iter_t needle_riter, haystack_riter;
    sz_utf8_folded_reverse_iter_init_(&needle_riter, needle_start, needle_end);
    sz_utf8_folded_reverse_iter_init_(&haystack_riter, haystack_start, haystack_end);

    sz_rune_t needle_rune = 0, haystack_rune = 0;
    for (;;) {
        sz_bool_t have_needle = sz_utf8_folded_reverse_iter_prev_(&needle_riter, &needle_rune);

        // Needle exhausted - success! Unconsumed haystack runes are OK.
        // Example: "fi" matches suffix of "ﬃ" (folds to "ffi"), leaving first 'f' unused.
        if (!have_needle) {
            *match_length = (sz_size_t)(haystack_end - haystack_riter.ptr);
            return sz_true_k;
        }

        sz_bool_t have_haystack = sz_utf8_folded_reverse_iter_prev_(&haystack_riter, &haystack_rune);
        if (!have_haystack) return sz_false_k;
        if (needle_rune != haystack_rune) return sz_false_k;
    }
}

/**
 *  @brief Verify tail region uncasedly (forward iteration).
 *
 *  Walks forward, comparing folded runes. Returns true if needle exhausts.
 *
 *  @param needle_start Start of needle tail region.
 *  @param needle_end End of needle tail region (= needle + needle_length).
 *  @param haystack_start Start of haystack tail region.
 *  @param haystack_end End of haystack (upper bound for forward scan).
 *  @param match_length Haystack bytes consumed by this match.
 */
SZ_HELPER_AUTO sz_bool_t sz_utf8_uncased_verify_tail_(sz_cptr_t needle_start, sz_cptr_t needle_end,
                                                      sz_cptr_t haystack_start, sz_cptr_t haystack_end,
                                                      sz_size_t *match_length) {

    sz_size_t needle_length = (sz_size_t)(needle_end - needle_start);

    // Empty tail is trivially matched
    if (needle_length == 0) {
        *match_length = 0;
        return sz_true_k;
    }

    sz_utf8_folded_iter_t needle_iter, haystack_iter;
    sz_utf8_folded_iter_init_(&needle_iter, needle_start, needle_length);
    sz_utf8_folded_iter_init_(&haystack_iter, haystack_start, (sz_size_t)(haystack_end - haystack_start));

    sz_rune_t needle_rune = 0, haystack_rune = 0;
    for (;;) {
        sz_bool_t have_needle = sz_utf8_folded_iter_next_(&needle_iter, &needle_rune);

        if (!have_needle) {
            // Needle exhausted - success!
            *match_length = (sz_size_t)(haystack_iter.ptr - haystack_start);
            return sz_true_k;
        }

        sz_bool_t have_haystack = sz_utf8_folded_iter_next_(&haystack_iter, &haystack_rune);
        if (!have_haystack) return sz_false_k;
        if (needle_rune != haystack_rune) return sz_false_k;
    }
}

/**
 *  @brief Verify a complete match around a SIMD-detected window.
 *
 *  Verifies two regions: "head" (before window) and "tail" (after window).
 *  It's important to note that the middle part may still be in part unprocessed, if its larger
 *  than the "folded slice" of the needle. We handle it as part of the "tail" and the `needle_tail_bytes`
 *  must be calculated accordingly.
 *
 *  @param haystack Haystack start pointer, arbitrary case.
 *  @param haystack_length Haystack length in bytes.
 *  @param needle Needle start pointer, arbitrary case.
 *  @param needle_length Needle length in bytes.
 *  @param haystack_matched_offset Start offset of matched safe window in haystack in bytes.
 *  @param haystack_matched_length Length of matched safe window in haystack in bytes.
 *  @param needle_head_bytes Start of matched safe window in needle in bytes.
 *  @param needle_tail_bytes Number of bytes in the needle remaining after the matched part.
 *  @param match_length Total length of the verified match in haystack bytes.
 *  @return Match start pointer, or SZ_NULL_CHAR if validation fails.
 */
typedef struct {
    sz_cptr_t match;
    sz_size_t length;
} sz_utf8_uncased_verify_result_t;

SZ_HELPER_AUTO sz_utf8_uncased_verify_result_t sz_utf8_uncased_verify_match_( //
    sz_cptr_t haystack, sz_size_t haystack_length,                        //
    sz_cptr_t needle, sz_size_t needle_length,                            //
    sz_size_t haystack_matched_offset, sz_size_t haystack_matched_length, //
    sz_size_t needle_head_bytes, sz_size_t needle_tail_bytes) {

    sz_utf8_uncased_verify_result_t result = {SZ_NULL_CHAR, 0};

    sz_cptr_t needle_end = needle + needle_length;
    sz_cptr_t haystack_end = haystack + haystack_length;

    // Verify head using backward iterators
    sz_size_t head_match_length = 0;
    if (needle_head_bytes)
        if (!sz_utf8_uncased_verify_head_(                    //
                needle, needle + needle_head_bytes,           // needle head region
                haystack, haystack + haystack_matched_offset, // haystack head region
                &head_match_length))
            return result;

    // Verify tail using forward iterators
    sz_size_t tail_match_length = 0;
    sz_cptr_t haystack_tail_start = haystack + haystack_matched_offset + haystack_matched_length;
    if (needle_tail_bytes)
        if (!sz_utf8_uncased_verify_tail_(                              //
                needle + needle_length - needle_tail_bytes, needle_end, // needle tail region
                haystack_tail_start, haystack_end,                      // haystack tail region
                &tail_match_length))
            return result;

    result.length = head_match_length + haystack_matched_length + tail_match_length;
    result.match = haystack + haystack_matched_offset - head_match_length;
    return result;
}

/**
 *  @brief Hash-free uncased search for needles that fold to exactly 1 rune.
 *      Examples: 'a', 'A', 'б', 'Б' (but NOT 'ß' (U+00DF, C3 9F) → "ss" = 2 runes).
 *
 *  Single-pass algorithm: parses each source rune, folds it, checks if it produces
 *  exactly one rune matching the target. No iterator overhead, no verification needed.
 *
 *  @param haystack Pointer to the haystack string to search within.
 *  @param haystack_length Length of the haystack in bytes.
 *  @param needle_folded The single folded rune to search for.
 *  @param match_length Output: length of the matched rune in haystack bytes on success.
 *  @return Pointer to the first matching rune, or SZ_NULL_CHAR if not found.
 */
SZ_HELPER_AUTO sz_cptr_t sz_utf8_uncased_search_1folded_serial_( //
    sz_cptr_t haystack, sz_size_t haystack_length,               //
    sz_rune_t needle_folded, sz_size_t *match_length) {

    sz_cptr_t const haystack_end = haystack + haystack_length;

    // Each haystack rune may fold in up to 3 runes
    sz_rune_t haystack_rune;
    sz_rune_length_t haystack_rune_length;

    // If we simply initialize the runes for zero, the code will break
    // when the needle itself is the NUL character
    sz_rune_t haystack_folded_runes[3] = {~needle_folded};
    while (haystack < haystack_end) {
        // A byte that does not begin a well-formed codepoint folds to itself and matches byte-for-byte;
        // resync by one byte. Fill the unused fold slots with sentinels so they never false-match.
        haystack_rune_length = sz_rune_decode(haystack, haystack_end, &haystack_rune);
        if (haystack_rune_length == sz_rune_invalid_k) {
            haystack_folded_runes[0] = sz_rune_malformed_byte_((sz_u8_t)*haystack);
            haystack_folded_runes[1] = ~needle_folded;
            haystack_folded_runes[2] = ~needle_folded;
            haystack_rune_length = sz_rune_1byte_k;
        }
        else { sz_unicode_fold_codepoint_(haystack_rune, haystack_folded_runes); }

        // Perform branchless equality check via arithmetic
        sz_u32_t has_match =                              //
            (haystack_folded_runes[0] == needle_folded) + //
            (haystack_folded_runes[1] == needle_folded) + //
            (haystack_folded_runes[2] == needle_folded);

        if (has_match) {
            *match_length = haystack_rune_length;
            return haystack;
        }

        haystack += haystack_rune_length;
    }

    *match_length = 0;
    return SZ_NULL_CHAR;
}

/**
 *  @brief Search a "danger zone" region using 1-folded candidate search + validation.
 *
 *  When SIMD kernels detect potentially problematic bytes (ligatures, Greek Extended, etc.),
 *  they fall back to this serial search within the affected chunk. This function:
 *  1. Extracts the first folded rune from the needle's safe window
 *  2. Searches for candidates matching that rune
 *  3. Validates each candidate using the full verification pipeline
 *
 *  @param haystack Full haystack string, arbitrary case.
 *  @param haystack_length Full haystack length in bytes.
 *  @param needle Full needle string, arbitrary case.
 *  @param needle_length Full needle length.
 *  @param danger_cursor Start of the danger zone region to search.
 *  @param danger_length Length of the danger zone region in bytes.
 *  @param needle_first_safe_folded_rune The first rune of the safe window, folded.
 *  @param needle_first_safe_folded_rune_offset Offset of the safe window within the needle.
 *  @param match_length Haystack bytes consumed by the match.
 *  @return Pointer to match start, or SZ_NULL_CHAR if not found in this region.
 */
SZ_HELPER_INLINE sz_cptr_t sz_utf8_uncased_search_in_danger_zone_( //
    sz_cptr_t haystack, sz_size_t haystack_length,               //
    sz_cptr_t needle, sz_size_t needle_length,                   //
    sz_cptr_t danger_cursor, sz_size_t danger_length,            //
    sz_rune_t needle_first_safe_folded_rune,                     //
    sz_size_t needle_first_safe_folded_rune_offset,              //
    sz_size_t *match_length) {

    sz_cptr_t const haystack_end = haystack + haystack_length;
    sz_cptr_t const danger_end = sz_min_of_two(danger_cursor + danger_length, haystack_end);
    while (danger_cursor < danger_end) {

        // Skip continuation bytes - they are mid-sequence, not valid rune starts.
        // Without this check, a continuation byte like 0xBA could be misinterpreted as U+00BA (º),
        // causing false matches when the danger zone starts mid-character.
        sz_u8_t lead_byte = *(sz_u8_t const *)danger_cursor;
        if ((lead_byte & 0xC0) == 0x80) {
            danger_cursor++;
            continue;
        }

        // The following part is practically the unpacked variant of `sz_utf8_uncased_search_1folded_serial_`,
        // that finds the first occurrence of the `needle_first_safe_folded_rune` haystack. The issue is that each one
        // `haystack_rune` may unpack into multiple `haystack_folded_runes`.
        sz_rune_t haystack_rune;
        sz_rune_length_t haystack_rune_length;
        sz_rune_t haystack_folded_runes[3] = {~needle_first_safe_folded_rune};
        // A byte that does not begin a well-formed codepoint folds to itself and resyncs by one byte.
        haystack_rune_length = sz_rune_decode(danger_cursor, haystack_end, &haystack_rune);
        sz_size_t haystack_folded_runes_count;
        if (haystack_rune_length == sz_rune_invalid_k) {
            haystack_folded_runes[0] = sz_rune_malformed_byte_((sz_u8_t)*danger_cursor);
            haystack_folded_runes_count = 1;
            haystack_rune_length = sz_rune_1byte_k;
        }
        else { haystack_folded_runes_count = sz_unicode_fold_codepoint_(haystack_rune, haystack_folded_runes); }

        // The simplest case is when the very first in `haystack_folded_runes` is our target:
        if (haystack_folded_runes[0] == needle_first_safe_folded_rune) {
            // Validate the full match using the unified validator
            sz_utf8_uncased_verify_result_t verified = sz_utf8_uncased_verify_match_( //
                haystack, haystack_length,                   //
                needle, needle_length,                       //
                danger_cursor - haystack, 0,                 // No pre-matched middle
                needle_first_safe_folded_rune_offset,
                needle_length - needle_first_safe_folded_rune_offset); // Verify everything after head serially

            if (verified.match) {
                *match_length = verified.length;
                return verified.match;
            }
            else { goto consider_second_haystack_folded_rune; } // We fall through here anyways :)
        }

    consider_second_haystack_folded_rune:

        // Check for a match at the second position in the folded haystack rune sequence
        if (haystack_folded_runes_count > 1 && haystack_folded_runes[1] == needle_first_safe_folded_rune) {
            sz_cptr_t haystack_match_start = 0, haystack_match_end = 0;

            // Check if the previous characters in the needle match the haystack before the danger zone began
            sz_rune_t needle_riter_rune = 0, haystack_riter_rune = 0;
            sz_utf8_folded_reverse_iter_t needle_riter, haystack_riter;
            sz_utf8_folded_reverse_iter_init_(&needle_riter, needle, needle + needle_first_safe_folded_rune_offset);
            sz_utf8_folded_reverse_iter_init_(&haystack_riter, haystack, danger_cursor);

            // Check if we even have needle bytes to check
            {
                sz_bool_t have_needle = sz_utf8_folded_reverse_iter_prev_(&needle_riter, &needle_riter_rune);
                if (have_needle && needle_riter_rune != haystack_folded_runes[0])
                    goto consider_third_haystack_folded_rune;
            }

            // Loop backwards until we exhaust the needle head or find a mismatch
            for (;;) {
                sz_bool_t have_needle = sz_utf8_folded_reverse_iter_prev_(&needle_riter, &needle_riter_rune);

                // Needle exhausted - success!
                if (!have_needle) {
                    haystack_match_start = haystack_riter.ptr;
                    break;
                }

                sz_bool_t have_haystack = sz_utf8_folded_reverse_iter_prev_(&haystack_riter, &haystack_riter_rune);
                if (!have_haystack) goto consider_third_haystack_folded_rune;
                if (needle_riter_rune != haystack_riter_rune) goto consider_third_haystack_folded_rune;
            }

            // First match the tail (from safe window start forward)
            sz_rune_t needle_iter_rune = 0, haystack_iter_rune = 0;
            sz_utf8_folded_iter_t needle_iter, haystack_iter;
            sz_utf8_folded_iter_init_(&needle_iter, needle + needle_first_safe_folded_rune_offset,
                                      needle_length - needle_first_safe_folded_rune_offset);
            sz_utf8_folded_iter_init_(&haystack_iter, danger_cursor + haystack_rune_length,
                                      haystack_end - (danger_cursor + haystack_rune_length));

            // Pop the `needle_first_safe_folded_rune` from the forward iterator
            {
                sz_bool_t have_needle = sz_utf8_folded_iter_next_(&needle_iter, &needle_iter_rune);
                sz_assert_(have_needle && needle_iter_rune == needle_first_safe_folded_rune);
            }

            // In some cases we already have the first point of comparison in the `haystack_folded_runes[2]`
            if (haystack_folded_runes_count == 3) {
                sz_bool_t have_needle = sz_utf8_folded_iter_next_(&needle_iter, &needle_iter_rune);
                if (have_needle && needle_iter_rune != haystack_folded_runes[2])
                    goto consider_third_haystack_folded_rune;
            }

            // Match the remaining tail runes
            for (;;) {
                sz_bool_t have_needle = sz_utf8_folded_iter_next_(&needle_iter, &needle_iter_rune);

                // Needle exhausted - success!
                if (!have_needle) {
                    haystack_match_end = haystack_iter.ptr;
                    break;
                }

                sz_bool_t have_haystack = sz_utf8_folded_iter_next_(&haystack_iter, &haystack_iter_rune);
                if (!have_haystack) goto consider_third_haystack_folded_rune;
                if (needle_iter_rune != haystack_iter_rune) goto consider_third_haystack_folded_rune;
            }

            // Check if we have a match to report
            if (haystack_match_start != 0 && haystack_match_end != 0) {
                *match_length = (sz_size_t)(haystack_match_end - haystack_match_start);
                return haystack_match_start;
            }
        }

    consider_third_haystack_folded_rune:

        // Check for a match at the second position in the folded haystack rune sequence
        if (haystack_folded_runes_count > 2 && haystack_folded_runes[2] == needle_first_safe_folded_rune) {
            sz_cptr_t haystack_match_start = 0, haystack_match_end = 0;

            // Check if the previous characters in the needle match the haystack before the danger zone began
            sz_rune_t needle_riter_rune = 0, haystack_riter_rune = 0;
            sz_utf8_folded_reverse_iter_t needle_riter, haystack_riter;
            sz_utf8_folded_reverse_iter_init_(&needle_riter, needle, needle + needle_first_safe_folded_rune_offset);
            sz_utf8_folded_reverse_iter_init_(&haystack_riter, haystack, danger_cursor);

            // Check if we even have needle bytes to check
            {
                sz_bool_t have_needle = sz_utf8_folded_reverse_iter_prev_(&needle_riter, &needle_riter_rune);
                if (have_needle && needle_riter_rune != haystack_folded_runes[1])
                    goto consider_following_haystack_runes;
                have_needle = sz_utf8_folded_reverse_iter_prev_(&needle_riter, &needle_riter_rune);
                if (have_needle && needle_riter_rune != haystack_folded_runes[0])
                    goto consider_following_haystack_runes;
            }

            // Loop backwards until we exhaust the needle head or find a mismatch
            for (;;) {
                sz_bool_t have_needle = sz_utf8_folded_reverse_iter_prev_(&needle_riter, &needle_riter_rune);

                // Needle exhausted - success!
                if (!have_needle) {
                    haystack_match_start = haystack_riter.ptr;
                    break;
                }

                sz_bool_t have_haystack = sz_utf8_folded_reverse_iter_prev_(&haystack_riter, &haystack_riter_rune);
                if (!have_haystack) goto consider_following_haystack_runes;
                if (needle_riter_rune != haystack_riter_rune) goto consider_following_haystack_runes;
            }

            // First match the tail (from safe window start forward)
            sz_rune_t needle_iter_rune = 0, haystack_iter_rune = 0;
            sz_utf8_folded_iter_t needle_iter, haystack_iter;
            sz_utf8_folded_iter_init_(&needle_iter, needle + needle_first_safe_folded_rune_offset,
                                      needle_length - needle_first_safe_folded_rune_offset);
            sz_utf8_folded_iter_init_(&haystack_iter, danger_cursor + haystack_rune_length,
                                      haystack_end - (danger_cursor + haystack_rune_length));

            // Pop the `needle_first_safe_folded_rune` from the forward iterator
            {
                sz_bool_t have_needle = sz_utf8_folded_iter_next_(&needle_iter, &needle_iter_rune);
                sz_assert_(have_needle && needle_iter_rune == needle_first_safe_folded_rune);
            }

            // Match the remaining tail runes
            for (;;) {
                sz_bool_t have_needle = sz_utf8_folded_iter_next_(&needle_iter, &needle_iter_rune);

                // Needle exhausted - success!
                if (!have_needle) {
                    haystack_match_end = haystack_iter.ptr;
                    break;
                }

                sz_bool_t have_haystack = sz_utf8_folded_iter_next_(&haystack_iter, &haystack_iter_rune);
                if (!have_haystack) goto consider_following_haystack_runes;
                if (needle_iter_rune != haystack_iter_rune) goto consider_following_haystack_runes;
            }

            // Check if we have a match to report
            if (haystack_match_start != 0 && haystack_match_end != 0) {
                *match_length = (sz_size_t)(haystack_match_end - haystack_match_start);
                return haystack_match_start;
            }
        }

    consider_following_haystack_runes:
        // Move to next candidate
        danger_cursor += haystack_rune_length;
    }

    return SZ_NULL_CHAR;
}

/**
 *  @brief Hash-free uncased search for needles that fold to exactly 2 runes.
 *      Examples: 'ab', 'AB', 'ß' (U+00DF) → "ss", 'ﬁ' (U+FB01) → "fi".
 *
 *  Single-pass sliding window over the folded rune stream. Handles expansions
 *  by buffering folded runes from each source and tracking source boundaries.
 *
 *  @param haystack Pointer to the haystack string to search within.
 *  @param haystack_length Length of the haystack in bytes.
 *  @param first_needle_folded First folded rune of the 2-rune needle.
 *  @param second_needle_folded Second folded rune of the 2-rune needle.
 *  @param match_length Output: length of the matched region in haystack bytes on success.
 *  @return Pointer to the first match, or SZ_NULL_CHAR if not found.
 */
SZ_HELPER_AUTO sz_cptr_t sz_utf8_uncased_search_2folded_serial_( //
    sz_cptr_t haystack, sz_size_t haystack_length,               //
    sz_rune_t first_needle_folded, sz_rune_t second_needle_folded, sz_size_t *match_length) {

    sz_cptr_t const haystack_end = haystack + haystack_length;

    sz_rune_t haystack_rune;
    sz_rune_length_t haystack_rune_length;

    // Source-codepoint begin pointer for the single history slot (slot [0]). It is never read
    // until at least one codepoint has been processed, because the sentinel `~first_needle_folded`
    // in slot [0] can never equal `first_needle_folded`, so `match_at_01` is 0 on the first step.
    sz_cptr_t first_history_source_begin = haystack;

    // If we simply initialize the runes for zero, the code will break
    // when the needle itself is the NUL character
    sz_rune_t haystack_folded_runes[4] = {~first_needle_folded};
    while (haystack < haystack_end) {
        // A byte that does not begin a well-formed codepoint folds to itself and resyncs by one byte.
        haystack_rune_length = sz_rune_decode(haystack, haystack_end, &haystack_rune);

        // Pre-fill positions [2] and [3] with sentinels before folding.
        // The fold will overwrite positions it uses; unused positions keep the sentinel.
        // This branchlessly prevents stale data from causing false matches.
        sz_rune_t sentinel = ~second_needle_folded;
        haystack_folded_runes[2] = sentinel;
        haystack_folded_runes[3] = sentinel;
        // Export into the last 3 rune entries of the 4-element array,
        // keeping the first position with historical data untouched
        sz_size_t folded_count;
        if (haystack_rune_length == sz_rune_invalid_k) {
            haystack_folded_runes[1] = sz_rune_malformed_byte_((sz_u8_t)*haystack);
            folded_count = 1;
            haystack_rune_length = sz_rune_1byte_k;
        }
        else { folded_count = sz_unicode_fold_codepoint_(haystack_rune, haystack_folded_runes + 1); }

        // Perform branchless equality check via arithmetic
        sz_u32_t has_match_f0 = first_needle_folded == haystack_folded_runes[0];
        sz_u32_t has_match_f1 = first_needle_folded == haystack_folded_runes[1];
        sz_u32_t has_match_f2 = first_needle_folded == haystack_folded_runes[2];
        sz_u32_t has_match_s1 = second_needle_folded == haystack_folded_runes[1];
        sz_u32_t has_match_s2 = second_needle_folded == haystack_folded_runes[2];
        sz_u32_t has_match_s3 = second_needle_folded == haystack_folded_runes[3];

        // Branchless match detection: each product is 0 or 1
        sz_u32_t match_at_01 = has_match_f0 * has_match_s1;
        sz_u32_t match_at_12 = has_match_f1 * has_match_s2;
        sz_u32_t match_at_23 = has_match_f2 * has_match_s3;
        sz_u32_t has_match = match_at_01 + match_at_12 + match_at_23;

        if (has_match) {
            // The first matched rune is in history slot [0] only for `match_at_01`; for `match_at_12`
            // and `match_at_23` it is in the current codepoint. The last matched rune is always within
            // the current codepoint, so the match always ends at the current codepoint's end.
            sz_cptr_t match_begin = match_at_01 ? first_history_source_begin : haystack;
            sz_cptr_t match_end = haystack + haystack_rune_length;
            *match_length = (sz_size_t)(match_end - match_begin);
            return match_begin;
        }

        // The history slot is always fed by the codepoint just processed.
        haystack_folded_runes[0] = haystack_folded_runes[folded_count];
        first_history_source_begin = haystack;
        haystack += haystack_rune_length;
    }

    *match_length = 0;
    return SZ_NULL_CHAR;
}

/**
 *  @brief Hash-free uncased search for needles that fold to exactly 3 runes.
 *      Examples: 'abc', 'ABC', "aß" → "ass", "ﬁa" (U+FB01) → "fia".
 *
 *  Single-pass sliding window of 3 folded runes over the haystack's folded stream.
 *  Handles expansions by buffering folded runes and tracking source boundaries.
 *
 *  @param haystack Pointer to the haystack string to search within.
 *  @param haystack_length Length of the haystack in bytes.
 *  @param first_needle_folded First folded rune of the 3-rune needle.
 *  @param second_needle_folded Second folded rune of the 3-rune needle.
 *  @param third_needle_folded Third folded rune of the 3-rune needle.
 *  @param match_length Output: length of the matched region in haystack bytes on success.
 *  @return Pointer to the first match, or SZ_NULL_CHAR if not found.
 */
SZ_HELPER_AUTO sz_cptr_t sz_utf8_uncased_search_3folded_serial_( //
    sz_cptr_t haystack, sz_size_t haystack_length,               //
    sz_rune_t first_needle_folded, sz_rune_t second_needle_folded, sz_rune_t third_needle_folded,
    sz_size_t *match_length) {

    sz_cptr_t const haystack_end = haystack + haystack_length;

    sz_rune_t haystack_rune;
    sz_rune_length_t haystack_rune_length;

    // Source-codepoint begin pointers for the two history slots ([0] and [1]). Never read until the
    // corresponding slot holds a real (non-sentinel) rune, because the sentinels in slots [0],[1] can
    // never match their needle runes.
    sz_cptr_t history_source_begin[2] = {haystack, haystack};

    // Initialize historical slots with sentinels that can never match their respective needle positions
    // This prevents false matches on first iterations when history is not yet populated
    sz_rune_t haystack_folded_runes[5] = {~first_needle_folded, ~second_needle_folded, 0, 0, 0};
    while (haystack < haystack_end) {
        // A byte that does not begin a well-formed codepoint folds to itself and resyncs by one byte.
        haystack_rune_length = sz_rune_decode(haystack, haystack_end, &haystack_rune);

        // Pre-fill positions [3] and [4] with sentinels before folding.
        // The fold will overwrite positions it uses; unused positions keep the sentinel.
        // This branchlessly prevents stale data from causing false matches.
        sz_rune_t sentinel = ~third_needle_folded;
        haystack_folded_runes[3] = sentinel;
        haystack_folded_runes[4] = sentinel;
        // Export into the last 3 rune entries of the 5-element array,
        // keeping the first two positions with historical data untouched
        sz_size_t folded_count;
        if (haystack_rune_length == sz_rune_invalid_k) {
            haystack_folded_runes[2] = sz_rune_malformed_byte_((sz_u8_t)*haystack);
            folded_count = 1;
            haystack_rune_length = sz_rune_1byte_k;
        }
        else { folded_count = sz_unicode_fold_codepoint_(haystack_rune, haystack_folded_runes + 2); }

        // Perform branchless equality check via arithmetic
        sz_u32_t has_match_f0 = first_needle_folded == haystack_folded_runes[0];
        sz_u32_t has_match_f1 = first_needle_folded == haystack_folded_runes[1];
        sz_u32_t has_match_f2 = first_needle_folded == haystack_folded_runes[2];
        sz_u32_t has_match_s1 = second_needle_folded == haystack_folded_runes[1];
        sz_u32_t has_match_s2 = second_needle_folded == haystack_folded_runes[2];
        sz_u32_t has_match_s3 = second_needle_folded == haystack_folded_runes[3];
        sz_u32_t has_match_t2 = third_needle_folded == haystack_folded_runes[2];
        sz_u32_t has_match_t3 = third_needle_folded == haystack_folded_runes[3];
        sz_u32_t has_match_t4 = third_needle_folded == haystack_folded_runes[4];

        // Branchless match detection: each product is 0 or 1
        sz_u32_t match_at_012 = has_match_f0 * has_match_s1 * has_match_t2;
        sz_u32_t match_at_123 = has_match_f1 * has_match_s2 * has_match_t3;
        sz_u32_t match_at_234 = has_match_f2 * has_match_s3 * has_match_t4;
        sz_u32_t has_match = match_at_012 + match_at_123 + match_at_234;

        if (has_match) {
            // First matched rune slot: [0] for `match_at_012`, [1] for `match_at_123` (both history),
            // [2] for `match_at_234` (the current codepoint). The last matched rune is always within
            // the current codepoint, so the match always ends at the current codepoint's end.
            sz_cptr_t match_begin;
            if (match_at_012) match_begin = history_source_begin[0];
            else if (match_at_123) match_begin = history_source_begin[1];
            else match_begin = haystack;
            sz_cptr_t match_end = haystack + haystack_rune_length;
            *match_length = (sz_size_t)(match_end - match_begin);
            return match_begin;
        }

        // Mirror the folded-rune shift for the per-slot source-begin pointers.
        if (folded_count >= 2) {
            // Both new history runes come from the current codepoint.
            haystack_folded_runes[0] = haystack_folded_runes[folded_count];
            haystack_folded_runes[1] = haystack_folded_runes[folded_count + 1];
            history_source_begin[0] = haystack;
            history_source_begin[1] = haystack;
        }
        else {
            sz_assert_(folded_count == 1);
            // Slot [0] inherits old slot [1]; slot [1] takes the current codepoint.
            haystack_folded_runes[0] = haystack_folded_runes[1];
            haystack_folded_runes[1] = haystack_folded_runes[2];
            history_source_begin[0] = history_source_begin[1];
            history_source_begin[1] = haystack;
        }

        haystack += haystack_rune_length;
    }

    *match_length = 0;
    return SZ_NULL_CHAR;
}

SZ_HELPER_NOINLINE sz_cptr_t sz_utf8_uncased_search_serial( //
    sz_cptr_t haystack, sz_size_t haystack_length,       //
    sz_cptr_t needle, sz_size_t needle_length,           //
    sz_utf8_uncased_needle_metadata_t *needle_metadata, sz_size_t *match_length) {

    (void)needle_metadata; // Only used by SIMD kernels for debugging

    if (needle_length == 0) {
        *match_length = 0;
        return haystack;
    }

    if (sz_utf8_find_cased_serial(needle, needle_length) == SZ_NULL_CHAR) {
        sz_cptr_t result = sz_find_serial(haystack, haystack_length, needle, needle_length);
        if (result) {
            *match_length = needle_length;
            return result;
        }
        *match_length = 0;
        return SZ_NULL_CHAR;
    }

    // For short needles (up to 12 bytes which can fold to at most ~6 runes), try hash-free search.
    // We fold the needle first and dispatch based on the folded rune count.
    // This avoids ring buffer setup, hash multiplier computation, and rolling hash updates.
    if (needle_length <= 12) {
        sz_rune_t folded_runes[4]; // 4th slot accessed before loop exit
        sz_size_t folded_count = 0;
        sz_utf8_folded_iter_t iter;
        sz_utf8_folded_iter_init_(&iter, needle, needle_length);
        sz_rune_t rune;
        while (folded_count < 4 && sz_utf8_folded_iter_next_(&iter, &rune)) folded_runes[folded_count++] = rune;

        // Dispatch based on folded rune count
        switch (folded_count) {
        case 1:
            return sz_utf8_uncased_search_1folded_serial_( //
                haystack, haystack_length,                 //
                folded_runes[0], match_length);
        case 2:
            return sz_utf8_uncased_search_2folded_serial_( //
                haystack, haystack_length,                 //
                folded_runes[0], folded_runes[1], match_length);
        case 3:
            return sz_utf8_uncased_search_3folded_serial_( //
                haystack, haystack_length,                 //
                folded_runes[0], folded_runes[1], folded_runes[2], match_length);
        default: break; // 4+ folded runes: fall through to Rabin-Karp
        }
    }

    sz_size_t const ring_capacity = 32;
    sz_rune_t needle_runes[32];
    sz_size_t needle_prefix_count = 0, needle_total_count = 0;
    sz_u64_t needle_hash = 0;
    {
        sz_utf8_folded_iter_t needle_iter;
        sz_utf8_folded_iter_init_(&needle_iter, needle, needle_length);
        sz_rune_t rune;
        while (needle_prefix_count < ring_capacity && sz_utf8_folded_iter_next_(&needle_iter, &rune)) {
            needle_runes[needle_prefix_count++] = rune;
            needle_hash = needle_hash * 257 + rune;
        }
        needle_total_count = needle_prefix_count;
        // For long needles, count remaining runes beyond ring buffer
        while (sz_utf8_folded_iter_next_(&needle_iter, &rune)) needle_total_count++;
    }
    if (!needle_prefix_count) {
        *match_length = 0;
        return SZ_NULL_CHAR;
    }

    sz_u64_t hash_multiplier = 1;
    for (sz_size_t lane_index = 1; lane_index < needle_prefix_count; ++lane_index) hash_multiplier *= 257;

    sz_rune_t window_runes[32];
    sz_cptr_t window_sources[32];     // Byte position of character that produced each window rune
    sz_size_t window_skip_counts[32]; // Runes to skip from first character's expansion
    sz_size_t ring_head = 0;
    sz_u64_t window_hash = 0;
    sz_utf8_folded_iter_t haystack_iter;
    sz_utf8_folded_iter_init_(&haystack_iter, haystack, haystack_length);

    sz_cptr_t window_start = haystack;
    sz_cptr_t current_source = haystack;
    sz_size_t current_skip = 0;
    sz_size_t window_count = 0;

    while (window_count < needle_prefix_count) {
        sz_cptr_t pre_advance_cursor = haystack_iter.ptr;
        sz_rune_t rune;
        if (!sz_utf8_folded_iter_next_(&haystack_iter, &rune)) break;
        window_runes[window_count] = rune;
        // Update source and skip only when starting a new character (not mid-expansion)
        if (haystack_iter.pending_idx <= 1 || haystack_iter.pending_count == 0) {
            current_source = pre_advance_cursor;
            current_skip = 0;
        }
        window_sources[window_count] = current_source;
        window_skip_counts[window_count] = current_skip;
        window_hash = window_hash * 257 + rune;
        window_count++;
        // For next rune from same expansion, increment skip
        if (haystack_iter.pending_idx > 0 && haystack_iter.pending_idx < haystack_iter.pending_count)
            current_skip = haystack_iter.pending_idx;
    }
    if (window_count < needle_prefix_count) {
        *match_length = 0;
        return SZ_NULL_CHAR;
    }
    sz_cptr_t window_end = haystack_iter.ptr;

    for (;;) {
        if (window_hash == needle_hash) {
            // The ring buffer is a circular array where `ring_head` points to the oldest (first) element.
            // A naive approach would use `window_runes[(ring_head + i) % needle_prefix_count]` for each comparison,
            // but modulo is expensive. Instead, we compare in two contiguous segments:
            //   - First segment:  window_runes[ring_head..needle_prefix_count) maps to needle_runes[0..first_segment)
            //   - Second segment: window_runes[0..ring_head) maps to needle_runes[first_segment..needle_prefix_count)
            sz_size_t first_segment = needle_prefix_count - ring_head;
            sz_size_t mismatches = 0;
            for (sz_size_t lane_index = 0; lane_index < first_segment; ++lane_index)
                mismatches += window_runes[ring_head + lane_index] != needle_runes[lane_index];
            for (sz_size_t lane_index = 0; lane_index < ring_head; ++lane_index)
                mismatches += window_runes[lane_index] != needle_runes[first_segment + lane_index];

            if (!mismatches) {
                sz_size_t skip_runes = window_skip_counts[ring_head];
                // Short needle: rune comparison above is sufficient verification
                if (needle_total_count <= ring_capacity) {
                    *match_length = (sz_size_t)(window_end - window_start);
                    return window_start;
                }
                // Long needle: verify FULL needle from window_start, skipping runes if match
                // starts mid-expansion. Example: ẚ→"aʾ", needle starting with "ʾ" must skip "a".
                sz_utf8_folded_iter_t verify_haystack_iter;
                sz_utf8_folded_iter_init_(&verify_haystack_iter, window_start,
                                          (sz_size_t)(haystack + haystack_length - window_start));
                // Skip runes within first character's expansion
                sz_rune_t skip_rune;
                for (sz_size_t skip_index = 0; skip_index < skip_runes; ++skip_index)
                    sz_utf8_folded_iter_next_(&verify_haystack_iter, &skip_rune);
                // Now verify full needle against remaining haystack
                sz_utf8_folded_iter_t verify_needle_iter;
                sz_utf8_folded_iter_init_(&verify_needle_iter, needle, needle_length);
                sz_rune_t needle_rune_v, haystack_rune_v;
                sz_bool_t match_ok = sz_true_k;
                while (sz_utf8_folded_iter_next_(&verify_needle_iter, &needle_rune_v)) {
                    if (!sz_utf8_folded_iter_next_(&verify_haystack_iter, &haystack_rune_v) ||
                        needle_rune_v != haystack_rune_v) {
                        match_ok = sz_false_k;
                        break;
                    }
                }
                if (match_ok) {
                    *match_length = (sz_size_t)(verify_haystack_iter.ptr - window_start);
                    return window_start;
                }
            }
        }

        sz_cptr_t pre_advance_cursor = haystack_iter.ptr;
        sz_rune_t new_rune;
        if (!sz_utf8_folded_iter_next_(&haystack_iter, &new_rune)) break;

        window_hash -= window_runes[ring_head] * hash_multiplier;
        window_hash = window_hash * 257 + new_rune;

        // Advance ring head, avoiding modulo with a conditional (cheaper than integer division)
        sz_size_t next_head = ring_head + 1;
        next_head = next_head == needle_prefix_count ? 0 : next_head;

        window_runes[ring_head] = new_rune;
        // Update source and skip only when starting a new character (not mid-expansion)
        if (haystack_iter.pending_idx <= 1 || haystack_iter.pending_count == 0) {
            current_source = pre_advance_cursor;
            current_skip = 0;
        }
        window_sources[ring_head] = current_source;
        window_skip_counts[ring_head] = current_skip;
        // For next rune from same expansion, increment skip
        if (haystack_iter.pending_idx > 0 && haystack_iter.pending_idx < haystack_iter.pending_count)
            current_skip = haystack_iter.pending_idx;

        ring_head = next_head;
        window_start = window_sources[ring_head];
        window_end = haystack_iter.ptr;
    }

    *match_length = 0;
    return SZ_NULL_CHAR;
}



/*  The character safety classifier and needle-metadata builder are ISA-agnostic: they only depend
 *  on the serial Unicode core (rune parsing & folding). The SIMD kernels (Ice Lake, etc.) consume
 *  the metadata it produces, so it lives here in the serial scaffolding rather than behind any
 *  `SZ_USE_*` gate, keeping it reachable for every backend including pure serial builds. */

/**
 *  @brief Determine safety profile for a character across all script contexts.
 *
 *  This function encodes the contextual safety rules from the ASCII selector
 *  and applies them consistently to all paths that include ASCII.
 *
 *  @param rune The decoded codepoint.
 *  @param rune_bytes UTF-8 byte length of this codepoint (1-4).
 *  @param prev_rune Previous codepoint (0 if at start).
 *  @param next_rune Next codepoint (0 if at end).
 *  @param prev_prev_rune Codepoint before prev_rune (0 if prev is at start).
 *  @param next_next_rune Codepoint after next_rune (0 if next is at end).
 *  @param safety_profiles Safety flags for each script path.
 *  @return The primary fast path preferred for this rune.
 *
 *  @note Using 0 for boundary markers is safe even though NUL (U+0000) is a valid
 *        codepoint in StringZilla's length-based strings. This works because:
 *        1. NUL is valid ASCII (< 0x80), so boundary and actual NUL are treated identically
 *        2. Ligature checks use inequality (lower_prev != 'f'), and 0 never matches letters
 *        3. NUL doesn't participate in any Unicode case folding or ligature expansions
 *
 *  @note The neighbor-of-neighbor context (prev_prev, next_next) enables position-1 and
 *        position-N-2 detection for the 's' rule: if prev_prev==0 && prev!=0, we're at
 *        position 1; if next_next==0 && next!=0, we're at position N-2.
 */
SZ_HELPER_AUTO sz_utf8_uncased_rune_safety_profile_t sz_utf8_uncased_rune_safety_profile_( //
    sz_rune_t rune, sz_size_t rune_bytes,                                                  //
    sz_rune_t prev_rune, sz_rune_t next_rune,                                              //
    sz_rune_t prev_prev_rune, sz_rune_t next_next_rune,                                    //
    unsigned int *safety_profiles) {

    unsigned safety = 0;

    // Bitmasks for profiles that share identical ASCII rules
    unsigned int western_group = //
        (1 << sz_utf8_uncased_rune_safe_western_europe_k);
    unsigned int central_viet_group =                       //
        (1 << sz_utf8_uncased_rune_safe_central_europe_k) | //
        (1 << sz_utf8_uncased_rune_safe_vietnamese_k);
    unsigned int strict_ascii_group =                   //
        (1 << sz_utf8_uncased_rune_ascii_invariant_k) | //
        (1 << sz_utf8_uncased_rune_safe_cyrillic_k) |   //
        (1 << sz_utf8_uncased_rune_safe_greek_k) |      //
        (1 << sz_utf8_uncased_rune_safe_armenian_k) |   //
        (1 << sz_utf8_uncased_rune_safe_georgian_k);

    // Helper: lowercase ASCII
    sz_rune_t lower = (rune >= 'A' && rune <= 'Z') ? (rune + 0x20) : rune;
    sz_rune_t lower_prev = (prev_rune >= 'A' && prev_rune <= 'Z') ? (prev_rune + 0x20) : prev_rune;
    sz_rune_t lower_next = (next_rune >= 'A' && next_rune <= 'Z') ? (next_rune + 0x20) : next_rune;

    // Helper: is neighbor ASCII letter? (explicit conversion for C++ compatibility)
    // Note: prev_rune/next_rune == 0 means boundary (start/end of needle)
    sz_bool_t prev_ascii = (prev_rune != 0 && prev_rune < 0x80) ? sz_true_k : sz_false_k;
    sz_bool_t next_ascii = (next_rune != 0 && next_rune < 0x80) ? sz_true_k : sz_false_k;
    sz_bool_t at_start = (prev_rune == 0) ? sz_true_k : sz_false_k;
    sz_bool_t at_end = (next_rune == 0) ? sz_true_k : sz_false_k;

    // Helper: position detection for 's' rule (mid-ß-expansion avoidance in Western profile)
    // Position 1: prev exists but prev_prev doesn't (prev is at position 0)
    // Position N-2: next exists but next_next doesn't (next is at position N-1)
    sz_bool_t at_pos_1 = (prev_rune != 0 && prev_prev_rune == 0) ? sz_true_k : sz_false_k;
    sz_bool_t at_pos_n_minus_2 = (next_rune != 0 && next_next_rune == 0) ? sz_true_k : sz_false_k;

    // ASCII character (1-byte UTF-8)
    if (rune < 0x80) {
        if (lower >= 'a' && lower <= 'z') {
            switch (lower) {

            // Unconditionally safe for all profiles.
            // No Unicode chars fold to sequences containing these,
            // and they don't participate in dangerous ligatures.
            case 'b':
            case 'c':
            case 'd':
            case 'e':
            case 'g':
            case 'm':
            case 'o':
            case 'p':
            case 'q':
            case 'r':
            case 'u':
            case 'v':
            case 'x':
            case 'z': safety |= strict_ascii_group | central_viet_group | western_group; break;

            // 'k':
            // - Strict: UNSAFE. 'K' (U+212A, E2 84 AA) → 'k' (U+006B, 6B).
            // - Western/Central/Viet: SAFE. Kelvin sign detected in haystack.
            case 'k': safety |= central_viet_group | western_group; break;

            // 'a':
            // - Strict/Central/Viet: Contextual. Can't be last; can't precede 'ʾ' (U+02BE, CA BE).
            //   Avoids: 'ẚ' (U+1E9A, E1 BA 9A) → "aʾ" (U+0061 U+02BE, 61 CA BE).
            // - Western: SAFE. Expansion detected in haystack.
            case 'a':
                if (at_end == sz_false_k && next_ascii) safety |= strict_ascii_group | central_viet_group;
                safety |= western_group;
                break;

            // 'h':
            // - Strict/Central/Viet: Contextual. Can't be last; can't precede '̱' (U+0331, CC B1).
            //   Avoids: 'ẖ' (U+1E96, E1 BA 96) → "ẖ" (U+0068 U+0331, 68 CC B1).
            // - Western: SAFE. Expansion detected in haystack.
            case 'h':
                if (at_end == sz_false_k && next_ascii) safety |= strict_ascii_group | central_viet_group;
                safety |= western_group;
                break;

            // 'j':
            // - All: Contextual. Can't be last; can't precede '̌' (U+030C).
            //   Avoids: 'ǰ' (U+01F0) → "ǰ" (U+006A U+030C, 6A CC 8C).
            //   Western profile does NOT detect this in haystack scan.
            case 'j':
                if (at_end == sz_false_k && next_ascii)
                    safety |= strict_ascii_group | central_viet_group | western_group;
                break;

            // 'w':
            // - Strict/Central/Viet: Contextual. Can't be last; can't precede '̊' (U+030A).
            //   Avoids: 'ẘ' (U+1E98) → "ẘ" (U+0077 U+030A, 77 CC 8A).
            // - Western: SAFE. Expansion detected in haystack.
            case 'w':
                if (at_end == sz_false_k && next_ascii) safety |= strict_ascii_group | central_viet_group;
                safety |= western_group;
                break;

            // 'y':
            // - Strict/Central/Viet: Contextual. Can't be last; can't precede '̊' (U+030A).
            //   Avoids: 'ẙ' (U+1E99) → "ẙ" (U+0079 U+030A, 79 CC 8A).
            // - Western: SAFE. Expansion detected in haystack.
            case 'y':
                if (at_end == sz_false_k && next_ascii) safety |= strict_ascii_group | central_viet_group;
                safety |= western_group;
                break;

            // 'n':
            // - ASCII/Cyrillic/Greek: Contextual. Can't be first; can't follow 'ʼ' (U+02BC, CA BC).
            //   Avoids: 'ŉ' (U+0149, C5 89) → "ʼn" (U+02BC U+006E, CA BC 6E).
            // - Armenian: UNSAFE. Armenian kernel cannot handle 'ŉ' (U+0149, C5 89) → "ʼn" (U+02BC U+006E, CA BC 6E).
            //   The character 'n' can match the 2nd part of the expansion, causing false positives.
            // - Western/Central/Viet: Contextual, same as above.
            //   Western profile does NOT detect this in haystack scan.
            case 'n':
                // Exclude Armenian - it cannot handle 'ŉ' (U+0149, C5 89) → "ʼn" (U+02BC U+006E, CA BC 6E)
                if (at_start == sz_false_k && prev_ascii) {
                    safety |= (1 << sz_utf8_uncased_rune_ascii_invariant_k) | //
                              (1 << sz_utf8_uncased_rune_safe_cyrillic_k) |   //
                              (1 << sz_utf8_uncased_rune_safe_greek_k);       //
                    // Armenian EXCLUDED: sz_utf8_uncased_rune_safe_armenian_k
                    safety |= central_viet_group | western_group;
                }
                break;

            // 'i':
            // - All: Contextual. Can't be first or last; can't follow 'f'; can't precede '̇' (U+0307, CC 87).
            //   Avoids: 'İ' (U+0130, C4 B0) → "i̇" (U+0069 U+0307, 69 CC 87),
            //   and 'ﬁ' (U+FB01, EF AC 81) → "fi" (U+0066 U+0069, 66 69).
            //   Western profile does NOT detect Turkish 'İ' expansion.
            case 'i':
                if (at_start == sz_false_k && at_end == sz_false_k && next_ascii && lower_prev != 'f')
                    safety |= strict_ascii_group | central_viet_group | western_group;
                break;

            // 'l':
            // - Strict/Central/Viet: Contextual. Can't be first; can't follow 'f'.
            //   Avoids: 'ﬂ' (U+FB02, EF AC 82) → "fl" (U+0066 U+006C, 66 6C).
            // - Western: SAFE. Ligatures detected in haystack.
            case 'l':
                if (at_start == sz_false_k && lower_prev != 'f') safety |= strict_ascii_group | central_viet_group;
                safety |= western_group;
                break;

            // 't':
            // - Strict/Central/Viet: Contextual. Can't be first/last; can't follow 's';
            //   can't precede '̈' (U+0308, CC 88).
            //   Avoids: 'ﬅ' (U+FB05, EF AC 85) → "st" (U+0073 U+0074, 73 74),
            //   'ﬆ' (U+FB06, EF AC 86) → "st" (U+0073 U+0074, 73 74),
            //   and 'ẗ' (U+1E97, E1 BA 97) → "ẗ" (U+0074 U+0308, 74 CC 88).
            // - Western: SAFE. Ligatures/expansion detected in haystack.
            case 't':
                if (at_start == sz_false_k && at_end == sz_false_k && next_ascii && lower_prev != 's')
                    safety |= strict_ascii_group | central_viet_group;
                safety |= western_group;
                break;

            // 'f':
            // - Strict/Central/Viet: Contextual. Can't be first/last; can't follow 'f';
            //   can't precede 'f', 'i', 'l'.
            //   Avoids:
            //   - 'ﬀ' (U+FB00, EF AC 80) → "ff" (U+0066 U+0066, 66 66)
            //   - 'ﬁ' (U+FB01, EF AC 81) → "fi" (U+0066 U+0069, 66 69)
            //   - 'ﬂ' (U+FB02, EF AC 82) → "fl" (U+0066 U+006C, 66 6C)
            //   - 'ﬃ' (U+FB03, EF AC 83) → "ffi" (U+0066 U+0066 U+0069, 66 66 69)
            //   - 'ﬄ' (U+FB04, EF AC 84) → "ffl" (U+0066 U+0066 U+006C, 66 66 6C)
            // - Western: SAFE. Ligatures detected in haystack.
            case 'f':
                if (at_start == sz_false_k && at_end == sz_false_k && prev_ascii && next_ascii && lower_prev != 'f' &&
                    lower_next != 'f' && lower_next != 'i' && lower_next != 'l')
                    safety |= strict_ascii_group | central_viet_group;
                safety |= western_group;
                break;

            // 's'
            // - Strict: UNSAFE. 'ſ' (U+017F, C5 BF) → 's' (U+0073, 73).
            // - Central/Vietnamese: Contextual. Can't be first/last; can't be adjacent to 's'/'t'.
            //   Avoids: 'ß' (U+00DF, C3 9F) → "ss" (U+0073 U+0073, 73 73),
            //   'ﬅ' (U+FB05, EF AC 85) → "st" (U+0073 U+0074, 73 74),
            //   'ﬆ' (U+FB06, EF AC 86) → "st" (U+0073 U+0074, 73 74), and 'ſ' (U+017F, C5 BF) → 's' (U+0073, 73).
            // - Western: Contextual. Can't be at positions 0, 1 (if prev='s'), N-1, or N-2 (if next='s').
            //   Avoids mid-ß-expansion matches: 'ß' (U+00DF, C3 9F) → "ss" (U+0073 U+0073, 73 73) in-place means
            //   needle with 's' at these positions could match at byte offset 1 (UTF-8 continuation byte 0x9F).
            //   Example: "ßStra" → "ssstra", needle "sstra" matches at pos 1 = mid-character.
            //   Interior 's' like "tesst" or "masse" are safe for SIMD.
            case 's':
                if (at_start == sz_false_k && at_end == sz_false_k && prev_ascii && next_ascii && lower_prev != 's' &&
                    lower_next != 's' && lower_next != 't')
                    safety |= central_viet_group;
                // Western: ban pos 0, pos 1 if prev='s', pos N-1, pos N-2 if next='s'
                if (at_start == sz_false_k && at_end == sz_false_k && //
                    !(at_pos_1 == sz_true_k && lower_prev == 's') &&  //
                    !(at_pos_n_minus_2 == sz_true_k && lower_next == 's'))
                    safety |= western_group;
                break;

            default:
                // Should not happen for a-z
                safety |= strict_ascii_group | central_viet_group | western_group;
                break;
            }
        }
        else {
            // Non-letters (digits, punctuation, whitespace) - always safe for all profiles
            safety |= strict_ascii_group | central_viet_group | western_group;
        }

        *safety_profiles = safety;
        return sz_utf8_uncased_rune_ascii_invariant_k;
    }

    // 2-byte UTF-8 (U+0080 to U+07FF)
    // Must check EXACT ranges that the fold functions handle, not just lead bytes
    if (rune_bytes == 2) {
        sz_u8_t lead = (rune >> 6) | 0xC0;     // Reconstruct lead byte
        sz_u8_t second = (rune & 0x3F) | 0x80; // Reconstruct continuation byte

        // Latin-1 Supplement (C2/C3 lead bytes)
        // Exclude: 'å' (U+00E5, C3 A5) - Angstrom Sign 'Å' (U+212B, E2 84 AB) → 'å' (U+00E5, C3 A5) also folds to it
        if (lead == 0xC2 || lead == 0xC3) {
            if (rune == 0x00E5) {
                // 'å' excluded from all Latin profiles due to Angstrom ambiguity
            }
            else if (rune == 0x00DF) {
                // 'ß' excluded from Central Europe and Vietnamese, allowed in Western Europe
                safety |= western_group;
            }
            else if (rune == 0x00B5) {
                // 'µ' (U+00B5, C2 B5) → 'μ' (U+03BC, CE BC).
                // Allow only the Greek SIMD path; Latin paths remain unsafe.
                safety |= (1 << sz_utf8_uncased_rune_safe_greek_k);
            }
            else { safety |= western_group | central_viet_group; }
        }

        // Latin Extended-A (C4/C5 lead bytes) - for central_europe and vietnamese
        if (lead == 0xC4 || lead == 0xC5) {
            // Exclude expansions/length-changes:
            // - 'İ' (U+0130, C4 B0) → "i̇" (U+0069 U+0307, 69 CC 87)
            // - 'ŉ' (U+0149, C5 89) → "ʼn" (U+02BC U+006E, CA BC 6E)
            // - 'ſ' (U+017F, C5 BF) → 's' (U+0073, 73)
            if (rune != 0x0130 && rune != 0x0149 && rune != 0x017F) { safety |= central_viet_group; }
        }

        // Latin Extended-B (C6 lead byte) - for vietnamese (supports ơ/ư)
        if (lead == 0xC6) { safety |= (1 << sz_utf8_uncased_rune_safe_vietnamese_k); }

        // Cyrillic - check exact ranges handled by sz_utf8_uncased_search_icelake_cyrillic_fold_zmm_
        // D0 80-BF: U+0400-U+043F (includes uppercase and lowercase)
        // D1 80-9F: U+0440-U+045F (lowercase continuation)
        // Note: D2/D3 Extended Cyrillic BANNED from SIMD kernel - needles with D2/D3 use serial fallback
        if ((lead == 0xD0 && second >= 0x80 && second <= 0xBF) || //
            (lead == 0xD1 && second >= 0x80 && second <= 0x9F)) { //
            safety |= (1 << sz_utf8_uncased_rune_safe_cyrillic_k);
        }

        // Greek - check exact ranges handled by sz_utf8_uncased_search_icelake_greek_fold_zmm_
        // CE 86-8F: accented uppercase Ά-Ώ (with gaps at 87, 8B, 8D)
        //   - EXCLUDE CE 90: 'ΐ' (U+0390) expands to 3 codepoints
        // CE 91-A9: basic uppercase Α-Ω
        // CE AA-AB: dialytika uppercase Ϊ-Ϋ
        // CE AC-AF: accented lowercase ά-ί
        //   - EXCLUDE CE B0: 'ΰ' (U+03B0) expands to 3 codepoints
        // CE B1-BF: basic lowercase α-ο
        // CF 80-89: basic lowercase π-ω (includes final sigma at 82, sigma at 83)
        // CF 8A-8E: accented/dialytika lowercase ϊ-ώ
        if (lead == 0xCE) {
            // Accented uppercase (with gaps) - exclude 87, 8B, 8D, 90
            if ((second >= 0x86 && second <= 0x8F) && second != 0x87 && second != 0x8B && second != 0x8D &&
                second != 0x90) {
                safety |= (1 << sz_utf8_uncased_rune_safe_greek_k);
            }
            // Basic uppercase Α-Ω
            if (second >= 0x91 && second <= 0xA9) { safety |= (1 << sz_utf8_uncased_rune_safe_greek_k); }
            // Dialytika uppercase Ϊ-Ϋ
            if (second >= 0xAA && second <= 0xAB) { safety |= (1 << sz_utf8_uncased_rune_safe_greek_k); }
            // Accented lowercase ά-ί
            if (second >= 0xAC && second <= 0xAF) { safety |= (1 << sz_utf8_uncased_rune_safe_greek_k); }
            // Basic lowercase α-ο - exclude B0 (ΰ expands)
            if (second >= 0xB1 && second <= 0xBF) { safety |= (1 << sz_utf8_uncased_rune_safe_greek_k); }
        }
        if (lead == 0xCF) {
            // Basic lowercase π-ω
            if (second >= 0x80 && second <= 0x89) { safety |= (1 << sz_utf8_uncased_rune_safe_greek_k); }
            // Accented/dialytika lowercase ϊ-ώ
            if (second >= 0x8A && second <= 0x8E) { safety |= (1 << sz_utf8_uncased_rune_safe_greek_k); }
        }

        // Armenian - check exact ranges with contextual constraints for ligatures
        // D4 B1-BF: uppercase Ա-Ի (U+0531-U+053F)
        // D5 80-96: uppercase Լ-Ֆ (U+0540-U+0556)
        // D5 A1-BF: lowercase ա-տ (U+0561-U+057F)
        // D6 80-86: lowercase ր-ֆ (U+0580-U+0586)
        //
        // Ligature constraints (from spec):
        // - 'ե' (U+0565): can't be first; can't follow 'մ'; can't precede 'ւ'
        // - 'ւ' (U+0582): can't be last; can't follow 'ե'
        // - 'մ' (U+0574): can't be last; can't precede 'ն', 'ե', 'ի', 'խ'
        // - 'ն' (U+0576): can't be first; can't follow 'մ', 'վ'
        // - 'ի' (U+056B): can't be first; can't follow 'մ'
        // - 'վ' (U+057E): can't be first; can't precede 'ն'
        // - 'խ' (U+056D): can't be first; can't follow 'մ'
        {
            sz_bool_t is_armenian_range = sz_false_k;
            sz_bool_t armenian_safe = sz_true_k;

            if ((lead == 0xD4 && second >= 0xB1 && second <= 0xBF) ||
                (lead == 0xD5 && second >= 0x80 && second <= 0x96) ||
                (lead == 0xD5 && second >= 0xA1 && second <= 0xBF) ||
                (lead == 0xD6 && second >= 0x80 && second <= 0x86)) {
                is_armenian_range = sz_true_k;

                // Helper: get lowercase Armenian codepoint for neighbor checks
                sz_rune_t lower_prev_arm = prev_rune;
                sz_rune_t lower_next_arm = next_rune;
                if (prev_rune >= 0x0531 && prev_rune <= 0x0556) lower_prev_arm = prev_rune + 0x30;
                if (next_rune >= 0x0531 && next_rune <= 0x0556) lower_next_arm = next_rune + 0x30;

                // Check ligature constraints
                switch (rune) {
                case 0x0565: // U+0565 ech - can't be first; can't follow U+0574 men; can't precede U+0582 yiwn
                case 0x0535: // U+0535 Ech uppercase
                    if (at_start || lower_prev_arm == 0x0574 || lower_next_arm == 0x0582) armenian_safe = sz_false_k;
                    break;
                case 0x0582: // U+0582 yiwn - can't be first; can't be last; can't follow U+0565 ech
                    // Armenian ligature և (U+0587) → ech + yiwn; needle starting with yiwn matches mid-expansion
                    if (at_start || at_end || lower_prev_arm == 0x0565) armenian_safe = sz_false_k;
                    break;
                case 0x0574: // U+0574 men - can't be last; can't precede U+0576, U+0565, U+056B, U+056D
                case 0x0544: // U+0544 Men uppercase
                    if (at_end || lower_next_arm == 0x0576 || lower_next_arm == 0x0565 || lower_next_arm == 0x056B ||
                        lower_next_arm == 0x056D)
                        armenian_safe = sz_false_k;
                    break;
                case 0x0576: // U+0576 now - can't be first; can't follow U+0574 men, U+057E vew
                case 0x0546: // U+0546 Now uppercase
                    if (at_start || lower_prev_arm == 0x0574 || lower_prev_arm == 0x057E) armenian_safe = sz_false_k;
                    break;
                case 0x056B: // U+056B ini - can't be first; can't follow U+0574 men
                case 0x053B: // U+053B Ini uppercase
                    if (at_start || lower_prev_arm == 0x0574) armenian_safe = sz_false_k;
                    break;
                case 0x057E: // U+057E vew - can't be last; can't precede U+0576 now
                case 0x054E: // U+054E Vew uppercase
                    if (at_end || lower_next_arm == 0x0576) armenian_safe = sz_false_k;
                    break;
                case 0x056D: // U+056D xeh - can't be first; can't follow U+0574 men
                case 0x053D: // U+053D Xeh uppercase
                    if (at_start || lower_prev_arm == 0x0574) armenian_safe = sz_false_k;
                    break;
                default: break;
                }
            }

            if (is_armenian_range && armenian_safe) { safety |= (1 << sz_utf8_uncased_rune_safe_armenian_k); }
        }

        // Output safety and determine primary script for 2-byte runes
        // For case-invariant non-ASCII runes, add the ASCII-invariant bit.
        // This enables fast ASCII kernel for needles like "中文字" that contain no cased characters.
        // ASCII fold only affects bytes 0x41-0x5A (A-Z), so all other bytes pass through unchanged.
        if (sz_rune_is_uncased_(rune)) safety |= (1 << sz_utf8_uncased_rune_ascii_invariant_k);
        *safety_profiles = safety;
        if (rune >= 0x0080 && rune <= 0x00FF) return sz_utf8_uncased_rune_safe_western_europe_k; // Latin-1 Supplement
        if (rune >= 0x0100 && rune <= 0x024F) return sz_utf8_uncased_rune_safe_central_europe_k; // Latin Extended-A/B
        if (rune >= 0x0370 && rune <= 0x03FF) return sz_utf8_uncased_rune_safe_greek_k;          // Greek
        if (rune >= 0x0400 && rune <= 0x04FF) return sz_utf8_uncased_rune_safe_cyrillic_k;       // Cyrillic
        if (rune >= 0x0530 && rune <= 0x058F) return sz_utf8_uncased_rune_safe_armenian_k;       // Armenian
        return sz_utf8_uncased_rune_invariant_k;
    }

    // 3-byte UTF-8 (U+0800 to U+FFFF)
    if (rune_bytes == 3) {
        sz_u8_t lead = (rune >> 12) | 0xE0;
        sz_u8_t second = ((rune >> 6) & 0x3F) | 0x80;
        sz_u8_t third = (rune & 0x3F) | 0x80;

        // Vietnamese/Latin Extended Additional (E1 B8-BB range)
        // U+1E00-U+1EFF maps to E1 B8 80 - E1 BB BF
        if (lead == 0xE1 && (second >= 0xB8 && second <= 0xBB)) {
            // Need detailed check for exclusions in U+1E96-U+1E9F
            // 1E96-1E9F: E1 BA 96 - E1 BA 9F
            if (second == 0xBA && third >= 0x96 && third <= 0x9F) {
                // Excluded: expansions or irregulars
            }
            else { safety |= (1 << sz_utf8_uncased_rune_safe_vietnamese_k); }
        }

        // Georgian Mkhedruli (E1 83 90-BF range)
        // U+10D0-U+10FF maps to E1 83 90 - E1 83 BF
        // Mkhedruli is caseless, so all characters are safe for the Georgian kernel.
        if (lead == 0xE1 && second == 0x83 && third >= 0x90) { safety |= (1 << sz_utf8_uncased_rune_safe_georgian_k); }

        // Output safety and determine primary script for 3-byte runes
        // For case-invariant non-ASCII runes (like CJK), add the ASCII-invariant bit.
        if (sz_rune_is_uncased_(rune)) safety |= (1 << sz_utf8_uncased_rune_ascii_invariant_k);
        *safety_profiles = safety;
        if (rune >= 0x10D0 && rune <= 0x10FF) return sz_utf8_uncased_rune_safe_georgian_k; // Georgian Mkhedruli
        if (rune >= 0x1E00 && rune <= 0x1EFF)
            return sz_utf8_uncased_rune_safe_vietnamese_k; // Latin Extended Additional
        return sz_utf8_uncased_rune_invariant_k;
    }

    // 4-byte UTF-8 - currently no fast paths, but case-invariant 4-byte runes can use ASCII kernel
    if (sz_rune_is_uncased_(rune)) safety |= (1 << sz_utf8_uncased_rune_ascii_invariant_k);
    *safety_profiles = safety;
    return sz_utf8_uncased_rune_invariant_k;
}

/**
 *  @brief Compute diversity score for a byte sequence.
 *
 *  Uses a 256-bit bitmap to efficiently count distinct byte values.
 *  Higher scores indicate more diverse byte values, which lead to better
 *  filtering during SIMD search (fewer false positives).
 *
 *  @param data Pointer to byte sequence.
 *  @param length Length of byte sequence.
 *  @return Count of distinct byte values (0-256).
 */
SZ_HELPER_AUTO sz_size_t sz_utf8_probe_diversity_score_(sz_u8_t const *data, sz_size_t length) {
    if (length <= 1) return length;
    sz_u64_t seen[4] = {0, 0, 0, 0}; // 256-bit bitmap
    sz_size_t distinct = 0;
    for (sz_size_t byte_index = 0; byte_index < length; ++byte_index) {
        sz_u8_t byte = data[byte_index];
        sz_size_t word = byte >> 6;                // Which 64-bit word (0-3)
        sz_u64_t bit = (sz_u64_t)1 << (byte & 63); // Bit within the word
        if (!(seen[word] & bit)) {
            seen[word] |= bit;
            ++distinct;
        }
    }
    return distinct;
}

/**
 *  @brief Find the "best safe window" in the needle for each script path.
 *
 *  The objective is as follows. For a given needle, find a slice, that when folded fits into 16 bytes
 *  and where all characters are "safe" with respect to a certain path. If no such path can be found,
 *  an empty result is returned. It might be the case for a search query like "s" or "n", that by itself
 *  isn't safe for any path given the number of Unicode characters expanding into multiple 's'- or 'n'-containing
 *  sequences. The selected safe folded slice will never begin mid-character in the needle, so if it starts with
 *  an 'ŉ' (U+0149, C5 89), we can't choose - 'n' (6E) - the second half of its folded sequence as a starting point.
 *
 *  The algorithm is as follows. Iterate through the arbitrary-case "ŉEeDlE_WITH_LONG_SUFFIX", unpacking runes.
 *  For each input rune, perform folding, expanding into a sequence, like:
 *
 *      'ŉ' (U+0149, C5 89) → "ʼn" (U+02BC U+006E, CA BC 6E).
 *
 *  Continue unpacking the rest, until we reach a 16-byte limit, like:
 *
 *      ʼ     n  e  e  d  l  e  _  w  i  t  h  _  l  o  n  g
 *      CA BC 6E 45 45 44 4C 45 5F 57 49 54 48 5F 4C 4F 4E 47
 *
 *  At this point, we need to trim it to make sure - its characters satisfy boundary conditions.
 *  Assuming at the next step we'll move the iterator to the next input rune to point to 'E' (U+0045) input character,
 *  we only trim from the end. But also invalidate the whole starting position if a bad character is chosen at start.
 *  For safe window starting position we can have multiple length variants, assuming different safe paths can have
 *  different rules for the last symbol in the safe sequence.
 *
 *  Once we have safe window for a certain script, we evaluate its diversity score - the number of distinct byte
 *  values in the folded window. The more diverse - the better! We keep track of best seen window for each script.
 *
 *  We also track not only the safety with respect to a certain profile, but also applicability. For example,
 *  the needle "xyz" is safe with respect to the Western European path, as well as Central European, Vietnamese,
 *  and potentially others. But it's pure ASCII. We shouldn't pay the cost of complex Vietnamese case-folding of
 *  triple-byte Latin extensions for just "xyz". So we must invalidate the "safe path" if its just "safe", but
 *  not ideal.
 *
 *  In the end, we'll have up to 7 best safe windows, one per script path.
 *  The heuristic is:
 *
 *  - Prefer ASCII, if there is an ASCII-safe path at least 4 bytes wide with at least 4 distinct byte values.
 *    It's only one subtraction, a comparison, and a masked addition. Cheapest of all kernels.
 *  - Pick the most diverse variant from all others, if ASCII variant isn't good enough.
 *
 *  We then identify the four "probe" positions within the <= 16 byte folded safe window, one more than
 *  in exact substring search kernels with Raita heuristics:
 *
 *  1. implicit at `refined->folded_slice[0]`
 *  2. stored in `refined->probe_second` - targets last byte of 2nd character when 4+ chars
 *  3. stored in `refined->probe_third` - targets last byte of 3rd character when 4+ chars
 *  4. implicit at `refined->folded_slice[refined->folded_slice_length - 1]`
 *
 *  By aiming at the last byte of each UTF-8 codepoint we maximize diversity, as in a Russian text almost
 *  all letters will have the same first byte, but mostly different second byte. The same is true for many
 *  other languages. For short strings (< 4 bytes), probes will necessarily overlap - this is expected.
 *  The function also sets `offset_in_unfolded` and `length_in_unfolded` to track where the
 *  selected folded slice came from in the original unfolded input.
 *
 *  @param needle Pointer to needle string (original, not folded).
 *  @param needle_length Length in bytes.
 *  @param refined Output metadata structure to populate.
 */
SZ_HELPER_AUTO void sz_utf8_uncased_needle_metadata_(sz_cptr_t needle, sz_size_t needle_length, //
                                                     sz_utf8_uncased_needle_metadata_t *refined) {

    // Per-script window state during iteration
    typedef struct {
        sz_size_t start_offset;   // Byte offset in original needle
        sz_size_t input_length;   // Bytes consumed from original needle
        sz_u8_t folded_bytes[16]; // Folded content
        sz_size_t folded_length;  // Length of folded content (bytes)
        sz_bool_t applicable;     // Has >=1 primary-script character
        sz_bool_t broken;         // Window continuity broken - skip further extension
        sz_size_t diversity;      // Distinct byte count (computed at end of each starting position)
    } script_window_t_;

    // Number of script kernels (indices 1-8 used, index 0 reserved)
    sz_size_t const num_scripts = 9;

    // Best window found so far for each script
    script_window_t_ best[9];
    for (sz_size_t script_index = 0; script_index < num_scripts; ++script_index) {
        best[script_index].start_offset = 0;
        best[script_index].input_length = 0;
        best[script_index].folded_length = 0;
        best[script_index].applicable = sz_false_k;
        best[script_index].broken = sz_false_k;
        best[script_index].diversity = 0;
    }

    // Handle empty needle
    if (needle_length == 0) {
        refined->kernel_id = sz_utf8_uncased_rune_fallback_serial_k;
        refined->offset_in_unfolded = 0;
        refined->length_in_unfolded = 0;
        refined->folded_slice_length = 0;
        refined->probe_second = 0;
        refined->probe_third = 0;
        return;
    }

    // A needle containing any byte that does not begin a well-formed codepoint cannot be window-analyzed by the
    // unchecked decode below; route it to the serial kernel, which handles malformed bytes losslessly (each is
    // folded to itself and resyncs by one byte), keeping SIMD and serial results identical.
    if (sz_utf8_find_malformed(needle, needle_length) != SZ_NULL_CHAR) {
        refined->kernel_id = sz_utf8_uncased_rune_fallback_serial_k;
        refined->offset_in_unfolded = 0;
        refined->length_in_unfolded = 0;
        refined->folded_slice_length = 0;
        refined->probe_second = 0;
        refined->probe_third = 0;
        return;
    }

    sz_u8_t const *needle_start = (sz_u8_t const *)needle;
    sz_u8_t const *needle_end = needle_start + needle_length;

    // Iterate through each starting position in the needle (stepping by rune)
    for (sz_u8_t const *needle_cursor = needle_start; needle_cursor < needle_end;) {
        // Current window being built for each script at this starting position
        script_window_t_ current[9];
        for (sz_size_t script_index = 0; script_index < num_scripts; ++script_index) {
            current[script_index].start_offset = (sz_size_t)(needle_cursor - needle_start);
            current[script_index].input_length = 0;
            current[script_index].folded_length = 0;
            current[script_index].applicable = sz_false_k;
            current[script_index].broken = sz_false_k;
            current[script_index].diversity = 0;
        }

        // Track context for safety profile evaluation
        sz_rune_t prev_prev_rune = 0;
        sz_rune_t prev_rune = 0;

        // Fold forward from needle_cursor until 16 bytes or needle end
        sz_u8_t const *position = needle_cursor;
        sz_bool_t any_active = sz_true_k;

        while (position < needle_end && any_active) {
            // Parse current rune
            sz_rune_t rune;
            sz_rune_length_t const rune_bytes = sz_rune_decode_unchecked((sz_cptr_t)position, &rune);
            if (position + rune_bytes > needle_end) break; // Incomplete rune

            // Parse next rune for context (if available)
            sz_rune_t next_rune = 0;
            sz_rune_length_t next_bytes = sz_rune_invalid_k;
            if (position + rune_bytes < needle_end) {
                next_bytes = sz_rune_decode_unchecked((sz_cptr_t)(position + rune_bytes), &next_rune);
                if (position + rune_bytes + next_bytes > needle_end) next_rune = 0;
            }

            // Parse next-next rune for context
            sz_rune_t next_next_rune = 0;
            if (next_rune != 0 && position + rune_bytes + next_bytes < needle_end) {
                sz_rune_length_t const next_next_bytes = sz_rune_decode_unchecked(
                    (sz_cptr_t)(position + rune_bytes + next_bytes), &next_next_rune);
                if (position + rune_bytes + next_bytes + next_next_bytes > needle_end) next_next_rune = 0;
            }

            // Get safety mask and primary script for this rune
            unsigned safety_mask = 0;
            sz_utf8_uncased_rune_safety_profile_t primary_script = sz_utf8_uncased_rune_safety_profile_( //
                rune, rune_bytes, prev_rune, next_rune, prev_prev_rune, next_next_rune, &safety_mask);

            // Fold this rune
            sz_rune_t folded_runes[4];
            sz_size_t folded_count = sz_unicode_fold_codepoint_(rune, folded_runes);

            // Convert folded runes to UTF-8 bytes
            sz_u8_t folded_utf8[16];
            sz_size_t folded_utf8_length = 0;
            for (sz_size_t rune_index = 0; rune_index < folded_count; ++rune_index) {
                folded_utf8_length += sz_rune_encode(folded_runes[rune_index], folded_utf8 + folded_utf8_length);
            }

            // Update each script's window
            any_active = sz_false_k;
            for (sz_size_t script_index = 1; script_index < num_scripts; ++script_index) {
                if (current[script_index].broken) continue;

                // Check if this rune is safe for this script
                sz_bool_t is_safe = (safety_mask & (1u << script_index)) ? sz_true_k : sz_false_k;

                // Check if adding this rune would exceed 16 bytes
                if (is_safe && current[script_index].folded_length + folded_utf8_length <= 16) {
                    // Extend this script's window
                    for (sz_size_t byte_index = 0; byte_index < folded_utf8_length; ++byte_index) {
                        current[script_index].folded_bytes[current[script_index].folded_length + byte_index] =
                            folded_utf8[byte_index];
                    }
                    current[script_index].folded_length += folded_utf8_length;
                    current[script_index].input_length += rune_bytes;

                    // Mark as applicable if primary script matches
                    if (primary_script == script_index) { current[script_index].applicable = sz_true_k; }
                    any_active = sz_true_k;
                }
                else {
                    // Window broken for this script
                    current[script_index].broken = sz_true_k;
                }
            }

            // Update context for next iteration
            prev_prev_rune = prev_rune;
            prev_rune = rune;
            position += rune_bytes;
        }

        // Compare current to best for each script
        for (sz_size_t script_index = 1; script_index < num_scripts; ++script_index) {
            if (!current[script_index].applicable || current[script_index].folded_length == 0) continue;

            // Compute diversity score
            current[script_index].diversity = sz_utf8_probe_diversity_score_(current[script_index].folded_bytes,
                                                                             current[script_index].folded_length);

            // Update best if this is better (prefer higher diversity, then longer length)
            if (current[script_index].diversity > best[script_index].diversity ||
                (current[script_index].diversity == best[script_index].diversity &&
                 current[script_index].folded_length > best[script_index].folded_length)) {
                best[script_index] = current[script_index];
            }
        }

        // Advance to next rune for next starting position
        sz_rune_t skip_rune;
        sz_rune_length_t const skip_length = sz_rune_decode_unchecked((sz_cptr_t)needle_cursor, &skip_rune);
        needle_cursor += skip_length;
    }

    // Select final kernel based on best windows
    // Rule: Prefer ASCII if >=4 bytes with >=4 diversity; otherwise pick most diverse applicable
    sz_size_t chosen_script = 0;
    sz_size_t best_diversity = 0;

    // Check ASCII preference
    if (best[sz_utf8_uncased_rune_ascii_invariant_k].applicable &&
        best[sz_utf8_uncased_rune_ascii_invariant_k].folded_length >= 4 &&
        best[sz_utf8_uncased_rune_ascii_invariant_k].diversity >= 4) {
        chosen_script = sz_utf8_uncased_rune_ascii_invariant_k;
    }
    else {
        // Find most diverse applicable script
        for (sz_size_t script_index = 1; script_index < num_scripts; ++script_index) {
            if (best[script_index].applicable && best[script_index].diversity > best_diversity) {
                best_diversity = best[script_index].diversity;
                chosen_script = script_index;
            }
        }
    }

    // If no applicable window found, fall back to serial
    if (chosen_script == 0) {
        refined->kernel_id = sz_utf8_uncased_rune_fallback_serial_k;
        refined->offset_in_unfolded = 0;
        refined->length_in_unfolded = 0;
        refined->folded_slice_length = 0;
        refined->probe_second = 0;
        refined->probe_third = 0;
        return;
    }

    // Populate output metadata
    refined->kernel_id = (sz_u8_t)chosen_script;
    refined->offset_in_unfolded = best[chosen_script].start_offset;
    refined->length_in_unfolded = best[chosen_script].input_length;
    refined->folded_slice_length = (sz_u8_t)best[chosen_script].folded_length;

    // Copy folded bytes
    for (sz_size_t byte_index = 0; byte_index < best[chosen_script].folded_length; ++byte_index) {
        refined->folded_slice[byte_index] = best[chosen_script].folded_bytes[byte_index];
    }

    // Compute probe positions - target last bytes of UTF-8 codepoints for maximum diversity
    sz_size_t folded_length = best[chosen_script].folded_length;
    if (folded_length == 0) {
        refined->probe_second = 0;
        refined->probe_third = 0;
        return;
    }

    // Find character end positions in the folded slice
    // A byte is a character's last byte if the next byte is a UTF-8 leader (not continuation)
    sz_size_t char_ends[16];
    sz_size_t char_count = 0;
    for (sz_size_t byte_index = 0; byte_index < folded_length; ++byte_index) {
        sz_u8_t next = (byte_index + 1 < folded_length) ? refined->folded_slice[byte_index + 1]
                                                        : 0xC0; // Fake leader at end
        if ((next & 0xC0) != 0x80) {                            // Next is not a continuation byte
            if (char_count < 16) char_ends[char_count++] = byte_index;
        }
    }

    // Determine probe positions
    if (char_count >= 4) {
        // 4+ characters: target last bytes of 2nd and 3rd characters
        refined->probe_second = (sz_u8_t)char_ends[1];
        refined->probe_third = (sz_u8_t)char_ends[2];
    }
    else if (folded_length <= 3) {
        // Very short: probes overlap
        refined->probe_second = (folded_length > 1) ? 1 : 0;
        refined->probe_third = (folded_length > 1) ? 1 : 0;
    }
    else {
        // 1-3 characters but 4+ bytes: use byte diversity search
        sz_u8_t byte_first = refined->folded_slice[0];
        sz_u8_t byte_last = refined->folded_slice[folded_length - 1];

        sz_size_t probe_second = folded_length / 3;
        sz_size_t probe_third = (folded_length * 2) / 3;

        // Try to find positions with bytes distinct from first/last
        for (sz_size_t byte_index = 1; byte_index < folded_length - 1; ++byte_index) {
            if (refined->folded_slice[byte_index] != byte_first && refined->folded_slice[byte_index] != byte_last) {
                probe_second = byte_index;
                break;
            }
        }

        sz_u8_t byte_second = refined->folded_slice[probe_second];
        for (sz_size_t byte_index = probe_second + 1; byte_index < folded_length - 1; ++byte_index) {
            if (refined->folded_slice[byte_index] != byte_first && refined->folded_slice[byte_index] != byte_last &&
                refined->folded_slice[byte_index] != byte_second) {
                probe_third = byte_index;
                break;
            }
        }

        // Clamp bounds
        if (probe_second == 0) probe_second = 1;
        if (probe_third >= folded_length - 1) probe_third = folded_length - 2;
        if (probe_third <= probe_second && probe_second + 1 < folded_length - 1) probe_third = probe_second + 1;

        refined->probe_second = (sz_u8_t)probe_second;
        refined->probe_third = (sz_u8_t)probe_third;
    }
}



/* ASCII folding primitive used by every NEON script kernel. */
SZ_HELPER_AUTO uint8x16_t sz_utf8_fold_neon_ascii_(uint8x16_t source) {
    uint8x16_t uppercase = vcltq_u8(vsubq_u8(source, vdupq_n_u8('A')), vdupq_n_u8(26));
    return vaddq_u8(source, vandq_u8(uppercase, vdupq_n_u8(0x20)));
}


#if defined(__clang__)
#pragma clang attribute push(__attribute__((target("+simd"))), apply_to = function)
#elif defined(__GNUC__)
#pragma GCC push_options
#pragma GCC target("+simd")
#endif


/**
 *  @brief Collapses two 16-byte comparison results into a 32-bit movemask, one bit per byte.
 *      Lane `i` of the low register sets bit `i`, lane `j` of the high register sets bit `16 + j`.
 *
 *  NEON has no `VPMOVMSKB`, so each register is AND-ed with the per-lane bit weights
 *  {1, 2, 4, ..., 128} repeated across the two 8-byte halves, then `vaddv_u8` horizontally sums
 *  each half into its 8 packed bits. Unlike the `vshrn` nibble trick (4 bits per byte), this
 *  yields exactly ONE bit per byte, so the probe filter's scalar shifts by runtime probe offsets
 *  port from the AVX2 driver unchanged.
 */
SZ_HELPER_INLINE sz_u32_t sz_utf8_uncased_neon_movemask_u8x16x2_(uint8x16_t low_cmp_u8x16, uint8x16_t high_cmp_u8x16) {
    // MSVC's ARM64 `uint8x16_t` is a `__n128` struct that rejects scalar brace-init, so load from `.rodata`.
    static sz_u8_t const bit_weights[16] = {1, 2, 4, 8, 16, 32, 64, 128, 1, 2, 4, 8, 16, 32, 64, 128};
    uint8x16_t const bit_weights_u8x16 = vld1q_u8(bit_weights);
    uint8x16_t low_bits_u8x16 = vandq_u8(low_cmp_u8x16, bit_weights_u8x16);
    uint8x16_t high_bits_u8x16 = vandq_u8(high_cmp_u8x16, bit_weights_u8x16);
    sz_u32_t low_mask = (sz_u32_t)vaddv_u8(vget_low_u8(low_bits_u8x16)) |
                        ((sz_u32_t)vaddv_u8(vget_high_u8(low_bits_u8x16)) << 8);
    sz_u32_t high_mask = (sz_u32_t)vaddv_u8(vget_low_u8(high_bits_u8x16)) |
                         ((sz_u32_t)vaddv_u8(vget_high_u8(high_bits_u8x16)) << 8);
    return low_mask | (high_mask << 16);
}

/**
 *  @brief Detects bytes in the unsigned range [range_start, range_start + range_length).
 *      NEON has unsigned byte compares, so `(x − start) < length` is one wrap-around subtraction
 *      plus one `VCLT`: bytes below `start` wrap above `length` and drop out.
 */
SZ_HELPER_INLINE uint8x16_t sz_utf8_uncased_neon_in_byte_range_u8x16_(uint8x16_t values_u8x16, sz_u8_t range_start,
                                                                      sz_u8_t range_length) {
    return vcltq_u8(vsubq_u8(values_u8x16, vdupq_n_u8(range_start)), vdupq_n_u8(range_length));
}

/**
 *  @brief Shifts the 32 chunk bytes right by one lane, so lane `i` holds byte `i − 1`. Lane 0 of
 *      the low register receives zero; lane 0 of the high register receives the low register's
 *      lane 15 - the carry across the internal 16-byte boundary. Vector-domain equivalent of
 *      Ice Lake's `k-mask << 1` and AVX2's `VPERM2I128` + `VPALIGNR`.
 *
 *      The zero fill at lane 0 is correct because a real match starts on a character boundary
 *      where the lead byte's fold never needs its predecessor; the candidate-window re-fold uses
 *      the same zero-predecessor convention, so both agree at every real match position.
 */
SZ_HELPER_INLINE uint8x16x2_t sz_utf8_uncased_neon_previous_bytes_u8x16x2_(uint8x16x2_t source_u8x16x2) {
    uint8x16_t const zero_u8x16 = vdupq_n_u8(0x00);
    uint8x16x2_t result_u8x16x2;
    result_u8x16x2.val[0] = vextq_u8(zero_u8x16, source_u8x16x2.val[0], 15);
    result_u8x16x2.val[1] = vextq_u8(source_u8x16x2.val[0], source_u8x16x2.val[1], 15);
    return result_u8x16x2;
}

/**
 *  @brief Shifts the 32 chunk bytes left by one lane, so lane `i` holds byte `i + 1`. Lane 15 of
 *      the high register receives zero; lane 15 of the low register receives the high register's
 *      lane 0. Vector-domain equivalent of Ice Lake's `k-mask >> 1`.
 */
SZ_HELPER_INLINE uint8x16x2_t sz_utf8_uncased_neon_next_bytes_u8x16x2_(uint8x16x2_t source_u8x16x2) {
    uint8x16_t const zero_u8x16 = vdupq_n_u8(0x00);
    uint8x16x2_t result_u8x16x2;
    result_u8x16x2.val[0] = vextq_u8(source_u8x16x2.val[0], source_u8x16x2.val[1], 1);
    result_u8x16x2.val[1] = vextq_u8(source_u8x16x2.val[1], zero_u8x16, 1);
    return result_u8x16x2;
}

/** @brief First N bits set, defined for `n == 32` (where `(1u << n) − 1` is undefined). */
SZ_HELPER_INLINE sz_u32_t sz_utf8_uncased_neon_mask_until_(sz_size_t n) {
    return n >= 32 ? 0xFFFFFFFFu : ((sz_u32_t)1 << n) - 1;
}

/**
 *  @brief Loads up to 32 bytes through a zeroed stack buffer, never touching memory past
 *      `source + length`. The zero padding mirrors Ice Lake's `maskz` loads: zero bytes match no
 *      probe inside a valid window and trip no alarm, so tail chunks reuse the main-loop logic
 *      unchanged instead of branching into a separate epilogue.
 */
SZ_HELPER_AUTO uint8x16x2_t sz_utf8_uncased_neon_load_padded_u8x16x2_(sz_cptr_t source, sz_size_t length) {
    sz_u8_t buffer[32] = {0};
    for (sz_size_t byte_index = 0; byte_index < length; ++byte_index) buffer[byte_index] = (sz_u8_t)source[byte_index];
    return vld1q_u8_x2(buffer);
}

/**
 *  @brief Loads up to 16 bytes for candidate-window verification without over-reading the
 *      haystack: the fast full load is taken whenever 16 bytes remain, and only the last few
 *      candidates near the haystack end pay for the zero-padded stack copy.
 */
SZ_HELPER_AUTO uint8x16_t sz_utf8_uncased_neon_load_window_u8x16_(sz_cptr_t source, sz_size_t available) {
    if (available >= 16) return vld1q_u8((sz_u8_t const *)source);
    sz_u8_t buffer[16] = {0};
    for (sz_size_t byte_index = 0; byte_index < available; ++byte_index)
        buffer[byte_index] = (sz_u8_t)source[byte_index];
    return vld1q_u8(buffer);
}



/**
 *  @brief Fold a 32-byte chunk using ASCII case folding rules.
 *  @sa sz_utf8_uncased_rune_ascii_invariant_k
 */
SZ_HELPER_AUTO uint8x16x2_t sz_utf8_uncased_search_neon_ascii_fold_u8x16x2_(uint8x16x2_t text_u8x16x2) {
    uint8x16x2_t result_u8x16x2;
    // Only fold bytes in range A-Z; the masked add stays branch-free across both registers
    result_u8x16x2.val[0] = sz_utf8_fold_neon_ascii_(text_u8x16x2.val[0]);
    result_u8x16x2.val[1] = sz_utf8_fold_neon_ascii_(text_u8x16x2.val[1]);
    return result_u8x16x2;
}

/**
 *  @brief 3-probe ASCII uncased search over 32-byte chunks.
 *
 *  For needles with folded_slice_length ≤ 3, probes at positions 0, mid, last cover ALL bytes
 *  of the window, so no candidate-window verification is needed - candidates go straight to
 *  head/tail validation. The chunk is folded ONCE and the probe equality masks are shifted as
 *  32-bit movemask integers: with windows ≤ 16 bytes every chunk still exposes ≥ 17 valid start
 *  positions per iteration.
 */
SZ_HELPER_AUTO sz_cptr_t sz_utf8_uncased_search_neon_ascii_3probe_( //
    sz_cptr_t haystack, sz_size_t haystack_length,                  //
    sz_cptr_t needle, sz_size_t needle_length,                      //
    sz_utf8_uncased_needle_metadata_t const *needle_metadata,       //
    sz_size_t *matched_length) {

    sz_size_t const folded_window_length = needle_metadata->folded_slice_length;
    sz_cptr_t const haystack_end = haystack + haystack_length;

    // For ≤3 bytes: positions 0, mid, last cover ALL positions
    // 1-byte: 0=last, 2-byte: 0,1, 3-byte: 0,1,2
    sz_size_t const offset_second = folded_window_length / 2;
    sz_size_t const offset_last = folded_window_length - 1;

    uint8x16_t const probe_first_u8x16 = vdupq_n_u8(needle_metadata->folded_slice[0]);
    uint8x16_t const probe_second_u8x16 = vdupq_n_u8(needle_metadata->folded_slice[offset_second]);
    uint8x16_t const probe_last_u8x16 = vdupq_n_u8(needle_metadata->folded_slice[offset_last]);

    sz_cptr_t haystack_ptr = haystack;
    while (haystack_ptr < haystack_end) {
        sz_size_t const available = (sz_size_t)(haystack_end - haystack_ptr);
        if (available < folded_window_length) break;
        sz_size_t const chunk_size = available < 32 ? available : 32;
        sz_size_t const valid_starts = chunk_size - folded_window_length + 1;

        uint8x16x2_t text_u8x16x2 = available >= 32
                                        ? vld1q_u8_x2((sz_u8_t const *)haystack_ptr)
                                        : sz_utf8_uncased_neon_load_padded_u8x16x2_(haystack_ptr, chunk_size);
        uint8x16x2_t folded_u8x16x2 = sz_utf8_uncased_search_neon_ascii_fold_u8x16x2_(text_u8x16x2);

        sz_u32_t matches = sz_utf8_uncased_neon_movemask_u8x16x2_(vceqq_u8(folded_u8x16x2.val[0], probe_first_u8x16),
                                                                  vceqq_u8(folded_u8x16x2.val[1], probe_first_u8x16));
        matches &= sz_utf8_uncased_neon_movemask_u8x16x2_(vceqq_u8(folded_u8x16x2.val[0], probe_second_u8x16),
                                                          vceqq_u8(folded_u8x16x2.val[1], probe_second_u8x16)) >>
                   offset_second;
        matches &= sz_utf8_uncased_neon_movemask_u8x16x2_(vceqq_u8(folded_u8x16x2.val[0], probe_last_u8x16),
                                                          vceqq_u8(folded_u8x16x2.val[1], probe_last_u8x16)) >>
                   offset_last;
        matches &= sz_utf8_uncased_neon_mask_until_(valid_starts);

        for (; matches; matches &= matches - 1) {
            sz_size_t const candidate_offset = (sz_size_t)sz_u32_ctz(matches);
            sz_cptr_t const haystack_candidate_ptr = haystack_ptr + candidate_offset;

            // No window verification needed - probes cover all positions,
            // go directly to head/tail validation
            sz_utf8_uncased_verify_result_t verified = sz_utf8_uncased_verify_match_(                     //
                haystack, haystack_length,                                                                 //
                needle, needle_length,                                                                     //
                haystack_candidate_ptr - haystack, folded_window_length,                                   //
                needle_metadata->offset_in_unfolded,                                                       //
                needle_length - needle_metadata->offset_in_unfolded - needle_metadata->length_in_unfolded);
            if (verified.match) {
                *matched_length = verified.length;
                return verified.match;
            }
        }
        haystack_ptr += valid_starts;
    }

    return SZ_NULL_CHAR;
}



/** @brief Folds one 32-byte chunk of haystack text using script-specific rules. */
typedef uint8x16x2_t (*sz_utf8_uncased_fold_u8x16x2_t)(uint8x16x2_t text_u8x16x2);

/**
 *  @brief Flags positions of "danger" characters that fold to a different byte width.
 *  @param load_mask Bitmask of the bytes actually loaded from the haystack, for tail-safe range checks.
 */
typedef sz_u32_t (*sz_utf8_uncased_alarm_u8x16x2_t)(uint8x16x2_t text_u8x16x2, sz_u32_t load_mask);

/**
 *  @brief Shared scan loop behind all script-specific uncased searches.
 *
 *  Scans the entire haystack from byte 0, looking for the folded window pattern.
 *  When found, verifies the head (backwards) and tail (forwards) using codepoint-by-codepoint
 *  comparison to handle variable-width folding correctly.
 *
 *  Every per-script kernel is a thin wrapper passing its own @p fold and @p alarm callbacks.
 *  The driver is force-inlined into each wrapper, so the callbacks resolve to direct calls
 *  with no indirect branches in the emitted code.
 *
 *  Tail chunks shorter than 32 bytes are zero-padded through a stack buffer, so they take the
 *  exact main-loop path; and like on Ice Lake, two danger-scan rules preserve correctness for
 *  expanding folds ('ẞ' → "ss"): an alarmed chunk is danger-scanned in FULL (not just its valid
 *  start positions), and once fewer than `folded_window_length` bytes remain, the leftover tail
 *  gets a final danger scan - a haystack span SHORTER than the folded window can still hide a
 *  real match there.
 *
 *  @param fold Script-specific 32-byte case-folding callback.
 *  @param alarm Script-specific danger detection callback, or NULL if the script has no
 *      danger characters: the danger branch disappears and the full step is used.
 */
SZ_HELPER_INLINE sz_cptr_t sz_utf8_uncased_search_neon_scripted_( //
    sz_utf8_uncased_fold_u8x16x2_t fold,                          //
    sz_utf8_uncased_alarm_u8x16x2_t alarm,                        //
    sz_cptr_t haystack, sz_size_t haystack_length,                //
    sz_cptr_t needle, sz_size_t needle_length,                    //
    sz_utf8_uncased_needle_metadata_t const *needle_metadata,     //
    sz_size_t *matched_length) {

    sz_assert_(needle_metadata && "needle_metadata must be provided");
    sz_assert_(needle_metadata->folded_slice_length > 0 && "folded window must be non-empty");

    sz_size_t const folded_window_length = needle_metadata->folded_slice_length;
    sz_cptr_t const haystack_end = haystack + haystack_length;
    sz_assert_(folded_window_length <= 16 && "expect folded needle part to fit in one register");

    // Pre-load folded window into one register; the byte-mask `window_keep` replicates Ice Lake's
    // `maskz` window load: bytes past the window are zeroed BEFORE folding, so a lead byte at the
    // window edge never borrows fold context from haystack bytes outside the window
    sz_u32_t const folded_window_mask = sz_utf8_uncased_neon_mask_until_(folded_window_length);
    uint8x16_t const needle_window_u8x16 = vld1q_u8((sz_u8_t const *)needle_metadata->folded_slice);
    static sz_u8_t const lane_indices[16] = {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15};
    uint8x16_t const lane_indices_u8x16 = vld1q_u8(lane_indices);
    uint8x16_t const window_keep_u8x16 = vcltq_u8(lane_indices_u8x16, vdupq_n_u8((sz_u8_t)folded_window_length));

    // 4 probe positions
    sz_size_t const offset_second = needle_metadata->probe_second;
    sz_size_t const offset_third = needle_metadata->probe_third;
    sz_size_t const offset_last = folded_window_length - 1;

    uint8x16_t const probe_first_u8x16 = vdupq_n_u8(needle_metadata->folded_slice[0]);
    uint8x16_t const probe_second_u8x16 = vdupq_n_u8(needle_metadata->folded_slice[offset_second]);
    uint8x16_t const probe_third_u8x16 = vdupq_n_u8(needle_metadata->folded_slice[offset_third]);
    uint8x16_t const probe_last_u8x16 = vdupq_n_u8(needle_metadata->folded_slice[offset_last]);

    // Pre-load the first folded rune for danger zone matching
    sz_rune_t needle_first_safe_folded_rune = 0;
    if (alarm) sz_rune_decode_unchecked((sz_cptr_t)(needle_metadata->folded_slice), &needle_first_safe_folded_rune);

    sz_cptr_t haystack_ptr = haystack;
    while (haystack_ptr < haystack_end) {
        sz_size_t const available = (sz_size_t)(haystack_end - haystack_ptr);
        if (available < folded_window_length) break;

        sz_size_t const chunk_size = available < 32 ? available : 32;
        sz_size_t const valid_starts = chunk_size - folded_window_length + 1;
        // For danger detection across chunk boundaries, reduce step size to ensure
        // 3-byte patterns at chunk end are fully visible in the next chunk.
        // For tail chunks (valid_starts <= 2), step = 1 ensures progress.
        // Scripts without danger characters advance by the full window count.
        sz_size_t const step = !alarm ? valid_starts : valid_starts > 2 ? valid_starts - 2 : 1;
        sz_u32_t const load_mask = sz_utf8_uncased_neon_mask_until_(chunk_size);
        sz_u32_t const valid_mask = sz_utf8_uncased_neon_mask_until_(valid_starts);

        uint8x16x2_t text_u8x16x2 = available >= 32
                                        ? vld1q_u8_x2((sz_u8_t const *)haystack_ptr)
                                        : sz_utf8_uncased_neon_load_padded_u8x16x2_(haystack_ptr, chunk_size);

        // Check for anomalies (characters that fold to different byte widths)
        if (alarm) {
            sz_u32_t danger_mask = alarm(text_u8x16x2, load_mask);
            if (danger_mask) {
                // The danger zone handler scans for the needle's first safe rune (at offset_in_unfolded).
                // The whole chunk is scanned, not just `valid_starts` positions: an expanding danger
                // character makes the haystack span SHORTER than the folded window, so a real match
                // can start within the window's length of the chunk end.
                sz_cptr_t match = sz_utf8_uncased_search_in_danger_zone_( //
                    haystack, haystack_length,                            //
                    needle, needle_length,                                //
                    haystack_ptr, chunk_size,                             // extended danger zone
                    needle_first_safe_folded_rune,                        // pivot point
                    needle_metadata->offset_in_unfolded,                  // its location in the needle
                    matched_length);
                if (match) return match;
                haystack_ptr += step;
                continue;
            }
        }

        // Fold once, then filter candidates with 4 probe positions: scalar shifts over the
        // 32-bit movemasks substitute Ice Lake's k-mask shifts bit-for-bit
        uint8x16x2_t folded_u8x16x2 = fold(text_u8x16x2);
        sz_u32_t matches = sz_utf8_uncased_neon_movemask_u8x16x2_(vceqq_u8(folded_u8x16x2.val[0], probe_first_u8x16),
                                                                  vceqq_u8(folded_u8x16x2.val[1], probe_first_u8x16));
        matches &= sz_utf8_uncased_neon_movemask_u8x16x2_(vceqq_u8(folded_u8x16x2.val[0], probe_second_u8x16),
                                                          vceqq_u8(folded_u8x16x2.val[1], probe_second_u8x16)) >>
                   offset_second;
        matches &= sz_utf8_uncased_neon_movemask_u8x16x2_(vceqq_u8(folded_u8x16x2.val[0], probe_third_u8x16),
                                                          vceqq_u8(folded_u8x16x2.val[1], probe_third_u8x16)) >>
                   offset_third;
        matches &= sz_utf8_uncased_neon_movemask_u8x16x2_(vceqq_u8(folded_u8x16x2.val[0], probe_last_u8x16),
                                                          vceqq_u8(folded_u8x16x2.val[1], probe_last_u8x16)) >>
                   offset_last;
        matches &= valid_mask;

        // Candidate Verification
        for (; matches; matches &= matches - 1) {
            sz_size_t const candidate_offset = (sz_size_t)sz_u32_ctz(matches);
            sz_cptr_t const haystack_candidate_ptr = haystack_ptr + candidate_offset;

            // Re-fold the candidate window: loading the ≤16-byte view into the low register with a
            // zeroed high register keeps the script fold's per-register semantics identical to the
            // main chunk fold (zero predecessor for byte 0, zero successor past the window)
            uint8x16_t window_u8x16 = sz_utf8_uncased_neon_load_window_u8x16_(
                haystack_candidate_ptr, (sz_size_t)(haystack_end - haystack_candidate_ptr));
            window_u8x16 = vandq_u8(window_u8x16, window_keep_u8x16);
            uint8x16x2_t window_chunk_u8x16x2;
            window_chunk_u8x16x2.val[0] = window_u8x16;
            window_chunk_u8x16x2.val[1] = vdupq_n_u8(0x00);
            uint8x16x2_t folded_window_u8x16x2 = fold(window_chunk_u8x16x2);
            sz_u32_t window_equal_mask = sz_utf8_uncased_neon_movemask_u8x16x2_(
                vceqq_u8(folded_window_u8x16x2.val[0], needle_window_u8x16), vdupq_n_u8(0x00));
            if ((window_equal_mask & folded_window_mask) != folded_window_mask) continue;

            sz_utf8_uncased_verify_result_t verified = sz_utf8_uncased_verify_match_(   //
                haystack, haystack_length,                                               //
                needle, needle_length,                                                   //
                haystack_candidate_ptr - haystack, needle_metadata->folded_slice_length, // matched offset & length
                needle_metadata->offset_in_unfolded,                                     // head
                needle_length - needle_metadata->offset_in_unfolded - needle_metadata->length_in_unfolded); // tail
            if (verified.match) {
                *matched_length = verified.length;
                return verified.match;
            }
        }
        haystack_ptr += step;
    }

    // Expanding danger characters ('ᾳ' folding to "αι") make the haystack span SHORTER than the
    // folded needle window, so a match can still start in the sub-window tail the loop never
    // probes. The tail is shorter than the 16-byte window, so the serial scan costs nothing.
    if (alarm && haystack_ptr < haystack_end) {
        sz_cptr_t match = sz_utf8_uncased_search_in_danger_zone_(   //
            haystack, haystack_length,                              //
            needle, needle_length,                                  //
            haystack_ptr, (sz_size_t)(haystack_end - haystack_ptr), // the unprobed tail
            needle_first_safe_folded_rune,                          // pivot point
            needle_metadata->offset_in_unfolded,                    // its location in the needle
            matched_length);
        if (match) { return match; }
    }

    return SZ_NULL_CHAR;
}

/**
 *  @brief 4-probe ASCII uncased search: the shared scripted driver with the ASCII fold
 *      and no alarm - ASCII never changes byte width when folded, so the danger machinery
 *      compiles away entirely and the step covers every valid start position.
 */
SZ_HELPER_AUTO sz_cptr_t sz_utf8_uncased_search_neon_ascii_4probe_( //
    sz_cptr_t haystack, sz_size_t haystack_length,                  //
    sz_cptr_t needle, sz_size_t needle_length,                      //
    sz_utf8_uncased_needle_metadata_t const *needle_metadata,       //
    sz_size_t *matched_length) {
    return sz_utf8_uncased_search_neon_scripted_( //
        sz_utf8_uncased_search_neon_ascii_fold_u8x16x2_,
        (sz_utf8_uncased_alarm_u8x16x2_t)SZ_NULL, //
        haystack, haystack_length, needle, needle_length, needle_metadata, matched_length);
}



/**
 *  @brief Fold a 32-byte chunk using Western European case-folding rules.
 *  @sa sz_utf8_uncased_rune_safe_western_europe_k
 *
 *  Handles ASCII A-Z, the Latin-1 Supplement uppercase range 'À'-'Þ' (C3 80-9E → +0x20,
 *  excluding the caseless '×' C3 97), and 'ß' (U+00DF, C3 9F) → "ss" where BOTH bytes of the
 *  pair become 's' so the folded image matches the needle's "ss".
 */
SZ_HELPER_NOINLINE uint8x16x2_t sz_utf8_uncased_search_neon_western_europe_fold_u8x16x2_(uint8x16x2_t text_u8x16x2) {
    uint8x16x2_t result_u8x16x2 = sz_utf8_uncased_search_neon_ascii_fold_u8x16x2_(text_u8x16x2);
    uint8x16x2_t previous_bytes_u8x16x2 = sz_utf8_uncased_neon_previous_bytes_u8x16x2_(text_u8x16x2);

    for (sz_size_t register_index = 0; register_index != 2; ++register_index) {
        uint8x16_t text_u8x16 = text_u8x16x2.val[register_index];
        uint8x16_t previous_bytes_u8x16 = previous_bytes_u8x16x2.val[register_index];
        uint8x16_t result_u8x16 = result_u8x16x2.val[register_index];
        uint8x16_t is_after_c3_u8x16 = vceqq_u8(previous_bytes_u8x16, vdupq_n_u8(0xC3));

        // 1. Handle Eszett: 'ß' (U+00DF, C3 9F) → "ss" (U+0073 U+0073, 73 73): the second-byte
        //    flag propagates one lane back to also rewrite the C3 lead. The back-propagation must
        //    cross the internal boundary, so the per-register next-shift carries the high
        //    register's lane 0 flag onto the low register's lane 15.
        uint8x16_t is_eszett_second_u8x16 = vandq_u8(is_after_c3_u8x16, vceqq_u8(text_u8x16, vdupq_n_u8(0x9F)));
        uint8x16x2_t is_eszett_second_u8x16x2;
        is_eszett_second_u8x16x2.val[register_index] = is_eszett_second_u8x16;
        is_eszett_second_u8x16x2.val[register_index ^ 1] = vdupq_n_u8(0x00);
        uint8x16_t next_eszett_second_u8x16 =
            sz_utf8_uncased_neon_next_bytes_u8x16x2_(is_eszett_second_u8x16x2).val[register_index];
        uint8x16_t is_eszett_u8x16 = vorrq_u8(is_eszett_second_u8x16, next_eszett_second_u8x16);
        result_u8x16 = vbslq_u8(is_eszett_u8x16, vdupq_n_u8('s'), result_u8x16);

        // 2. Handle Latin-1 supplement uppercase letters (C3 80-9E) → add 0x20,
        //    excluding '×' (C3 97, no case variant) and 'ß' (C3 9F, already handled above)
        uint8x16_t is_97_u8x16 = vceqq_u8(text_u8x16, vdupq_n_u8(0x97));
        uint8x16_t is_latin1_upper_u8x16 = vandq_u8(
            is_after_c3_u8x16, vbicq_u8(sz_utf8_uncased_neon_in_byte_range_u8x16_(text_u8x16, 0x80, 0x1F),
                                        vorrq_u8(is_eszett_second_u8x16, is_97_u8x16)));
        result_u8x16 = vaddq_u8(result_u8x16, vandq_u8(is_latin1_upper_u8x16, vdupq_n_u8(0x20)));
        result_u8x16x2.val[register_index] = result_u8x16;
    }
    return result_u8x16x2;
}

/**
 *  @brief Alarm function for Western Europe danger zone detection.
 *
 *  Detects positions where danger characters occur that require special handling:
 *  - E1 BA 96-9E: 'ẖ'-'ẞ' all expand to ASCII-led sequences when folded; the third-byte
 *    qualification matters because the rest of E1 BA covers Vietnamese letters that fold
 *    in place - flagging them blanket-style would send dense Vietnamese text into the
 *    serial danger-zone scanner on every chunk
 *  - E2 84 AA/AB: 'K' (U+212A) → 'k' and 'Å' (U+212B) → 'å' (3 bytes → 1-2 bytes)
 *  - EF AC 80-86: Latin ligatures 'ﬀ'-'ﬆ' → ASCII pairs/triples
 *  - C5 BF: 'ſ' (U+017F) → 's' (2 bytes → 1 byte)
 *  - C5 B8: 'Ÿ' (U+0178) → 'ÿ' (C3 BF), crosses lead bytes
 *  - C3 9F: 'ß' (U+00DF) → "ss" (1 rune → 2 runes)
 *
 *  All pair tests run as scalar shift+AND over the compare movemasks - the same bit algebra
 *  as Ice Lake's k-masks, including the boundary behavior where a lead at lane 31 defers to
 *  the next (overlapping) chunk.
 */
SZ_HELPER_NOINLINE sz_u32_t sz_utf8_uncased_search_neon_western_europe_alarm_u8x16x2_(uint8x16x2_t text_u8x16x2,
                                                                                      sz_u32_t load_mask) {
    sz_unused_(load_mask); // Present for the shared `sz_utf8_uncased_alarm_u8x16x2_t` signature

    // The driver only tests the danger mask for non-emptiness, so the whole alarm stays in the
    // byte-mask domain: every pattern is anchored at its SECOND byte, with the lead read from the
    // `previous` view and the third byte from the `next` view. AVX2's `movemask <</>> 1` predecessor
    // algebra becomes plain `vceqq` against those shifted views, and a single `vmaxvq_u8` over the
    // accumulated danger lanes replaces the dozen per-value horizontal reductions.
    uint8x16x2_t previous_u8x16x2 = sz_utf8_uncased_neon_previous_bytes_u8x16x2_(text_u8x16x2);
    uint8x16x2_t next_u8x16x2 = sz_utf8_uncased_neon_next_bytes_u8x16x2_(text_u8x16x2);

    uint8x16_t any_danger_u8x16 = vdupq_n_u8(0);
    for (sz_size_t register_index = 0; register_index != 2; ++register_index) {
        uint8x16_t text_u8x16 = text_u8x16x2.val[register_index];
        uint8x16_t previous_u8x16 = previous_u8x16x2.val[register_index];
        uint8x16_t next_u8x16 = next_u8x16x2.val[register_index];
        uint8x16_t is_after_c5_u8x16 = vceqq_u8(previous_u8x16, vdupq_n_u8(0xC5));

        uint8x16_t danger_u8x16 = vandq_u8( // Capital Sharp S & co (E1 BA 96-9E)
            vandq_u8(vceqq_u8(text_u8x16, vdupq_n_u8(0xBA)), vceqq_u8(previous_u8x16, vdupq_n_u8(0xE1))),
            sz_utf8_uncased_neon_in_byte_range_u8x16_(next_u8x16, 0x96, 0x09));
        danger_u8x16 = vorrq_u8( // Kelvin/Angstrom (E2 84 AA/AB)
            danger_u8x16,
            vandq_u8(vandq_u8(vceqq_u8(text_u8x16, vdupq_n_u8(0x84)), vceqq_u8(previous_u8x16, vdupq_n_u8(0xE2))),
                     vorrq_u8(vceqq_u8(next_u8x16, vdupq_n_u8(0xAA)), vceqq_u8(next_u8x16, vdupq_n_u8(0xAB)))));
        danger_u8x16 = vorrq_u8( // Ligatures (EF AC xx)
            danger_u8x16, vandq_u8(vceqq_u8(text_u8x16, vdupq_n_u8(0xAC)), vceqq_u8(previous_u8x16, vdupq_n_u8(0xEF))));
        danger_u8x16 = vorrq_u8( // Long S (C5 BF)
            danger_u8x16, vandq_u8(is_after_c5_u8x16, vceqq_u8(text_u8x16, vdupq_n_u8(0xBF))));
        danger_u8x16 = vorrq_u8( // 'Ÿ' (C5 B8) → 'ÿ' (C3 BF)
            danger_u8x16, vandq_u8(is_after_c5_u8x16, vceqq_u8(text_u8x16, vdupq_n_u8(0xB8))));
        danger_u8x16 = vorrq_u8( // Sharp S (C3 9F)
            danger_u8x16, vandq_u8(vceqq_u8(text_u8x16, vdupq_n_u8(0x9F)), vceqq_u8(previous_u8x16, vdupq_n_u8(0xC3))));
        any_danger_u8x16 = vorrq_u8(any_danger_u8x16, danger_u8x16);
    }
    return vmaxvq_u8(any_danger_u8x16);
}

/**
 *  @brief Western European uncased search for needles with safe slices up to 16 bytes.
 *  @sa sz_utf8_uncased_rune_safe_western_europe_k
 */
SZ_HELPER_AUTO sz_cptr_t sz_utf8_uncased_search_neon_western_europe_( //
    sz_cptr_t haystack, sz_size_t haystack_length,                    //
    sz_cptr_t needle, sz_size_t needle_length,                        //
    sz_utf8_uncased_needle_metadata_t const *needle_metadata,         //
    sz_size_t *matched_length) {
    return sz_utf8_uncased_search_neon_scripted_( //
        sz_utf8_uncased_search_neon_western_europe_fold_u8x16x2_,
        sz_utf8_uncased_search_neon_western_europe_alarm_u8x16x2_, //
        haystack, haystack_length, needle, needle_length, needle_metadata, matched_length);
}



/**
 *  @brief Fold a 32-byte chunk using Central European case-folding rules.
 *  @sa sz_utf8_uncased_rune_safe_central_europe_k
 *
 *  Latin-1 Supplement folds with +0x20 (C3 80-9E, except '×' C3 97); Latin Extended-A folds
 *  with +1 on a parity pattern that flips across sub-ranges:
 *  - C4 80-B7 (U+0100-U+0137): uppercase = EVEN second bytes
 *  - C4 B9-BD (U+0139-U+013D): uppercase = ODD ('Ĺ','Ļ','Ľ'); 'ĸ' (C4 B8) is caseless and
 *    'Ŀ' (C4 BF) folds across leads to 'ŀ' (C5 80), so it is routed through the alarm instead
 *  - C5 81-87 (U+0141-U+0147): uppercase = ODD ('Ł','Ń','Ņ','Ň')
 *  - C5 8A-B6 (U+014A-U+0176): uppercase = EVEN ('Ŋ'-'Ŷ')
 *  - C5 B9-BD (U+0179-U+017D): uppercase = ODD ('Ź','Ż','Ž')
 */
SZ_HELPER_NOINLINE uint8x16x2_t sz_utf8_uncased_search_neon_central_europe_fold_u8x16x2_(uint8x16x2_t text_u8x16x2) {
    uint8x16x2_t result_u8x16x2 = sz_utf8_uncased_search_neon_ascii_fold_u8x16x2_(text_u8x16x2);
    uint8x16x2_t previous_bytes_u8x16x2 = sz_utf8_uncased_neon_previous_bytes_u8x16x2_(text_u8x16x2);
    uint8x16x4_t const c4_deltas_lut_u8x16x4 = vld1q_u8_x4(sz_utf8_uncased_central_c4_deltas_lut_);
    uint8x16x4_t const c5_deltas_lut_u8x16x4 = vld1q_u8_x4(sz_utf8_uncased_central_c5_deltas_lut_);

    for (sz_size_t register_index = 0; register_index != 2; ++register_index) {
        uint8x16_t text_u8x16 = text_u8x16x2.val[register_index];
        uint8x16_t previous_bytes_u8x16 = previous_bytes_u8x16x2.val[register_index];
        uint8x16_t result_u8x16 = result_u8x16x2.val[register_index];
        uint8x16_t is_after_c3_u8x16 = vceqq_u8(previous_bytes_u8x16, vdupq_n_u8(0xC3));
        uint8x16_t is_after_c4_u8x16 = vceqq_u8(previous_bytes_u8x16, vdupq_n_u8(0xC4));
        uint8x16_t is_after_c5_u8x16 = vceqq_u8(previous_bytes_u8x16, vdupq_n_u8(0xC5));
        uint8x16_t is_continuation_u8x16 = vcltq_u8(vsubq_u8(text_u8x16, vdupq_n_u8(0x80)), vdupq_n_u8(0x40));

        // 1. Latin-1 Supplement: C3 80-9E → +0x20, except '×' (C3 97)
        uint8x16_t is_latin1_upper_u8x16 = vandq_u8(
            is_after_c3_u8x16, vbicq_u8(sz_utf8_uncased_neon_in_byte_range_u8x16_(text_u8x16, 0x80, 0x1F),
                                        vceqq_u8(text_u8x16, vdupq_n_u8(0x97))));
        result_u8x16 = vaddq_u8(result_u8x16, vandq_u8(is_latin1_upper_u8x16, vdupq_n_u8(0x20)));

        // 2. Latin Extended-A: one `vqtbl4q_u8` per lead family resolves the +1 parity that the
        //    range/odd-parity checks used to assemble. The `is_continuation` mask drops bytes
        //    outside [0x80, 0xBF] that `text & 0x3F` would otherwise alias onto a folding index.
        uint8x16_t delta_indices_u8x16 = vandq_u8(text_u8x16, vdupq_n_u8(0x3F));
        uint8x16_t fold_extended_u8x16 = vorrq_u8(
            vandq_u8(vqtbl4q_u8(c4_deltas_lut_u8x16x4, delta_indices_u8x16), is_after_c4_u8x16),
            vandq_u8(vqtbl4q_u8(c5_deltas_lut_u8x16x4, delta_indices_u8x16), is_after_c5_u8x16));
        fold_extended_u8x16 = vandq_u8(fold_extended_u8x16, is_continuation_u8x16);
        result_u8x16 = vaddq_u8(result_u8x16, fold_extended_u8x16);
        result_u8x16x2.val[register_index] = result_u8x16;
    }
    return result_u8x16x2;
}

/**
 *  @brief Alarm function for Central Europe danger zone detection.
 *
 *  Detects positions where danger characters occur that require special handling:
 *  - E2 84: 'K' Kelvin sign (E2 84 AA, 3 bytes → 1 byte)
 *  - C3 9F: 'ß' (U+00DF) → "ss" (1 rune → 2 runes)
 *  - C4 B0: 'İ' (U+0130) → "i̇" (2 bytes → 3 bytes)
 *  - C4 BF: 'Ŀ' (U+013F) → 'ŀ' (C5 80), crosses lead bytes
 *  - C5 BF: 'ſ' (U+017F) → 's' (2 bytes → 1 byte)
 *  - C5 B8: 'Ÿ' (U+0178) → 'ÿ' (C3 BF), crosses lead bytes
 *  - EF AC 80-86: Latin ligatures 'ﬀ'-'ﬆ' → ASCII pairs/triples
 */
SZ_HELPER_NOINLINE sz_u32_t sz_utf8_uncased_search_neon_central_europe_alarm_u8x16x2_(uint8x16x2_t text_u8x16x2,
                                                                                      sz_u32_t load_mask) {
    sz_unused_(load_mask); // Present for the shared `sz_utf8_uncased_alarm_u8x16x2_t` signature

    // Byte-mask danger detection anchored at the second byte; the lead comes from the `previous`
    // view. All pairs are two-byte, so no `next` lookup is needed, and one `vmaxvq_u8` gates the
    // driver's danger branch in place of seven per-value movemasks.
    uint8x16x2_t previous_u8x16x2 = sz_utf8_uncased_neon_previous_bytes_u8x16x2_(text_u8x16x2);

    uint8x16_t any_danger_u8x16 = vdupq_n_u8(0);
    for (sz_size_t register_index = 0; register_index != 2; ++register_index) {
        uint8x16_t text_u8x16 = text_u8x16x2.val[register_index];
        uint8x16_t previous_u8x16 = previous_u8x16x2.val[register_index];
        uint8x16_t is_after_c4_u8x16 = vceqq_u8(previous_u8x16, vdupq_n_u8(0xC4));
        uint8x16_t is_after_c5_u8x16 = vceqq_u8(previous_u8x16, vdupq_n_u8(0xC5));

        uint8x16_t danger_u8x16 = vandq_u8( // Kelvin (E2 84 AA)
            vceqq_u8(text_u8x16, vdupq_n_u8(0x84)), vceqq_u8(previous_u8x16, vdupq_n_u8(0xE2)));
        danger_u8x16 = vorrq_u8( // Sharp S (C3 9F)
            danger_u8x16, vandq_u8(vceqq_u8(text_u8x16, vdupq_n_u8(0x9F)), vceqq_u8(previous_u8x16, vdupq_n_u8(0xC3))));
        danger_u8x16 = vorrq_u8( // Dotted I (C4 B0)
            danger_u8x16, vandq_u8(is_after_c4_u8x16, vceqq_u8(text_u8x16, vdupq_n_u8(0xB0))));
        danger_u8x16 = vorrq_u8( // 'Ŀ' (C4 BF) → 'ŀ' (C5 80), crosses lead bytes
            danger_u8x16, vandq_u8(is_after_c4_u8x16, vceqq_u8(text_u8x16, vdupq_n_u8(0xBF))));
        danger_u8x16 = vorrq_u8( // Long S (C5 BF)
            danger_u8x16, vandq_u8(is_after_c5_u8x16, vceqq_u8(text_u8x16, vdupq_n_u8(0xBF))));
        danger_u8x16 = vorrq_u8( // 'Ÿ' (C5 B8) → 'ÿ' (C3 BF)
            danger_u8x16, vandq_u8(is_after_c5_u8x16, vceqq_u8(text_u8x16, vdupq_n_u8(0xB8))));
        danger_u8x16 = vorrq_u8( // Ligatures (EF AC xx)
            danger_u8x16, vandq_u8(vceqq_u8(text_u8x16, vdupq_n_u8(0xAC)), vceqq_u8(previous_u8x16, vdupq_n_u8(0xEF))));
        any_danger_u8x16 = vorrq_u8(any_danger_u8x16, danger_u8x16);
    }
    return vmaxvq_u8(any_danger_u8x16);
}

/**
 *  @brief Central European uncased search for needles with safe slices up to 16 bytes.
 *  @sa sz_utf8_uncased_rune_safe_central_europe_k
 */
SZ_HELPER_AUTO sz_cptr_t sz_utf8_uncased_search_neon_central_europe_( //
    sz_cptr_t haystack, sz_size_t haystack_length,                    //
    sz_cptr_t needle, sz_size_t needle_length,                        //
    sz_utf8_uncased_needle_metadata_t const *needle_metadata,         //
    sz_size_t *matched_length) {
    return sz_utf8_uncased_search_neon_scripted_( //
        sz_utf8_uncased_search_neon_central_europe_fold_u8x16x2_,
        sz_utf8_uncased_search_neon_central_europe_alarm_u8x16x2_, //
        haystack, haystack_length, needle, needle_length, needle_metadata, matched_length);
}



/**
 *  @brief Fold a 32-byte chunk using Cyrillic case-folding rules.
 *  @sa sz_utf8_uncased_rune_safe_cyrillic_k
 *
 *  Basic Cyrillic has a clean high-nibble pattern on the second byte after a D0 lead:
 *  8x → +0x10 ('Ѐ'-'Џ' land in the D1 block), 9x → +0x20 ('А'-'П' stay under D0),
 *  Ax → −0x20 ('Р'-'Я' land in the D1 block), Bx → 0 (already lowercase). One `vqtbl1q_u8`
 *  lookup over a 16-entry table replaces 3 range comparisons + 3 masked adds. Extended Cyrillic
 *  (D2/D3) needles are BANNED at classification time, so only D0 continuations need folding.
 */
SZ_HELPER_NOINLINE uint8x16x2_t sz_utf8_uncased_search_neon_cyrillic_fold_u8x16x2_(uint8x16x2_t text_u8x16x2) {
    uint8x16x2_t result_u8x16x2 = sz_utf8_uncased_search_neon_ascii_fold_u8x16x2_(text_u8x16x2);
    uint8x16x2_t previous_bytes_u8x16x2 = sz_utf8_uncased_neon_previous_bytes_u8x16x2_(text_u8x16x2);
    uint8x16x2_t next_bytes_u8x16x2 = sz_utf8_uncased_neon_next_bytes_u8x16x2_(text_u8x16x2);

    // Second-byte offsets keyed by the high nibble: 8 → +0x10, 9 → +0x20, A → −0x20 (0xE0)
    static sz_u8_t const cyrillic_offset_lut[16] = {0, 0, 0, 0, 0, 0, 0, 0, 0x10, 0x20, 0xE0, 0, 0, 0, 0, 0};
    uint8x16_t const cyrillic_offset_lut_u8x16 = vld1q_u8(cyrillic_offset_lut);

    for (sz_size_t register_index = 0; register_index != 2; ++register_index) {
        uint8x16_t text_u8x16 = text_u8x16x2.val[register_index];
        uint8x16_t previous_bytes_u8x16 = previous_bytes_u8x16x2.val[register_index];
        uint8x16_t next_bytes_u8x16 = next_bytes_u8x16x2.val[register_index];
        uint8x16_t result_u8x16 = result_u8x16x2.val[register_index];
        uint8x16_t is_after_d0_u8x16 = vceqq_u8(previous_bytes_u8x16, vdupq_n_u8(0xD0));

        uint8x16_t high_nibbles_u8x16 = vshrq_n_u8(text_u8x16, 4);
        uint8x16_t offsets_u8x16 = vqtbl1q_u8(cyrillic_offset_lut_u8x16, high_nibbles_u8x16);
        result_u8x16 = vaddq_u8(result_u8x16, vandq_u8(is_after_d0_u8x16, offsets_u8x16));

        // Lead fixup: 'Ѐ'-'Џ' (seconds 80-8F) and 'Р'-'Я' (seconds A0-AF) have lowercase in the D1
        // block, so their D0 lead takes a masked +1; 'А'-'П' (seconds 90-9F) stay under D0
        uint8x16_t is_d0_u8x16 = vceqq_u8(text_u8x16, vdupq_n_u8(0xD0));
        uint8x16_t needs_d1_u8x16 = vandq_u8(
            is_d0_u8x16, vorrq_u8(sz_utf8_uncased_neon_in_byte_range_u8x16_(next_bytes_u8x16, 0x80, 0x10),
                                  sz_utf8_uncased_neon_in_byte_range_u8x16_(next_bytes_u8x16, 0xA0, 0x10)));
        result_u8x16 = vaddq_u8(result_u8x16, vandq_u8(needs_d1_u8x16, vdupq_n_u8(0x01)));
        result_u8x16x2.val[register_index] = result_u8x16;
    }
    return result_u8x16x2;
}

/**
 *  @brief Alarm function for Cyrillic danger zone detection.
 *
 *  Basic Cyrillic itself never changes byte width when folded, and Extended Cyrillic needles
 *  (D2/D3 leads) are banned at needle-analysis time. The one haystack-side hazard is Cyrillic
 *  Extended-C: 'ᲀ'-'ᲈ' (U+1C80-1C88, E1 B2 80-88) fold INTO basic 2-byte Cyrillic letters
 *  ('в', 'д', 'о', 'с', 'т', 'ъ', 'ѣ'), so a 3-byte haystack character can match a 2-byte
 *  needle character and must go through the serial danger-zone scanner. The E1 B2 pair is
 *  absent from virtually all real Cyrillic text, so the third-byte refinement hides behind
 *  a branch and the hot path is two compares.
 */
SZ_HELPER_NOINLINE sz_u32_t sz_utf8_uncased_search_neon_cyrillic_alarm_u8x16x2_(uint8x16x2_t text_u8x16x2,
                                                                                sz_u32_t load_mask) {
    sz_unused_(load_mask); // Present for the shared `sz_utf8_uncased_alarm_u8x16x2_t` signature

    // E1 B2 is dangerous only when the third (next) byte folds, i.e. lands in 80-88. Anchored at the
    // B2 second byte: lead from `previous`, third from `next`, gated by one `vmaxvq_u8`.
    uint8x16x2_t previous_u8x16x2 = sz_utf8_uncased_neon_previous_bytes_u8x16x2_(text_u8x16x2);
    uint8x16x2_t next_u8x16x2 = sz_utf8_uncased_neon_next_bytes_u8x16x2_(text_u8x16x2);

    uint8x16_t any_danger_u8x16 = vdupq_n_u8(0);
    for (sz_size_t register_index = 0; register_index != 2; ++register_index) {
        uint8x16_t danger_u8x16 = vandq_u8(
            vandq_u8(vceqq_u8(text_u8x16x2.val[register_index], vdupq_n_u8(0xB2)),
                     vceqq_u8(previous_u8x16x2.val[register_index], vdupq_n_u8(0xE1))),
            sz_utf8_uncased_neon_in_byte_range_u8x16_(next_u8x16x2.val[register_index], 0x80, 0x09));
        any_danger_u8x16 = vorrq_u8(any_danger_u8x16, danger_u8x16);
    }
    return vmaxvq_u8(any_danger_u8x16);
}

/**
 *  @brief Cyrillic uncased search for needles with safe slices up to 16 bytes.
 *  @sa sz_utf8_uncased_rune_safe_cyrillic_k
 */
SZ_HELPER_AUTO sz_cptr_t sz_utf8_uncased_search_neon_cyrillic_( //
    sz_cptr_t haystack, sz_size_t haystack_length,              //
    sz_cptr_t needle, sz_size_t needle_length,                  //
    sz_utf8_uncased_needle_metadata_t const *needle_metadata,   //
    sz_size_t *matched_length) {
    return sz_utf8_uncased_search_neon_scripted_( //
        sz_utf8_uncased_search_neon_cyrillic_fold_u8x16x2_,
        sz_utf8_uncased_search_neon_cyrillic_alarm_u8x16x2_, //
        haystack, haystack_length, needle, needle_length, needle_metadata, matched_length);
}



/**
 *  @brief Fold a 32-byte chunk using Armenian case-folding rules.
 *  @sa sz_utf8_uncased_rune_safe_armenian_k
 *
 *  Armenian uppercase spans two lead bytes and folds into three target blocks:
 *  - D4 B1-BF: 'Ա'-'Ձ' → D5 A1-AF 'ա'-'ձ' (second −0x10, lead D4 → D5)
 *  - D5 80-8F: 'Ղ'-'Տ' → D5 B0-BF 'ղ'-'տ' (second +0x30, lead unchanged)
 *  - D5 90-96: 'Ր'-'Ֆ' → D6 80-86 'ր'-'ֆ' (second −0x10, lead D5 → D6)
 *
 *  Both lead rewrites are a +1 increment (D4 → D5, D5 → D6), so the second-byte flags propagate
 *  one lane back through `next_bytes` and join the single merged offset add - all rule masks flag
 *  disjoint byte positions. The D4 range checks only the lower bound, mirroring the reference:
 *  valid continuation bytes never exceed BF.
 */
SZ_HELPER_NOINLINE uint8x16x2_t sz_utf8_uncased_search_neon_armenian_fold_u8x16x2_(uint8x16x2_t text_u8x16x2) {
    uint8x16x2_t result_u8x16x2 = sz_utf8_uncased_search_neon_ascii_fold_u8x16x2_(text_u8x16x2);
    uint8x16x2_t previous_bytes_u8x16x2 = sz_utf8_uncased_neon_previous_bytes_u8x16x2_(text_u8x16x2);

    // The lead +1 bump comes from the SECOND byte's class shifted one lane forward, which must
    // cross the internal boundary - so the two per-register `is_minus_10` masks are assembled
    // first, then a single 32-byte `next_bytes` carries the high register's lane 0 to lane 15.
    uint8x16x2_t is_minus_10_u8x16x2;
    uint8x16x2_t is_d5_low_u8x16x2;
    for (sz_size_t register_index = 0; register_index != 2; ++register_index) {
        uint8x16_t text_u8x16 = text_u8x16x2.val[register_index];
        uint8x16_t previous_bytes_u8x16 = previous_bytes_u8x16x2.val[register_index];
        uint8x16_t is_after_d4_u8x16 = vceqq_u8(previous_bytes_u8x16, vdupq_n_u8(0xD4));
        uint8x16_t is_after_d5_u8x16 = vceqq_u8(previous_bytes_u8x16, vdupq_n_u8(0xD5));

        // Second-byte classes; the [B1, FF] range realizes the unbounded `≥ B1` check
        uint8x16_t is_d4_upper_u8x16 = vandq_u8(is_after_d4_u8x16, //
                                                sz_utf8_uncased_neon_in_byte_range_u8x16_(text_u8x16, 0xB1, 0x4F));
        uint8x16_t is_d5_low_u8x16 = vandq_u8(is_after_d5_u8x16, //
                                              sz_utf8_uncased_neon_in_byte_range_u8x16_(text_u8x16, 0x80, 0x10));
        uint8x16_t is_d5_high_u8x16 = vandq_u8(is_after_d5_u8x16, //
                                               sz_utf8_uncased_neon_in_byte_range_u8x16_(text_u8x16, 0x90, 0x07));
        is_minus_10_u8x16x2.val[register_index] = vorrq_u8(is_d4_upper_u8x16, is_d5_high_u8x16);
        is_d5_low_u8x16x2.val[register_index] = is_d5_low_u8x16;
    }

    uint8x16x2_t lead_plus_one_u8x16x2 = sz_utf8_uncased_neon_next_bytes_u8x16x2_(is_minus_10_u8x16x2);
    for (sz_size_t register_index = 0; register_index != 2; ++register_index) {
        // Disjoint positions merge into ONE offset vector and a single add
        uint8x16_t offsets_u8x16 = vandq_u8(is_minus_10_u8x16x2.val[register_index], vdupq_n_u8(0xF0));
        offsets_u8x16 = vorrq_u8(offsets_u8x16, vandq_u8(is_d5_low_u8x16x2.val[register_index], vdupq_n_u8(0x30)));
        offsets_u8x16 = vorrq_u8(offsets_u8x16, vandq_u8(lead_plus_one_u8x16x2.val[register_index], vdupq_n_u8(0x01)));
        result_u8x16x2.val[register_index] = vaddq_u8(result_u8x16x2.val[register_index], offsets_u8x16);
    }
    return result_u8x16x2;
}

/**
 *  @brief Alarm function for Armenian danger zone detection.
 *
 *  Detects positions where danger characters occur that require special handling:
 *  - D6 87: 'և' (U+0587) → "եւ" (D5 A5 D6 82), the Ech-Yiwn ligature (2 bytes → 4 bytes)
 *  - EF AC 93-97: presentation-form ligatures 'ﬓ'-'ﬗ' (U+FB13-U+FB17) → 2 codepoints each
 *
 *  The EF AC pair alarms without a third-byte refinement, exactly like the reference: the only
 *  EF AC neighbors are the Latin/Hebrew presentation forms, which never appear inside Armenian
 *  haystacks, so the coarser test costs nothing in practice.
 */
SZ_HELPER_NOINLINE sz_u32_t sz_utf8_uncased_search_neon_armenian_alarm_u8x16x2_(uint8x16x2_t text_u8x16x2,
                                                                                sz_u32_t load_mask) {
    sz_unused_(load_mask); // Present for the shared `sz_utf8_uncased_alarm_u8x16x2_t` signature

    // Two two-byte pairs anchored at their second byte; lead from `previous`, gated by `vmaxvq_u8`.
    uint8x16x2_t previous_u8x16x2 = sz_utf8_uncased_neon_previous_bytes_u8x16x2_(text_u8x16x2);

    uint8x16_t any_danger_u8x16 = vdupq_n_u8(0);
    for (sz_size_t register_index = 0; register_index != 2; ++register_index) {
        uint8x16_t text_u8x16 = text_u8x16x2.val[register_index];
        uint8x16_t previous_u8x16 = previous_u8x16x2.val[register_index];
        uint8x16_t danger_u8x16 = vandq_u8( // Ech-Yiwn (D6 87)
            vceqq_u8(text_u8x16, vdupq_n_u8(0x87)), vceqq_u8(previous_u8x16, vdupq_n_u8(0xD6)));
        danger_u8x16 = vorrq_u8( // Ligatures (EF AC xx)
            danger_u8x16, vandq_u8(vceqq_u8(text_u8x16, vdupq_n_u8(0xAC)), vceqq_u8(previous_u8x16, vdupq_n_u8(0xEF))));
        any_danger_u8x16 = vorrq_u8(any_danger_u8x16, danger_u8x16);
    }
    return vmaxvq_u8(any_danger_u8x16);
}

/**
 *  @brief Armenian uncased search for needles with safe slices up to 16 bytes.
 *  @sa sz_utf8_uncased_rune_safe_armenian_k
 */
SZ_HELPER_AUTO sz_cptr_t sz_utf8_uncased_search_neon_armenian_( //
    sz_cptr_t haystack, sz_size_t haystack_length,              //
    sz_cptr_t needle, sz_size_t needle_length,                  //
    sz_utf8_uncased_needle_metadata_t const *needle_metadata,   //
    sz_size_t *matched_length) {
    return sz_utf8_uncased_search_neon_scripted_( //
        sz_utf8_uncased_search_neon_armenian_fold_u8x16x2_,
        sz_utf8_uncased_search_neon_armenian_alarm_u8x16x2_, //
        haystack, haystack_length, needle, needle_length, needle_metadata, matched_length);
}



/**
 *  @brief Fold a 32-byte chunk using Greek case-folding rules.
 *  @sa sz_utf8_uncased_rune_safe_greek_k
 *
 *  Monotonic Greek folds entirely on the byte after a CE/CF/C2 lead:
 *  - CE 91-9F: 'Α'-'Ο' → CE B1-BF 'α'-'ο' (second +0x20)
 *  - CE A0-A9: 'Π'-'Ω' → CF 80-89 'π'-'ω' (second −0x20, lead CE → CF)
 *  - CE 86: 'Ά' → CE AC 'ά' (second +0x26)
 *  - CE 88-8A: 'Έ'-'Ί' → CE AD-AF 'έ'-'ί' (second +0x25)
 *  - CE 8C: 'Ό' → CF 8C 'ό' (lead change only)
 *  - CE 8E-8F: 'Ύ','Ώ' → CF 8D-8E 'ύ','ώ' (second −1, lead CE → CF)
 *  - CE AA-AB: 'Ϊ','Ϋ' → CF 8A-8B 'ϊ','ϋ' (second −0x20, lead CE → CF)
 *  - CF 82: 'ς' → CF 83 'σ' (final sigma, +1)
 *  - C2 B5: 'µ' → CE BC 'μ' (micro sign joins Greek mu: lead +0x0C, second +0x07)
 *
 *  Every rule hits DISJOINT byte positions, so the per-rule deltas merge into one offset
 *  vector with AND/OR ops and a single add applies them all. All CE → CF lead rewrites are
 *  a +1 increment, propagated back from the second-byte flags through `next_bytes` like
 *  the Eszett rewrite in the Western European fold. The lead rewrites must cross the internal
 *  boundary, so the second-byte class masks are assembled per register first, then a single
 *  32-byte `next_bytes` carries the high register's lane 0 to the low register's lane 15.
 */
SZ_HELPER_NOINLINE uint8x16x2_t sz_utf8_uncased_search_neon_greek_fold_u8x16x2_(uint8x16x2_t text_u8x16x2) {
    uint8x16x2_t result_u8x16x2 = sz_utf8_uncased_search_neon_ascii_fold_u8x16x2_(text_u8x16x2);
    uint8x16x2_t previous_bytes_u8x16x2 = sz_utf8_uncased_neon_previous_bytes_u8x16x2_(text_u8x16x2);

    uint8x16x4_t const ce_deltas_lut_u8x16x4 = vld1q_u8_x4(sz_utf8_uncased_greek_ce_deltas_lut_);
    uint8x16x4_t const ce_promotes_lut_u8x16x4 = vld1q_u8_x4(sz_utf8_uncased_greek_ce_promotes_lut_);

    uint8x16x2_t promote_seconds_u8x16x2; // CE → CF (+1) second-byte flags, pre-shift
    uint8x16x2_t micro_seconds_u8x16x2;   // C2 → CE (+0x0C) micro-sign second flags, pre-shift
    uint8x16x2_t partial_offsets_u8x16x2; // every delta that does NOT need a cross-boundary shift

    for (sz_size_t register_index = 0; register_index != 2; ++register_index) {
        uint8x16_t text_u8x16 = text_u8x16x2.val[register_index];
        uint8x16_t previous_bytes_u8x16 = previous_bytes_u8x16x2.val[register_index];
        uint8x16_t is_after_ce_u8x16 = vceqq_u8(previous_bytes_u8x16, vdupq_n_u8(0xCE));
        uint8x16_t is_after_cf_u8x16 = vceqq_u8(previous_bytes_u8x16, vdupq_n_u8(0xCF));
        uint8x16_t is_after_c2_u8x16 = vceqq_u8(previous_bytes_u8x16, vdupq_n_u8(0xC2));
        uint8x16_t is_continuation_u8x16 = vcltq_u8(vsubq_u8(text_u8x16, vdupq_n_u8(0x80)), vdupq_n_u8(0x40));
        uint8x16_t after_ce_cont_u8x16 = vandq_u8(is_after_ce_u8x16, is_continuation_u8x16);
        uint8x16_t delta_indices_u8x16 = vandq_u8(text_u8x16, vdupq_n_u8(0x3F));

        // Second-byte deltas after CE and the matching CE → CF promotion flags come from two table
        // lookups; the `is_continuation` mask keeps `text & 0x3F` from aliasing onto a folding index
        uint8x16_t ce_delta_u8x16 = vandq_u8(vqtbl4q_u8(ce_deltas_lut_u8x16x4, delta_indices_u8x16),
                                             after_ce_cont_u8x16);
        promote_seconds_u8x16x2.val[register_index] = vandq_u8(vqtbl4q_u8(ce_promotes_lut_u8x16x4, delta_indices_u8x16),
                                                               after_ce_cont_u8x16);

        // Final sigma 'ς' (CF 82) and the micro sign's second byte (C2 B5)
        uint8x16_t is_final_sigma_u8x16 = vandq_u8(is_after_cf_u8x16, vceqq_u8(text_u8x16, vdupq_n_u8(0x82)));
        uint8x16_t is_micro_second_u8x16 = vandq_u8(is_after_c2_u8x16, vceqq_u8(text_u8x16, vdupq_n_u8(0xB5)));
        micro_seconds_u8x16x2.val[register_index] = is_micro_second_u8x16;

        uint8x16_t offsets_u8x16 = ce_delta_u8x16;
        offsets_u8x16 = vorrq_u8(offsets_u8x16, vandq_u8(is_final_sigma_u8x16, vdupq_n_u8(0x01)));
        offsets_u8x16 = vorrq_u8(offsets_u8x16, vandq_u8(is_micro_second_u8x16, vdupq_n_u8(0x07)));
        partial_offsets_u8x16x2.val[register_index] = offsets_u8x16;
    }

    // Propagate the lead rewrites back from the second-byte flags across the internal boundary
    uint8x16x2_t promote_lead_u8x16x2 = sz_utf8_uncased_neon_next_bytes_u8x16x2_(promote_seconds_u8x16x2);
    uint8x16x2_t micro_lead_u8x16x2 = sz_utf8_uncased_neon_next_bytes_u8x16x2_(micro_seconds_u8x16x2);
    for (sz_size_t register_index = 0; register_index != 2; ++register_index) {
        uint8x16_t offsets_u8x16 = partial_offsets_u8x16x2.val[register_index];
        offsets_u8x16 = vorrq_u8(offsets_u8x16, vandq_u8(promote_lead_u8x16x2.val[register_index], vdupq_n_u8(0x01)));
        offsets_u8x16 = vorrq_u8(offsets_u8x16, vandq_u8(micro_lead_u8x16x2.val[register_index], vdupq_n_u8(0x0C)));
        result_u8x16x2.val[register_index] = vaddq_u8(result_u8x16x2.val[register_index], offsets_u8x16);
    }
    return result_u8x16x2;
}

/**
 *  @brief Alarm function for Greek danger zone detection.
 *
 *  Detects positions where danger characters occur that require special handling:
 *  - CE 90 / CE B0: 'ΐ', 'ΰ' expand to 3 codepoints when folded
 *  - CF 90, 91, 95, 96: Greek symbols 'ϐ', 'ϑ', 'ϕ', 'ϖ' fold to basic letters
 *  - CF B0, B1, B4, B5: 'ϰ', 'ϱ', 'ϴ', 'ϵ' fold to basic letters ('ϴ' → CE B8 'θ')
 *  - E2 84: Ohm sign 'Ω' (U+2126) prefix (3 bytes → 2 bytes)
 *  - E1 (blanket): polytonic Greek Extended, with single-, double-, and triple-expanding folds
 *  - CD (blanket): archaic letters and combining marks adjacent to the Greek block
 *
 *  Modern Greek text is pure CE/CF sequences, so the blanket E1/CD lead alarms almost
 *  never fire - but when they do, the driver's step−2 retreat keeps a 3-byte danger
 *  sequence straddling the chunk edge fully visible in the next chunk.
 */
SZ_HELPER_NOINLINE sz_u32_t sz_utf8_uncased_search_neon_greek_alarm_u8x16x2_(uint8x16x2_t text_u8x16x2,
                                                                             sz_u32_t load_mask) {
    sz_unused_(load_mask); // Present for the shared `sz_utf8_uncased_alarm_u8x16x2_t` signature

    // Pair danger anchored at the second byte (lead from `previous`); the polytonic & archaic leads
    // E1/CD are blanket hazards flagged at their own lane. One `vmaxvq_u8` gates the danger branch.
    uint8x16x2_t previous_u8x16x2 = sz_utf8_uncased_neon_previous_bytes_u8x16x2_(text_u8x16x2);

    uint8x16_t any_danger_u8x16 = vdupq_n_u8(0);
    for (sz_size_t register_index = 0; register_index != 2; ++register_index) {
        uint8x16_t text_u8x16 = text_u8x16x2.val[register_index];
        uint8x16_t previous_u8x16 = previous_u8x16x2.val[register_index];

        // 'ΐ', 'ΰ' (CE 90 / CE B0)
        uint8x16_t second_90_or_b0_u8x16 = vorrq_u8(vceqq_u8(text_u8x16, vdupq_n_u8(0x90)),
                                                    vceqq_u8(text_u8x16, vdupq_n_u8(0xB0)));
        uint8x16_t danger_u8x16 = vandq_u8(vceqq_u8(previous_u8x16, vdupq_n_u8(0xCE)), second_90_or_b0_u8x16);

        // Greek symbols (CF 9x / CF Bx)
        uint8x16_t second_9x_u8x16 = vorrq_u8(
            vorrq_u8(vceqq_u8(text_u8x16, vdupq_n_u8(0x90)), vceqq_u8(text_u8x16, vdupq_n_u8(0x91))),
            vorrq_u8(vceqq_u8(text_u8x16, vdupq_n_u8(0x95)), vceqq_u8(text_u8x16, vdupq_n_u8(0x96))));
        uint8x16_t second_bx_u8x16 = vorrq_u8(
            vorrq_u8(vceqq_u8(text_u8x16, vdupq_n_u8(0xB0)), vceqq_u8(text_u8x16, vdupq_n_u8(0xB1))),
            vorrq_u8(vceqq_u8(text_u8x16, vdupq_n_u8(0xB4)), vceqq_u8(text_u8x16, vdupq_n_u8(0xB5))));
        danger_u8x16 = vorrq_u8(danger_u8x16, vandq_u8(vceqq_u8(previous_u8x16, vdupq_n_u8(0xCF)),
                                                       vorrq_u8(second_9x_u8x16, second_bx_u8x16)));

        // Ohm sign (E2 84 A6)
        danger_u8x16 = vorrq_u8(
            danger_u8x16, vandq_u8(vceqq_u8(text_u8x16, vdupq_n_u8(0x84)), vceqq_u8(previous_u8x16, vdupq_n_u8(0xE2))));

        // Blanket polytonic & archaic leads
        danger_u8x16 = vorrq_u8(
            danger_u8x16, vorrq_u8(vceqq_u8(text_u8x16, vdupq_n_u8(0xE1)), vceqq_u8(text_u8x16, vdupq_n_u8(0xCD))));
        any_danger_u8x16 = vorrq_u8(any_danger_u8x16, danger_u8x16);
    }
    return vmaxvq_u8(any_danger_u8x16);
}

/**
 *  @brief Greek uncased search for needles with safe slices up to 16 bytes.
 *  @sa sz_utf8_uncased_rune_safe_greek_k
 */
SZ_HELPER_AUTO sz_cptr_t sz_utf8_uncased_search_neon_greek_(  //
    sz_cptr_t haystack, sz_size_t haystack_length,            //
    sz_cptr_t needle, sz_size_t needle_length,                //
    sz_utf8_uncased_needle_metadata_t const *needle_metadata, //
    sz_size_t *matched_length) {
    return sz_utf8_uncased_search_neon_scripted_( //
        sz_utf8_uncased_search_neon_greek_fold_u8x16x2_,
        sz_utf8_uncased_search_neon_greek_alarm_u8x16x2_, //
        haystack, haystack_length, needle, needle_length, needle_metadata, matched_length);
}



/**
 *  @brief Fold a 32-byte chunk using Vietnamese case-folding rules.
 *  @sa sz_utf8_uncased_rune_safe_vietnamese_k
 *
 *  Vietnamese letters spread across four Latin blocks, all folding in place:
 *  - C3 80-9E: Latin-1 Supplement uppercase → +0x20, except the caseless '×' (C3 97)
 *  - C4/C5: Latin Extended-A folds with +1 keyed on continuation parity - the codepoint's
 *    low bit equals the byte's low bit. Most of the block folds EVEN seconds; the
 *    sub-ranges C4 B9-BE ('Ĺ'-'ľ') and C5 80-88 ('ŀ'-'ň') invert and fold ODD seconds
 *  - C6 A0 / C6 AF: 'Ơ' → 'ơ' and 'Ư' → 'ư' (+1)
 *  - E1 B8-BB: Latin Extended Additional folds EVEN third bytes with +1, except the
 *    expanding E1 BA 96-9F block ('ẖ'-'ẟ'), which the alarm routes to the serial scanner
 *
 *  The third-byte rule needs the byte TWO lanes back, so a second `previous_bytes` pass
 *  materializes it; all rule masks flag disjoint positions and merge into one offset add.
 */
SZ_HELPER_NOINLINE uint8x16x2_t sz_utf8_uncased_search_neon_vietnamese_fold_u8x16x2_(uint8x16x2_t text_u8x16x2) {
    uint8x16x2_t result_u8x16x2 = sz_utf8_uncased_search_neon_ascii_fold_u8x16x2_(text_u8x16x2);
    uint8x16x2_t previous_bytes_u8x16x2 = sz_utf8_uncased_neon_previous_bytes_u8x16x2_(text_u8x16x2);
    uint8x16x2_t previous2_bytes_u8x16x2 = sz_utf8_uncased_neon_previous_bytes_u8x16x2_(previous_bytes_u8x16x2);

    for (sz_size_t register_index = 0; register_index != 2; ++register_index) {
        uint8x16_t text_u8x16 = text_u8x16x2.val[register_index];
        uint8x16_t previous_bytes_u8x16 = previous_bytes_u8x16x2.val[register_index];
        uint8x16_t previous2_bytes_u8x16 = previous2_bytes_u8x16x2.val[register_index];
        uint8x16_t result_u8x16 = result_u8x16x2.val[register_index];
        uint8x16_t is_after_c3_u8x16 = vceqq_u8(previous_bytes_u8x16, vdupq_n_u8(0xC3));
        uint8x16_t is_after_c4_u8x16 = vceqq_u8(previous_bytes_u8x16, vdupq_n_u8(0xC4));
        uint8x16_t is_after_c5_u8x16 = vceqq_u8(previous_bytes_u8x16, vdupq_n_u8(0xC5));
        uint8x16_t is_after_c6_u8x16 = vceqq_u8(previous_bytes_u8x16, vdupq_n_u8(0xC6));

        // 1. Latin-1 Supplement: C3 80-9E → +0x20, except '×' (C3 97)
        uint8x16_t is_c3_target_u8x16 = vandq_u8(
            is_after_c3_u8x16, vbicq_u8(sz_utf8_uncased_neon_in_byte_range_u8x16_(text_u8x16, 0x80, 0x1F),
                                        vceqq_u8(text_u8x16, vdupq_n_u8(0x97))));

        // 2. Latin Extended-A: +1 on EVEN seconds, except the inverted sub-ranges C4 B9-BE and
        //    C5 00-88 (the unsigned `≤ 88` bound mirrors the reference) which fold ODD
        uint8x16_t is_odd_u8x16 = vceqq_u8(vandq_u8(text_u8x16, vdupq_n_u8(0x01)), vdupq_n_u8(0x01));
        uint8x16_t is_inverted_u8x16 = vorrq_u8(
            vandq_u8(is_after_c4_u8x16, sz_utf8_uncased_neon_in_byte_range_u8x16_(text_u8x16, 0xB9, 0x06)),
            vandq_u8(is_after_c5_u8x16, sz_utf8_uncased_neon_in_byte_range_u8x16_(text_u8x16, 0x00, 0x89)));
        uint8x16_t is_extended_even_u8x16 = vbicq_u8(
            vbicq_u8(vorrq_u8(is_after_c4_u8x16, is_after_c5_u8x16), is_inverted_u8x16), is_odd_u8x16);
        uint8x16_t fold_extended_u8x16 = vorrq_u8(is_extended_even_u8x16, vandq_u8(is_inverted_u8x16, is_odd_u8x16));

        // 3. Latin Extended-B: 'Ơ' (C6 A0) and 'Ư' (C6 AF) → +1
        uint8x16_t is_c6_target_u8x16 = vandq_u8(is_after_c6_u8x16, vorrq_u8(vceqq_u8(text_u8x16, vdupq_n_u8(0xA0)),
                                                                             vceqq_u8(text_u8x16, vdupq_n_u8(0xAF))));

        // 4. Latin Extended Additional: EVEN third bytes after an E1 B8-BB pair → +1,
        //    except the expanding E1 BA 96-9F block
        uint8x16_t is_after_e1_pair_u8x16 = vandq_u8(
            vceqq_u8(previous2_bytes_u8x16, vdupq_n_u8(0xE1)),
            sz_utf8_uncased_neon_in_byte_range_u8x16_(previous_bytes_u8x16, 0xB8, 0x04));
        uint8x16_t is_excluded_third_u8x16 = vandq_u8(
            vceqq_u8(previous_bytes_u8x16, vdupq_n_u8(0xBA)),
            sz_utf8_uncased_neon_in_byte_range_u8x16_(text_u8x16, 0x96, 0x0A));
        uint8x16_t fold_e1_u8x16 = vbicq_u8(vbicq_u8(is_after_e1_pair_u8x16, is_excluded_third_u8x16), is_odd_u8x16);

        // Disjoint positions merge into ONE offset vector and a single add
        uint8x16_t is_plus_one_u8x16 = vorrq_u8(fold_extended_u8x16, vorrq_u8(is_c6_target_u8x16, fold_e1_u8x16));
        uint8x16_t offsets_u8x16 = vorrq_u8(vandq_u8(is_c3_target_u8x16, vdupq_n_u8(0x20)),
                                            vandq_u8(is_plus_one_u8x16, vdupq_n_u8(0x01)));
        result_u8x16x2.val[register_index] = vaddq_u8(result_u8x16, offsets_u8x16);
    }
    return result_u8x16x2;
}

/**
 *  @brief Alarm function for Vietnamese danger zone detection.
 *
 *  Detects positions where danger characters occur that require special handling:
 *  - E1 BA 96-9F: 'ẖ'-'ẟ' expand to ASCII-led sequences when folded ('ẞ' → "ss"); the
 *    third-byte qualification matters because the rest of E1 BA covers Vietnamese letters
 *    that fold in place - flagging them blanket-style would send dense Vietnamese text
 *    into the serial danger-zone scanner on every chunk
 *  - C3 9F: 'ß' (U+00DF) → "ss" (1 rune → 2 runes)
 *  - C5 BF: 'ſ' (U+017F) → 's' (2 bytes → 1 byte)
 *  - EF AC 80-86: Latin ligatures 'ﬀ'-'ﬆ' → ASCII pairs/triples
 *  - E2 84 AA: 'K' Kelvin sign (3 bytes → 1 byte)
 *
 *  Ice Lake qualifies the third-byte range compare with the load mask; here the driver's
 *  padded loads already zero every absent byte, and zero never lands inside [96, 9F], so
 *  the unqualified compare is exactly as safe-negative on tail chunks. Unlike the other
 *  alarms, the result is shifted back to the SEQUENCE-START positions, mirroring the
 *  reference bit-for-bit.
 */
SZ_HELPER_NOINLINE sz_u32_t sz_utf8_uncased_search_neon_vietnamese_alarm_u8x16x2_(uint8x16x2_t text_u8x16x2,
                                                                                  sz_u32_t load_mask) {
    sz_unused_(load_mask); // Padded loads zero absent bytes, so range compares are safe-negative

    // All hazards anchored at their second byte: lead from `previous`, the E1 BA expanding third
    // byte from `next`. The dense-E1 early exit is unnecessary now that the per-value movemasks are
    // gone - the whole alarm is a handful of `vceqq`/`vand` ops gated by one `vmaxvq_u8`.
    uint8x16x2_t previous_u8x16x2 = sz_utf8_uncased_neon_previous_bytes_u8x16x2_(text_u8x16x2);
    uint8x16x2_t next_u8x16x2 = sz_utf8_uncased_neon_next_bytes_u8x16x2_(text_u8x16x2);

    uint8x16_t any_danger_u8x16 = vdupq_n_u8(0);
    for (sz_size_t register_index = 0; register_index != 2; ++register_index) {
        uint8x16_t text_u8x16 = text_u8x16x2.val[register_index];
        uint8x16_t previous_u8x16 = previous_u8x16x2.val[register_index];
        uint8x16_t next_u8x16 = next_u8x16x2.val[register_index];

        uint8x16_t danger_u8x16 = vandq_u8( // E1 BA 96-9F (expanding third byte)
            vandq_u8(vceqq_u8(text_u8x16, vdupq_n_u8(0xBA)), vceqq_u8(previous_u8x16, vdupq_n_u8(0xE1))),
            sz_utf8_uncased_neon_in_byte_range_u8x16_(next_u8x16, 0x96, 0x0A));
        danger_u8x16 = vorrq_u8( // Sharp S (C3 9F)
            danger_u8x16, vandq_u8(vceqq_u8(text_u8x16, vdupq_n_u8(0x9F)), vceqq_u8(previous_u8x16, vdupq_n_u8(0xC3))));
        danger_u8x16 = vorrq_u8( // Long S (C5 BF)
            danger_u8x16, vandq_u8(vceqq_u8(text_u8x16, vdupq_n_u8(0xBF)), vceqq_u8(previous_u8x16, vdupq_n_u8(0xC5))));
        danger_u8x16 = vorrq_u8( // Ligatures (EF AC xx)
            danger_u8x16, vandq_u8(vceqq_u8(text_u8x16, vdupq_n_u8(0xAC)), vceqq_u8(previous_u8x16, vdupq_n_u8(0xEF))));
        danger_u8x16 = vorrq_u8( // Kelvin (E2 84 xx)
            danger_u8x16, vandq_u8(vceqq_u8(text_u8x16, vdupq_n_u8(0x84)), vceqq_u8(previous_u8x16, vdupq_n_u8(0xE2))));
        any_danger_u8x16 = vorrq_u8(any_danger_u8x16, danger_u8x16);
    }
    return vmaxvq_u8(any_danger_u8x16);
}

/**
 *  @brief Vietnamese uncased search for needles with safe slices up to 16 bytes.
 *  @sa sz_utf8_uncased_rune_safe_vietnamese_k
 */
SZ_HELPER_AUTO sz_cptr_t sz_utf8_uncased_search_neon_vietnamese_( //
    sz_cptr_t haystack, sz_size_t haystack_length,                //
    sz_cptr_t needle, sz_size_t needle_length,                    //
    sz_utf8_uncased_needle_metadata_t const *needle_metadata,     //
    sz_size_t *matched_length) {
    return sz_utf8_uncased_search_neon_scripted_( //
        sz_utf8_uncased_search_neon_vietnamese_fold_u8x16x2_,
        sz_utf8_uncased_search_neon_vietnamese_alarm_u8x16x2_, //
        haystack, haystack_length, needle, needle_length, needle_metadata, matched_length);
}



/**
 *  @brief Alarm function for Georgian danger zone detection.
 *
 *  Georgian Mkhedruli (E1 83 xx and the tail of E1 82) is caseless, so the haystack-side
 *  hazards are the OTHER Georgian scripts, which all fold across blocks:
 *  - E1 B2 xx: Mtavruli uppercase, folds to Mkhedruli
 *  - E1 82 A0-E5: Asomtavruli historical uppercase, folds to Nuskhuri
 *  - E2 B4 xx: Nuskhuri, target of Asomtavruli folds
 *
 *  Modern Georgian is E1 83 leads, so neither second-byte pair matches and the kernel
 *  almost never alarms. The Asomtavruli third-byte range compare is unqualified by the
 *  load mask: the driver's padded loads zero absent bytes, and zero never lands inside
 *  [A0, E5], so tail chunks stay safe-negative. The result is shifted back to the
 *  SEQUENCE-START positions, mirroring the reference bit-for-bit.
 */
SZ_HELPER_NOINLINE sz_u32_t sz_utf8_uncased_search_neon_georgian_alarm_u8x16x2_(uint8x16x2_t text_u8x16x2,
                                                                                sz_u32_t load_mask) {
    sz_unused_(load_mask); // Padded loads zero absent bytes, so range compares are safe-negative

    // E1 B2 = Mtavruli; E1 82 refines on the A0-E5 third (next) byte for Asomtavruli; E2 B4 =
    // Nuskhuri. Anchored at the second byte (lead from `previous`), gated by one `vmaxvq_u8`.
    uint8x16x2_t previous_u8x16x2 = sz_utf8_uncased_neon_previous_bytes_u8x16x2_(text_u8x16x2);
    uint8x16x2_t next_u8x16x2 = sz_utf8_uncased_neon_next_bytes_u8x16x2_(text_u8x16x2);

    uint8x16_t any_danger_u8x16 = vdupq_n_u8(0);
    for (sz_size_t register_index = 0; register_index != 2; ++register_index) {
        uint8x16_t text_u8x16 = text_u8x16x2.val[register_index];
        uint8x16_t previous_u8x16 = previous_u8x16x2.val[register_index];
        uint8x16_t next_u8x16 = next_u8x16x2.val[register_index];
        uint8x16_t is_after_e1_u8x16 = vceqq_u8(previous_u8x16, vdupq_n_u8(0xE1));

        uint8x16_t danger_u8x16 = vandq_u8( // Mtavruli (E1 B2)
            vceqq_u8(text_u8x16, vdupq_n_u8(0xB2)), is_after_e1_u8x16);
        danger_u8x16 = vorrq_u8( // Asomtavruli (E1 82 A0-E5)
            danger_u8x16, vandq_u8(vandq_u8(vceqq_u8(text_u8x16, vdupq_n_u8(0x82)), is_after_e1_u8x16),
                                   sz_utf8_uncased_neon_in_byte_range_u8x16_(next_u8x16, 0xA0, 0x46)));
        danger_u8x16 = vorrq_u8( // Nuskhuri (E2 B4)
            danger_u8x16, vandq_u8(vceqq_u8(text_u8x16, vdupq_n_u8(0xB4)), vceqq_u8(previous_u8x16, vdupq_n_u8(0xE2))));
        any_danger_u8x16 = vorrq_u8(any_danger_u8x16, danger_u8x16);
    }
    return vmaxvq_u8(any_danger_u8x16);
}

/**
 *  @brief Georgian uncased search for needles with safe slices up to 16 bytes.
 *  @sa sz_utf8_uncased_rune_safe_georgian_k
 *
 *  The fastest non-ASCII kernel: Mkhedruli is caseless, so the fold callback is just the
 *  ASCII fold for mixed Latin text and the alarm only watches for the historical scripts.
 */
SZ_HELPER_AUTO sz_cptr_t sz_utf8_uncased_search_neon_georgian_( //
    sz_cptr_t haystack, sz_size_t haystack_length,              //
    sz_cptr_t needle, sz_size_t needle_length,                  //
    sz_utf8_uncased_needle_metadata_t const *needle_metadata,   //
    sz_size_t *matched_length) {
    return sz_utf8_uncased_search_neon_scripted_( //
        sz_utf8_uncased_search_neon_ascii_fold_u8x16x2_,
        sz_utf8_uncased_search_neon_georgian_alarm_u8x16x2_, //
        haystack, haystack_length, needle, needle_length, needle_metadata, matched_length);
}


SZ_HELPER_AUTO sz_cptr_t sz_utf8_uncased_search_neon( //
    sz_cptr_t haystack, sz_size_t haystack_length,     //
    sz_cptr_t needle, sz_size_t needle_length,         //
    sz_utf8_uncased_needle_metadata_t *needle_metadata, sz_size_t *matched_length) {

    // Handle the obvious edge cases first
    if (needle_length == 0) {
        *matched_length = 0;
        return haystack;
    }

    // If the needle is entirely made of case-less characters - perform direct substring search
    int const is_unknown = needle_metadata->kernel_id == sz_utf8_uncased_rune_unknown_k;
    int const known_agnostic = needle_metadata->kernel_id == sz_utf8_uncased_rune_invariant_k;
    if (known_agnostic || (is_unknown && sz_utf8_find_cased_neon(needle, needle_length) == SZ_NULL_CHAR)) {
        sz_cptr_t result = sz_find_neon(haystack, haystack_length, needle, needle_length);
        *matched_length = result ? needle_length : 0;
        return result;
    }

    // Analyze needle to find the best safe window for each script
    if (is_unknown) {
        sz_utf8_uncased_needle_metadata_(needle, needle_length, needle_metadata);
        // If no SIMD-safe window found, fall back to serial immediately
        if (needle_metadata->kernel_id == sz_utf8_uncased_rune_fallback_serial_k)
            return sz_utf8_uncased_search_serial(haystack, haystack_length, needle, needle_length, needle_metadata,
                                                 matched_length);
    }

    // Dispatch to appropriate kernel
    if (needle_metadata->kernel_id == sz_utf8_uncased_rune_ascii_invariant_k) {
        if (needle_metadata->folded_slice_length <= 3)
            return sz_utf8_uncased_search_neon_ascii_3probe_( //
                haystack, haystack_length, needle, needle_length, needle_metadata, matched_length);
        else
            return sz_utf8_uncased_search_neon_ascii_4probe_( //
                haystack, haystack_length, needle, needle_length, needle_metadata, matched_length);
    }

    if (needle_metadata->kernel_id == sz_utf8_uncased_rune_safe_western_europe_k)
        return sz_utf8_uncased_search_neon_western_europe_( //
            haystack, haystack_length, needle, needle_length, needle_metadata, matched_length);

    if (needle_metadata->kernel_id == sz_utf8_uncased_rune_safe_central_europe_k)
        return sz_utf8_uncased_search_neon_central_europe_( //
            haystack, haystack_length, needle, needle_length, needle_metadata, matched_length);

    if (needle_metadata->kernel_id == sz_utf8_uncased_rune_safe_cyrillic_k)
        return sz_utf8_uncased_search_neon_cyrillic_( //
            haystack, haystack_length, needle, needle_length, needle_metadata, matched_length);

    if (needle_metadata->kernel_id == sz_utf8_uncased_rune_safe_greek_k)
        return sz_utf8_uncased_search_neon_greek_( //
            haystack, haystack_length, needle, needle_length, needle_metadata, matched_length);

    if (needle_metadata->kernel_id == sz_utf8_uncased_rune_safe_armenian_k)
        return sz_utf8_uncased_search_neon_armenian_( //
            haystack, haystack_length, needle, needle_length, needle_metadata, matched_length);

    if (needle_metadata->kernel_id == sz_utf8_uncased_rune_safe_vietnamese_k)
        return sz_utf8_uncased_search_neon_vietnamese_( //
            haystack, haystack_length, needle, needle_length, needle_metadata, matched_length);

    if (needle_metadata->kernel_id == sz_utf8_uncased_rune_safe_georgian_k)
        return sz_utf8_uncased_search_neon_georgian_( //
            haystack, haystack_length, needle, needle_length, needle_metadata, matched_length);

    // No suitable SIMD path found (needle has complex Unicode), fall back to serial
    needle_metadata->kernel_id = sz_utf8_uncased_rune_fallback_serial_k;
    return sz_utf8_uncased_search_serial(haystack, haystack_length, needle, needle_length, needle_metadata,
                                         matched_length);
}

SZ_API_COMPTIME sz_cptr_t sz_utf8_find_cased_neon(sz_cptr_t str, sz_size_t length) {
    sz_cptr_t text_cursor = str;

    // Single loop: advance by min(length, 29), check leads in the first `block_length` positions;
    // the 3-byte slack keeps every checked lead's continuation bytes inside the same 32-byte load
    while (length) {
        sz_size_t block_length = sz_min_of_two(length, 29);
        sz_u32_t lead_mask = sz_utf8_uncased_neon_mask_until_(block_length);
        uint8x16x2_t text_u8x16x2 = length >= 32 ? vld1q_u8_x2((sz_u8_t const *)text_cursor)
                                                 : sz_utf8_uncased_neon_load_padded_u8x16x2_(text_cursor, length);
        uint8x16_t low_u8x16 = text_u8x16x2.val[0], high_u8x16 = text_u8x16x2.val[1];

        // 1. ASCII letter check (zeros beyond the string are fine - not letters)
        sz_u32_t is_upper_mask = sz_utf8_uncased_neon_movemask_u8x16x2_(
            sz_utf8_uncased_neon_in_byte_range_u8x16_(low_u8x16, 'A', 26),
            sz_utf8_uncased_neon_in_byte_range_u8x16_(high_u8x16, 'A', 26));
        sz_u32_t is_lower_mask = sz_utf8_uncased_neon_movemask_u8x16x2_(
            sz_utf8_uncased_neon_in_byte_range_u8x16_(low_u8x16, 'a', 26),
            sz_utf8_uncased_neon_in_byte_range_u8x16_(high_u8x16, 'a', 26));
        if (is_upper_mask | is_lower_mask) return sz_utf8_find_cased_serial(text_cursor, length);

        // 2. Check for non-ASCII in lead positions
        sz_u32_t is_non_ascii_mask = sz_utf8_uncased_neon_movemask_u8x16x2_(vcgeq_u8(low_u8x16, vdupq_n_u8(0x80)),
                                                                            vcgeq_u8(high_u8x16, vdupq_n_u8(0x80))) &
                                     lead_mask;
        if (is_non_ascii_mask) {
            // 3. Identify UTF-8 lead bytes
            uint8x16_t const xe0_u8x16 = vdupq_n_u8(0xE0);
            uint8x16_t const xf0_u8x16 = vdupq_n_u8(0xF0);
            sz_u32_t is_two_mask = sz_utf8_uncased_neon_movemask_u8x16x2_(
                                       vceqq_u8(vandq_u8(low_u8x16, xe0_u8x16), vdupq_n_u8(0xC0)),
                                       vceqq_u8(vandq_u8(high_u8x16, xe0_u8x16), vdupq_n_u8(0xC0))) &
                                   lead_mask;
            sz_u32_t is_three_mask = sz_utf8_uncased_neon_movemask_u8x16x2_(
                                         vceqq_u8(vandq_u8(low_u8x16, xf0_u8x16), xe0_u8x16),
                                         vceqq_u8(vandq_u8(high_u8x16, xf0_u8x16), xe0_u8x16)) &
                                     lead_mask;
            sz_u32_t is_four_mask = sz_utf8_uncased_neon_movemask_u8x16x2_(
                                        vceqq_u8(vandq_u8(low_u8x16, vdupq_n_u8(0xF8)), xf0_u8x16),
                                        vceqq_u8(vandq_u8(high_u8x16, vdupq_n_u8(0xF8)), xf0_u8x16)) &
                                    lead_mask;

            // 4. Check 4-byte bicameral scripts (SMP): F0 with second byte 90/91/96/9D/9E
            if (is_four_mask) {
                sz_u32_t after_f0_mask = is_four_mask << 1;
                sz_u32_t is_90_mask = sz_utf8_uncased_neon_movemask_u8x16x2_(vceqq_u8(low_u8x16, vdupq_n_u8(0x90)),
                                                                             vceqq_u8(high_u8x16, vdupq_n_u8(0x90)));
                sz_u32_t is_91_mask = sz_utf8_uncased_neon_movemask_u8x16x2_(vceqq_u8(low_u8x16, vdupq_n_u8(0x91)),
                                                                             vceqq_u8(high_u8x16, vdupq_n_u8(0x91)));
                sz_u32_t is_96_mask = sz_utf8_uncased_neon_movemask_u8x16x2_(vceqq_u8(low_u8x16, vdupq_n_u8(0x96)),
                                                                             vceqq_u8(high_u8x16, vdupq_n_u8(0x96)));
                sz_u32_t is_9d_mask = sz_utf8_uncased_neon_movemask_u8x16x2_(vceqq_u8(low_u8x16, vdupq_n_u8(0x9D)),
                                                                             vceqq_u8(high_u8x16, vdupq_n_u8(0x9D)));
                sz_u32_t is_9e_mask = sz_utf8_uncased_neon_movemask_u8x16x2_(vceqq_u8(low_u8x16, vdupq_n_u8(0x9E)),
                                                                             vceqq_u8(high_u8x16, vdupq_n_u8(0x9E)));
                if (after_f0_mask & (is_90_mask | is_91_mask | is_96_mask | is_9d_mask | is_9e_mask))
                    return sz_utf8_find_cased_serial(text_cursor, length);
            }

            // 5. Check 2-byte bicameral leads: C3-D6
            // C3-CF: Latin Extended (umlauts, accents, Eszett)
            // D0-D1: Cyrillic, D4-D6: Armenian (D6 needed for small letters U+0580+)
            if (is_two_mask) {
                sz_u32_t is_bicameral_mask = sz_utf8_uncased_neon_movemask_u8x16x2_(
                    sz_utf8_uncased_neon_in_byte_range_u8x16_(low_u8x16, 0xC3, 0x14),
                    sz_utf8_uncased_neon_in_byte_range_u8x16_(high_u8x16, 0xC3, 0x14));

                // Special case: C2 B5 = U+00B5 MICRO SIGN folds to Greek mu (U+03BC)
                sz_u32_t is_c2_mask = sz_utf8_uncased_neon_movemask_u8x16x2_(vceqq_u8(low_u8x16, vdupq_n_u8(0xC2)),
                                                                             vceqq_u8(high_u8x16, vdupq_n_u8(0xC2))) &
                                      is_two_mask;
                if (is_c2_mask) {
                    sz_u32_t is_b5_mask = sz_utf8_uncased_neon_movemask_u8x16x2_(
                        vceqq_u8(low_u8x16, vdupq_n_u8(0xB5)), vceqq_u8(high_u8x16, vdupq_n_u8(0xB5)));
                    if ((is_c2_mask << 1) & is_b5_mask) return sz_utf8_find_cased_serial(text_cursor, length);
                }

                // Note: CA 80-BF includes both IPA Extensions (U+0280-02AF) and Spacing Modifier Letters
                // (U+02B0-02BF). Spacing Modifier Letters CAN appear in case fold expansions:
                // e.g., ẚ (U+1E9A) folds to [a, ʾ] where ʾ = U+02BE is a Spacing Modifier Letter.
                // So we must NOT exclude this range from the bicameral check.
                if (is_bicameral_mask & is_two_mask) return sz_utf8_find_cased_serial(text_cursor, length);
            }

            // 6. Check 3-byte bicameral sequences
            if (is_three_mask) {
                // E1: Georgian, Greek Extended, Latin Extended Additional
                sz_u32_t is_e1_mask = sz_utf8_uncased_neon_movemask_u8x16x2_(vceqq_u8(low_u8x16, vdupq_n_u8(0xE1)),
                                                                             vceqq_u8(high_u8x16, vdupq_n_u8(0xE1)));
                if (is_e1_mask & is_three_mask) return sz_utf8_find_cased_serial(text_cursor, length);

                // EF: Fullwidth Latin
                sz_u32_t is_ef_mask = sz_utf8_uncased_neon_movemask_u8x16x2_(vceqq_u8(low_u8x16, vdupq_n_u8(0xEF)),
                                                                             vceqq_u8(high_u8x16, vdupq_n_u8(0xEF)));
                if (is_ef_mask & is_three_mask) return sz_utf8_find_cased_serial(text_cursor, length);

                // E2: Safe only for second byte 80-83
                sz_u32_t is_e2_mask = sz_utf8_uncased_neon_movemask_u8x16x2_(vceqq_u8(low_u8x16, vdupq_n_u8(0xE2)),
                                                                             vceqq_u8(high_u8x16, vdupq_n_u8(0xE2))) &
                                      is_three_mask;
                if (is_e2_mask) {
                    sz_u32_t e2_second_safe_mask = sz_utf8_uncased_neon_movemask_u8x16x2_(
                        sz_utf8_uncased_neon_in_byte_range_u8x16_(low_u8x16, 0x80, 0x04),
                        sz_utf8_uncased_neon_in_byte_range_u8x16_(high_u8x16, 0x80, 0x04));
                    if ((is_e2_mask << 1) & ~e2_second_safe_mask) return sz_utf8_find_cased_serial(text_cursor, length);
                }

                // EA: Bicameral second bytes 99-9F, AC-AE
                sz_u32_t is_ea_mask = sz_utf8_uncased_neon_movemask_u8x16x2_(vceqq_u8(low_u8x16, vdupq_n_u8(0xEA)),
                                                                             vceqq_u8(high_u8x16, vdupq_n_u8(0xEA))) &
                                      is_three_mask;
                if (is_ea_mask) {
                    sz_u32_t is_99_range_mask = sz_utf8_uncased_neon_movemask_u8x16x2_(
                        sz_utf8_uncased_neon_in_byte_range_u8x16_(low_u8x16, 0x99, 0x07),
                        sz_utf8_uncased_neon_in_byte_range_u8x16_(high_u8x16, 0x99, 0x07));
                    sz_u32_t is_ac_range_mask = sz_utf8_uncased_neon_movemask_u8x16x2_(
                        sz_utf8_uncased_neon_in_byte_range_u8x16_(low_u8x16, 0xAC, 0x03),
                        sz_utf8_uncased_neon_in_byte_range_u8x16_(high_u8x16, 0xAC, 0x03));
                    if ((is_ea_mask << 1) & (is_99_range_mask | is_ac_range_mask))
                        return sz_utf8_find_cased_serial(text_cursor, length);
                }
            }
        }

        text_cursor += block_length;
        length -= block_length;
    }

    return SZ_NULL_CHAR;
}


#if defined(__clang__)
#pragma clang attribute pop
#elif defined(__GNUC__)
#pragma GCC pop_options
#endif


/**
 * Finds the first full-Unicode case-insensitive UTF-8 match.
 *
 * The returned index and both lengths are byte counts. A result of -1 means
 * no match. An empty needle returns 0. No NUL terminators are required.
 */
// gocc: indexFoldNeon(haystack string, needle string, metadata *indexFoldNeedleMetadata, matched_length *uint64) int
int64_t index_utf8_uncased_neon(unsigned char *haystack, uint64_t haystack_len,
                                unsigned char *needle, uint64_t needle_len,
                                void *metadata, uint64_t *matched_length) {
    sz_utf8_uncased_needle_metadata_t *needle_metadata =
        (sz_utf8_uncased_needle_metadata_t *)metadata;
    sz_cptr_t match;

    _Static_assert(sizeof(sz_utf8_uncased_needle_metadata_t) == 40, "unexpected metadata layout");

    if (needle_len == 0) return 0;
    if (haystack_len == 0) return -1;
    if (haystack_len > (uint64_t)INT64_MAX || needle_len > (uint64_t)INT64_MAX) return -1;

    match = sz_utf8_uncased_search_neon((sz_cptr_t)haystack, (sz_size_t)haystack_len, (sz_cptr_t)needle,
                                        (sz_size_t)needle_len, needle_metadata, (sz_size_t *)matched_length);
    return match ? (int64_t)(match - (sz_cptr_t)haystack) : -1;
}
