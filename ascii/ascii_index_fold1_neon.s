//go:build !noasm && arm64

// Hand-optimized ARM64 NEON assembly for substring search kernels

#include "textflag.h"

// Magic constant for syndrome extraction (2 bits per byte)
#define SYNDROME_MAGIC $0x40100401

TEXT ·indexFold1Byte(SB), NOSPLIT, $0-48
	MOVD  haystack+0(FP), R0      // R0 = haystack ptr
	MOVD  haystack_len+8(FP), R1  // R1 = haystack len
	MOVD  needle+16(FP), R2       // R2 = needle ptr
	MOVD  needle_len+24(FP), R3   // R3 = needle len
	MOVD  off1+32(FP), R4         // R4 = off1

	// Early exits
	SUBS  R3, R1, R9              // R9 = searchLen = haystack_len - needle_len
	BLT   fold1_not_found
	CBZ   R3, fold1_found_zero

	// Load rare byte from needle[off1]
	ADD   R2, R4, R8              // R8 = needle + off1
	MOVBU (R8), R5                // R5 = rare1 byte (from normalized needle)

	// Compute mask and target for rare1
	// For letters: OR haystack with 0x20 to force lowercase, compare to lowercase target
	// For non-letters: mask with 0x00 (identity), compare exact
	// R5 = rare byte, will be kept as target
	SUBW  $97, R5, R8             // R8 = rare1 - 'a'
	CMPW  $26, R8
	BCS   fold1_not_letter
	MOVW  $0x20, R26              // mask = 0x20 (OR to force lowercase)
	B     fold1_setup
fold1_not_letter:
	MOVW  $0x00, R26              // mask = 0x00 (OR with 0 = no change)

fold1_setup:
	// R5 = target byte (rare byte from normalized needle)
	VDUP  R26, V0.B16             // V0 = rare1 mask broadcast
	VDUP  R5, V1.B16              // V1 = rare1 target broadcast

	// Syndrome magic constant for position extraction
	MOVD  $0x4010040140100401, R8
	VMOV  R8, V5.D[0]
	VMOV  R8, V5.D[1]

	// Constants for vectorized verification
	WORD  $0x4f04e7e4             // VMOVI $159, V4.B16 (-97 as unsigned)
	WORD  $0x4f00e747             // VMOVI $26, V7.B16
	WORD  $0x4f01e408             // VMOVI $32, V8.B16
	MOVD  $tail_mask_table(SB), R24  // R24 = tail mask table

	// Setup pointers
	ADD   R4, R0, R10             // R10 = searchPtr = haystack + off1
	MOVD  R10, R11                // R11 = searchStart
	ADD   $1, R9, R12             // R12 = remaining = searchLen + 1

	// Initialize failure counter
	MOVD  ZR, R25                 // R25 = failure count = 0

	// Dispatch based on letter/non-letter and size
	CBZ   R26, fold1_dispatch_nonletter

	// Letter path: check size threshold
	CMP   $768, R12               // Threshold for 128B loop
	BGE   fold1_loop128
	CMP   $32, R12
	BLT   fold1_loop16_entry

// ============================================================================
// 32-BYTE LOOP (letter path, medium inputs)
// ============================================================================
fold1_loop32:
	VLD1.P 32(R10), [V16.B16, V17.B16]
	SUBS  $32, R12, R12

	// Case-fold and compare
	VORR  V0.B16, V16.B16, V20.B16
	VORR  V0.B16, V17.B16, V21.B16
	VCMEQ V1.B16, V20.B16, V20.B16
	VCMEQ V1.B16, V21.B16, V21.B16

	// Quick check: any matches?
	VORR  V20.B16, V21.B16, V6.B16
	VADDP V6.D2, V6.D2, V6.D2
	VMOV  V6.D[0], R13

	BLT   fold1_end32
	CBZ   R13, fold1_loop32

fold1_end32:
	CBZ   R13, fold1_loop16_entry

	// Extract syndromes
	VAND  V5.B16, V20.B16, V20.B16
	VAND  V5.B16, V21.B16, V21.B16

	// Check chunk 0
	VADDP V20.B16, V20.B16, V6.B16
	VADDP V6.B16, V6.B16, V6.B16
	VMOV  V6.S[0], R13
	MOVD  $128, R14               // Mark as 32-byte mode (128 = chunk0)
	CBNZ  R13, fold1_try32

	// Check chunk 1
	VADDP V21.B16, V21.B16, V6.B16
	VADDP V6.B16, V6.B16, V6.B16
	VMOV  V6.S[0], R13
	MOVD  $144, R14               // 128 + 16 = chunk 1
	CBNZ  R13, fold1_try32

	// No matches, continue
	CMP   $32, R12
	BGE   fold1_loop32
	B     fold1_loop16_entry

fold1_try32:
	RBIT  R13, R15
	CLZ   R15, R15
	LSR   $1, R15, R15
	AND   $0x7F, R14, R17         // Extract actual chunk offset
	ADD   R17, R15, R15

	SUB   $32, R10, R16
	ADD   R15, R16, R16
	SUB   R11, R16, R16           // R16 = position in searchable range

	CMP   R9, R16
	BGT   fold1_clear32

	// Verify the match
	ADD   R0, R16, R8             // R8 = &haystack[candidate]
	B     fold1_verify

fold1_clear32:
	AND   $0x7F, R14, R17          // chunk offset (0 or 16)
	SUB   R17, R15, R20            // byte index within chunk
	LSL   $1, R20, R20             // bitpos = byteIndex << 1
	MOVD  $1, R19
	LSL   R20, R19, R20            // 1 << bitpos
	BIC   R20, R13, R13            // Clear just that bit
	CBNZ  R13, fold1_try32

	// Move to chunk 1 if in chunk 0
	CMP   $128, R14
	BNE   fold1_continue32
	VADDP V21.B16, V21.B16, V6.B16
	VADDP V6.B16, V6.B16, V6.B16
	VMOV  V6.S[0], R13
	MOVD  $144, R14
	CBNZ  R13, fold1_try32

fold1_continue32:
	CMP   $32, R12
	BGE   fold1_loop32
	B     fold1_loop16_entry

// ============================================================================
// 128-BYTE LOOP (letter path, large inputs ≥768B)
// ============================================================================
fold1_loop128:
	VLD1.P 64(R10), [V16.B16, V17.B16, V18.B16, V19.B16]
	VLD1.P 64(R10), [V24.B16, V25.B16, V26.B16, V27.B16]
	SUB   $128, R12, R12

	// Process first 64 bytes
	VORR  V0.B16, V16.B16, V20.B16
	VORR  V0.B16, V17.B16, V21.B16
	VORR  V0.B16, V18.B16, V22.B16
	VORR  V0.B16, V19.B16, V23.B16
	VCMEQ V1.B16, V20.B16, V20.B16
	VCMEQ V1.B16, V21.B16, V21.B16
	VCMEQ V1.B16, V22.B16, V22.B16
	VCMEQ V1.B16, V23.B16, V23.B16

	// Process second 64 bytes
	VORR  V0.B16, V24.B16, V28.B16
	VORR  V0.B16, V25.B16, V29.B16
	VORR  V0.B16, V26.B16, V30.B16
	VORR  V0.B16, V27.B16, V31.B16
	VCMEQ V1.B16, V28.B16, V28.B16
	VCMEQ V1.B16, V29.B16, V29.B16
	VCMEQ V1.B16, V30.B16, V30.B16
	VCMEQ V1.B16, V31.B16, V31.B16

	// Combine all 8 chunks for quick check - keep V9 (first64) and V11 (second64)
	VORR  V20.B16, V21.B16, V9.B16
	VORR  V22.B16, V23.B16, V10.B16
	VORR  V28.B16, V29.B16, V11.B16
	VORR  V30.B16, V31.B16, V12.B16
	VORR  V9.B16, V10.B16, V9.B16    // V9 = OR(first64)
	VORR  V11.B16, V12.B16, V11.B16  // V11 = OR(second64)
	VORR  V9.B16, V11.B16, V13.B16   // V13 = OR(all) - preserve V9 and V11

	VADDP V13.D2, V13.D2, V13.D2
	VMOV  V13.D[0], R13

	CMP   $128, R12
	BLT   fold1_end128
	CBZ   R13, fold1_loop128

fold1_end128:
	CBZ   R13, fold1_loop32

	// Check first 64 bytes - V9 already has OR(first64), just reduce
	VADDP V9.D2, V9.D2, V9.D2
	VMOV  V9.D[0], R13
	CBNZ  R13, fold1_first64

	// Check second 64 bytes - V11 already has OR(second64), just reduce
	VADDP V11.D2, V11.D2, V11.D2
	VMOV  V11.D[0], R13
	CBNZ  R13, fold1_second64
	CMP   $128, R12
	BGE   fold1_loop128
	B     fold1_loop32

fold1_first64:
	VAND  V5.B16, V20.B16, V20.B16
	VAND  V5.B16, V21.B16, V21.B16
	VAND  V5.B16, V22.B16, V22.B16
	VAND  V5.B16, V23.B16, V23.B16
	MOVD  $64, R20                // block offset = 64 (first block)
	B     fold1_check_chunks

fold1_second64:
	VMOV  V28.B16, V20.B16
	VMOV  V29.B16, V21.B16
	VMOV  V30.B16, V22.B16
	VMOV  V31.B16, V23.B16
	VAND  V5.B16, V20.B16, V20.B16
	VAND  V5.B16, V21.B16, V21.B16
	VAND  V5.B16, V22.B16, V22.B16
	VAND  V5.B16, V23.B16, V23.B16
	MOVD  ZR, R20                 // block offset = 0 (second block)

fold1_check_chunks:
	VADDP V20.B16, V20.B16, V6.B16
	VADDP V6.B16, V6.B16, V6.B16
	VMOV  V6.S[0], R13
	MOVD  ZR, R14
	CBNZ  R13, fold1_try128

	VADDP V21.B16, V21.B16, V6.B16
	VADDP V6.B16, V6.B16, V6.B16
	VMOV  V6.S[0], R13
	MOVD  $16, R14
	CBNZ  R13, fold1_try128

	VADDP V22.B16, V22.B16, V6.B16
	VADDP V6.B16, V6.B16, V6.B16
	VMOV  V6.S[0], R13
	MOVD  $32, R14
	CBNZ  R13, fold1_try128

	VADDP V23.B16, V23.B16, V6.B16
	VADDP V6.B16, V6.B16, V6.B16
	VMOV  V6.S[0], R13
	MOVD  $48, R14
	CBNZ  R13, fold1_try128

	// Check if we need second 64 block
	CBNZ  R20, fold1_check_second64_after
	CMP   $128, R12
	BGE   fold1_loop128
	B     fold1_loop32

fold1_check_second64_after:
	VORR  V28.B16, V29.B16, V6.B16
	VORR  V30.B16, V31.B16, V9.B16
	VORR  V6.B16, V9.B16, V6.B16
	VADDP V6.D2, V6.D2, V6.D2
	VMOV  V6.D[0], R13
	CBZ   R13, fold1_continue128
	B     fold1_second64

fold1_continue128:
	CMP   $128, R12
	BGE   fold1_loop128
	B     fold1_loop32

fold1_try128:
	RBIT  R13, R15
	CLZ   R15, R15
	LSR   $1, R15, R15
	ADD   R14, R15, R15
	SUB   R20, R15, R15
	ADD   $64, R15, R15

	SUB   $128, R10, R16
	ADD   R15, R16, R16
	SUB   R11, R16, R16

	CMP   R9, R16
	BGT   fold1_clear128

	ADD   R0, R16, R8
	B     fold1_verify

fold1_clear128:
	SUB   R14, R15, R17            // byte index within chunk
	LSL   $1, R17, R17             // bitpos = byteIndex << 1
	MOVD  $1, R19
	LSL   R17, R19, R17            // 1 << bitpos
	BIC   R17, R13, R13            // Clear just that bit
	CBNZ  R13, fold1_try128

	ADD   $16, R14, R14
	CMP   $64, R14
	BLT   fold1_next_chunk
	CBNZ  R20, fold1_check_second64_after
	B     fold1_continue128

fold1_next_chunk:
	CMP   $16, R14
	BEQ   fold1_chunk1
	CMP   $32, R14
	BEQ   fold1_chunk2
	CMP   $48, R14
	BEQ   fold1_chunk3
	B     fold1_continue128

fold1_chunk1:
	VADDP V21.B16, V21.B16, V6.B16
	VADDP V6.B16, V6.B16, V6.B16
	VMOV  V6.S[0], R13
	CBNZ  R13, fold1_try128
	ADD   $16, R14, R14
fold1_chunk2:
	VADDP V22.B16, V22.B16, V6.B16
	VADDP V6.B16, V6.B16, V6.B16
	VMOV  V6.S[0], R13
	CBNZ  R13, fold1_try128
	ADD   $16, R14, R14
fold1_chunk3:
	VADDP V23.B16, V23.B16, V6.B16
	VADDP V6.B16, V6.B16, V6.B16
	VMOV  V6.S[0], R13
	CBNZ  R13, fold1_try128
	CBNZ  R20, fold1_check_second64_after
	B     fold1_continue128

// ============================================================================
// NON-LETTER PATH (skips VORR for 5-op loop)
// ============================================================================
fold1_dispatch_nonletter:
	CMP   $768, R12
	BGE   fold1_loop128_nl
	CMP   $32, R12
	BLT   fold1_loop16_nl_entry

fold1_loop32_nl:
	VLD1.P 32(R10), [V16.B16, V17.B16]
	SUBS  $32, R12, R12

	// Direct compare (no case folding)
	VCMEQ V1.B16, V16.B16, V20.B16
	VCMEQ V1.B16, V17.B16, V21.B16

	VORR  V20.B16, V21.B16, V6.B16
	VADDP V6.D2, V6.D2, V6.D2
	VMOV  V6.D[0], R13

	BLT   fold1_end32_nl
	CBZ   R13, fold1_loop32_nl

fold1_end32_nl:
	CBZ   R13, fold1_loop16_nl_entry

	VAND  V5.B16, V20.B16, V20.B16
	VAND  V5.B16, V21.B16, V21.B16

	VADDP V20.B16, V20.B16, V6.B16
	VADDP V6.B16, V6.B16, V6.B16
	VMOV  V6.S[0], R13
	MOVD  $128, R14
	CBNZ  R13, fold1_try32_nl

	VADDP V21.B16, V21.B16, V6.B16
	VADDP V6.B16, V6.B16, V6.B16
	VMOV  V6.S[0], R13
	MOVD  $144, R14
	CBNZ  R13, fold1_try32_nl

	CMP   $32, R12
	BGE   fold1_loop32_nl
	B     fold1_loop16_nl_entry

fold1_try32_nl:
	RBIT  R13, R15
	CLZ   R15, R15
	LSR   $1, R15, R15
	AND   $0x7F, R14, R17
	ADD   R17, R15, R15

	SUB   $32, R10, R16
	ADD   R15, R16, R16
	SUB   R11, R16, R16

	CMP   R9, R16
	BGT   fold1_clear32_nl

	ADD   R0, R16, R8
	B     fold1_verify

fold1_clear32_nl:
	AND   $0x7F, R14, R17          // chunk offset (0 or 16)
	SUB   R17, R15, R20            // byte index within chunk
	LSL   $1, R20, R20             // bitpos = byteIndex << 1
	MOVD  $1, R19
	LSL   R20, R19, R20            // 1 << bitpos
	BIC   R20, R13, R13            // Clear just that bit
	CBNZ  R13, fold1_try32_nl

	CMP   $128, R14
	BNE   fold1_continue32_nl
	VADDP V21.B16, V21.B16, V6.B16
	VADDP V6.B16, V6.B16, V6.B16
	VMOV  V6.S[0], R13
	MOVD  $144, R14
	CBNZ  R13, fold1_try32_nl

fold1_continue32_nl:
	CMP   $32, R12
	BGE   fold1_loop32_nl
	B     fold1_loop16_nl_entry

// 128-byte non-letter loop
fold1_loop128_nl:
	VLD1.P 64(R10), [V16.B16, V17.B16, V18.B16, V19.B16]
	VLD1.P 64(R10), [V24.B16, V25.B16, V26.B16, V27.B16]
	SUB   $128, R12, R12

	VCMEQ V1.B16, V16.B16, V20.B16
	VCMEQ V1.B16, V17.B16, V21.B16
	VCMEQ V1.B16, V18.B16, V22.B16
	VCMEQ V1.B16, V19.B16, V23.B16
	VCMEQ V1.B16, V24.B16, V28.B16
	VCMEQ V1.B16, V25.B16, V29.B16
	VCMEQ V1.B16, V26.B16, V30.B16
	VCMEQ V1.B16, V27.B16, V31.B16

	// Combine all 8 chunks - keep V9 (first64) and V11 (second64)
	VORR  V20.B16, V21.B16, V9.B16
	VORR  V22.B16, V23.B16, V10.B16
	VORR  V28.B16, V29.B16, V11.B16
	VORR  V30.B16, V31.B16, V12.B16
	VORR  V9.B16, V10.B16, V9.B16    // V9 = OR(first64)
	VORR  V11.B16, V12.B16, V11.B16  // V11 = OR(second64)
	VORR  V9.B16, V11.B16, V13.B16   // V13 = OR(all) - preserve V9 and V11

	VADDP V13.D2, V13.D2, V13.D2
	VMOV  V13.D[0], R13

	CMP   $128, R12
	BLT   fold1_end128_nl
	CBZ   R13, fold1_loop128_nl

fold1_end128_nl:
	CBZ   R13, fold1_loop32_nl

	// Check first 64 bytes - V9 already has OR(first64), just reduce
	VADDP V9.D2, V9.D2, V9.D2
	VMOV  V9.D[0], R13
	CBNZ  R13, fold1_first64_nl

	// Check second 64 bytes - V11 already has OR(second64), just reduce
	VADDP V11.D2, V11.D2, V11.D2
	VMOV  V11.D[0], R13
	CBNZ  R13, fold1_second64_nl
	CMP   $128, R12
	BGE   fold1_loop128_nl
	B     fold1_loop32_nl

fold1_first64_nl:
	VAND  V5.B16, V20.B16, V20.B16
	VAND  V5.B16, V21.B16, V21.B16
	VAND  V5.B16, V22.B16, V22.B16
	VAND  V5.B16, V23.B16, V23.B16
	MOVD  $64, R20
	B     fold1_check_chunks_nl

fold1_second64_nl:
	VMOV  V28.B16, V20.B16
	VMOV  V29.B16, V21.B16
	VMOV  V30.B16, V22.B16
	VMOV  V31.B16, V23.B16
	VAND  V5.B16, V20.B16, V20.B16
	VAND  V5.B16, V21.B16, V21.B16
	VAND  V5.B16, V22.B16, V22.B16
	VAND  V5.B16, V23.B16, V23.B16
	MOVD  ZR, R20

fold1_check_chunks_nl:
	VADDP V20.B16, V20.B16, V6.B16
	VADDP V6.B16, V6.B16, V6.B16
	VMOV  V6.S[0], R13
	MOVD  ZR, R14
	CBNZ  R13, fold1_try128_nl

	VADDP V21.B16, V21.B16, V6.B16
	VADDP V6.B16, V6.B16, V6.B16
	VMOV  V6.S[0], R13
	MOVD  $16, R14
	CBNZ  R13, fold1_try128_nl

	VADDP V22.B16, V22.B16, V6.B16
	VADDP V6.B16, V6.B16, V6.B16
	VMOV  V6.S[0], R13
	MOVD  $32, R14
	CBNZ  R13, fold1_try128_nl

	VADDP V23.B16, V23.B16, V6.B16
	VADDP V6.B16, V6.B16, V6.B16
	VMOV  V6.S[0], R13
	MOVD  $48, R14
	CBNZ  R13, fold1_try128_nl

	CBNZ  R20, fold1_check_second64_nl_after
	CMP   $128, R12
	BGE   fold1_loop128_nl
	B     fold1_loop32_nl

fold1_check_second64_nl_after:
	VORR  V28.B16, V29.B16, V6.B16
	VORR  V30.B16, V31.B16, V9.B16
	VORR  V6.B16, V9.B16, V6.B16
	VADDP V6.D2, V6.D2, V6.D2
	VMOV  V6.D[0], R13
	CBZ   R13, fold1_continue128_nl
	B     fold1_second64_nl

fold1_continue128_nl:
	CMP   $128, R12
	BGE   fold1_loop128_nl
	B     fold1_loop32_nl

fold1_try128_nl:
	RBIT  R13, R15
	CLZ   R15, R15
	LSR   $1, R15, R15
	ADD   R14, R15, R15
	SUB   R20, R15, R15
	ADD   $64, R15, R15

	SUB   $128, R10, R16
	ADD   R15, R16, R16
	SUB   R11, R16, R16

	CMP   R9, R16
	BGT   fold1_clear128_nl

	ADD   R0, R16, R8
	B     fold1_verify

fold1_clear128_nl:
	SUB   R14, R15, R17            // byte index within chunk
	LSL   $1, R17, R17             // bitpos = byteIndex << 1
	MOVD  $1, R19
	LSL   R17, R19, R17            // 1 << bitpos
	BIC   R17, R13, R13            // Clear just that bit
	CBNZ  R13, fold1_try128_nl

	ADD   $16, R14, R14
	CMP   $64, R14
	BLT   fold1_next_chunk_nl
	CBNZ  R20, fold1_check_second64_nl_after
	B     fold1_continue128_nl

fold1_next_chunk_nl:
	CMP   $16, R14
	BEQ   fold1_chunk1_nl
	CMP   $32, R14
	BEQ   fold1_chunk2_nl
	CMP   $48, R14
	BEQ   fold1_chunk3_nl
	B     fold1_continue128_nl

fold1_chunk1_nl:
	VADDP V21.B16, V21.B16, V6.B16
	VADDP V6.B16, V6.B16, V6.B16
	VMOV  V6.S[0], R13
	CBNZ  R13, fold1_try128_nl
	ADD   $16, R14, R14
fold1_chunk2_nl:
	VADDP V22.B16, V22.B16, V6.B16
	VADDP V6.B16, V6.B16, V6.B16
	VMOV  V6.S[0], R13
	CBNZ  R13, fold1_try128_nl
	ADD   $16, R14, R14
fold1_chunk3_nl:
	VADDP V23.B16, V23.B16, V6.B16
	VADDP V6.B16, V6.B16, V6.B16
	VMOV  V6.S[0], R13
	CBNZ  R13, fold1_try128_nl
	CBNZ  R20, fold1_check_second64_nl_after
	B     fold1_continue128_nl

// ============================================================================
// 16-BYTE LOOPS
// ============================================================================
fold1_loop16_entry:
	CMP   $16, R12
	BLT   fold1_scalar_entry

fold1_loop16:
	VLD1.P 16(R10), [V16.B16]
	SUB   $16, R12, R12

	VORR  V0.B16, V16.B16, V20.B16
	VCMEQ V1.B16, V20.B16, V20.B16
	VAND  V5.B16, V20.B16, V20.B16
	VADDP V20.B16, V20.B16, V20.B16
	VADDP V20.B16, V20.B16, V20.B16
	VMOV  V20.S[0], R13
	CBZ   R13, fold1_check16_continue

fold1_try16:
	RBIT  R13, R15
	CLZ   R15, R15
	LSR   $1, R15, R15

	SUB   $16, R10, R16
	ADD   R15, R16, R16
	SUB   R11, R16, R16

	CMP   R9, R16
	BGT   fold1_clear16

	ADD   R0, R16, R8
	MOVD  $0x100, R14             // Mark as 16-byte path
	B     fold1_verify

fold1_clear16:
	LSL   $1, R15, R17             // bitpos = byteIndex << 1
	MOVD  $1, R19
	LSL   R17, R19, R17            // 1 << bitpos
	BIC   R17, R13, R13            // Clear just that bit
	CBNZ  R13, fold1_try16

fold1_check16_continue:
	CMP   $16, R12
	BGE   fold1_loop16

fold1_scalar_entry:
	CMP   $0, R12
	BLE   fold1_not_found

fold1_scalar:
	MOVBU (R10), R13
	ORRW  R26, R13, R14
	CMPW  R5, R14
	BNE   fold1_scalar_next

	SUB   R11, R10, R16
	CMP   R9, R16
	BGT   fold1_scalar_next

	ADD   R0, R16, R8
	B     fold1_verify_scalar

fold1_scalar_next:
	ADD   $1, R10
	SUB   $1, R12
	CBNZ  R12, fold1_scalar
	B     fold1_not_found

// 16-byte non-letter path
fold1_loop16_nl_entry:
	CMP   $16, R12
	BLT   fold1_scalar_nl_entry

fold1_loop16_nl:
	VLD1.P 16(R10), [V16.B16]
	SUB   $16, R12, R12

	VCMEQ V1.B16, V16.B16, V20.B16
	VAND  V5.B16, V20.B16, V20.B16
	VADDP V20.B16, V20.B16, V20.B16
	VADDP V20.B16, V20.B16, V20.B16
	VMOV  V20.S[0], R13
	CBZ   R13, fold1_check16_nl_continue

fold1_try16_nl:
	RBIT  R13, R15
	CLZ   R15, R15
	LSR   $1, R15, R15

	SUB   $16, R10, R16
	ADD   R15, R16, R16
	SUB   R11, R16, R16

	CMP   R9, R16
	BGT   fold1_clear16_nl

	ADD   R0, R16, R8
	MOVD  $0x100, R14
	B     fold1_verify

fold1_clear16_nl:
	LSL   $1, R15, R17             // bitpos = byteIndex << 1
	MOVD  $1, R19
	LSL   R17, R19, R17            // 1 << bitpos
	BIC   R17, R13, R13            // Clear just that bit
	CBNZ  R13, fold1_try16_nl

fold1_check16_nl_continue:
	CMP   $16, R12
	BGE   fold1_loop16_nl

fold1_scalar_nl_entry:
	CMP   $0, R12
	BLE   fold1_not_found

fold1_scalar_nl:
	MOVBU (R10), R13
	CMPW  R5, R13
	BNE   fold1_scalar_nl_next

	SUB   R11, R10, R16
	CMP   R9, R16
	BGT   fold1_scalar_nl_next

	ADD   R0, R16, R8
	B     fold1_verify_scalar_nl

fold1_scalar_nl_next:
	ADD   $1, R10
	SUB   $1, R12
	CBNZ  R12, fold1_scalar_nl
	B     fold1_not_found

// ============================================================================
// SCALAR VERIFICATION (separate paths to avoid syndrome-clearing logic)
// ============================================================================
fold1_verify_scalar:
	MOVD  R3, R19                 // R19 = remaining needle length
	MOVD  R8, R21                 // R21 = haystack candidate ptr
	MOVD  R2, R22                 // R22 = needle ptr

fold1_scalar_vloop:
	SUBS  $16, R19, R23
	BLT   fold1_scalar_vtail

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
	CBZW  R23, fold1_scalar_vloop
	B     fold1_scalar_verify_fail

fold1_scalar_vtail:
	CMP   $1, R19
	BLT   fold1_found

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
	CBNZW R23, fold1_scalar_verify_fail
	B     fold1_found

fold1_scalar_verify_fail:
	ADD   $1, R25, R25
	SUB   R11, R10, R17
	LSR   $3, R17, R17
	ADD   $32, R17, R17
	CMP   R17, R25
	BGT   fold1_exceeded
	B     fold1_scalar_next

fold1_verify_scalar_nl:
	MOVD  R3, R19
	MOVD  R8, R21
	MOVD  R2, R22

fold1_scalar_nl_vloop:
	SUBS  $16, R19, R23
	BLT   fold1_scalar_nl_vtail

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
	CBZW  R23, fold1_scalar_nl_vloop
	B     fold1_scalar_nl_verify_fail

fold1_scalar_nl_vtail:
	CMP   $1, R19
	BLT   fold1_found

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
	CBNZW R23, fold1_scalar_nl_verify_fail
	B     fold1_found

fold1_scalar_nl_verify_fail:
	ADD   $1, R25, R25
	SUB   R11, R10, R17
	LSR   $3, R17, R17
	ADD   $32, R17, R17
	CMP   R17, R25
	BGT   fold1_exceeded
	B     fold1_scalar_nl_next

// ============================================================================
// SIMD VERIFICATION (for 16/32/128-byte loop paths)
// ============================================================================
fold1_verify:
	MOVD  R3, R19                 // R19 = remaining needle length
	MOVD  R8, R21                 // R21 = haystack candidate ptr
	MOVD  R2, R22                 // R22 = needle ptr

fold1_vloop:
	SUBS  $16, R19, R23
	BLT   fold1_vtail

	VLD1.P 16(R21), [V10.B16]
	VLD1.P 16(R22), [V11.B16]
	MOVD   R23, R19

	// Derive fold mask from needle (not haystack) - needle is pre-normalized
	VADD  V4.B16, V11.B16, V14.B16  // V14 = needle + 159
	WORD  $0x6e2e34ee               // VCMHI V14.B16, V7.B16, V14.B16 (needle_is_letter mask)
	VAND  V8.B16, V14.B16, V14.B16  // V14 = needle_is_letter ? 0x20 : 0
	VORR  V14.B16, V10.B16, V10.B16 // V10 = h | mask (fold only where needle is letter)
	VEOR  V10.B16, V11.B16, V10.B16 // V10 = h_folded XOR needle
	WORD  $0x6e30a94a               // VUMAXV V10.B16, V10 (any non-zero?)
	FMOVS F10, R23
	CBZW  R23, fold1_vloop
	B     fold1_verify_fail

fold1_vtail:
	CMP   $1, R19
	BLT   fold1_found

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
	CBNZW R23, fold1_verify_fail
	B     fold1_found

fold1_verify_fail:
	// Increment failure counter and check threshold
	ADD   $1, R25, R25
	SUB   R11, R10, R17           // bytes_scanned
	LSR   $3, R17, R17            // >> 3 (allow 1 failure per 8 bytes)
	ADD   $32, R17, R17           // threshold = 32 + (bytes_scanned >> 3)
	CMP   R17, R25
	BGT   fold1_exceeded

	// Clear bit and continue - use R14 to determine which loop
	CMP   $0x100, R14
	BEQ   fold1_clear16_from_verify
	CMP   $64, R14
	BLT   fold1_clear128_from_verify
	B     fold1_clear32_from_verify

fold1_clear16_from_verify:
	LSL   $1, R15, R17             // bitpos = byteIndex << 1
	MOVD  $1, R19
	LSL   R17, R19, R17            // 1 << bitpos
	BIC   R17, R13, R13            // Clear just that bit
	CBZ   R26, fold1_clear16_nl_from_verify
	CBNZ  R13, fold1_try16
	B     fold1_check16_continue

fold1_clear16_nl_from_verify:
	CBNZ  R13, fold1_try16_nl
	B     fold1_check16_nl_continue

fold1_clear128_from_verify:
	SUB   R14, R15, R17            // byte index within chunk
	LSL   $1, R17, R17             // bitpos = byteIndex << 1
	MOVD  $1, R19
	LSL   R17, R19, R17            // 1 << bitpos
	BIC   R17, R13, R13            // Clear just that bit
	CBZ   R26, fold1_try128_nl_retry
	CBNZ  R13, fold1_try128
	ADD   $16, R14, R14
	CMP   $64, R14
	BLT   fold1_next_chunk
	CBNZ  R20, fold1_check_second64_after
	B     fold1_continue128

fold1_try128_nl_retry:
	CBNZ  R13, fold1_try128_nl
	ADD   $16, R14, R14
	CMP   $64, R14
	BLT   fold1_next_chunk_nl
	CBNZ  R20, fold1_check_second64_nl_after
	B     fold1_continue128_nl

fold1_clear32_from_verify:
	AND   $0x7F, R14, R17          // chunk offset (0 or 16)
	SUB   R17, R15, R20            // byte index within chunk
	LSL   $1, R20, R20             // bitpos = byteIndex << 1
	MOVD  $1, R19
	LSL   R20, R19, R20            // 1 << bitpos
	BIC   R20, R13, R13            // Clear just that bit
	CBZ   R26, fold1_try32_nl_retry
	CBNZ  R13, fold1_try32
	CMP   $128, R14
	BNE   fold1_continue32
	VADDP V21.B16, V21.B16, V6.B16
	VADDP V6.B16, V6.B16, V6.B16
	VMOV  V6.S[0], R13
	MOVD  $144, R14
	CBNZ  R13, fold1_try32
	B     fold1_continue32

fold1_try32_nl_retry:
	CBNZ  R13, fold1_try32_nl
	CMP   $128, R14
	BNE   fold1_continue32_nl
	VADDP V21.B16, V21.B16, V6.B16
	VADDP V6.B16, V6.B16, V6.B16
	VMOV  V6.S[0], R13
	MOVD  $144, R14
	CBNZ  R13, fold1_try32_nl
	B     fold1_continue32_nl

// ============================================================================
// EXIT PATHS
// ============================================================================
fold1_exceeded:
	MOVD  $0x8000000000000000, R17   // exceeded flag (bit 63)
	ADD   R16, R17, R0              // add position
	MOVD  R0, ret+40(FP)
	RET

fold1_not_found:
	MOVD  $-1, R0
	MOVD  R0, ret+40(FP)
	RET

fold1_found_zero:
	MOVD  ZR, R0
	MOVD  R0, ret+40(FP)
	RET

fold1_found:
	MOVD  R16, R0
	MOVD  R0, ret+40(FP)
	RET

// ============================================================================
// indexFold2Byte - Case-insensitive 2-byte filter with pre-normalized needle
//
// func indexFold2Byte(haystack string, needle string, off1 int, off2_delta int) uint64
//
// Returns:
//   - Position if found
//   - 0xFFFFFFFFFFFFFFFF (-1) if not found
//   - 0x8000000000000000 + position if exceeded threshold
//
// Register allocation:
//   R0  = haystack ptr
//   R1  = haystack len
//   R2  = needle ptr
//   R3  = needle len
//   R4  = off1
//   R5  = off2_delta
//   R6  = rare1 byte (from normalized needle)
//   R7  = rare2 byte (from normalized needle)
//   R8  = rare1 mask (0x20 for letter, 0x00 for non-letter)
//   R9  = searchLen = haystack_len - needle_len
//   R10 = ptr1 (searchPtr, post-incremented by VLD1.P)
//   R11 = searchStart
//   R12 = remaining bytes
//   R13 = syndrome result
//   R14 = ptr2 (ptr1 + off2_delta, post-incremented by VLD1.P)
//   R15 = bit position
//   R16 = candidate position
//   R24 = tail_mask_table ptr
//   R25 = failure count
//   R26 = rare2 mask (0x20 for letter, 0x00 for non-letter)
//
// Vector registers:
//   V0  = rare1 mask broadcast
//   V1  = rare1 target broadcast
//   V2  = rare2 mask broadcast
//   V3  = rare2 target broadcast
//   V4  = 159 (-97 as unsigned) for letter detection in verify
//   V5  = syndrome magic constant
//   V7  = 26 (letter range check)
//   V8  = 32 (0x20 for case folding)
// ============================================================================

