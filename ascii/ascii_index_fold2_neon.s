//go:build !noasm && arm64

// Hand-optimized ARM64 NEON assembly for substring search kernels

#include "textflag.h"

// Magic constant for syndrome extraction (2 bits per byte)
#define SYNDROME_MAGIC $0x40100401

TEXT ·indexFold2Byte(SB), NOSPLIT, $0-56
	MOVD  haystack+0(FP), R0
	MOVD  haystack_len+8(FP), R1
	MOVD  needle+16(FP), R2
	MOVD  needle_len+24(FP), R3
	MOVD  off1+32(FP), R4
	MOVD  off2_delta+40(FP), R5

	// Early exits
	SUBS  R3, R1, R9              // R9 = searchLen = haystack_len - needle_len
	BLT   fold2_not_found
	CBZ   R3, fold2_found_zero

	// Load rare bytes from normalized needle[off1] and needle[off1+off2_delta]
	ADD   R2, R4, R17             // R17 = needle + off1
	MOVBU (R17), R6               // R6 = rare1 byte
	ADD   R17, R5, R17            // R17 = needle + off1 + off2_delta
	MOVBU (R17), R7               // R7 = rare2 byte

	// Compute masks for rare1 and rare2
	// For letters: OR haystack with 0x20 to force lowercase, compare to lowercase target
	// For non-letters: OR with 0x00 (identity)
	SUBW  $97, R6, R17            // R17 = rare1 - 'a'
	CMPW  $26, R17
	BCS   fold2_rare1_not_letter
	MOVW  $0x20, R8               // rare1 mask = 0x20
	B     fold2_check_rare2
fold2_rare1_not_letter:
	MOVW  $0x00, R8               // rare1 mask = 0x00

fold2_check_rare2:
	SUBW  $97, R7, R17            // R17 = rare2 - 'a'
	CMPW  $26, R17
	BCS   fold2_rare2_not_letter
	MOVW  $0x20, R26              // rare2 mask = 0x20
	B     fold2_setup
fold2_rare2_not_letter:
	MOVW  $0x00, R26              // rare2 mask = 0x00

fold2_setup:
	// Broadcast masks and targets
	VDUP  R8, V0.B16              // V0 = rare1 mask
	VDUP  R6, V1.B16              // V1 = rare1 target
	VDUP  R26, V2.B16             // V2 = rare2 mask
	VDUP  R7, V3.B16              // V3 = rare2 target

	// Syndrome magic constant
	MOVD  $0x4010040140100401, R17
	VMOV  R17, V5.D[0]
	VMOV  R17, V5.D[1]

	// Constants for verification
	WORD  $0x4f04e7e4             // VMOVI $159, V4.B16 (-97 as unsigned)
	WORD  $0x4f00e747             // VMOVI $26, V7.B16
	WORD  $0x4f01e408             // VMOVI $32, V8.B16
	MOVD  $tail_mask_table(SB), R24

	// Setup pointers
	ADD   R4, R0, R10             // R10 = searchPtr = haystack + off1
	MOVD  R10, R11                // R11 = searchStart
	ADD   R5, R10, R14            // R14 = ptr2 = ptr1 + off2_delta
	ADD   $1, R9, R12             // R12 = remaining = searchLen + 1

	// Initialize failure counter
	MOVD  ZR, R25                 // R25 = failure count = 0

	CMP   $64, R12
	BLT   fold2_loop32_entry

// ============================================================================
// 64-BYTE LOOP: Process 64 bytes per iteration
// ============================================================================
fold2_loop64:
	// Load 64 bytes from both pointers with post-increment
	VLD1.P 64(R10), [V16.B16, V17.B16, V18.B16, V19.B16]
	VLD1.P 64(R14), [V24.B16, V25.B16, V26.B16, V27.B16]

	SUB   $64, R12, R12

	// Case-fold and compare rare1
	VORR  V0.B16, V16.B16, V20.B16
	VORR  V0.B16, V17.B16, V21.B16
	VORR  V0.B16, V18.B16, V22.B16
	VORR  V0.B16, V19.B16, V23.B16
	VCMEQ V1.B16, V20.B16, V20.B16
	VCMEQ V1.B16, V21.B16, V21.B16
	VCMEQ V1.B16, V22.B16, V22.B16
	VCMEQ V1.B16, V23.B16, V23.B16

	// Case-fold and compare rare2
	VORR  V2.B16, V24.B16, V28.B16
	VORR  V2.B16, V25.B16, V29.B16
	VORR  V2.B16, V26.B16, V30.B16
	VORR  V2.B16, V27.B16, V31.B16
	VCMEQ V3.B16, V28.B16, V28.B16
	VCMEQ V3.B16, V29.B16, V29.B16
	VCMEQ V3.B16, V30.B16, V30.B16
	VCMEQ V3.B16, V31.B16, V31.B16

	// AND both results
	VAND  V20.B16, V28.B16, V20.B16
	VAND  V21.B16, V29.B16, V21.B16
	VAND  V22.B16, V30.B16, V22.B16
	VAND  V23.B16, V31.B16, V23.B16

	// Quick check: any match?
	VORR  V20.B16, V21.B16, V6.B16
	VORR  V22.B16, V23.B16, V9.B16
	VORR  V6.B16, V9.B16, V6.B16
	VADDP V6.D2, V6.D2, V6.D2
	VMOV  V6.D[0], R13

	CMP   $64, R12
	BLT   fold2_end64
	CBZ   R13, fold2_loop64

fold2_end64:
	CBZ   R13, fold2_loop16_entry

	// Extract syndromes
	VAND  V5.B16, V20.B16, V20.B16
	VAND  V5.B16, V21.B16, V21.B16
	VAND  V5.B16, V22.B16, V22.B16
	VAND  V5.B16, V23.B16, V23.B16

	// Check chunks 0-3
	MOVD  $0, R20                 // chunk offset (R20 not used in verify)

fold2_check_chunk64:
	CMP   $0, R20
	BEQ   fold2_chunk64_0
	CMP   $16, R20
	BEQ   fold2_chunk64_1
	CMP   $32, R20
	BEQ   fold2_chunk64_2
	B     fold2_chunk64_3

fold2_chunk64_0:
	VADDP V20.B16, V20.B16, V6.B16
	B     fold2_extract64
fold2_chunk64_1:
	VADDP V21.B16, V21.B16, V6.B16
	B     fold2_extract64
fold2_chunk64_2:
	VADDP V22.B16, V22.B16, V6.B16
	B     fold2_extract64
fold2_chunk64_3:
	VADDP V23.B16, V23.B16, V6.B16

fold2_extract64:
	VADDP V6.B16, V6.B16, V6.B16
	VMOV  V6.S[0], R13
	CBZ   R13, fold2_next_chunk64

fold2_try64:
	RBIT  R13, R15
	CLZ   R15, R15
	LSR   $1, R15, R15

	SUB   $64, R10, R16
	ADD   R20, R16, R16
	ADD   R15, R16, R16
	SUB   R11, R16, R16           // R16 = candidate position

	CMP   R9, R16
	BGT   fold2_clear64

	// Verify match
	ADD   R0, R16, R17            // R17 = &haystack[candidate]
	B     fold2_verify

fold2_clear64:
	LSL   $1, R15, R17             // bitpos = byteIndex << 1
	MOVD  $1, R19
	LSL   R17, R19, R17            // 1 << bitpos
	BIC   R17, R13, R13            // Clear just that bit
	CBNZ  R13, fold2_try64

fold2_next_chunk64:
	ADD   $16, R20
	CMP   $64, R20
	BLT   fold2_check_chunk64
	CMP   $64, R12
	BGE   fold2_loop64

// ============================================================================
// 32-BYTE LOOP: For remainders 32-63 bytes
// ============================================================================
fold2_loop32_entry:
	CMP   $32, R12
	BLT   fold2_loop16_entry

fold2_loop32:
	VLD1.P 32(R10), [V16.B16, V17.B16]
	VLD1.P 32(R14), [V18.B16, V19.B16]
	SUBS  $32, R12, R12

	// Case-fold and compare rare1
	VORR  V0.B16, V16.B16, V20.B16
	VORR  V0.B16, V17.B16, V21.B16
	VCMEQ V1.B16, V20.B16, V20.B16
	VCMEQ V1.B16, V21.B16, V21.B16

	// Case-fold and compare rare2
	VORR  V2.B16, V18.B16, V22.B16
	VORR  V2.B16, V19.B16, V23.B16
	VCMEQ V3.B16, V22.B16, V22.B16
	VCMEQ V3.B16, V23.B16, V23.B16

	// AND both results
	VAND  V20.B16, V22.B16, V20.B16
	VAND  V21.B16, V23.B16, V21.B16

	// Quick check
	VORR  V20.B16, V21.B16, V6.B16
	VADDP V6.D2, V6.D2, V6.D2
	VMOV  V6.D[0], R13

	BLT   fold2_end32
	CBZ   R13, fold2_loop32

fold2_end32:
	CBZ   R13, fold2_loop16_entry

	// Extract syndromes
	VAND  V5.B16, V20.B16, V20.B16
	VAND  V5.B16, V21.B16, V21.B16

	// Check chunk 0
	VADDP V20.B16, V20.B16, V6.B16
	VADDP V6.B16, V6.B16, V6.B16
	VMOV  V6.S[0], R13
	MOVD  $0, R20
	CBNZ  R13, fold2_try32

	// Check chunk 1
	VADDP V21.B16, V21.B16, V6.B16
	VADDP V6.B16, V6.B16, V6.B16
	VMOV  V6.S[0], R13
	MOVD  $16, R20
	CBNZ  R13, fold2_try32

	// No matches, continue
	CMP   $32, R12
	BGE   fold2_loop32
	B     fold2_loop16_entry

fold2_try32:
	RBIT  R13, R15
	CLZ   R15, R15
	LSR   $1, R15, R15

	SUB   $32, R10, R16
	ADD   R20, R16, R16
	ADD   R15, R16, R16
	SUB   R11, R16, R16

	CMP   R9, R16
	BGT   fold2_clear32

	ADD   R0, R16, R17
	ADD   $0x200, R20, R20        // Mark as 32-byte path (0x200 + chunk_offset)
	B     fold2_verify

fold2_clear32:
	LSL   $1, R15, R17
	MOVD  $1, R19
	LSL   R17, R19, R17
	BIC   R17, R13, R13
	CBNZ  R13, fold2_try32

	// Move to chunk 1 if in chunk 0
	CBZ   R20, fold2_check32_chunk1
	CMP   $32, R12
	BGE   fold2_loop32
	B     fold2_loop16_entry

fold2_check32_chunk1:
	VADDP V21.B16, V21.B16, V6.B16
	VADDP V6.B16, V6.B16, V6.B16
	VMOV  V6.S[0], R13
	MOVD  $16, R20
	CBNZ  R13, fold2_try32
	CMP   $32, R12
	BGE   fold2_loop32
	B     fold2_loop16_entry

// ============================================================================
// 16-BYTE LOOP
// ============================================================================
fold2_loop16_entry:
	CMP   $16, R12
	BLT   fold2_scalar_entry

fold2_loop16:
	// Load 16 bytes from both pointers with post-increment
	VLD1.P 16(R10), [V16.B16]
	VLD1.P 16(R14), [V17.B16]
	SUB   $16, R12, R12

	// Case-fold and compare
	VORR  V0.B16, V16.B16, V20.B16
	VORR  V2.B16, V17.B16, V21.B16
	VCMEQ V1.B16, V20.B16, V20.B16
	VCMEQ V3.B16, V21.B16, V21.B16
	VAND  V20.B16, V21.B16, V20.B16

	VAND  V5.B16, V20.B16, V20.B16
	VADDP V20.B16, V20.B16, V20.B16
	VADDP V20.B16, V20.B16, V20.B16
	VMOV  V20.S[0], R13

	CMP   $16, R12
	BLT   fold2_end16
	CBZ   R13, fold2_loop16

fold2_end16:
	CBZ   R13, fold2_scalar_entry

fold2_try16:
	RBIT  R13, R15
	CLZ   R15, R15
	LSR   $1, R15, R15

	SUB   $16, R10, R16
	ADD   R15, R16, R16
	SUB   R11, R16, R16

	CMP   R9, R16
	BGT   fold2_clear16

	ADD   R0, R16, R17
	MOVD  $0x100, R20             // Mark as 16-byte path
	B     fold2_verify

fold2_clear16:
	LSL   $1, R15, R17             // bitpos = byteIndex << 1
	MOVD  $1, R19
	LSL   R17, R19, R17            // 1 << bitpos
	BIC   R17, R13, R13            // Clear just that bit
	CBNZ  R13, fold2_try16
	CMP   $16, R12
	BGE   fold2_loop16

// ============================================================================
// SCALAR LOOP
// ============================================================================
fold2_scalar_entry:
	CMP   $0, R12
	BLE   fold2_not_found

fold2_scalar:
	// Load bytes at rare1 and rare2 positions
	MOVBU (R10), R13
	ADD   R5, R10, R14
	MOVBU (R14), R14

	// Case-fold and compare rare1
	ORRW  R8, R13, R15
	CMPW  R6, R15
	BNE   fold2_scalar_next

	// Case-fold and compare rare2
	ORRW  R26, R14, R15
	CMPW  R7, R15
	BNE   fold2_scalar_next

	SUB   R11, R10, R16
	CMP   R9, R16
	BGT   fold2_scalar_next

	ADD   R0, R16, R17
	B     fold2_verify_scalar

fold2_scalar_next:
	ADD   $1, R10
	SUB   $1, R12
	CBNZ  R12, fold2_scalar
	B     fold2_not_found

// ============================================================================
// SCALAR VERIFICATION (separate path to avoid syndrome-clearing logic)
// ============================================================================
fold2_verify_scalar:
	MOVD  R3, R19                 // R19 = remaining needle length
	MOVD  R17, R21                // R21 = haystack candidate ptr
	MOVD  R2, R22                 // R22 = needle ptr

fold2_scalar_vloop:
	SUBS  $16, R19, R23
	BLT   fold2_scalar_vtail

	VLD1.P 16(R21), [V10.B16]
	VLD1.P 16(R22), [V11.B16]
	MOVD   R23, R19

	// Derive fold mask from needle (not haystack) - needle is pre-normalized
	VADD  V4.B16, V11.B16, V14.B16  // V14 = needle + 159
	WORD  $0x6e2e34ee               // VCMHI V14.B16, V7.B16, V14.B16 (needle_is_letter mask)
	VAND  V8.B16, V14.B16, V14.B16  // V14 = needle_is_letter ? 0x20 : 0
	VORR  V14.B16, V10.B16, V10.B16 // V10 = h | mask (fold only where needle is letter)
	VEOR  V10.B16, V11.B16, V10.B16 // V10 = h_folded XOR needle
	WORD  $0x6e30a94a               // VUMAXV V10.B16, V10
	FMOVS F10, R23
	CBZW  R23, fold2_scalar_vloop
	B     fold2_scalar_verify_fail

fold2_scalar_vtail:
	CMP   $1, R19
	BLT   fold2_found

	VLD1  (R21), [V10.B16]
	VLD1  (R22), [V11.B16]
	WORD  $0x3cf37b0d               // LDR Q13, [R24, R19, LSL #4] - tail mask

	// Derive fold mask from needle (not haystack) - needle is pre-normalized
	VADD  V4.B16, V11.B16, V15.B16  // V15 = needle + 159
	WORD  $0x6e2f34ef               // VCMHI V15.B16, V7.B16, V15.B16 (needle_is_letter mask)
	VAND  V8.B16, V15.B16, V15.B16  // V15 = needle_is_letter ? 0x20 : 0
	VORR  V15.B16, V10.B16, V10.B16 // V10 = h | mask (fold only where needle is letter)
	VEOR  V10.B16, V11.B16, V10.B16 // V10 = h_folded XOR needle
	VAND  V13.B16, V10.B16, V10.B16 // Mask out bytes beyond needle
	WORD  $0x6e30a94a               // VUMAXV V10.B16, V10
	FMOVS F10, R23
	CBNZW R23, fold2_scalar_verify_fail
	B     fold2_found

fold2_scalar_verify_fail:
	ADD   $1, R25, R25
	SUB   R11, R10, R17
	LSR   $3, R17, R17
	ADD   $32, R17, R17
	CMP   R17, R25
	BGT   fold2_exceeded
	B     fold2_scalar_next

// ============================================================================
// SIMD VERIFICATION (for 16/64-byte loop paths)
// ============================================================================
fold2_verify:
	MOVD  R3, R19                 // R19 = remaining needle length
	MOVD  R17, R21                // R21 = haystack candidate ptr
	MOVD  R2, R22                 // R22 = needle ptr

fold2_vloop:
	SUBS  $16, R19, R23
	BLT   fold2_vtail

	VLD1.P 16(R21), [V10.B16]
	VLD1.P 16(R22), [V11.B16]
	MOVD   R23, R19

	// Derive fold mask from needle (not haystack) - needle is pre-normalized
	VADD  V4.B16, V11.B16, V14.B16  // V14 = needle + 159
	WORD  $0x6e2e34ee               // VCMHI V14.B16, V7.B16, V14.B16 (needle_is_letter mask)
	VAND  V8.B16, V14.B16, V14.B16  // V14 = needle_is_letter ? 0x20 : 0
	VORR  V14.B16, V10.B16, V10.B16 // V10 = h | mask (fold only where needle is letter)
	VEOR  V10.B16, V11.B16, V10.B16 // V10 = h_folded XOR needle
	WORD  $0x6e30a94a               // VUMAXV V10.B16, V10
	FMOVS F10, R23
	CBZW  R23, fold2_vloop
	B     fold2_verify_fail

fold2_vtail:
	CMP   $1, R19
	BLT   fold2_found

	VLD1  (R21), [V10.B16]
	VLD1  (R22), [V11.B16]
	WORD  $0x3cf37b0d               // LDR Q13, [R24, R19, LSL #4] - tail mask

	// Derive fold mask from needle (not haystack) - needle is pre-normalized
	VADD  V4.B16, V11.B16, V15.B16  // V15 = needle + 159
	WORD  $0x6e2f34ef               // VCMHI V15.B16, V7.B16, V15.B16 (needle_is_letter mask)
	VAND  V8.B16, V15.B16, V15.B16  // V15 = needle_is_letter ? 0x20 : 0
	VORR  V15.B16, V10.B16, V10.B16 // V10 = h | mask (fold only where needle is letter)
	VEOR  V10.B16, V11.B16, V10.B16 // V10 = h_folded XOR needle
	VAND  V13.B16, V10.B16, V10.B16 // Mask out bytes beyond needle
	WORD  $0x6e30a94a               // VUMAXV V10.B16, V10
	FMOVS F10, R23
	CBNZW R23, fold2_verify_fail
	B     fold2_found

fold2_verify_fail:
	ADD   $1, R25, R25
	SUB   R11, R10, R17
	LSR   $3, R17, R17
	ADD   $32, R17, R17
	CMP   R17, R25
	BGT   fold2_exceeded

	// Clear bit and continue
	CMP   $0x100, R20
	BEQ   fold2_clear16_from_verify
	CMP   $0x200, R20
	BGE   fold2_clear32_from_verify
	B     fold2_clear64_from_verify

fold2_clear16_from_verify:
	LSL   $1, R15, R17             // bitpos = byteIndex << 1
	MOVD  $1, R19
	LSL   R17, R19, R17            // 1 << bitpos
	BIC   R17, R13, R13            // Clear just that bit
	CBNZ  R13, fold2_try16
	CMP   $16, R12
	BGE   fold2_loop16
	B     fold2_scalar_entry

fold2_clear32_from_verify:
	// R20 = 0x200 + chunk_offset (0 or 16)
	LSL   $1, R15, R17
	MOVD  $1, R19
	LSL   R17, R19, R17
	BIC   R17, R13, R13
	AND   $0xFF, R20, R20         // Extract chunk offset back
	CBNZ  R13, fold2_try32
	// Current chunk exhausted, check next chunk or loop
	CBZ   R20, fold2_check32_chunk1
	CMP   $32, R12
	BGE   fold2_loop32
	B     fold2_loop16_entry

fold2_clear64_from_verify:
	LSL   $1, R15, R17             // bitpos = byteIndex << 1
	MOVD  $1, R19
	LSL   R17, R19, R17            // 1 << bitpos
	BIC   R17, R13, R13            // Clear just that bit
	CBNZ  R13, fold2_try64
	ADD   $16, R20
	CMP   $64, R20
	BLT   fold2_check_chunk64
	CMP   $64, R12
	BGE   fold2_loop64
	B     fold2_loop16_entry

// ============================================================================
// EXIT PATHS
// ============================================================================
fold2_exceeded:
	MOVD  $0x8000000000000000, R17
	ADD   R16, R17, R0
	MOVD  R0, ret+48(FP)
	RET

fold2_not_found:
	MOVD  $-1, R0
	MOVD  R0, ret+48(FP)
	RET

fold2_found_zero:
	MOVD  ZR, R0
	MOVD  R0, ret+48(FP)
	RET

fold2_found:
	MOVD  R16, R0
	MOVD  R0, ret+48(FP)
	RET

// func indexFold1ByteRaw(haystack string, needle string, off1 int) uint64
//
// Same as indexFold1Byte but normalizes needle byte on-the-fly (uppercase→lowercase)
// and uses dual-normalize verification (normalizes both haystack AND needle).
//
// Returns:
//   - Position if found
//   - 0xFFFFFFFFFFFFFFFF (-1) if not found
//   - 0x8000000000000000 + position if exceeded threshold (resume from position)

