//go:build !noasm && arm64

// Hand-optimized ARM64 NEON assembly for substring search kernels

#include "textflag.h"

// Magic constant for syndrome extraction (2 bits per byte)
#define SYNDROME_MAGIC $0x40100401

TEXT ·indexExact2Byte(SB), NOSPLIT, $0-56
	MOVD  haystack+0(FP), R0
	MOVD  haystack_len+8(FP), R1
	MOVD  needle+16(FP), R2
	MOVD  needle_len+24(FP), R3
	MOVD  off1+32(FP), R4
	MOVD  off2_delta+40(FP), R5

	SUBS  R3, R1, R9
	BLT   not_found_exact2
	CBZ   R3, found_zero_exact2

	// Load target bytes
	ADD   R4, R2, R6
	MOVBU (R6), R7              // R7 = rare1
	MOVBU (R6)(R5), R8          // R8 = rare2

	ADD   $1, R9, R10           // remaining
	ADD   R4, R0, R11           // searchPtr (ptr1)
	ADD   R5, R11, R14          // ptr2 = ptr1 + off2_delta
	MOVD  R11, R12              // searchStart
	MOVD  ZR, R13               // failures

	VMOV  R7, V0.B16
	VMOV  R8, V1.B16

	MOVD  SYNDROME_MAGIC, R23
	VMOV  R23, V5.S4

	// Tail mask table base
	MOVD  $tail_mask_table(SB), R24

	CMP   $2048, R10
	BGE   loop128_exact2_entry
	CMP   $64, R10
	BLT   loop32_exact2_entry
	B     loop64_exact2

// ============================================================================
// 128-BYTE LOOP: Process 128 bytes per iteration for large inputs (≥512B)
// ============================================================================
loop128_exact2_entry:
	CMP   $128, R10
	BLT   loop64_exact2

loop128_exact2:
	// Load 128 bytes from both pointers (8 vectors each, 16 total)
	// First pointer (rare1)
	VLD1.P 64(R11), [V16.B16, V17.B16, V18.B16, V19.B16]
	VLD1.P 64(R11), [V20.B16, V21.B16, V22.B16, V23.B16]
	// Second pointer (rare2) - store in stack temporarily, reuse vector slots
	VLD1.P 64(R14), [V24.B16, V25.B16, V26.B16, V27.B16]
	VLD1.P 64(R14), [V28.B16, V29.B16, V30.B16, V31.B16]
	SUB    $128, R10, R10

	// Compare and AND in place for first 64 bytes
	VCMEQ  V0.B16, V16.B16, V2.B16    // rare1 cmp chunks 0-3
	VCMEQ  V0.B16, V17.B16, V3.B16
	VCMEQ  V0.B16, V18.B16, V4.B16
	VCMEQ  V0.B16, V19.B16, V6.B16
	VCMEQ  V1.B16, V24.B16, V7.B16    // rare2 cmp chunks 0-3
	VCMEQ  V1.B16, V25.B16, V8.B16
	VCMEQ  V1.B16, V26.B16, V9.B16
	VCMEQ  V1.B16, V27.B16, V10.B16
	VAND   V2.B16, V7.B16, V16.B16    // AND results -> V16-V19
	VAND   V3.B16, V8.B16, V17.B16
	VAND   V4.B16, V9.B16, V18.B16
	VAND   V6.B16, V10.B16, V19.B16

	// Compare and AND for second 64 bytes
	VCMEQ  V0.B16, V20.B16, V2.B16    // rare1 cmp chunks 4-7
	VCMEQ  V0.B16, V21.B16, V3.B16
	VCMEQ  V0.B16, V22.B16, V4.B16
	VCMEQ  V0.B16, V23.B16, V6.B16
	VCMEQ  V1.B16, V28.B16, V7.B16    // rare2 cmp chunks 4-7
	VCMEQ  V1.B16, V29.B16, V8.B16
	VCMEQ  V1.B16, V30.B16, V9.B16
	VCMEQ  V1.B16, V31.B16, V10.B16
	VAND   V2.B16, V7.B16, V24.B16    // AND results -> V24-V27
	VAND   V3.B16, V8.B16, V25.B16
	VAND   V4.B16, V9.B16, V26.B16
	VAND   V6.B16, V10.B16, V27.B16

	// Combine all 8 chunks for quick check - keep V6 (first64) and V8 (second64)
	VORR   V16.B16, V17.B16, V6.B16
	VORR   V18.B16, V19.B16, V7.B16
	VORR   V24.B16, V25.B16, V8.B16
	VORR   V26.B16, V27.B16, V9.B16
	VORR   V6.B16, V7.B16, V6.B16    // V6 = OR(first64)
	VORR   V8.B16, V9.B16, V8.B16    // V8 = OR(second64)
	VORR   V6.B16, V8.B16, V10.B16   // V10 = OR(all) - preserve V6 and V8

	VADDP  V10.D2, V10.D2, V10.D2
	VMOV   V10.D[0], R15

	CMP    $128, R10
	BLT    end128_exact2
	CBZ    R15, loop128_exact2

end128_exact2:
	CBZ    R15, loop64_exact2

	// Check first 64 bytes - V6 already has OR(first64), just reduce
	VADDP  V6.D2, V6.D2, V6.D2
	VMOV   V6.D[0], R15
	CBNZ   R15, first64_exact2

	// Check second 64 bytes - V8 already has OR(second64), just reduce
	VADDP  V8.D2, V8.D2, V8.D2
	VMOV   V8.D[0], R15
	CBNZ   R15, second64_exact2
	CMP    $128, R10
	BGE    loop128_exact2
	B      loop64_exact2

first64_exact2:
	// Extract syndromes for chunks 0-3 (V16-V19)
	VAND   V5.B16, V16.B16, V16.B16
	VAND   V5.B16, V17.B16, V17.B16
	VAND   V5.B16, V18.B16, V18.B16
	VAND   V5.B16, V19.B16, V19.B16
	MOVD   $64, R20                  // block offset = 64 (first block)
	B      check_chunks128_exact2

second64_exact2:
	// Move second half results (V24-V27) to V16-V19
	VMOV   V24.B16, V16.B16
	VMOV   V25.B16, V17.B16
	VMOV   V26.B16, V18.B16
	VMOV   V27.B16, V19.B16
	VAND   V5.B16, V16.B16, V16.B16
	VAND   V5.B16, V17.B16, V17.B16
	VAND   V5.B16, V18.B16, V18.B16
	VAND   V5.B16, V19.B16, V19.B16
	MOVD   ZR, R20                   // block offset = 0 (second block)

check_chunks128_exact2:
	// Check chunk 0
	VADDP  V16.B16, V16.B16, V6.B16
	VADDP  V6.B16, V6.B16, V6.B16
	VMOV   V6.S[0], R15
	MOVD   ZR, R16
	CBNZ   R15, try128_exact2

	// Check chunk 1
	VADDP  V17.B16, V17.B16, V6.B16
	VADDP  V6.B16, V6.B16, V6.B16
	VMOV   V6.S[0], R15
	MOVD   $16, R16
	CBNZ   R15, try128_exact2

	// Check chunk 2
	VADDP  V18.B16, V18.B16, V6.B16
	VADDP  V6.B16, V6.B16, V6.B16
	VMOV   V6.S[0], R15
	MOVD   $32, R16
	CBNZ   R15, try128_exact2

	// Check chunk 3
	VADDP  V19.B16, V19.B16, V6.B16
	VADDP  V6.B16, V6.B16, V6.B16
	VMOV   V6.S[0], R15
	MOVD   $48, R16
	CBNZ   R15, try128_exact2

	// All chunks exhausted, check second 64B or continue
	CBNZ   R20, check_second64_after128_exact2
	CMP    $128, R10
	BGE    loop128_exact2
	B      loop64_exact2

check_second64_after128_exact2:
	VORR   V24.B16, V25.B16, V6.B16
	VORR   V26.B16, V27.B16, V7.B16
	VORR   V6.B16, V7.B16, V6.B16
	VADDP  V6.D2, V6.D2, V6.D2
	VMOV   V6.D[0], R15
	CBZ    R15, continue128_exact2
	B      second64_exact2

continue128_exact2:
	CMP    $128, R10
	BGE    loop128_exact2
	B      loop64_exact2

try128_exact2:
	RBIT   R15, R17
	CLZ    R17, R17
	LSR    $1, R17, R17
	ADD    R16, R17, R17             // add chunk offset
	SUB    R20, R17, R17             // adjust for block
	ADD    $64, R17, R17             // add 64 back

	SUB    $128, R11, R19
	ADD    R17, R19, R19
	SUB    R12, R19, R19

	CMP    R9, R19
	BGT    clear128_exact2

	// Verify match - use R21-R25, R6 only (preserve R7=rare1, R8=rare2)
	ADD    R0, R19, R21              // R21 = haystack + position
	MOVD   R2, R22                   // R22 = needle
	MOVD   R3, R25                   // R25 = needle_len

verify128_exact2:
	CMP    $16, R25
	BLT    verify_tail128_exact2
	VLD1.P 16(R21), [V10.B16]
	VLD1.P 16(R22), [V11.B16]
	VEOR   V10.B16, V11.B16, V12.B16
	WORD   $0x6e30a98c               // UMAXV B12, V12.B16
	FMOVS  F12, R6
	CBNZW  R6, fail128_exact2
	SUBS   $16, R25, R25
	BGT    verify128_exact2
	MOVD   R19, R0
	MOVD   R0, ret+48(FP)
	RET

verify_tail128_exact2:
	CMP    $0, R25
	BLE    found128_exact2
	VLD1   (R21), [V10.B16]
	VLD1   (R22), [V11.B16]
	VEOR   V10.B16, V11.B16, V12.B16
	WORD   $0x3cf97b0d               // LDR Q13, [R24, R25, LSL #4]
	VAND   V13.B16, V12.B16, V12.B16
	WORD   $0x6e30a98c
	FMOVS  F12, R6
	CBNZW  R6, fail128_exact2

found128_exact2:
	MOVD   R19, R0
	MOVD   R0, ret+48(FP)
	RET

fail128_exact2:
	SUB    R12, R11, R21
	LSR    $3, R21, R21
	ADD    $32, R21
	ADD    $1, R13
	CMP    R21, R13
	BGE    exceeded128_exact2

clear128_exact2:
	SUB    R16, R17, R21             // byte index within chunk
	SUB    $64, R21
	ADD    R20, R21, R21
	LSL    $1, R21, R21              // bitpos = byteIndex << 1
	MOVD   $1, R22
	LSL    R21, R22, R21             // 1 << bitpos
	BIC    R21, R15, R15             // Clear just that bit
	CBNZ   R15, try128_exact2

	// Advance to next chunk
	ADD    $16, R16, R16
	CMP    $64, R16
	BLT    next_chunk128_exact2
	CBNZ   R20, check_second64_after128_exact2
	B      continue128_exact2

next_chunk128_exact2:
	CMP    $16, R16
	BEQ    chunk128_1_exact2
	CMP    $32, R16
	BEQ    chunk128_2_exact2
	CMP    $48, R16
	BEQ    chunk128_3_exact2
	B      continue128_exact2

chunk128_1_exact2:
	VADDP  V17.B16, V17.B16, V6.B16
	VADDP  V6.B16, V6.B16, V6.B16
	VMOV   V6.S[0], R15
	CBNZ   R15, try128_exact2
	ADD    $16, R16, R16
chunk128_2_exact2:
	VADDP  V18.B16, V18.B16, V6.B16
	VADDP  V6.B16, V6.B16, V6.B16
	VMOV   V6.S[0], R15
	CBNZ   R15, try128_exact2
	ADD    $16, R16, R16
chunk128_3_exact2:
	VADDP  V19.B16, V19.B16, V6.B16
	VADDP  V6.B16, V6.B16, V6.B16
	VMOV   V6.S[0], R15
	CBNZ   R15, try128_exact2
	CBNZ   R20, check_second64_after128_exact2
	B      continue128_exact2

exceeded128_exact2:
	MOVD   $0x8000000000000000, R21   // exceeded flag (bit 63)
	ADD    R19, R21, R0
	MOVD   R0, ret+48(FP)
	RET

loop64_exact2:
	// Load 64 bytes from both pointers with post-increment
	VLD1.P 64(R11), [V16.B16, V17.B16, V18.B16, V19.B16]
	VLD1.P 64(R14), [V20.B16, V21.B16, V22.B16, V23.B16]

	SUBS  $64, R10, R10

	// Compare
	VCMEQ V0.B16, V16.B16, V24.B16
	VCMEQ V0.B16, V17.B16, V25.B16
	VCMEQ V0.B16, V18.B16, V26.B16
	VCMEQ V0.B16, V19.B16, V27.B16

	VCMEQ V1.B16, V20.B16, V28.B16
	VCMEQ V1.B16, V21.B16, V29.B16
	VCMEQ V1.B16, V22.B16, V30.B16
	VCMEQ V1.B16, V23.B16, V31.B16

	// AND both
	VAND  V24.B16, V28.B16, V24.B16
	VAND  V25.B16, V29.B16, V25.B16
	VAND  V26.B16, V30.B16, V26.B16
	VAND  V27.B16, V31.B16, V27.B16

	// Quick check
	VORR  V24.B16, V25.B16, V6.B16
	VORR  V26.B16, V27.B16, V7.B16
	VORR  V6.B16, V7.B16, V6.B16
	VADDP V6.D2, V6.D2, V6.D2
	VMOV  V6.D[0], R15

	BLS   end64_exact2
	CBZ   R15, loop64_exact2

end64_exact2:
	CBZ   R15, loop16_exact2

	// Process chunks 0-3
	MOVD  $0, R16

process_chunk64_exact2:
	CMP   $0, R16
	BEQ   chunk0_exact2
	CMP   $16, R16
	BEQ   chunk1_exact2
	CMP   $32, R16
	BEQ   chunk2_exact2
	B     chunk3_exact2

chunk0_exact2:
	VMOV  V24.B16, V6.B16
	B     extract64_exact2
chunk1_exact2:
	VMOV  V25.B16, V6.B16
	B     extract64_exact2
chunk2_exact2:
	VMOV  V26.B16, V6.B16
	B     extract64_exact2
chunk3_exact2:
	VMOV  V27.B16, V6.B16

extract64_exact2:
	VAND  V5.B16, V6.B16, V6.B16
	VADDP V6.B16, V6.B16, V6.B16
	VADDP V6.B16, V6.B16, V6.B16
	VMOV  V6.S[0], R15
	CBZ   R15, next_chunk64_exact2

process_match64_exact2:
	RBIT  R15, R17
	CLZ   R17, R17
	LSR   $1, R17, R17

	SUB   $64, R11, R19
	ADD   R16, R19, R19
	ADD   R17, R19, R19
	SUB   R12, R19, R19

	CMP   R9, R19
	BGT   clear64_exact2

	// Verify - use R20-R22, R6 only (not R8 which holds rare2!)
	ADD   R0, R19, R20        // R20 = haystack + position
	MOVD  R2, R21             // R21 = needle
	MOVD  R3, R22             // R22 = needle_len

verify64_exact2:
	CMP   $16, R22
	BLT   verify_tail64_exact2
	VLD1.P 16(R20), [V10.B16]
	VLD1.P 16(R21), [V11.B16]
	VEOR  V10.B16, V11.B16, V12.B16
	WORD  $0x6e30a98c
	FMOVS F12, R6
	CBNZW R6, fail64_exact2
	SUBS  $16, R22, R22
	BGT   verify64_exact2
	MOVD  R19, R0
	MOVD  R0, ret+48(FP)
	RET

verify_tail64_exact2:
	CMP   $0, R22
	BLE   found64_exact2
	VLD1  (R20), [V10.B16]
	VLD1  (R21), [V11.B16]
	VEOR  V10.B16, V11.B16, V12.B16
	WORD  $0x3cf67b0d               // LDR Q13, [R24, R22, LSL #4]
	VAND  V13.B16, V12.B16, V12.B16
	WORD  $0x6e30a98c
	FMOVS F12, R6
	CBNZW R6, fail64_exact2

found64_exact2:
	MOVD  R19, R0
	MOVD  R0, ret+48(FP)
	RET

fail64_exact2:
	SUB   R12, R11, R20
	LSR   $3, R20, R20
	ADD   $32, R20
	ADD   $1, R13
	CMP   R20, R13
	BGE   exceeded64_exact2

clear64_exact2:
	LSL   $1, R17, R20             // bitpos = byteIndex << 1
	MOVD  $1, R21
	LSL   R20, R21, R20            // 1 << bitpos
	BIC   R20, R15, R15            // Clear just that bit
	CBNZ  R15, process_match64_exact2

next_chunk64_exact2:
	ADD   $16, R16
	CMP   $64, R16
	BLT   process_chunk64_exact2
	CMP   $64, R10
	BGE   loop64_exact2

// ============================================================================
// FALLBACK 32-BYTE LOOP: For remainders 32-63 bytes
// ============================================================================
loop32_exact2_entry:
	CMP   $32, R10
	BLT   loop16_exact2

loop32_exact2:
	VLD1.P 32(R11), [V16.B16, V17.B16]
	VLD1.P 32(R14), [V18.B16, V19.B16]
	SUBS   $32, R10, R10

	VCMEQ  V0.B16, V16.B16, V20.B16
	VCMEQ  V0.B16, V17.B16, V21.B16
	VCMEQ  V1.B16, V18.B16, V22.B16
	VCMEQ  V1.B16, V19.B16, V23.B16
	VAND   V20.B16, V22.B16, V20.B16
	VAND   V21.B16, V23.B16, V21.B16
	VORR   V20.B16, V21.B16, V6.B16
	VADDP  V6.D2, V6.D2, V6.D2
	VMOV   V6.D[0], R15
	BLS    end32_exact2
	CBNZ   R15, end32_match_exact2
	CMP    $32, R10
	BGE    loop32_exact2
	B      loop16_exact2

end32_exact2:
	CBZ   R15, loop16_exact2

end32_match_exact2:
	VAND  V5.B16, V20.B16, V20.B16
	VAND  V5.B16, V21.B16, V21.B16
	VADDP V21.B16, V20.B16, V6.B16
	VADDP V6.B16, V6.B16, V6.B16
	VMOV  V6.D[0], R15

process_syndrome32_exact2:
	RBIT  R15, R17
	CLZ   R17, R17
	LSR   $1, R17, R17

	SUB   $32, R11, R19
	ADD   R17, R19, R19
	SUB   R12, R19, R19

	CMP   R9, R19
	BGT   clear_bit32_exact2

	ADD   R0, R19, R20
	MOVD  R2, R21
	MOVD  R3, R22

verify32_exact2:
	CMP   $16, R22
	BLT   verify_tail32_exact2
	VLD1.P 16(R20), [V10.B16]
	VLD1.P 16(R21), [V11.B16]
	VEOR  V10.B16, V11.B16, V12.B16
	WORD  $0x6e30a98c
	FMOVS F12, R6
	CBNZW R6, fail32_exact2
	SUBS  $16, R22, R22
	BGT   verify32_exact2
	MOVD  R19, R0
	MOVD  R0, ret+48(FP)
	RET

verify_tail32_exact2:
	CMP   $0, R22
	BLE   found32_exact2
	VLD1  (R20), [V10.B16]
	VLD1  (R21), [V11.B16]
	VEOR  V10.B16, V11.B16, V12.B16
	WORD  $0x3cf67b0d               // LDR Q13, [R24, R22, LSL #4]
	VAND  V13.B16, V12.B16, V12.B16
	WORD  $0x6e30a98c
	FMOVS F12, R6
	CBNZW R6, fail32_exact2

found32_exact2:
	MOVD  R19, R0
	MOVD  R0, ret+48(FP)
	RET

fail32_exact2:
	SUB   R12, R11, R20
	LSR   $3, R20, R20
	ADD   $32, R20
	ADD   $1, R13
	CMP   R20, R13
	BGE   exceeded32_exact2

clear_bit32_exact2:
	LSL   $1, R17, R20
	MOVD  $1, R21
	LSL   R20, R21, R20
	BIC   R20, R15, R15
	CBNZ  R15, process_syndrome32_exact2

loop16_exact2:
	CMP   $16, R10
	BLT   scalar_exact2

loop16_inner_exact2:
	VLD1.P 16(R11), [V16.B16]
	VLD1.P 16(R14), [V17.B16]
	SUBS  $16, R10, R10

	VCMEQ V0.B16, V16.B16, V19.B16
	VCMEQ V1.B16, V17.B16, V20.B16
	VAND  V19.B16, V20.B16, V19.B16

	VAND  V5.B16, V19.B16, V19.B16
	VADDP V19.B16, V19.B16, V19.B16
	VADDP V19.B16, V19.B16, V19.B16
	VMOV  V19.S[0], R15

	CBNZ  R15, end16_exact2           // match found
	CMP   $16, R10                    // no match - check if we can continue
	BGE   loop16_inner_exact2
	B     scalar_exact2               // not enough bytes for another iteration

end16_exact2:

process16_exact2:
	RBIT  R15, R17
	CLZ   R17, R17
	LSR   $1, R17, R17

	SUB   $16, R11, R19
	ADD   R17, R19, R19
	SUB   R12, R19, R19

	CMP   R9, R19
	BGT   clear16_exact2

	// Verify - use R20-R22, R6 only (not R8 which holds rare2!)
	ADD   R0, R19, R20        // R20 = haystack + position
	MOVD  R2, R21             // R21 = needle
	MOVD  R3, R22             // R22 = needle_len

verify16_exact2:
	CMP   $16, R22
	BLT   verify_tail16_exact2
	VLD1.P 16(R20), [V10.B16]
	VLD1.P 16(R21), [V11.B16]
	VEOR  V10.B16, V11.B16, V12.B16
	WORD  $0x6e30a98c
	FMOVS F12, R6
	CBNZW R6, fail16_exact2
	SUBS  $16, R22, R22
	BGT   verify16_exact2
	MOVD  R19, R0
	MOVD  R0, ret+48(FP)
	RET

verify_tail16_exact2:
	CMP   $0, R22
	BLE   found16_exact2
	VLD1  (R20), [V10.B16]
	VLD1  (R21), [V11.B16]
	VEOR  V10.B16, V11.B16, V12.B16
	WORD  $0x3cf67b0d               // LDR Q13, [R24, R22, LSL #4]
	VAND  V13.B16, V12.B16, V12.B16
	WORD  $0x6e30a98c
	FMOVS F12, R6
	CBNZW R6, fail16_exact2

found16_exact2:
	MOVD  R19, R0
	MOVD  R0, ret+48(FP)
	RET

fail16_exact2:
	SUB   R12, R11, R20
	LSR   $3, R20, R20
	ADD   $32, R20
	ADD   $1, R13
	CMP   R20, R13
	BGE   exceeded16_exact2

clear16_exact2:
	LSL   $1, R17, R20             // bitpos = byteIndex << 1
	MOVD  $1, R21
	LSL   R20, R21, R20            // 1 << bitpos
	BIC   R20, R15, R15            // Clear just that bit
	CBNZ  R15, process16_exact2
	CMP   $16, R10
	BGE   loop16_inner_exact2

scalar_exact2:
	CMP   $0, R10
	BLE   not_found_exact2

scalar_loop_exact2:
	MOVBU (R11), R15
	MOVBU (R14), R16

	CMP   R7, R15
	BNE   scalar_next_exact2
	CMP   R8, R16
	BNE   scalar_next_exact2

	SUB   R12, R11, R19
	CMP   R9, R19
	BGT   scalar_next_exact2

	// Verify with NEON - use R20-R22, R6 only (not R8 which holds rare2!)
	ADD   R0, R19, R20        // R20 = haystack + position
	MOVD  R2, R21             // R21 = needle
	MOVD  R3, R22             // R22 = needle_len

scalar_verify_exact2:
	CMP   $16, R22
	BLT   scalar_verify_tail_exact2
	VLD1.P 16(R20), [V10.B16]
	VLD1.P 16(R21), [V11.B16]
	VEOR  V10.B16, V11.B16, V12.B16
	WORD  $0x6e30a98c              // UMAXV B12, V12.B16
	FMOVS F12, R6
	CBNZW R6, scalar_fail_exact2
	SUBS  $16, R22, R22
	BGT   scalar_verify_exact2
	MOVD  R19, R0
	MOVD  R0, ret+48(FP)
	RET

scalar_verify_tail_exact2:
	CMP   $0, R22
	BLE   scalar_found_exact2
	VLD1  (R20), [V10.B16]
	VLD1  (R21), [V11.B16]
	VEOR  V10.B16, V11.B16, V12.B16
	WORD  $0x3cf67b0d               // LDR Q13, [R24, R22, LSL #4]
	VAND  V13.B16, V12.B16, V12.B16
	WORD  $0x6e30a98c
	FMOVS F12, R6
	CBNZW R6, scalar_fail_exact2

scalar_found_exact2:
	MOVD  R19, R0
	MOVD  R0, ret+48(FP)
	RET

scalar_fail_exact2:
	SUB   R12, R11, R20
	LSR   $3, R20, R20
	ADD   $32, R20
	ADD   $1, R13
	CMP   R20, R13
	BGE   exceeded_scalar_exact2

scalar_next_exact2:
	ADD   $1, R11
	ADD   $1, R14
	SUBS  $1, R10, R10
	BGT   scalar_loop_exact2

not_found_exact2:
	MOVD  $-1, R0
	MOVD  R0, ret+48(FP)
	RET

found_zero_exact2:
	MOVD  ZR, R0
	MOVD  R0, ret+48(FP)
	RET

exceeded64_exact2:
exceeded32_exact2:
exceeded16_exact2:
exceeded_scalar_exact2:
	MOVD  $0x8000000000000000, R20   // exceeded flag (bit 63)
	ADD   R19, R20, R0              // add position
	MOVD  R0, ret+48(FP)
	RET

// ============================================================================
// HAND-ROLLED FOLD FUNCTIONS - 128-byte loops for optimal throughput
// ============================================================================

// func indexFold1Byte(haystack string, needle string, off1 int) uint64
//
// Returns:
//   - Position if found
//   - 0xFFFFFFFFFFFFFFFF (-1) if not found
//   - 0x8000000000000000 + position if exceeded threshold (resume from position)
//
// Register allocation (matches ascii_fold_neon.s pattern):
//   R0  = haystack ptr (input)
//   R1  = haystack len (input)
//   R2  = needle ptr (input)
//   R3  = needle len (input)
//   R4  = off1 (input)
//   R9  = searchLen = haystack_len - needle_len
//   R10 = searchPtr (post-incremented by VLD1.P)
//   R11 = searchStart (for candidate offset calc)
//   R12 = remaining bytes to search
//   R13 = syndrome result / temp
//   R14 = chunk offset within 64-byte block
//   R15 = bit position temp
//   R16 = candidate position temp
//   R17 = temp for bit clearing
//   R19 = remaining needle len during verify / temp
//   R20 = block offset (64 = first block, 0 = second block in 128B mode)
//   R21 = haystack candidate ptr during verify
//   R22 = needle ptr during verify
//   R23 = temp during verify / threshold calc
//   R24 = tail_mask_table ptr (callee-saved, set once)
//   R25 = failure count (callee-saved)
//   R26 = scalar mask (0x20 for letters, 0x00 for non-letters)
//   R5 = scalar target (rare byte, lowercase if letter) [kept after setup, off1 remains in R4]
//
// Vector registers:
//   V0  = rare1 mask broadcast (0x20 or 0x00)
//   V1  = rare1 target broadcast
//   V4  = 159 (-97 as unsigned) for letter detection
//   V5  = syndrome magic constant
//   V7  = 26 (letter range check)
//   V8  = 32 (0x20 for case folding)
//   V16-V19 = first 64 bytes loaded
//   V20-V23 = compare results for first 64 bytes
//   V24-V27 = second 64 bytes loaded
//   V28-V31 = compare results for second 64 bytes
//   V6, V9-V12 = reduction temporaries

