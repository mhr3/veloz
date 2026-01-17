#include <stdint.h>
#include <stdbool.h>
#include <arm_neon.h>

// IndexAny using 256-bit bitset approach.
// Supports unlimited search chars with consistent O(n) performance.
// The bitset is passed pre-built from Go to avoid stack arrays in C.
//
// gocc: indexAnyNeonBitset(data string, bitset0 uint64, bitset1 uint64, bitset2 uint64, bitset3 uint64) int
int64_t index_any_neon_bitset(unsigned char *data, uint64_t data_len, 
    uint64_t bitset0, uint64_t bitset1, uint64_t bitset2, uint64_t bitset3)
{
    if (data_len == 0) {
        return -1;
    }

    const uint64_t blockSize = 16;
    const unsigned char *data_start = data;
    
    // Build 256-bit bitset from 4 uint64s - fits in 2 NEON registers for TBL2
    uint8x16x2_t bitset;
    bitset.val[0] = vcombine_u8(vcreate_u8(bitset0), vcreate_u8(bitset1));
    bitset.val[1] = vcombine_u8(vcreate_u8(bitset2), vcreate_u8(bitset3));
    
    // Mask to extract bit position (0-7) and index (0-31)
    const uint8x16_t mask7 = vdupq_n_u8(7);
    const uint8x16_t mask31 = vdupq_n_u8(31);
    
    // Bit position lookup table: 1<<0, 1<<1, ..., 1<<7 (repeated for 16 bytes)
    // In little-endian: byte 0 = 0x01, byte 1 = 0x02, ..., byte 7 = 0x80
    const uint8x16_t bit_lut = vcombine_u8(
        vcreate_u8(0x8040201008040201ULL),
        vcreate_u8(0x8040201008040201ULL)
    );
    
    // Process 16 bytes at a time
    for (const unsigned char *data_end = (data + data_len) - (data_len % blockSize); data < data_end; data += blockSize)
    {
        uint8x16_t d = vld1q_u8(data);
        
        // idx = d >> 3 (which byte in 32-byte bitset, masked to 0-31)
        uint8x16_t idx = vandq_u8(vshrq_n_u8(d, 3), mask31);
        
        // bit_pos = d & 7 (which bit within that byte)
        uint8x16_t bit_pos = vandq_u8(d, mask7);
        
        // Look up the bitset byte for each lane
        uint8x16_t bitset_bytes = vqtbl2q_u8(bitset, idx);
        
        // Look up the bit mask for each lane (bit_lut[bit_pos] = 1 << bit_pos)
        uint8x16_t bit_masks = vqtbl1q_u8(bit_lut, bit_pos);
        
        // Check if the bit is set: (bitset_bytes & bit_masks) != 0
        uint8x16_t match = vtstq_u8(bitset_bytes, bit_masks);
        
        // Check if any match found
        uint64_t match64 = vget_lane_u64(vshrn_n_u16(match, 4), 0);
        if (match64) {
            int pos = __builtin_ctzll(match64) / 4;
            return (data - data_start) + pos;
        }
    }
    data_len %= blockSize;
    
    // Handle remainder with scalar bitset lookup
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



// gocc: ValidString(data string) bool
bool ascii_valid_string(unsigned char *data, uint64_t length)
{
    const uint64_t blockSize = 16; // NEON can process 128 bits (16 bytes) at a time
    const uint64_t ld4BlockSize = blockSize * 4;

    if (length >= blockSize)
    {
        const uint8x16_t msb_mask = vdupq_n_u8(0x80); // Create a mask for the MSB

#ifndef SKIP_LD1x4
        for (const unsigned char *data64_end = (data + length) - (length % ld4BlockSize); data < data64_end; data += ld4BlockSize)
        {
            uint8x16x4_t blocks = vld1q_u8_x4(data);           // Load 64 bytes of data
            blocks.val[0] = vtstq_u8(blocks.val[0], msb_mask); // AND with the mask to isolate MSB
            blocks.val[1] = vtstq_u8(blocks.val[1], msb_mask); // AND with the mask to isolate MSB
            blocks.val[2] = vtstq_u8(blocks.val[2], msb_mask); // AND with the mask to isolate MSB
            blocks.val[3] = vtstq_u8(blocks.val[3], msb_mask); // AND with the mask to isolate MSB

            uint8x16_t result = vorrq_u8(vorrq_u8(blocks.val[0], blocks.val[1]), vorrq_u8(blocks.val[2], blocks.val[3])); // OR the results
            // SHRN can be faster than UMAXV
            if (vget_lane_u64(vshrn_n_u16(result, 4), 0) > 0)
            {
                return false;
            }
        }
        length %= ld4BlockSize;
#endif
        for (const unsigned char *data16_end = (data + length) - (length % blockSize); data < data16_end; data += blockSize)
        {
            uint8x16_t block = vld1q_u8(data);             // Load 16 bytes of data
            uint8x16_t result = vtstq_u8(block, msb_mask); // AND with the mask to isolate MSB
            // SHRN can be faster than UMAXV
            if (vget_lane_u64(vshrn_n_u16(result, 4), 0) > 0)
            {
                return false;
            }
        }
        length %= blockSize;
    }

    if (length & 8)
    {
        uint64_t lo64, hi64;
        uint8_t *data_end = data + length;
        __builtin_memcpy(&lo64, data, sizeof(lo64));
        __builtin_memcpy(&hi64, data_end - 8, sizeof(hi64));

        uint64_t data64 = lo64 | hi64;
        return (data64 & 0x8080808080808080ull) ? false : true;
    }

    if (length & 4)
    {
        uint32_t lo32, hi32;
        uint8_t *data_end = data + length;
        __builtin_memcpy(&lo32, data, sizeof(lo32));
        __builtin_memcpy(&hi32, data_end - 4, sizeof(hi32));

        uint32_t data32 = lo32 | hi32;
        return (data32 & 0x80808080) ? false : true;
    }

    if (length == 0) return true;

    uint32_t data32 = 0;

    // branchless check for 1-3 bytes
    uint8_t *data_end = data + length;
    int idx = length >> 1;
    data32 |= data[0];
    data32 |= data[idx];
    data32 |= data_end[-1];

    return (data32 & 0x80808080) ? false : true;
}

// gocc: IndexMask(data string, mask byte) int
int64_t index_mask(unsigned char *data, uint64_t length, uint8_t mask)
{
    const uint64_t blockSize = 16; // NEON can process 128 bits (16 bytes) at a time
    const uint64_t ld4BlockSize = blockSize * 4;

    const unsigned char *data_start = data;
    
    if (length >= blockSize)
    {
        uint8x16_t mask_vec = vdupq_n_u8(mask); // Create a vector mask
#ifndef SKIP_LD1x4
        for (const unsigned char *data64_end = (data + length) - (length % ld4BlockSize); data < data64_end; data += ld4BlockSize)
        {
            uint8x16x4_t blocks = vld1q_u8_x4(data);       // Load 64 bytes of data
            blocks.val[0] = vtstq_u8(blocks.val[0], mask_vec); // AND with the mask
            blocks.val[1] = vtstq_u8(blocks.val[1], mask_vec); // AND with the mask
            blocks.val[2] = vtstq_u8(blocks.val[2], mask_vec); // AND with the mask
            blocks.val[3] = vtstq_u8(blocks.val[3], mask_vec); // AND with the mask

            uint8x16_t result = vorrq_u8(vorrq_u8(blocks.val[0], blocks.val[1]), vorrq_u8(blocks.val[2], blocks.val[3])); // OR the results
            // SHRN can be faster than UMAXV
            if (vget_lane_u64(vshrn_n_u16(result, 4), 0) > 0)
            {
                // now a few operations to find the index of the first set bit
                for (int j = 0; j < 4; j++)
                {
                    uint64_t data64 = vget_lane_u64(vshrn_n_u16(blocks.val[j], 4), 0);
                    if (data64 != 0)
                    {
                        int64_t offset = j*16 + __builtin_ctzll(data64) / 4;
                        return (data - data_start) + offset;
                    }
                }
            }
        }
        length %= ld4BlockSize;
#endif
        for (const unsigned char *data16_end = (data + length) - (length % blockSize); data < data16_end; data += blockSize)
        {
            uint8x16_t block = vld1q_u8(data);         // Load 16 bytes of data
            uint8x16_t result = vtstq_u8(block, mask_vec); // AND with the mask

            // SHRN can be faster than UMAXV
            uint64_t data64 = vget_lane_u64(vshrn_n_u16(result, 4), 0);
            if (data64 > 0)
            {
                int offset = __builtin_ctzll(data64) / 4;
                return (data - data_start) + offset;
            }
        }
        length %= blockSize;
    }

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

static uint8_t uppercasingTable[32] = {
    0,32,32,32,32,32,32,32,32,32,32,32,32,32,32,32,
    32,32,32,32,32,32,32,32,32,32,32,0, 0, 0, 0, 0,
};

// Inline exact comparison (avoids bcmp/memcmp function call)
__attribute__((always_inline)) static inline bool equal_exact_inline(
    const unsigned char *a, const unsigned char *b, int64_t length)
{
    const int64_t blockSize = 16;
    int64_t i = 0;

    for (; i + blockSize <= length; i += blockSize) {
        uint8x16_t va = vld1q_u8(a + i);
        uint8x16_t vb = vld1q_u8(b + i);
        uint8x16_t cmp = vceqq_u8(va, vb);
        if (vget_lane_u64(vshrn_n_u16(cmp, 4), 0) != ~0ULL) {
            return false;
        }
    }

    for (; i < length; i++) {
        if (a[i] != b[i]) return false;
    }
    return true;
}

__attribute__((always_inline)) static inline bool equal_fold_core(unsigned char *a, unsigned char *b, int64_t length,
    const uint8x16x2_t table, const uint8x16_t shift)
{
    const uint64_t blockSize = 16; // NEON can process 128 bits (16 bytes) at a time

    if (length < 0) return false;

    for (const unsigned char *data_bound = (a + length) - (length % blockSize); a < data_bound; a += blockSize, b += blockSize)
    {
        uint8x16_t a_data = vld1q_u8(a); // Load 16 bytes of data
        uint8x16_t b_data = vld1q_u8(b); // Load 16 bytes of data

        a_data = vsubq_u8(a_data, shift);
        a_data = vsubq_u8(a_data, vqtbl2q_u8(table, a_data));

        b_data = vsubq_u8(b_data, shift);
        b_data = vsubq_u8(b_data, vqtbl2q_u8(table, b_data));

        // we should shift the data back, but we just need to compare, so we can skip that
        const uint8x16_t result = vceqq_u8(a_data, b_data);

        // SHRN can be faster than UMAXV
        if (vget_lane_u64(vshrn_n_u16(result, 4), 0) != ~0ull)
        {
            return false;
        }
    }
    length %= blockSize;

    // same as above, but with just half the vector register
    if (length >= 8)
    {
        uint8x8_t a_data = vld1_u8(a);
        uint8x8_t b_data = vld1_u8(b);

        a_data = vsub_u8(a_data, vget_low_u8(shift));
        a_data = vsub_u8(a_data, vqtbl2_u8(table, a_data));

        b_data = vsub_u8(b_data, vget_low_u8(shift));
        b_data = vsub_u8(b_data, vqtbl2_u8(table, b_data));

        // we should shift the data back, but we just need to compare, so we can skip that
        const uint8x8_t result = vceq_u8(a_data, b_data);

        // Check if there's any 0 bytes
        if (vget_lane_u64(result, 0) != ~0ull)
        {
            return false;
        }
        a += 8;
        b += 8;
        length %= 8;
    }

    if (length == 0) {
        return true;
    }

    uint64_t a_data64 = 0;
    uint64_t b_data64 = 0;

    if (length >= 4)
    {
        a_data64 = *(uint32_t *)(a);
        a += 4;
        b_data64 = *(uint32_t *)(b);
        b += 4;
        length -= 4;
    }

    // FIXME: this is reordering the bytes, though does it for both a and b, so it's fine
    switch (length)
    {
    case 3:
        a_data64 <<= 24;
        a_data64 |= *(uint16_t *)(a) << 8;
        a_data64 |= a[2];
        // same for b
        b_data64 <<= 24;
        b_data64 |= *(uint16_t *)(b) << 8;
        b_data64 |= b[2];
        break;
    case 2:
        a_data64 <<= 16;
        a_data64 |= *(uint16_t *)(a);
        // same for b
        b_data64 <<= 16;
        b_data64 |= *(uint16_t *)(b);
        break;
    case 1:
        a_data64 <<= 8;
        a_data64 |= *a;
        // same for b
        b_data64 <<= 8;
        b_data64 |= *b;
        break;
    }

    uint8x8_t a_data = vcreate_u8(a_data64);
    uint8x8_t b_data = vcreate_u8(b_data64);

    a_data = vsub_u8(a_data, vget_low_u8(shift));
    a_data = vsub_u8(a_data, vqtbl2_u8(table, a_data));

    b_data = vsub_u8(b_data, vget_low_u8(shift));
    b_data = vsub_u8(b_data, vqtbl2_u8(table, b_data));

    // we should shift the data back, but we just need to compare, so we can skip that
    const uint8x8_t result = vceq_u8(a_data, b_data);

    // Check if there's any 0 bytes
    if (vget_lane_u64(result, 0) != ~0ull)
    {
        return false;
    }

    return true;
}

// gocc: EqualFold(a, b string) bool
bool equal_fold(unsigned char *a, uint64_t a_len, unsigned char *b, uint64_t b_len)
{
    if (a_len != b_len)
    {
        return false;
    }

    const uint8x16x2_t table = vld1q_u8_x2(uppercasingTable);
    const uint8x16_t shift = vdupq_n_u8(0x60);

    return equal_fold_core(a, b, a_len, table, shift);
}

// loads up to 16 bytes of data into a 128-bit register
__attribute__((always_inline)) static inline uint8x16_t load_data16(const unsigned char *src, int64_t len) {
    if (len >= 16) {
        return vld1q_u8(src);
    } else if (len == 8) {
        return vcombine_u64(vld1_u64((uint64_t *)src), vcreate_u64(0));
    } else if (len <= 0) {
        return vdupq_n_u8(0);
    }

    const uint64_t orig_len = len;
    uint64_t data64 = 0;
    uint64_t data64lo;

    if (len & 8) {
        data64lo = *(uint64_t *)(src);
        src += 8;
        len -= 8;
    }

    if (len & 4) {
        data64 = *(uint32_t *)(src);
        src += 4;
        len -= 4;
    }

    uint64_t tmp;
    switch (len) {
    case 3:
        tmp = *(uint16_t *)(src);
        int shift = 8 * (orig_len & 4);
        data64 |= tmp << shift;
        tmp = src[2];
        data64 |= tmp << (16 + shift);
        break;
    case 2:
        tmp = *(uint16_t *)(src);
        data64 |= tmp << (8 * (orig_len & 4));
        break;
    case 1:
        tmp = *src;
        data64 |= tmp << (8 * (orig_len & 4));
        break;
    }

    if (orig_len < 8) {
        return vcombine_u64(vcreate_u64(data64), vcreate_u64(0));
    }
    return vcombine_u64(vcreate_u64(data64lo), vcreate_u64(data64));
}

// =============================================================================
// SIMD Rabin-Karp with Sills' unscaled hash + clausecker's NEON parallelism
// =============================================================================
// Key insight from Sills: Instead of scaling hash each iteration (H_{n+1} = B*H_n + ...),
// use unscaled hash H'_n = H_n * B^n. This moves the multiply out of the critical path.
// Recurrence: H'_{n+1} = H'_n + a_{n+w}*B^{n+w} - a_n*B^n
// To compare: H'_n == target * B^n
//
// Key insight from clausecker: Process 4 parallel hash streams using NEON uint32x4_t.
// Base B=31 is shift-friendly: x*31 = (x<<5) - x

// Case-fold lookup table: 'a'-'z' -> 'A'-'Z', others unchanged
static const uint8_t fold_table[256] = {
    0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16,17,18,19,20,21,22,23,24,25,26,27,28,29,30,31,
    32,33,34,35,36,37,38,39,40,41,42,43,44,45,46,47,48,49,50,51,52,53,54,55,56,57,58,59,60,61,62,63,
    64,65,66,67,68,69,70,71,72,73,74,75,76,77,78,79,80,81,82,83,84,85,86,87,88,89,90,91,92,93,94,95,
    96,65,66,67,68,69,70,71,72,73,74,75,76,77,78,79,80,81,82,83,84,85,86,87,88,89,90,123,124,125,126,127,
    128,129,130,131,132,133,134,135,136,137,138,139,140,141,142,143,144,145,146,147,148,149,150,151,152,153,154,155,156,157,158,159,
    160,161,162,163,164,165,166,167,168,169,170,171,172,173,174,175,176,177,178,179,180,181,182,183,184,185,186,187,188,189,190,191,
    192,193,194,195,196,197,198,199,200,201,202,203,204,205,206,207,208,209,210,211,212,213,214,215,216,217,218,219,220,221,222,223,
    224,225,226,227,228,229,230,231,232,233,234,235,236,237,238,239,240,241,242,243,244,245,246,247,248,249,250,251,252,253,254,255,
};

// Use same prime as Go stdlib for better hash distribution
#define PRIME_RK 16777619

// Compute B^n mod 2^32 using repeated squaring
static inline uint32_t pow_prime(uint64_t n) {
    uint32_t result = 1;
    uint32_t base = PRIME_RK;
    while (n > 0) {
        if (n & 1) result *= base;
        base *= base;
        n >>= 1;
    }
    return result;
}

// =============================================================================
// Fast NEON block-based initial hashing (reduces dependency depth)
// =============================================================================
// Key identity: H(s || t) = H(s) * B^{|t|} + H(t)
// Instead of ~3000 dependent steps, use ~48 steps for 3KB by hashing 64-byte blocks.

// Fold 16 bytes ASCII: 'a'..'z' -> 'A'..'Z'
static inline uint8x16_t fold16_ascii_rk(uint8x16_t v,
                                         uint8x16_t va, uint8x16_t vz, uint8x16_t v20)
{
    uint8x16_t x = vsubq_u8(v, va);
    uint8x16_t m = vcleq_u8(x, vz);
    return vsubq_u8(v, vandq_u8(m, v20));
}

// Compute hash of 16 folded bytes: b0*B^15 + b1*B^14 + ... + b15
__attribute__((always_inline)) static inline uint32_t hash16_from_folded(uint8x16_t b,
                                          uint32x4_t maskFF,
                                          uint32_t B, uint32_t B2, uint32_t B3,
                                          uint32x4_t w16 /* {B^12, B^8, B^4, 1} */)
{
    // Treat as 4x 32-bit words: word0 = bytes 0..3, word1 = bytes 4..7, etc.
    uint32x4_t w = vreinterpretq_u32_u8(b);

    // Extract byte-columns across 4 words:
    // v0 lanes: [b0,b4,b8,b12], v1: [b1,b5,b9,b13], ...
    uint32x4_t v0 = vandq_u32(w, maskFF);
    uint32x4_t v1 = vandq_u32(vshrq_n_u32(w, 8), maskFF);
    uint32x4_t v2 = vandq_u32(vshrq_n_u32(w, 16), maskFF);
    uint32x4_t v3 = vshrq_n_u32(w, 24);

    // q lane j = b[4j+0]*B^3 + b[4j+1]*B^2 + b[4j+2]*B + b[4j+3]
    uint32x4_t q = vmlaq_n_u32(v3, v2, B);
    q = vmlaq_n_u32(q, v1, B2);
    q = vmlaq_n_u32(q, v0, B3);

    // hash16 = q0*B^12 + q1*B^8 + q2*B^4 + q3
    uint32x4_t prod = vmulq_u32(q, w16);
    return vaddvq_u32(prod);
}

// Compute hash of 64 bytes with folding
__attribute__((always_inline)) static inline uint32_t hash64_fold_neon(const uint8_t *p,
                                        uint8x16_t va, uint8x16_t vz, uint8x16_t v20,
                                        uint32x4_t maskFF,
                                        uint32_t B, uint32_t B2, uint32_t B3,
                                        uint32x4_t w16, uint32x4_t w64 /* {B^48, B^32, B^16, 1} */)
{
    uint8x16_t b0 = fold16_ascii_rk(vld1q_u8(p +  0), va, vz, v20);
    uint8x16_t b1 = fold16_ascii_rk(vld1q_u8(p + 16), va, vz, v20);
    uint8x16_t b2 = fold16_ascii_rk(vld1q_u8(p + 32), va, vz, v20);
    uint8x16_t b3 = fold16_ascii_rk(vld1q_u8(p + 48), va, vz, v20);

    uint32_t h0 = hash16_from_folded(b0, maskFF, B, B2, B3, w16);
    uint32_t h1 = hash16_from_folded(b1, maskFF, B, B2, B3, w16);
    uint32_t h2 = hash16_from_folded(b2, maskFF, B, B2, B3, w16);
    uint32_t h3 = hash16_from_folded(b3, maskFF, B, B2, B3, w16);

    // hash64 = h0*B^48 + h1*B^32 + h2*B^16 + h3
    uint32x4_t hv = (uint32x4_t){h0, h1, h2, h3};
    return vaddvq_u32(vmulq_u32(hv, w64));
}

// Compute two hashes in one pass (needle and hay[0:w]) for better ILP
__attribute__((always_inline)) static inline void hash2_rk_fold_neon_fast(const uint8_t *a,
                                           const uint8_t *b,
                                           int64_t len,
                                           uint32_t *out_a,
                                           uint32_t *out_b)
{
    const uint32_t B   = PRIME_RK;
    const uint32_t B2  = B * B;
    const uint32_t B3  = B2 * B;
    const uint32_t B4  = B2 * B2;
    const uint32_t B8  = B4 * B4;
    const uint32_t B12 = B8 * B4;
    const uint32_t B16 = B8 * B8;
    const uint32_t B32 = B16 * B16;
    const uint32_t B48 = B32 * B16;
    const uint32_t B64 = B32 * B32;

    const uint32x4_t maskFF = vdupq_n_u32(0xFFu);
    const uint32x4_t w16 = (uint32x4_t){ B12, B8, B4, 1u };
    const uint32x4_t w64 = (uint32x4_t){ B48, B32, B16, 1u };

    const uint8x16_t va16  = vdupq_n_u8('a');
    const uint8x16_t vz16  = vdupq_n_u8('z' - 'a');
    const uint8x16_t v20_16= vdupq_n_u8(0x20u);

    uint32_t ha = 0u, hb = 0u;
    int64_t i = 0;

    // 64-byte blocks: dependency depth ~ len/64
    for (; i + 64 <= len; i += 64) {
        uint32_t ba = hash64_fold_neon(a + i, va16, vz16, v20_16,
                                       maskFF, B, B2, B3, w16, w64);
        uint32_t bb = hash64_fold_neon(b + i, va16, vz16, v20_16,
                                       maskFF, B, B2, B3, w16, w64);
        ha = ha * B64 + ba;
        hb = hb * B64 + bb;
    }

    // 16-byte blocks
    for (; i + 16 <= len; i += 16) {
        uint8x16_t fa = fold16_ascii_rk(vld1q_u8(a + i), va16, vz16, v20_16);
        uint8x16_t fb = fold16_ascii_rk(vld1q_u8(b + i), va16, vz16, v20_16);

        uint32_t ba = hash16_from_folded(fa, maskFF, B, B2, B3, w16);
        uint32_t bb = hash16_from_folded(fb, maskFF, B, B2, B3, w16);

        ha = ha * B16 + ba;
        hb = hb * B16 + bb;
    }

    // tail bytes
    for (; i < len; i++) {
        ha = ha * B + fold_table[a[i]];
        hb = hb * B + fold_table[b[i]];
    }

    *out_a = ha;
    *out_b = hb;
}

// =============================================================================
// SIMD Rabin-Karp macro - generates both case-sensitive and case-insensitive
// =============================================================================
// Key insight: Process 4 hash positions per iteration using NEON uint32x4_t.
// The only loop-carried dependency is H = H*B^4 + S, amortising mul latency by 4.
// CASE_FOLD: 1 = case-insensitive (fold to uppercase), 0 = case-sensitive

#define RABIN_KARP_IMPL(func_name, CASE_FOLD, HASH2_FN, VERIFY_FN) \
__attribute__((always_inline)) static inline int64_t func_name( \
    unsigned char *haystack, int64_t haystack_len, \
    unsigned char *needle, int64_t needle_len) \
{ \
    if (needle_len <= 0) return 0; \
    if (haystack_len < needle_len) return -1; \
    \
    const int64_t search_len = haystack_len - needle_len + 1; \
    const int64_t w = needle_len; \
    \
    /* For SIMD verification (case-fold only) */ \
    const uint8x16x2_t table = vld1q_u8_x2(uppercasingTable); \
    const uint8x16_t vtbl_shift = vdupq_n_u8(0x60); \
    (void)table; (void)vtbl_shift; \
    \
    /* Precompute constants */ \
    const uint32_t B = PRIME_RK; \
    const uint32_t B2 = B * B; \
    const uint32_t B3 = B2 * B; \
    const uint32_t B4 = B2 * B2; \
    const uint32_t Bw = pow_prime(w); \
    const uint32_t antisigma = 0u - Bw; \
    \
    /* Fast initial hashes: compute needle and hay[0:w] together */ \
    uint32_t target_hash, h0; \
    HASH2_FN(needle, haystack, w, &target_hash, &h0); \
    \
    /* For small search_len, use scalar */ \
    if (search_len < 4) { \
        uint32_t h = h0; \
        if (CASE_FOLD) { \
            if (h == target_hash && equal_fold_core(haystack, needle, needle_len, table, vtbl_shift)) \
                return 0; \
        } else { \
            if (h == target_hash && equal_exact_inline(haystack, needle, needle_len)) \
                return 0; \
        } \
        for (int64_t i = 1; i < search_len; i++) { \
            uint32_t oldc = CASE_FOLD ? fold_table[haystack[i - 1]] : haystack[i - 1]; \
            uint32_t newc = CASE_FOLD ? fold_table[haystack[i + w - 1]] : haystack[i + w - 1]; \
            h = h * B + newc + antisigma * oldc; \
            if (CASE_FOLD) { \
                if (h == target_hash && equal_fold_core(haystack + i, needle, needle_len, table, vtbl_shift)) \
                    return i; \
            } else { \
                if (h == target_hash && equal_exact_inline(haystack + i, needle, needle_len)) \
                    return i; \
            } \
        } \
        return -1; \
    } \
    \
    /* Build initial 4 hashes (pos 0..3) with 3 scalar rolls from h0 */ \
    uint32_t h1, h2, h3; \
    if (CASE_FOLD) { \
        h1 = h0 * B + fold_table[haystack[w + 0]] + antisigma * fold_table[haystack[0]]; \
        h2 = h1 * B + fold_table[haystack[w + 1]] + antisigma * fold_table[haystack[1]]; \
        h3 = h2 * B + fold_table[haystack[w + 2]] + antisigma * fold_table[haystack[2]]; \
    } else { \
        h1 = h0 * B + haystack[w + 0] + antisigma * haystack[0]; \
        h2 = h1 * B + haystack[w + 1] + antisigma * haystack[1]; \
        h3 = h2 * B + haystack[w + 2] + antisigma * haystack[2]; \
    } \
    \
    uint32x4_t H = (uint32x4_t){ h0, h1, h2, h3 }; \
    \
    /* Hoist vector constants */ \
    const uint32x4_t vB   = vdupq_n_u32(B); \
    const uint32x4_t vB2  = vdupq_n_u32(B2); \
    const uint32x4_t vB3  = vdupq_n_u32(B3); \
    const uint32x4_t vB4  = vdupq_n_u32(B4); \
    const uint32x4_t vAnti = vdupq_n_u32(antisigma); \
    const uint32x4_t vTgt = vdupq_n_u32(target_hash); \
    \
    /* Folding constants for 16-byte NEON path */ \
    const uint8x16_t va16   = vdupq_n_u8('a'); \
    const uint8x16_t vz16   = vdupq_n_u8('z' - 'a'); \
    const uint8x16_t v20_16 = vdupq_n_u8(0x20); \
    \
    int64_t pos = 0; \
    \
    for (;;) { \
        /* 1) Check 4 candidates in parallel */ \
        uint32x4_t eq = vceqq_u32(H, vTgt); \
        if (vmaxvq_u32(eq)) { \
            if (CASE_FOLD) { \
                if (vgetq_lane_u32(eq, 0) && equal_fold_core(haystack + pos + 0, needle, needle_len, table, vtbl_shift)) return pos + 0; \
                if (vgetq_lane_u32(eq, 1) && equal_fold_core(haystack + pos + 1, needle, needle_len, table, vtbl_shift)) return pos + 1; \
                if (vgetq_lane_u32(eq, 2) && equal_fold_core(haystack + pos + 2, needle, needle_len, table, vtbl_shift)) return pos + 2; \
                if (vgetq_lane_u32(eq, 3) && equal_fold_core(haystack + pos + 3, needle, needle_len, table, vtbl_shift)) return pos + 3; \
            } else { \
                if (vgetq_lane_u32(eq, 0) && equal_exact_inline(haystack + pos + 0, needle, needle_len)) return pos + 0; \
                if (vgetq_lane_u32(eq, 1) && equal_exact_inline(haystack + pos + 1, needle, needle_len)) return pos + 1; \
                if (vgetq_lane_u32(eq, 2) && equal_exact_inline(haystack + pos + 2, needle, needle_len)) return pos + 2; \
                if (vgetq_lane_u32(eq, 3) && equal_exact_inline(haystack + pos + 3, needle, needle_len)) return pos + 3; \
            } \
        } \
        \
        if (pos + 4 >= search_len) break; \
        if (pos > search_len - 9) break; \
        \
        /* 2) Load 16 old/new bytes */ \
        uint8x16_t old16b = vld1q_u8(haystack + pos); \
        uint8x16_t new16b = vld1q_u8(haystack + pos + w); \
        \
        /* 3) Fold ASCII to uppercase (only if CASE_FOLD) */ \
        if (CASE_FOLD) { \
            { \
                uint8x16_t x = vsubq_u8(old16b, va16); \
                uint8x16_t m = vcleq_u8(x, vz16); \
                old16b = vsubq_u8(old16b, vandq_u8(m, v20_16)); \
            } \
            { \
                uint8x16_t x = vsubq_u8(new16b, va16); \
                uint8x16_t m = vcleq_u8(x, vz16); \
                new16b = vsubq_u8(new16b, vandq_u8(m, v20_16)); \
            } \
        } \
        \
        /* 4) Widen low 8 bytes -> two vectors of 4x u32 */ \
        uint16x8_t old16 = vmovl_u8(vget_low_u8(old16b)); \
        uint16x8_t new16 = vmovl_u8(vget_low_u8(new16b)); \
        \
        uint32x4_t old0 = vmovl_u16(vget_low_u16(old16)); \
        uint32x4_t old1 = vmovl_u16(vget_high_u16(old16)); \
        uint32x4_t new0 = vmovl_u16(vget_low_u16(new16)); \
        uint32x4_t new1 = vmovl_u16(vget_high_u16(new16)); \
        \
        /* 5) t[k] = new[k] + antisigma*old[k] */ \
        uint32x4_t t0 = vmlaq_u32(new0, old0, vAnti); \
        uint32x4_t t1 = vmlaq_u32(new1, old1, vAnti); \
        \
        /* 6) Build sliding windows */ \
        uint32x4_t T0 = t0; \
        uint32x4_t T1 = vextq_u32(t0, t1, 1); \
        uint32x4_t T2 = vextq_u32(t0, t1, 2); \
        uint32x4_t T3 = vextq_u32(t0, t1, 3); \
        \
        /* 7) S = T0*B^3 + T1*B^2 + T2*B + T3 */ \
        uint32x4_t S = T3; \
        S = vmlaq_u32(S, T2, vB); \
        S = vmlaq_u32(S, T1, vB2); \
        S = vmlaq_u32(S, T0, vB3); \
        \
        /* 8) Advance: H = H*B^4 + S */ \
        H = vmlaq_u32(S, H, vB4); \
        \
        pos += 4; \
    } \
    \
    /* Scalar tail */ \
    { \
        uint32_t h = vgetq_lane_u32(H, 3); \
        int64_t i = pos + 3; \
        \
        for (int64_t j = i + 1; j < search_len; j++) { \
            uint32_t oldc = CASE_FOLD ? fold_table[haystack[j - 1]] : haystack[j - 1]; \
            uint32_t newc = CASE_FOLD ? fold_table[haystack[j + w - 1]] : haystack[j + w - 1]; \
            h = h * B + newc + antisigma * oldc; \
            if (CASE_FOLD) { \
                if (h == target_hash && equal_fold_core(haystack + j, needle, needle_len, table, vtbl_shift)) \
                    return j; \
            } else { \
                if (h == target_hash && equal_exact_inline(haystack + j, needle, needle_len)) \
                    return j; \
            } \
        } \
    } \
    \
    return -1; \
}

// Case-sensitive hash: no folding
__attribute__((always_inline)) static inline void hash2_rk_exact_neon_fast(const uint8_t *a,
                                            const uint8_t *b,
                                            int64_t len,
                                            uint32_t *out_a,
                                            uint32_t *out_b)
{
    const uint32_t B   = PRIME_RK;
    const uint32_t B2  = B * B;
    const uint32_t B3  = B2 * B;
    const uint32_t B4  = B2 * B2;
    const uint32_t B8  = B4 * B4;
    const uint32_t B12 = B8 * B4;
    const uint32_t B16 = B8 * B8;
    const uint32_t B32 = B16 * B16;
    const uint32_t B48 = B32 * B16;
    const uint32_t B64 = B32 * B32;

    const uint32x4_t maskFF = vdupq_n_u32(0xFFu);
    const uint32x4_t w16 = (uint32x4_t){ B12, B8, B4, 1u };
    const uint32x4_t w64 = (uint32x4_t){ B48, B32, B16, 1u };

    uint32_t ha = 0u, hb = 0u;
    int64_t i = 0;

    // 64-byte blocks (no folding - just load directly)
    for (; i + 64 <= len; i += 64) {
        uint8x16_t b0a = vld1q_u8(a + i +  0);
        uint8x16_t b1a = vld1q_u8(a + i + 16);
        uint8x16_t b2a = vld1q_u8(a + i + 32);
        uint8x16_t b3a = vld1q_u8(a + i + 48);

        uint8x16_t b0b = vld1q_u8(b + i +  0);
        uint8x16_t b1b = vld1q_u8(b + i + 16);
        uint8x16_t b2b = vld1q_u8(b + i + 32);
        uint8x16_t b3b = vld1q_u8(b + i + 48);

        uint32_t h0a = hash16_from_folded(b0a, maskFF, B, B2, B3, w16);
        uint32_t h1a = hash16_from_folded(b1a, maskFF, B, B2, B3, w16);
        uint32_t h2a = hash16_from_folded(b2a, maskFF, B, B2, B3, w16);
        uint32_t h3a = hash16_from_folded(b3a, maskFF, B, B2, B3, w16);

        uint32_t h0b = hash16_from_folded(b0b, maskFF, B, B2, B3, w16);
        uint32_t h1b = hash16_from_folded(b1b, maskFF, B, B2, B3, w16);
        uint32_t h2b = hash16_from_folded(b2b, maskFF, B, B2, B3, w16);
        uint32_t h3b = hash16_from_folded(b3b, maskFF, B, B2, B3, w16);

        uint32x4_t hva = (uint32x4_t){h0a, h1a, h2a, h3a};
        uint32x4_t hvb = (uint32x4_t){h0b, h1b, h2b, h3b};

        ha = ha * B64 + vaddvq_u32(vmulq_u32(hva, w64));
        hb = hb * B64 + vaddvq_u32(vmulq_u32(hvb, w64));
    }

    // 16-byte blocks
    for (; i + 16 <= len; i += 16) {
        uint8x16_t fa = vld1q_u8(a + i);
        uint8x16_t fb = vld1q_u8(b + i);

        uint32_t ba = hash16_from_folded(fa, maskFF, B, B2, B3, w16);
        uint32_t bb = hash16_from_folded(fb, maskFF, B, B2, B3, w16);

        ha = ha * B16 + ba;
        hb = hb * B16 + bb;
    }

    // tail bytes
    for (; i < len; i++) {
        ha = ha * B + a[i];
        hb = hb * B + b[i];
    }

    *out_a = ha;
    *out_b = hb;
}

// Forward declaration for equal_fold_normalized (defined later, used by RK prefolded)
static inline bool equal_fold_normalized(const unsigned char *a, const unsigned char *b, int64_t len, int64_t haystack_remaining);

// Hash function for pre-folded needle: needle is exact (already folded), haystack needs folding
__attribute__((always_inline)) static inline void hash2_rk_prefolded_neon_fast(const uint8_t *needle_prefolded,
                                                const uint8_t *haystack,
                                                int64_t len,
                                                uint32_t *out_needle,
                                                uint32_t *out_hay)
{
    const uint32_t B   = PRIME_RK;
    const uint32_t B2  = B * B;
    const uint32_t B3  = B2 * B;
    const uint32_t B4  = B2 * B2;
    const uint32_t B8  = B4 * B4;
    const uint32_t B12 = B8 * B4;
    const uint32_t B16 = B8 * B8;
    const uint32_t B32 = B16 * B16;
    const uint32_t B48 = B32 * B16;
    const uint32_t B64 = B32 * B32;

    const uint32x4_t maskFF = vdupq_n_u32(0xFFu);
    const uint32x4_t w16 = (uint32x4_t){ B12, B8, B4, 1u };
    const uint32x4_t w64 = (uint32x4_t){ B48, B32, B16, 1u };

    const uint8x16_t va16  = vdupq_n_u8('a');
    const uint8x16_t vz16  = vdupq_n_u8('z' - 'a');
    const uint8x16_t v20_16= vdupq_n_u8(0x20u);

    uint32_t hn = 0u, hh = 0u;
    int64_t i = 0;

    // 64-byte blocks
    for (; i + 64 <= len; i += 64) {
        // Needle: already folded, just load
        uint8x16_t n0 = vld1q_u8(needle_prefolded + i +  0);
        uint8x16_t n1 = vld1q_u8(needle_prefolded + i + 16);
        uint8x16_t n2 = vld1q_u8(needle_prefolded + i + 32);
        uint8x16_t n3 = vld1q_u8(needle_prefolded + i + 48);

        // Haystack: needs folding
        uint8x16_t h0 = fold16_ascii_rk(vld1q_u8(haystack + i +  0), va16, vz16, v20_16);
        uint8x16_t h1 = fold16_ascii_rk(vld1q_u8(haystack + i + 16), va16, vz16, v20_16);
        uint8x16_t h2 = fold16_ascii_rk(vld1q_u8(haystack + i + 32), va16, vz16, v20_16);
        uint8x16_t h3 = fold16_ascii_rk(vld1q_u8(haystack + i + 48), va16, vz16, v20_16);

        uint32_t hn0 = hash16_from_folded(n0, maskFF, B, B2, B3, w16);
        uint32_t hn1 = hash16_from_folded(n1, maskFF, B, B2, B3, w16);
        uint32_t hn2 = hash16_from_folded(n2, maskFF, B, B2, B3, w16);
        uint32_t hn3 = hash16_from_folded(n3, maskFF, B, B2, B3, w16);

        uint32_t hh0 = hash16_from_folded(h0, maskFF, B, B2, B3, w16);
        uint32_t hh1 = hash16_from_folded(h1, maskFF, B, B2, B3, w16);
        uint32_t hh2 = hash16_from_folded(h2, maskFF, B, B2, B3, w16);
        uint32_t hh3 = hash16_from_folded(h3, maskFF, B, B2, B3, w16);

        uint32x4_t hvn = (uint32x4_t){hn0, hn1, hn2, hn3};
        uint32x4_t hvh = (uint32x4_t){hh0, hh1, hh2, hh3};

        hn = hn * B64 + vaddvq_u32(vmulq_u32(hvn, w64));
        hh = hh * B64 + vaddvq_u32(vmulq_u32(hvh, w64));
    }

    // 16-byte blocks
    for (; i + 16 <= len; i += 16) {
        uint8x16_t fn = vld1q_u8(needle_prefolded + i);
        uint8x16_t fh = fold16_ascii_rk(vld1q_u8(haystack + i), va16, vz16, v20_16);

        uint32_t bn = hash16_from_folded(fn, maskFF, B, B2, B3, w16);
        uint32_t bh = hash16_from_folded(fh, maskFF, B, B2, B3, w16);

        hn = hn * B16 + bn;
        hh = hh * B16 + bh;
    }

    // tail bytes
    for (; i < len; i++) {
        hn = hn * B + needle_prefolded[i];
        hh = hh * B + fold_table[haystack[i]];
    }

    *out_needle = hn;
    *out_hay = hh;
}

// Generate all three implementations
RABIN_KARP_IMPL(index_fold_rabin_karp_impl, 1, hash2_rk_fold_neon_fast, equal_fold_core)
RABIN_KARP_IMPL(index_exact_rabin_karp_impl, 0, hash2_rk_exact_neon_fast, __builtin_memcmp)

// Pre-folded needle variant: needle already normalized, only fold haystack
// Uses CASE_FOLD=1 for haystack rolling/verification but needle hash is exact
#define RABIN_KARP_PREFOLDED_IMPL(func_name) \
__attribute__((always_inline)) static inline int64_t func_name( \
    unsigned char *haystack, int64_t haystack_len, \
    unsigned char *needle, int64_t needle_len) \
{ \
    if (needle_len <= 0) return 0; \
    if (haystack_len < needle_len) return -1; \
    \
    const int64_t search_len = haystack_len - needle_len + 1; \
    const int64_t w = needle_len; \
    \
    const uint8x16x2_t table = vld1q_u8_x2(uppercasingTable); \
    const uint8x16_t vtbl_shift = vdupq_n_u8(0x60); \
    \
    const uint32_t B = PRIME_RK; \
    const uint32_t B2 = B * B; \
    const uint32_t B3 = B2 * B; \
    const uint32_t B4 = B2 * B2; \
    const uint32_t Bw = pow_prime(w); \
    const uint32_t antisigma = 0u - Bw; \
    \
    uint32_t target_hash, h0; \
    hash2_rk_prefolded_neon_fast(needle, haystack, w, &target_hash, &h0); \
    \
    if (search_len < 4) { \
        uint32_t h = h0; \
        if (h == target_hash && equal_fold_normalized(haystack, needle, needle_len, haystack_len)) \
            return 0; \
        for (int64_t i = 1; i < search_len; i++) { \
            uint32_t oldc = fold_table[haystack[i - 1]]; \
            uint32_t newc = fold_table[haystack[i + w - 1]]; \
            h = h * B + newc + antisigma * oldc; \
            if (h == target_hash && equal_fold_normalized(haystack + i, needle, needle_len, haystack_len - i)) \
                return i; \
        } \
        return -1; \
    } \
    \
    uint32_t h1 = h0 * B + fold_table[haystack[w + 0]] + antisigma * fold_table[haystack[0]]; \
    uint32_t h2 = h1 * B + fold_table[haystack[w + 1]] + antisigma * fold_table[haystack[1]]; \
    uint32_t h3 = h2 * B + fold_table[haystack[w + 2]] + antisigma * fold_table[haystack[2]]; \
    \
    uint32x4_t H = (uint32x4_t){ h0, h1, h2, h3 }; \
    \
    const uint32x4_t vB   = vdupq_n_u32(B); \
    const uint32x4_t vB2  = vdupq_n_u32(B2); \
    const uint32x4_t vB3  = vdupq_n_u32(B3); \
    const uint32x4_t vB4  = vdupq_n_u32(B4); \
    const uint32x4_t vAnti = vdupq_n_u32(antisigma); \
    const uint32x4_t vTgt = vdupq_n_u32(target_hash); \
    \
    const uint8x16_t va16   = vdupq_n_u8('a'); \
    const uint8x16_t vz16   = vdupq_n_u8('z' - 'a'); \
    const uint8x16_t v20_16 = vdupq_n_u8(0x20); \
    \
    int64_t pos = 0; \
    \
    for (;;) { \
        uint32x4_t eq = vceqq_u32(H, vTgt); \
        if (vmaxvq_u32(eq)) { \
            if (vgetq_lane_u32(eq, 0) && equal_fold_normalized(haystack + pos + 0, needle, needle_len, haystack_len - pos - 0)) return pos + 0; \
            if (vgetq_lane_u32(eq, 1) && equal_fold_normalized(haystack + pos + 1, needle, needle_len, haystack_len - pos - 1)) return pos + 1; \
            if (vgetq_lane_u32(eq, 2) && equal_fold_normalized(haystack + pos + 2, needle, needle_len, haystack_len - pos - 2)) return pos + 2; \
            if (vgetq_lane_u32(eq, 3) && equal_fold_normalized(haystack + pos + 3, needle, needle_len, haystack_len - pos - 3)) return pos + 3; \
        } \
        \
        if (pos + 4 >= search_len) break; \
        if (pos > search_len - 9) break; \
        \
        uint8x16_t old16b = vld1q_u8(haystack + pos); \
        uint8x16_t new16b = vld1q_u8(haystack + pos + w); \
        \
        { \
            uint8x16_t x = vsubq_u8(old16b, va16); \
            uint8x16_t m = vcleq_u8(x, vz16); \
            old16b = vsubq_u8(old16b, vandq_u8(m, v20_16)); \
        } \
        { \
            uint8x16_t x = vsubq_u8(new16b, va16); \
            uint8x16_t m = vcleq_u8(x, vz16); \
            new16b = vsubq_u8(new16b, vandq_u8(m, v20_16)); \
        } \
        \
        uint16x8_t old16 = vmovl_u8(vget_low_u8(old16b)); \
        uint16x8_t new16 = vmovl_u8(vget_low_u8(new16b)); \
        \
        uint32x4_t old0 = vmovl_u16(vget_low_u16(old16)); \
        uint32x4_t old1 = vmovl_u16(vget_high_u16(old16)); \
        uint32x4_t new0 = vmovl_u16(vget_low_u16(new16)); \
        uint32x4_t new1 = vmovl_u16(vget_high_u16(new16)); \
        \
        uint32x4_t t0 = vmlaq_u32(new0, old0, vAnti); \
        uint32x4_t t1 = vmlaq_u32(new1, old1, vAnti); \
        \
        uint32x4_t T0 = t0; \
        uint32x4_t T1 = vextq_u32(t0, t1, 1); \
        uint32x4_t T2 = vextq_u32(t0, t1, 2); \
        uint32x4_t T3 = vextq_u32(t0, t1, 3); \
        \
        uint32x4_t S = T3; \
        S = vmlaq_u32(S, T2, vB); \
        S = vmlaq_u32(S, T1, vB2); \
        S = vmlaq_u32(S, T0, vB3); \
        \
        H = vmlaq_u32(S, H, vB4); \
        \
        pos += 4; \
    } \
    \
    { \
        uint32_t h = vgetq_lane_u32(H, 3); \
        int64_t i = pos + 3; \
        \
        for (int64_t j = i + 1; j < search_len; j++) { \
            uint32_t oldc = fold_table[haystack[j - 1]]; \
            uint32_t newc = fold_table[haystack[j + w - 1]]; \
            h = h * B + newc + antisigma * oldc; \
            if (h == target_hash && equal_fold_normalized(haystack + j, needle, needle_len, haystack_len - j)) \
                return j; \
        } \
    } \
    \
    return -1; \
}

RABIN_KARP_PREFOLDED_IMPL(index_prefolded_rabin_karp_impl)

// gocc: indexFoldRabinKarp(haystack string, needle string) int
int64_t index_fold_rabin_karp_simd(unsigned char *haystack, int64_t haystack_len,
                                    unsigned char *needle, int64_t needle_len)
{
    return index_fold_rabin_karp_impl(haystack, haystack_len, needle, needle_len);
}

// gocc: indexExactRabinKarp(haystack string, needle string) int
int64_t index_exact_rabin_karp_simd(unsigned char *haystack, int64_t haystack_len,
                                     unsigned char *needle, int64_t needle_len)
{
    return index_exact_rabin_karp_impl(haystack, haystack_len, needle, needle_len);
}

// gocc: indexPrefoldedRabinKarp(haystack string, needle string) int
int64_t index_prefolded_rabin_karp_simd(unsigned char *haystack, int64_t haystack_len,
                                         unsigned char *needle, int64_t needle_len)
{
    return index_prefolded_rabin_karp_impl(haystack, haystack_len, needle, needle_len);
}

// Tail masks for handling remainder bytes without scalar loops (Sneller-style)
static const uint8_t tail_mask_table[16][16] __attribute__((aligned(16))) = {
    {0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00}, // 0
    {0xFF,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00}, // 1
    {0xFF,0xFF,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00}, // 2
    {0xFF,0xFF,0xFF,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00}, // 3
    {0xFF,0xFF,0xFF,0xFF,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00}, // 4
    {0xFF,0xFF,0xFF,0xFF,0xFF,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00}, // 5
    {0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00}, // 6
    {0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00}, // 7
    {0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00}, // 8
    {0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0x00,0x00,0x00,0x00,0x00,0x00,0x00}, // 9
    {0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0x00,0x00,0x00,0x00,0x00,0x00}, // 10
    {0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0x00,0x00,0x00,0x00,0x00}, // 11
    {0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0x00,0x00,0x00,0x00}, // 12
    {0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0x00,0x00,0x00}, // 13
    {0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0x00,0x00}, // 14
    {0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0x00}, // 15
};

// Normalize to lowercase: ~4 instructions, no table lookup
static inline uint8x16_t normalize_lower(uint8x16_t v) {
    const uint8x16_t char_A = vdupq_n_u8('A');
    const uint8x16_t char_Z = vdupq_n_u8('Z');
    const uint8x16_t flip = vdupq_n_u8(0x20);
    
    // is_upper = (v >= 'A') & (v <= 'Z')
    uint8x16_t ge_A = vcgeq_u8(v, char_A);
    uint8x16_t le_Z = vcleq_u8(v, char_Z);
    uint8x16_t is_upper = vandq_u8(ge_A, le_Z);
    
    // OR with 0x20 where uppercase to convert to lowercase
    return vorrq_u8(v, vandq_u8(is_upper, flip));
}

// =============================================================================
// Verification functions for different search modes
// =============================================================================
// All take haystack_remaining to support safe loading near end of haystack.
// This enables SIMD search for all needle sizes (including ≤16 bytes).

// Compare haystack against pre-normalized (lowercase) needle, return true if equal
// IMPORTANT: 'b' (norm_needle) is already lowercase, so we only normalize 'a' (haystack)
__attribute__((always_inline)) static inline bool equal_fold_normalized(const unsigned char *a, const unsigned char *b, int64_t len, int64_t haystack_remaining) {
    while (len >= 16 && haystack_remaining >= 16) {
        uint8x16_t va = normalize_lower(vld1q_u8(a));
        uint8x16_t vb = vld1q_u8(b);  // b is already normalized - don't waste ops!
        uint8x16_t diff = veorq_u8(va, vb);
        if (vmaxvq_u8(diff)) return false;
        a += 16;
        b += 16;
        len -= 16;
        haystack_remaining -= 16;
    }
    if (len > 0) {
        uint8x16_t mask = vld1q_u8(tail_mask_table[len]);
        uint8x16_t va = (haystack_remaining >= 16) ? vld1q_u8(a) : load_data16(a, haystack_remaining);
        va = vandq_u8(normalize_lower(va), mask);
        uint8x16_t vb = vandq_u8(vld1q_u8(b), mask);  // b is already normalized
        uint8x16_t diff = veorq_u8(va, vb);
        if (vmaxvq_u8(diff)) return false;
    }
    return true;
}

