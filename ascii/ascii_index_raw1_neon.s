//go:build !noasm && arm64

// Hand-optimized ARM64 NEON assembly for substring search kernels

#include "textflag.h"

// Magic constant for syndrome extraction (2 bits per byte)
#define SYNDROME_MAGIC $0x40100401

TEXT ·indexFold1ByteRaw(SB), NOSPLIT, $0-48
	MOVD  haystack+0(FP), R0      // R0 = haystack ptr
	MOVD  haystack_len+8(FP), R1  // R1 = haystack len
	MOVD  needle+16(FP), R2       // R2 = needle ptr
	MOVD  needle_len+24(FP), R3   // R3 = needle len
	MOVD  off1+32(FP), R4         // R4 = off1

	// Early exits
	SUBS  R3, R1, R9              // R9 = searchLen = haystack_len - needle_len
	BLT   raw1_not_found
	CBZ   R3, raw1_found_zero

	// Load byte from needle[off1] and normalize (uppercase → lowercase)
	ADD   R2, R4, R8              // R8 = needle + off1
	MOVBU (R8), R5                // R5 = raw byte from needle
	SUBW  $65, R5, R8             // R8 = byte - 'A'
	ORRW  $32, R5, R6             // R6 = byte | 0x20 (lowercase)
	CMPW  $26, R8                 // if (byte - 'A') < 26, it's uppercase
	CSELW LO, R6, R5, R5          // R5 = normalized byte (lowercase if was uppercase)

	// Compute mask and target for rare1 (same as indexFold1Byte)
	// R5 = normalized byte (lowercase if letter), keep it as target
	SUBW  $97, R5, R8             // R8 = byte - 'a'
	CMPW  $26, R8
	BCS   raw1_not_letter
	MOVW  $0x20, R26              // mask = 0x20 (OR to force lowercase)
	B     raw1_setup
raw1_not_letter:
	MOVW  $0x00, R26              // mask = 0x00 (identity)

raw1_setup:
	// R5 = target byte (already holds normalized byte)
	VDUP  R26, V0.B16             // V0 = mask broadcast
	VDUP  R5, V1.B16              // V1 = target broadcast

	// Syndrome magic constant
	MOVD  $0x4010040140100401, R8
	VMOV  R8, V5.D[0]
	VMOV  R8, V5.D[1]

	// Constants for verification (XOR-based, same as fold1)
	// V2 = 159 (-97 as unsigned), V3 = 26, V4 = 32 (0x20)
	WORD  $0x4f04e7e2             // VMOVI $159, V2.B16
	WORD  $0x4f00e743             // VMOVI $26, V3.B16
	WORD  $0x4f01e404             // VMOVI $32, V4.B16
	MOVD  $tail_mask_table(SB), R24  // R24 = tail mask table

	// Setup pointers
	ADD   R4, R0, R10             // R10 = searchPtr = haystack + off1
	MOVD  R10, R11                // R11 = searchStart
	ADD   $1, R9, R12             // R12 = remaining = searchLen + 1

	// Initialize failure counter
	MOVD  ZR, R25                 // R25 = failure count = 0

	// Dispatch based on letter/non-letter and size
	CBZ   R26, raw1_dispatch_nonletter

	// Letter path: check size threshold
	CMP   $768, R12
	BGE   raw1_loop128
	CMP   $32, R12
	BLT   raw1_loop16_entry

// ============================================================================
// 32-BYTE LOOP (letter path)
// ============================================================================
raw1_loop32:
	VLD1.P 32(R10), [V16.B16, V17.B16]
	SUBS  $32, R12, R12

	VORR  V0.B16, V16.B16, V20.B16
	VORR  V0.B16, V17.B16, V21.B16
	VCMEQ V1.B16, V20.B16, V20.B16
	VCMEQ V1.B16, V21.B16, V21.B16

	VORR  V20.B16, V21.B16, V6.B16
	VADDP V6.D2, V6.D2, V6.D2
	VMOV  V6.D[0], R13

	BLT   raw1_end32
	CBZ   R13, raw1_loop32

raw1_end32:
	CBZ   R13, raw1_loop16_entry

	VAND  V5.B16, V20.B16, V20.B16
	VAND  V5.B16, V21.B16, V21.B16

	VADDP V20.B16, V20.B16, V6.B16
	VADDP V6.B16, V6.B16, V6.B16
	VMOV  V6.S[0], R13
	MOVD  $128, R14
	CBNZ  R13, raw1_try32

	VADDP V21.B16, V21.B16, V6.B16
	VADDP V6.B16, V6.B16, V6.B16
	VMOV  V6.S[0], R13
	MOVD  $144, R14
	CBNZ  R13, raw1_try32

	CMP   $32, R12
	BGE   raw1_loop32
	B     raw1_loop16_entry

raw1_try32:
	RBIT  R13, R15
	CLZ   R15, R15
	LSR   $1, R15, R15
	AND   $0x7F, R14, R17
	ADD   R17, R15, R15

	SUB   $32, R10, R16
	ADD   R15, R16, R16
	SUB   R11, R16, R16

	CMP   R9, R16
	BGT   raw1_clear32

	ADD   R0, R16, R8
	B     raw1_verify

raw1_clear32:
	AND   $0x7F, R14, R17          // R17 = chunk offset
	SUB   R17, R15, R20            // R20 = byteIndex - chunkOffset
	LSL   $1, R20, R20             // bitpos = localByteIndex << 1
	MOVD  $1, R19
	LSL   R20, R19, R20            // 1 << bitpos
	BIC   R20, R13, R13            // Clear just that bit
	CBNZ  R13, raw1_try32

	CMP   $128, R14
	BNE   raw1_continue32
	VADDP V21.B16, V21.B16, V6.B16
	VADDP V6.B16, V6.B16, V6.B16
	VMOV  V6.S[0], R13
	MOVD  $144, R14
	CBNZ  R13, raw1_try32

raw1_continue32:
	CMP   $32, R12
	BGE   raw1_loop32
	B     raw1_loop16_entry

// ============================================================================
// 128-BYTE LOOP (letter path)
// ============================================================================
raw1_loop128:
	VLD1.P 64(R10), [V16.B16, V17.B16, V18.B16, V19.B16]
	VLD1.P 64(R10), [V24.B16, V25.B16, V26.B16, V27.B16]
	SUB   $128, R12, R12

	VORR  V0.B16, V16.B16, V20.B16
	VORR  V0.B16, V17.B16, V21.B16
	VORR  V0.B16, V18.B16, V22.B16
	VORR  V0.B16, V19.B16, V23.B16
	VCMEQ V1.B16, V20.B16, V20.B16
	VCMEQ V1.B16, V21.B16, V21.B16
	VCMEQ V1.B16, V22.B16, V22.B16
	VCMEQ V1.B16, V23.B16, V23.B16

	VORR  V0.B16, V24.B16, V28.B16
	VORR  V0.B16, V25.B16, V29.B16
	VORR  V0.B16, V26.B16, V30.B16
	VORR  V0.B16, V27.B16, V31.B16
	VCMEQ V1.B16, V28.B16, V28.B16
	VCMEQ V1.B16, V29.B16, V29.B16
	VCMEQ V1.B16, V30.B16, V30.B16
	VCMEQ V1.B16, V31.B16, V31.B16

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
	BLT   raw1_end128
	CBZ   R13, raw1_loop128

raw1_end128:
	CBZ   R13, raw1_loop32

	// Check first 64 bytes - V9 already has OR(first64), just reduce
	VADDP V9.D2, V9.D2, V9.D2
	VMOV  V9.D[0], R13
	CBNZ  R13, raw1_first64

	// Check second 64 bytes - V11 already has OR(second64), just reduce
	VADDP V11.D2, V11.D2, V11.D2
	VMOV  V11.D[0], R13
	CBNZ  R13, raw1_second64
	CMP   $128, R12
	BGE   raw1_loop128
	B     raw1_loop32

raw1_first64:
	VAND  V5.B16, V20.B16, V20.B16
	VAND  V5.B16, V21.B16, V21.B16
	VAND  V5.B16, V22.B16, V22.B16
	VAND  V5.B16, V23.B16, V23.B16
	MOVD  $64, R20
	B     raw1_check_chunks

raw1_second64:
	VMOV  V28.B16, V20.B16
	VMOV  V29.B16, V21.B16
	VMOV  V30.B16, V22.B16
	VMOV  V31.B16, V23.B16
	VAND  V5.B16, V20.B16, V20.B16
	VAND  V5.B16, V21.B16, V21.B16
	VAND  V5.B16, V22.B16, V22.B16
	VAND  V5.B16, V23.B16, V23.B16
	MOVD  ZR, R20

raw1_check_chunks:
	VADDP V20.B16, V20.B16, V6.B16
	VADDP V6.B16, V6.B16, V6.B16
	VMOV  V6.S[0], R13
	MOVD  ZR, R14
	CBNZ  R13, raw1_try128

	VADDP V21.B16, V21.B16, V6.B16
	VADDP V6.B16, V6.B16, V6.B16
	VMOV  V6.S[0], R13
	MOVD  $16, R14
	CBNZ  R13, raw1_try128

	VADDP V22.B16, V22.B16, V6.B16
	VADDP V6.B16, V6.B16, V6.B16
	VMOV  V6.S[0], R13
	MOVD  $32, R14
	CBNZ  R13, raw1_try128

	VADDP V23.B16, V23.B16, V6.B16
	VADDP V6.B16, V6.B16, V6.B16
	VMOV  V6.S[0], R13
	MOVD  $48, R14
	CBNZ  R13, raw1_try128

	CBNZ  R20, raw1_check_second64_after
	CMP   $128, R12
	BGE   raw1_loop128
	B     raw1_loop32

raw1_check_second64_after:
	VORR  V28.B16, V29.B16, V6.B16
	VORR  V30.B16, V31.B16, V9.B16
	VORR  V6.B16, V9.B16, V6.B16
	VADDP V6.D2, V6.D2, V6.D2
	VMOV  V6.D[0], R13
	CBZ   R13, raw1_continue128
	B     raw1_second64

raw1_continue128:
	CMP   $128, R12
	BGE   raw1_loop128
	B     raw1_loop32

raw1_try128:
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
	BGT   raw1_clear128

	ADD   R0, R16, R8
	B     raw1_verify

raw1_clear128:
	SUB   R14, R15, R17            // R17 = byteIndex - chunkOffset
	LSL   $1, R17, R17             // bitpos = localByteIndex << 1
	MOVD  $1, R19
	LSL   R17, R19, R17            // 1 << bitpos
	BIC   R17, R13, R13            // Clear just that bit
	CBNZ  R13, raw1_try128

	ADD   $16, R14, R14
	CMP   $64, R14
	BLT   raw1_next_chunk
	CBNZ  R20, raw1_check_second64_after
	B     raw1_continue128

raw1_next_chunk:
	CMP   $16, R14
	BEQ   raw1_chunk1
	CMP   $32, R14
	BEQ   raw1_chunk2
	CMP   $48, R14
	BEQ   raw1_chunk3
	B     raw1_continue128

raw1_chunk1:
	VADDP V21.B16, V21.B16, V6.B16
	VADDP V6.B16, V6.B16, V6.B16
	VMOV  V6.S[0], R13
	CBNZ  R13, raw1_try128
	ADD   $16, R14, R14
raw1_chunk2:
	VADDP V22.B16, V22.B16, V6.B16
	VADDP V6.B16, V6.B16, V6.B16
	VMOV  V6.S[0], R13
	CBNZ  R13, raw1_try128
	ADD   $16, R14, R14
raw1_chunk3:
	VADDP V23.B16, V23.B16, V6.B16
	VADDP V6.B16, V6.B16, V6.B16
	VMOV  V6.S[0], R13
	CBNZ  R13, raw1_try128
	CBNZ  R20, raw1_check_second64_after
	B     raw1_continue128

// ============================================================================
// NON-LETTER PATH (skips VORR for 5-op loop)
// ============================================================================
raw1_dispatch_nonletter:
	CMP   $768, R12
	BGE   raw1_loop128_nl
	CMP   $32, R12
	BLT   raw1_loop16_nl_entry

raw1_loop32_nl:
	VLD1.P 32(R10), [V16.B16, V17.B16]
	SUBS  $32, R12, R12

	VCMEQ V1.B16, V16.B16, V20.B16
	VCMEQ V1.B16, V17.B16, V21.B16

	VORR  V20.B16, V21.B16, V6.B16
	VADDP V6.D2, V6.D2, V6.D2
	VMOV  V6.D[0], R13

	BLT   raw1_end32_nl
	CBZ   R13, raw1_loop32_nl

raw1_end32_nl:
	CBZ   R13, raw1_loop16_nl_entry

	VAND  V5.B16, V20.B16, V20.B16
	VAND  V5.B16, V21.B16, V21.B16

	VADDP V20.B16, V20.B16, V6.B16
	VADDP V6.B16, V6.B16, V6.B16
	VMOV  V6.S[0], R13
	MOVD  $128, R14
	CBNZ  R13, raw1_try32_nl

	VADDP V21.B16, V21.B16, V6.B16
	VADDP V6.B16, V6.B16, V6.B16
	VMOV  V6.S[0], R13
	MOVD  $144, R14
	CBNZ  R13, raw1_try32_nl

	CMP   $32, R12
	BGE   raw1_loop32_nl
	B     raw1_loop16_nl_entry

raw1_try32_nl:
	RBIT  R13, R15
	CLZ   R15, R15
	LSR   $1, R15, R15
	AND   $0x7F, R14, R17
	ADD   R17, R15, R15

	SUB   $32, R10, R16
	ADD   R15, R16, R16
	SUB   R11, R16, R16

	CMP   R9, R16
	BGT   raw1_clear32_nl

	ADD   R0, R16, R8
	B     raw1_verify

raw1_clear32_nl:
	AND   $0x7F, R14, R17          // R17 = chunk offset
	SUB   R17, R15, R20            // R20 = byteIndex - chunkOffset
	LSL   $1, R20, R20             // bitpos = localByteIndex << 1
	MOVD  $1, R19
	LSL   R20, R19, R20            // 1 << bitpos
	BIC   R20, R13, R13            // Clear just that bit
	CBNZ  R13, raw1_try32_nl

	CMP   $128, R14
	BNE   raw1_continue32_nl
	VADDP V21.B16, V21.B16, V6.B16
	VADDP V6.B16, V6.B16, V6.B16
	VMOV  V6.S[0], R13
	MOVD  $144, R14
	CBNZ  R13, raw1_try32_nl

raw1_continue32_nl:
	CMP   $32, R12
	BGE   raw1_loop32_nl
	B     raw1_loop16_nl_entry

// 128-byte non-letter loop
raw1_loop128_nl:
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
	BLT   raw1_end128_nl
	CBZ   R13, raw1_loop128_nl

raw1_end128_nl:
	CBZ   R13, raw1_loop32_nl

	// Check first 64 bytes - V9 already has OR(first64), just reduce
	VADDP V9.D2, V9.D2, V9.D2
	VMOV  V9.D[0], R13
	CBNZ  R13, raw1_first64_nl

	// Check second 64 bytes - V11 already has OR(second64), just reduce
	VADDP V11.D2, V11.D2, V11.D2
	VMOV  V11.D[0], R13
	CBNZ  R13, raw1_second64_nl
	CMP   $128, R12
	BGE   raw1_loop128_nl
	B     raw1_loop32_nl

raw1_first64_nl:
	VAND  V5.B16, V20.B16, V20.B16
	VAND  V5.B16, V21.B16, V21.B16
	VAND  V5.B16, V22.B16, V22.B16
	VAND  V5.B16, V23.B16, V23.B16
	MOVD  $64, R20
	B     raw1_check_chunks_nl

raw1_second64_nl:
	VMOV  V28.B16, V20.B16
	VMOV  V29.B16, V21.B16
	VMOV  V30.B16, V22.B16
	VMOV  V31.B16, V23.B16
	VAND  V5.B16, V20.B16, V20.B16
	VAND  V5.B16, V21.B16, V21.B16
	VAND  V5.B16, V22.B16, V22.B16
	VAND  V5.B16, V23.B16, V23.B16
	MOVD  ZR, R20

raw1_check_chunks_nl:
	VADDP V20.B16, V20.B16, V6.B16
	VADDP V6.B16, V6.B16, V6.B16
	VMOV  V6.S[0], R13
	MOVD  ZR, R14
	CBNZ  R13, raw1_try128_nl

	VADDP V21.B16, V21.B16, V6.B16
	VADDP V6.B16, V6.B16, V6.B16
	VMOV  V6.S[0], R13
	MOVD  $16, R14
	CBNZ  R13, raw1_try128_nl

	VADDP V22.B16, V22.B16, V6.B16
	VADDP V6.B16, V6.B16, V6.B16
	VMOV  V6.S[0], R13
	MOVD  $32, R14
	CBNZ  R13, raw1_try128_nl

	VADDP V23.B16, V23.B16, V6.B16
	VADDP V6.B16, V6.B16, V6.B16
	VMOV  V6.S[0], R13
	MOVD  $48, R14
	CBNZ  R13, raw1_try128_nl

	CBNZ  R20, raw1_check_second64_nl_after
	CMP   $128, R12
	BGE   raw1_loop128_nl
	B     raw1_loop32_nl

raw1_check_second64_nl_after:
	VORR  V28.B16, V29.B16, V6.B16
	VORR  V30.B16, V31.B16, V9.B16
	VORR  V6.B16, V9.B16, V6.B16
	VADDP V6.D2, V6.D2, V6.D2
	VMOV  V6.D[0], R13
	CBZ   R13, raw1_continue128_nl
	B     raw1_second64_nl

raw1_continue128_nl:
	CMP   $128, R12
	BGE   raw1_loop128_nl
	B     raw1_loop32_nl

raw1_try128_nl:
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
	BGT   raw1_clear128_nl

	ADD   R0, R16, R8
	B     raw1_verify

raw1_clear128_nl:
	SUB   R14, R15, R17            // R17 = byteIndex - chunkOffset
	LSL   $1, R17, R17             // bitpos = localByteIndex << 1
	MOVD  $1, R19
	LSL   R17, R19, R17            // 1 << bitpos
	BIC   R17, R13, R13            // Clear just that bit
	CBNZ  R13, raw1_try128_nl

	ADD   $16, R14, R14
	CMP   $64, R14
	BLT   raw1_next_chunk_nl
	CBNZ  R20, raw1_check_second64_nl_after
	B     raw1_continue128_nl

raw1_next_chunk_nl:
	CMP   $16, R14
	BEQ   raw1_chunk1_nl
	CMP   $32, R14
	BEQ   raw1_chunk2_nl
	CMP   $48, R14
	BEQ   raw1_chunk3_nl
	B     raw1_continue128_nl

raw1_chunk1_nl:
	VADDP V21.B16, V21.B16, V6.B16
	VADDP V6.B16, V6.B16, V6.B16
	VMOV  V6.S[0], R13
	CBNZ  R13, raw1_try128_nl
	ADD   $16, R14, R14
raw1_chunk2_nl:
	VADDP V22.B16, V22.B16, V6.B16
	VADDP V6.B16, V6.B16, V6.B16
	VMOV  V6.S[0], R13
	CBNZ  R13, raw1_try128_nl
	ADD   $16, R14, R14
raw1_chunk3_nl:
	VADDP V23.B16, V23.B16, V6.B16
	VADDP V6.B16, V6.B16, V6.B16
	VMOV  V6.S[0], R13
	CBNZ  R13, raw1_try128_nl
	CBNZ  R20, raw1_check_second64_nl_after
	B     raw1_continue128_nl

// ============================================================================
// 16-BYTE LOOPS
// ============================================================================
raw1_loop16_entry:
	CMP   $16, R12
	BLT   raw1_scalar_entry

raw1_loop16:
	VLD1.P 16(R10), [V16.B16]
	SUB   $16, R12, R12

	VORR  V0.B16, V16.B16, V20.B16
	VCMEQ V1.B16, V20.B16, V20.B16
	VAND  V5.B16, V20.B16, V20.B16
	VADDP V20.B16, V20.B16, V20.B16
	VADDP V20.B16, V20.B16, V20.B16
	VMOV  V20.S[0], R13
	CBZ   R13, raw1_check16_continue

raw1_try16:
	RBIT  R13, R15
	CLZ   R15, R15
	LSR   $1, R15, R15

	SUB   $16, R10, R16
	ADD   R15, R16, R16
	SUB   R11, R16, R16

	CMP   R9, R16
	BGT   raw1_clear16

	ADD   R0, R16, R8
	MOVD  $0x100, R14
	B     raw1_verify

raw1_clear16:
	LSL   $1, R15, R17             // bitpos = byteIndex << 1
	MOVD  $1, R19
	LSL   R17, R19, R17            // 1 << bitpos
	BIC   R17, R13, R13            // Clear just that bit
	CBNZ  R13, raw1_try16

raw1_check16_continue:
	CMP   $16, R12
	BGE   raw1_loop16

raw1_scalar_entry:
	CMP   $0, R12
	BLE   raw1_not_found

raw1_scalar:
	MOVBU (R10), R13
	ORRW  R26, R13, R14
	CMPW  R5, R14
	BNE   raw1_scalar_next

	SUB   R11, R10, R16
	CMP   R9, R16
	BGT   raw1_scalar_next

	ADD   R0, R16, R8
	B     raw1_verify_scalar

raw1_scalar_next:
	ADD   $1, R10
	SUB   $1, R12
	CBNZ  R12, raw1_scalar
	B     raw1_not_found

// 16-byte non-letter path
raw1_loop16_nl_entry:
	CMP   $16, R12
	BLT   raw1_scalar_nl_entry

raw1_loop16_nl:
	VLD1.P 16(R10), [V16.B16]
	SUB   $16, R12, R12

	VCMEQ V1.B16, V16.B16, V20.B16
	VAND  V5.B16, V20.B16, V20.B16
	VADDP V20.B16, V20.B16, V20.B16
	VADDP V20.B16, V20.B16, V20.B16
	VMOV  V20.S[0], R13
	CBZ   R13, raw1_check16_nl_continue

raw1_try16_nl:
	RBIT  R13, R15
	CLZ   R15, R15
	LSR   $1, R15, R15

	SUB   $16, R10, R16
	ADD   R15, R16, R16
	SUB   R11, R16, R16

	CMP   R9, R16
	BGT   raw1_clear16_nl

	ADD   R0, R16, R8
	MOVD  $0x100, R14
	B     raw1_verify

raw1_clear16_nl:
	LSL   $1, R15, R17             // bitpos = byteIndex << 1
	MOVD  $1, R19
	LSL   R17, R19, R17            // 1 << bitpos
	BIC   R17, R13, R13            // Clear just that bit
	CBNZ  R13, raw1_try16_nl

raw1_check16_nl_continue:
	CMP   $16, R12
	BGE   raw1_loop16_nl

raw1_scalar_nl_entry:
	CMP   $0, R12
	BLE   raw1_not_found

raw1_scalar_nl:
	MOVBU (R10), R13
	CMPW  R5, R13
	BNE   raw1_scalar_nl_next

	SUB   R11, R10, R16
	CMP   R9, R16
	BGT   raw1_scalar_nl_next

	ADD   R0, R16, R8
	B     raw1_verify_scalar_nl

raw1_scalar_nl_next:
	ADD   $1, R10
	SUB   $1, R12
	CBNZ  R12, raw1_scalar_nl
	B     raw1_not_found

// ============================================================================
// SCALAR VERIFICATION (separate paths to avoid syndrome-clearing logic)
// Dual-normalizes BOTH haystack AND needle (for raw needle input)
// ============================================================================
raw1_verify_scalar:
	MOVD  R3, R19                 // R19 = remaining needle length
	MOVD  R8, R21                 // R21 = haystack candidate ptr
	MOVD  R2, R22                 // R22 = needle ptr

raw1_scalar_vloop:
	SUBS  $16, R19, R23
	BLT   raw1_scalar_vtail

	VLD1.P 16(R21), [V10.B16]
	VLD1.P 16(R22), [V11.B16]
	MOVD   R23, R19

	// XOR-based case-insensitive compare
	VEOR  V10.B16, V11.B16, V12.B16   // V12 = diff
	VCMEQ V4.B16, V12.B16, V14.B16    // V14 = (diff == 0x20) ? 0xFF : 0
	VORR  V4.B16, V10.B16, V16.B16    // V16 = haystack | 0x20
	VADD  V2.B16, V16.B16, V16.B16    // V16 = (haystack | 0x20) + 159
	WORD  $0x6e303470                 // VCMHI V16.B16, V3.B16, V16.B16 (is_letter)
	VAND  V14.B16, V16.B16, V16.B16   // V16 = (diff==0x20) AND is_letter
	VAND  V4.B16, V16.B16, V16.B16    // V16 = tolerable diff mask
	VEOR  V16.B16, V12.B16, V10.B16   // V10 = diff XOR tolerable = real mismatch
	WORD  $0x6e30a94a                 // VUMAXV V10.B16, V10
	FMOVS F10, R23
	CBZW  R23, raw1_scalar_vloop
	B     raw1_scalar_verify_fail

raw1_scalar_vtail:
	CMP   $1, R19
	BLT   raw1_found

	VLD1  (R21), [V10.B16]
	VLD1  (R22), [V11.B16]
	WORD  $0x3cf37b0d               // LDR Q13, [R24, R19, LSL #4]

	// XOR-based case-insensitive compare
	VEOR  V10.B16, V11.B16, V12.B16   // V12 = diff
	VCMEQ V4.B16, V12.B16, V14.B16    // V14 = (diff == 0x20) ? 0xFF : 0
	VORR  V4.B16, V10.B16, V16.B16    // V16 = haystack | 0x20
	VADD  V2.B16, V16.B16, V16.B16    // V16 = (haystack | 0x20) + 159
	WORD  $0x6e303470                 // VCMHI V16.B16, V3.B16, V16.B16 (is_letter)
	VAND  V14.B16, V16.B16, V16.B16   // V16 = (diff==0x20) AND is_letter
	VAND  V4.B16, V16.B16, V16.B16    // V16 = tolerable diff mask
	VEOR  V16.B16, V12.B16, V10.B16   // V10 = diff XOR tolerable = real mismatch
	VAND  V13.B16, V10.B16, V10.B16   // Mask out bytes beyond needle
	WORD  $0x6e30a94a                 // VUMAXV V10.B16, V10
	FMOVS F10, R23
	CBNZW R23, raw1_scalar_verify_fail
	B     raw1_found

raw1_scalar_verify_fail:
	ADD   $1, R25, R25
	SUB   R11, R10, R17
	LSR   $3, R17, R17
	ADD   $32, R17, R17
	CMP   R17, R25
	BGT   raw1_exceeded
	B     raw1_scalar_next

raw1_verify_scalar_nl:
	MOVD  R3, R19
	MOVD  R8, R21
	MOVD  R2, R22

raw1_scalar_nl_vloop:
	SUBS  $16, R19, R23
	BLT   raw1_scalar_nl_vtail

	VLD1.P 16(R21), [V10.B16]
	VLD1.P 16(R22), [V11.B16]
	MOVD   R23, R19

	// XOR-based case-insensitive compare
	VEOR  V10.B16, V11.B16, V12.B16   // V12 = diff
	VCMEQ V4.B16, V12.B16, V14.B16    // V14 = (diff == 0x20) ? 0xFF : 0
	VORR  V4.B16, V10.B16, V16.B16    // V16 = haystack | 0x20
	VADD  V2.B16, V16.B16, V16.B16    // V16 = (haystack | 0x20) + 159
	WORD  $0x6e303470                 // VCMHI V16.B16, V3.B16, V16.B16 (is_letter)
	VAND  V14.B16, V16.B16, V16.B16   // V16 = (diff==0x20) AND is_letter
	VAND  V4.B16, V16.B16, V16.B16    // V16 = tolerable diff mask
	VEOR  V16.B16, V12.B16, V10.B16   // V10 = diff XOR tolerable = real mismatch
	WORD  $0x6e30a94a                 // VUMAXV V10.B16, V10
	FMOVS F10, R23
	CBZW  R23, raw1_scalar_nl_vloop
	B     raw1_scalar_nl_verify_fail

raw1_scalar_nl_vtail:
	CMP   $1, R19
	BLT   raw1_found

	VLD1  (R21), [V10.B16]
	VLD1  (R22), [V11.B16]
	WORD  $0x3cf37b0d               // LDR Q13, [R24, R19, LSL #4]

	// XOR-based case-insensitive compare
	VEOR  V10.B16, V11.B16, V12.B16   // V12 = diff
	VCMEQ V4.B16, V12.B16, V14.B16    // V14 = (diff == 0x20) ? 0xFF : 0
	VORR  V4.B16, V10.B16, V16.B16    // V16 = haystack | 0x20
	VADD  V2.B16, V16.B16, V16.B16    // V16 = (haystack | 0x20) + 159
	WORD  $0x6e303470                 // VCMHI V16.B16, V3.B16, V16.B16 (is_letter)
	VAND  V14.B16, V16.B16, V16.B16   // V16 = (diff==0x20) AND is_letter
	VAND  V4.B16, V16.B16, V16.B16    // V16 = tolerable diff mask
	VEOR  V16.B16, V12.B16, V10.B16   // V10 = diff XOR tolerable = real mismatch
	VAND  V13.B16, V10.B16, V10.B16   // Mask out bytes beyond needle
	WORD  $0x6e30a94a                 // VUMAXV V10.B16, V10
	FMOVS F10, R23
	CBNZW R23, raw1_scalar_nl_verify_fail
	B     raw1_found

raw1_scalar_nl_verify_fail:
	ADD   $1, R25, R25
	SUB   R11, R10, R17
	LSR   $3, R17, R17
	ADD   $32, R17, R17
	CMP   R17, R25
	BGT   raw1_exceeded
	B     raw1_scalar_nl_next

// ============================================================================
// SIMD VERIFICATION (for 16/32/128-byte loop paths)
// XOR-based case-insensitive compare
// Uses V2=159, V3=26, V4=32 for letter detection
// ============================================================================
raw1_verify:
	MOVD  R3, R19                 // R19 = remaining needle length
	MOVD  R8, R21                 // R21 = haystack candidate ptr
	MOVD  R2, R22                 // R22 = needle ptr

raw1_vloop:
	SUBS  $16, R19, R23
	BLT   raw1_vtail

	VLD1.P 16(R21), [V10.B16]     // Load haystack
	VLD1.P 16(R22), [V11.B16]     // Load needle (raw)
	MOVD   R23, R19

	// XOR-based case-insensitive compare (same pattern as fold1_verify)
	// V2=159, V3=26, V4=32
	VEOR  V10.B16, V11.B16, V12.B16   // V12 = diff
	VCMEQ V4.B16, V12.B16, V14.B16    // V14 = (diff == 0x20) ? 0xFF : 0
	VORR  V4.B16, V10.B16, V16.B16    // V16 = haystack | 0x20
	VADD  V2.B16, V16.B16, V16.B16    // V16 = (haystack | 0x20) + 159
	WORD  $0x6e303470                 // VCMHI V16.B16, V3.B16, V16.B16 (is_letter)
	VAND  V14.B16, V16.B16, V16.B16   // V16 = (diff==0x20) AND is_letter
	VAND  V4.B16, V16.B16, V16.B16    // V16 = tolerable diff mask
	VEOR  V16.B16, V12.B16, V10.B16   // V10 = diff XOR tolerable = real mismatch
	WORD  $0x6e30a94a                 // VUMAXV V10.B16, V10
	FMOVS F10, R23
	CBZW  R23, raw1_vloop
	B     raw1_verify_fail

raw1_vtail:
	CMP   $1, R19
	BLT   raw1_found

	VLD1  (R21), [V10.B16]
	VLD1  (R22), [V11.B16]
	WORD  $0x3cf37b0d               // LDR Q13, [R24, R19, LSL #4]

	// XOR-based case-insensitive compare
	VEOR  V10.B16, V11.B16, V12.B16   // V12 = diff
	VCMEQ V4.B16, V12.B16, V14.B16    // V14 = (diff == 0x20) ? 0xFF : 0
	VORR  V4.B16, V10.B16, V16.B16    // V16 = haystack | 0x20
	VADD  V2.B16, V16.B16, V16.B16    // V16 = (haystack | 0x20) + 159
	WORD  $0x6e303470                 // VCMHI V16.B16, V3.B16, V16.B16 (is_letter)
	VAND  V14.B16, V16.B16, V16.B16   // V16 = (diff==0x20) AND is_letter
	VAND  V4.B16, V16.B16, V16.B16    // V16 = tolerable diff mask
	VEOR  V16.B16, V12.B16, V10.B16   // V10 = diff XOR tolerable = real mismatch
	VAND  V13.B16, V10.B16, V10.B16   // Mask out bytes beyond needle
	WORD  $0x6e30a94a                 // VUMAXV V10.B16, V10
	FMOVS F10, R23
	CBNZW R23, raw1_verify_fail
	B     raw1_found

raw1_verify_fail:
	ADD   $1, R25, R25
	SUB   R11, R10, R17
	LSR   $3, R17, R17
	ADD   $32, R17, R17
	CMP   R17, R25
	BGT   raw1_exceeded

	// Clear bit and continue - use R14 to determine which loop
	CMP   $0x100, R14
	BEQ   raw1_clear16_from_verify
	CMP   $64, R14
	BLT   raw1_clear128_from_verify
	B     raw1_clear32_from_verify

raw1_clear16_from_verify:
	LSL   $1, R15, R17             // bitpos = byteIndex << 1
	MOVD  $1, R19
	LSL   R17, R19, R17            // 1 << bitpos
	BIC   R17, R13, R13            // Clear just that bit
	CBZ   R26, raw1_clear16_nl_from_verify
	CBNZ  R13, raw1_try16
	B     raw1_check16_continue

raw1_clear16_nl_from_verify:
	CBNZ  R13, raw1_try16_nl
	B     raw1_check16_nl_continue

raw1_clear128_from_verify:
	SUB   R14, R15, R17            // R17 = byteIndex - chunkOffset
	LSL   $1, R17, R17             // bitpos = localByteIndex << 1
	MOVD  $1, R19
	LSL   R17, R19, R17            // 1 << bitpos
	BIC   R17, R13, R13            // Clear just that bit
	CBZ   R26, raw1_try128_nl_retry
	CBNZ  R13, raw1_try128
	ADD   $16, R14, R14
	CMP   $64, R14
	BLT   raw1_next_chunk
	CBNZ  R20, raw1_check_second64_after
	B     raw1_continue128

raw1_try128_nl_retry:
	CBNZ  R13, raw1_try128_nl
	ADD   $16, R14, R14
	CMP   $64, R14
	BLT   raw1_next_chunk_nl
	CBNZ  R20, raw1_check_second64_nl_after
	B     raw1_continue128_nl

raw1_clear32_from_verify:
	AND   $0x7F, R14, R17          // R17 = chunk offset
	SUB   R17, R15, R20            // R20 = byteIndex - chunkOffset
	LSL   $1, R20, R20             // bitpos = localByteIndex << 1
	MOVD  $1, R19
	LSL   R20, R19, R20            // 1 << bitpos
	BIC   R20, R13, R13            // Clear just that bit
	CBZ   R26, raw1_try32_nl_retry
	CBNZ  R13, raw1_try32
	CMP   $128, R14
	BNE   raw1_continue32
	VADDP V21.B16, V21.B16, V6.B16
	VADDP V6.B16, V6.B16, V6.B16
	VMOV  V6.S[0], R13
	MOVD  $144, R14
	CBNZ  R13, raw1_try32
	B     raw1_continue32

raw1_try32_nl_retry:
	CBNZ  R13, raw1_try32_nl
	CMP   $128, R14
	BNE   raw1_continue32_nl
	VADDP V21.B16, V21.B16, V6.B16
	VADDP V6.B16, V6.B16, V6.B16
	VMOV  V6.S[0], R13
	MOVD  $144, R14
	CBNZ  R13, raw1_try32_nl
	B     raw1_continue32_nl

// ============================================================================
// EXIT PATHS
// ============================================================================
raw1_exceeded:
	MOVD  $0x8000000000000000, R17   // exceeded flag (bit 63)
	ADD   R16, R17, R0              // add position
	MOVD  R0, ret+40(FP)
	RET

raw1_not_found:
	MOVD  $-1, R0
	MOVD  R0, ret+40(FP)
	RET

raw1_found_zero:
	MOVD  ZR, R0
	MOVD  R0, ret+40(FP)
	RET

raw1_found:
	MOVD  R16, R0
	MOVD  R0, ret+40(FP)
	RET

