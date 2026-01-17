//go:build !noasm && arm64

// Hand-optimized ARM64 NEON assembly for substring search kernels

#include "textflag.h"

// Magic constant for syndrome extraction (2 bits per byte)
#define SYNDROME_MAGIC $0x40100401

TEXT ·indexExact1Byte(SB), NOSPLIT, $0-48
	MOVD  haystack+0(FP), R0      // R0 = haystack ptr
	MOVD  haystack_len+8(FP), R1  // R1 = haystack len
	MOVD  needle+16(FP), R2       // R2 = needle ptr
	MOVD  needle_len+24(FP), R3   // R3 = needle len
	MOVD  off1+32(FP), R4         // R4 = off1

	// Early exits
	SUBS  R3, R1, R9              // R9 = searchLen = haystack_len - needle_len
	BLT   not_found_exact1
	CBZ   R3, found_zero_exact1

	// Load target byte from needle[off1]
	MOVBU (R2)(R4), R5            // R5 = target byte

	// Setup
	ADD   $1, R9, R10             // R10 = remaining = searchLen + 1
	ADD   R4, R0, R11             // R11 = searchPtr = haystack + off1
	MOVD  R11, R12                // R12 = searchStart (for position calc)
	MOVD  ZR, R13                 // R13 = failure count

	// Broadcast target byte
	VMOV  R5, V0.B16

	// Magic constant for syndrome
	MOVD  SYNDROME_MAGIC, R14
	VMOV  R14, V5.S4

	// Tail mask table base (used for indexed loads in verify tails)
	MOVD  $tail_mask_table(SB), R24

	CMP   $2048, R10
	BGE   loop128_exact1_entry
	CMP   $64, R10
	BLT   loop32_exact1_entry
	B     loop64_exact1

// ============================================================================
// 128-BYTE LOOP: Process 128 bytes per iteration for large inputs (≥512B)
// ============================================================================
loop128_exact1_entry:
	CMP   $128, R10
	BLT   loop64_exact1

loop128_exact1:
	// Load 128 bytes (8 vectors)
	VLD1.P 64(R11), [V1.B16, V2.B16, V3.B16, V4.B16]
	VLD1.P 64(R11), [V24.B16, V25.B16, V26.B16, V27.B16]
	SUB    $128, R10, R10

	// Compare all 8 chunks against target byte
	VCMEQ  V0.B16, V1.B16, V16.B16
	VCMEQ  V0.B16, V2.B16, V17.B16
	VCMEQ  V0.B16, V3.B16, V18.B16
	VCMEQ  V0.B16, V4.B16, V19.B16
	VCMEQ  V0.B16, V24.B16, V28.B16
	VCMEQ  V0.B16, V25.B16, V29.B16
	VCMEQ  V0.B16, V26.B16, V30.B16
	VCMEQ  V0.B16, V27.B16, V31.B16

	// Combine all 8 chunks for quick check - keep V6 (first64) and V8 (second64)
	VORR   V16.B16, V17.B16, V6.B16
	VORR   V18.B16, V19.B16, V7.B16
	VORR   V28.B16, V29.B16, V8.B16
	VORR   V30.B16, V31.B16, V9.B16
	VORR   V6.B16, V7.B16, V6.B16    // V6 = OR(first64)
	VORR   V8.B16, V9.B16, V8.B16    // V8 = OR(second64)
	VORR   V6.B16, V8.B16, V9.B16    // V9 = OR(all) - preserve V6 and V8

	VADDP  V9.D2, V9.D2, V9.D2
	VMOV   V9.D[0], R15

	CMP    $128, R10
	BLT    end128_exact1
	CBZ    R15, loop128_exact1

end128_exact1:
	CBZ    R15, loop64_exact1

	// Check first 64 bytes - V6 already has OR(first64), just reduce
	VADDP  V6.D2, V6.D2, V6.D2
	VMOV   V6.D[0], R15
	CBNZ   R15, first64_exact1

	// Check second 64 bytes - V8 already has OR(second64), just reduce
	VADDP  V8.D2, V8.D2, V8.D2
	VMOV   V8.D[0], R15
	CBNZ   R15, second64_exact1
	CMP    $128, R10
	BGE    loop128_exact1
	B      loop64_exact1

first64_exact1:
	// Extract syndromes for chunks 0-3
	VAND   V5.B16, V16.B16, V16.B16
	VAND   V5.B16, V17.B16, V17.B16
	VAND   V5.B16, V18.B16, V18.B16
	VAND   V5.B16, V19.B16, V19.B16
	MOVD   $64, R20                  // block offset = 64 (first block, subtracted from 128)
	B      check_chunks128_exact1

second64_exact1:
	// Move second half results to V16-V19 for unified processing
	VMOV   V28.B16, V16.B16
	VMOV   V29.B16, V17.B16
	VMOV   V30.B16, V18.B16
	VMOV   V31.B16, V19.B16
	VAND   V5.B16, V16.B16, V16.B16
	VAND   V5.B16, V17.B16, V17.B16
	VAND   V5.B16, V18.B16, V18.B16
	VAND   V5.B16, V19.B16, V19.B16
	MOVD   ZR, R20                   // block offset = 0 (second block)

check_chunks128_exact1:
	// Check chunk 0
	VADDP  V16.B16, V16.B16, V6.B16
	VADDP  V6.B16, V6.B16, V6.B16
	VMOV   V6.S[0], R15
	MOVD   ZR, R16
	CBNZ   R15, try128_exact1

	// Check chunk 1
	VADDP  V17.B16, V17.B16, V6.B16
	VADDP  V6.B16, V6.B16, V6.B16
	VMOV   V6.S[0], R15
	MOVD   $16, R16
	CBNZ   R15, try128_exact1

	// Check chunk 2
	VADDP  V18.B16, V18.B16, V6.B16
	VADDP  V6.B16, V6.B16, V6.B16
	VMOV   V6.S[0], R15
	MOVD   $32, R16
	CBNZ   R15, try128_exact1

	// Check chunk 3
	VADDP  V19.B16, V19.B16, V6.B16
	VADDP  V6.B16, V6.B16, V6.B16
	VMOV   V6.S[0], R15
	MOVD   $48, R16
	CBNZ   R15, try128_exact1

	// All chunks exhausted, check second 64B or continue
	CBNZ   R20, check_second64_after128_exact1
	CMP    $128, R10
	BGE    loop128_exact1
	B      loop64_exact1

check_second64_after128_exact1:
	VORR   V28.B16, V29.B16, V6.B16
	VORR   V30.B16, V31.B16, V7.B16
	VORR   V6.B16, V7.B16, V6.B16
	VADDP  V6.D2, V6.D2, V6.D2
	VMOV   V6.D[0], R15
	CBZ    R15, continue128_exact1
	B      second64_exact1

continue128_exact1:
	CMP    $128, R10
	BGE    loop128_exact1
	B      loop64_exact1

try128_exact1:
	RBIT   R15, R17
	CLZ    R17, R17
	LSR    $1, R17, R17
	ADD    R16, R17, R17             // add chunk offset
	SUB    R20, R17, R17             // adjust for block (subtract 64 or 0)
	ADD    $64, R17, R17             // add 64 back

	SUB    $128, R11, R19
	ADD    R17, R19, R19
	SUB    R12, R19, R19

	CMP    R9, R19
	BGT    clear128_exact1

	// Verify match
	ADD    R0, R19, R8
	MOVD   R8, R21
	MOVD   R2, R22
	MOVD   R3, R23

verify128_exact1:
	CMP    $16, R23
	BLT    verify_tail128_exact1
	VLD1.P 16(R21), [V10.B16]
	VLD1.P 16(R22), [V11.B16]
	VEOR   V10.B16, V11.B16, V12.B16
	WORD   $0x6e30a98c               // UMAXV B12, V12.B16
	FMOVS  F12, R6
	CBNZW  R6, fail128_exact1
	SUBS   $16, R23, R23
	BGT    verify128_exact1
	MOVD   R19, R0
	B      found_exact1

verify_tail128_exact1:
	CMP    $0, R23
	BLE    found128_match_exact1
	VLD1   (R21), [V10.B16]
	VLD1   (R22), [V11.B16]
	VEOR   V10.B16, V11.B16, V12.B16
	WORD   $0x3cf77b0d               // LDR Q13, [R24, R23, LSL #4]
	VAND   V13.B16, V12.B16, V12.B16
	WORD   $0x6e30a98c
	FMOVS  F12, R6
	CBNZW  R6, fail128_exact1

found128_match_exact1:
	MOVD   R19, R17
	B      found_exact1

fail128_exact1:
	// Threshold = 32 + (bytes_scanned >> 3)
	SUB    R12, R11, R21
	LSR    $3, R21, R21
	ADD    $32, R21
	ADD    $1, R13
	CMP    R21, R13
	BGE    exceeded128_exact1

clear128_exact1:
	SUB    R16, R17, R21             // byte index within chunk = R17 - R16
	SUB    $64, R21                  // remove the +64 we added
	ADD    R20, R21, R21             // add back block offset
	LSL    $1, R21, R21              // bitpos = byteIndex << 1
	MOVD   $1, R22
	LSL    R21, R22, R21             // 1 << bitpos
	BIC    R21, R15, R15             // Clear just that bit
	CBNZ   R15, try128_exact1

	// Advance to next chunk
	ADD    $16, R16, R16
	CMP    $64, R16
	BLT    next_chunk128_exact1
	CBNZ   R20, check_second64_after128_exact1
	B      continue128_exact1

next_chunk128_exact1:
	CMP    $16, R16
	BEQ    chunk128_1_exact1
	CMP    $32, R16
	BEQ    chunk128_2_exact1
	CMP    $48, R16
	BEQ    chunk128_3_exact1
	B      continue128_exact1

chunk128_1_exact1:
	VADDP  V17.B16, V17.B16, V6.B16
	VADDP  V6.B16, V6.B16, V6.B16
	VMOV   V6.S[0], R15
	CBNZ   R15, try128_exact1
	ADD    $16, R16, R16
chunk128_2_exact1:
	VADDP  V18.B16, V18.B16, V6.B16
	VADDP  V6.B16, V6.B16, V6.B16
	VMOV   V6.S[0], R15
	CBNZ   R15, try128_exact1
	ADD    $16, R16, R16
chunk128_3_exact1:
	VADDP  V19.B16, V19.B16, V6.B16
	VADDP  V6.B16, V6.B16, V6.B16
	VMOV   V6.S[0], R15
	CBNZ   R15, try128_exact1
	CBNZ   R20, check_second64_after128_exact1
	B      continue128_exact1

exceeded128_exact1:
	MOVD   $0x8000000000000000, R21   // exceeded flag (bit 63)
	ADD    R19, R21, R0              // add position
	MOVD   R0, ret+40(FP)
	RET

// ============================================================================
// 64-BYTE LOOP: Process 64 bytes per iteration (simpler, no pipelining)
// ============================================================================
loop64_exact1:
	VLD1.P 64(R11), [V1.B16, V2.B16, V3.B16, V4.B16]
	SUBS   $64, R10, R10
	
	// Compare all 4 chunks
	VCMEQ  V0.B16, V1.B16, V16.B16
	VCMEQ  V0.B16, V2.B16, V17.B16
	VCMEQ  V0.B16, V3.B16, V18.B16
	VCMEQ  V0.B16, V4.B16, V19.B16
	
	// Quick check: any match?
	VORR   V16.B16, V17.B16, V6.B16
	VORR   V18.B16, V19.B16, V7.B16
	VORR   V6.B16, V7.B16, V6.B16
	VADDP  V6.D2, V6.D2, V6.D2
	VMOV   V6.D[0], R15
	
	BLS    end64_check_exact1         // Exit if no more data
	CBZ    R15, loop64_exact1         // No match, continue

end64_check_exact1:
	CBZ    R15, loop32_exact1_entry   // No match in this chunk

end64_exact1:
	// Extract syndromes from V16-V19
	VAND   V5.B16, V16.B16, V16.B16
	VAND   V5.B16, V17.B16, V17.B16
	VAND   V5.B16, V18.B16, V18.B16
	VAND   V5.B16, V19.B16, V19.B16
	MOVD   $0, R16                     // chunk offset

process_chunk64_exact1:
	// Check each chunk
	CMP    $0, R16
	BEQ    chunk64_0_exact1
	CMP    $16, R16
	BEQ    chunk64_1_exact1
	CMP    $32, R16
	BEQ    chunk64_2_exact1
	B      chunk64_3_exact1

chunk64_0_exact1:
	VADDP  V16.B16, V16.B16, V6.B16
	B      extract64_exact1
chunk64_1_exact1:
	VADDP  V17.B16, V17.B16, V6.B16
	B      extract64_exact1
chunk64_2_exact1:
	VADDP  V18.B16, V18.B16, V6.B16
	B      extract64_exact1
chunk64_3_exact1:
	VADDP  V19.B16, V19.B16, V6.B16

extract64_exact1:
	VADDP  V6.B16, V6.B16, V6.B16
	VMOV   V6.S[0], R15
	CBZ    R15, next_chunk64_exact1

process_match64_exact1:
	RBIT   R15, R17
	CLZ    R17, R17
	LSR    $1, R17, R17
	
	SUB    $64, R11, R19
	ADD    R16, R19, R19
	ADD    R17, R19, R19
	SUB    R12, R19, R19
	
	CMP    R9, R19
	BGT    clear64_exact1
	
	// Verify match
	ADD    R0, R19, R8
	MOVD   R8, R20
	MOVD   R2, R21
	MOVD   R3, R22

verify64_exact1:
	CMP    $16, R22
	BLT    verify_tail64_exact1
	VLD1.P 16(R20), [V10.B16]
	VLD1.P 16(R21), [V11.B16]
	VEOR   V10.B16, V11.B16, V12.B16
	WORD   $0x6e30a98c
	FMOVS  F12, R6
	CBNZW  R6, fail64_exact1
	SUBS   $16, R22, R22
	BGT    verify64_exact1
	MOVD   R19, R17
	B      found_exact1

verify_tail64_exact1:
	CMP    $0, R22
	BLE    found64_match_exact1
	VLD1   (R20), [V10.B16]
	VLD1   (R21), [V11.B16]
	VEOR   V10.B16, V11.B16, V12.B16
	WORD   $0x3cf67b0d               // LDR Q13, [R24, R22, LSL #4]
	VAND   V13.B16, V12.B16, V12.B16
	WORD   $0x6e30a98c
	FMOVS  F12, R6
	CBNZW  R6, fail64_exact1

found64_match_exact1:
	MOVD   R19, R17
	B      found_exact1

fail64_exact1:
	// Threshold = 32 + (bytes_scanned >> 3)
	SUB    R12, R11, R20
	LSR    $3, R20, R20
	ADD    $32, R20
	ADD    $1, R13
	CMP    R20, R13
	BGE    exceeded64_exact1

clear64_exact1:
	LSL    $1, R17, R20            // bitpos = byteIndex << 1
	MOVD   $1, R21
	LSL    R20, R21, R20           // 1 << bitpos
	BIC    R20, R15, R15           // Clear just that bit
	CBNZ   R15, process_match64_exact1

next_chunk64_exact1:
	ADD    $16, R16
	CMP    $64, R16
	BLT    process_chunk64_exact1
	// All chunks exhausted, try next 64-byte iteration
	CMP    $64, R10
	BGE    loop64_exact1
	B      loop32_exact1_entry

exceeded64_exact1:
	MOVD   $0x8000000000000000, R20   // exceeded flag (bit 63)
	ADD    R19, R20, R0              // add position
	MOVD   R0, ret+40(FP)
	RET

loop32_exact1_entry:
	CMP   $32, R10
	BLT   loop16_exact1

// ============================================================================
// FALLBACK 32-BYTE LOOP: Standard pattern for small remainders
// ============================================================================
loop32_exact1:
	VLD1.P 32(R11), [V1.B16, V2.B16]
	SUBS   $32, R10, R10
	VCMEQ  V0.B16, V1.B16, V3.B16
	VCMEQ  V0.B16, V2.B16, V4.B16
	VORR   V4.B16, V3.B16, V6.B16
	VADDP  V6.D2, V6.D2, V6.D2
	VMOV   V6.D[0], R15
	BLS    end32_exact1             // Last block - check for match
	CBNZ   R15, end32_match_exact1  // Match found mid-loop
	CMP    $32, R10
	BGE    loop32_exact1
	B      loop16_exact1            // No match, not enough for another 32B

end32_exact1:
	CBZ   R15, loop16_exact1        // No match in final block

end32_match_exact1:
	// Extract syndromes
	VAND  V5.B16, V3.B16, V3.B16
	VAND  V5.B16, V4.B16, V4.B16
	VADDP V4.B16, V3.B16, V6.B16
	VADDP V6.B16, V6.B16, V6.B16
	VMOV  V6.D[0], R15

process_syndrome32_exact1:

	// Find position
	RBIT  R15, R16
	CLZ   R16, R16
	LSR   $1, R16, R16

	// Calculate position in haystack
	SUB   $32, R11, R17
	ADD   R16, R17, R17
	SUB   R12, R17, R17

	CMP   R9, R17
	BGT   clear_bit32_exact1

	// Verify
	ADD   R0, R17, R8
	MOVD  R8, R19
	MOVD  R2, R20
	MOVD  R3, R21

verify32_exact1:
	CMP   $16, R21
	BLT   verify_tail32_exact1
	VLD1.P 16(R19), [V10.B16]
	VLD1.P 16(R20), [V11.B16]
	VEOR  V10.B16, V11.B16, V12.B16
	WORD  $0x6e30a98c              // VUMAXV V12.B16, V12
	FMOVS F12, R6
	CBNZW R6, fail32_exact1
	SUBS  $16, R21, R21
	BGT   verify32_exact1
	B     found_exact1

verify_tail32_exact1:
	CMP   $0, R21
	BLE   found_exact1
	VLD1  (R19), [V10.B16]
	VLD1  (R20), [V11.B16]
	VEOR  V10.B16, V11.B16, V12.B16
	WORD  $0x3cf57b0d               // LDR Q13, [R24, R21, LSL #4]
	VAND  V13.B16, V12.B16, V12.B16
	WORD  $0x6e30a98c
	FMOVS F12, R6
	CBZW  R6, found_exact1

fail32_exact1:
	// Threshold = 32 + (bytes_scanned >> 3)
	SUB   R12, R11, R19
	LSR   $3, R19, R19
	ADD   $32, R19
	ADD   $1, R13
	CMP   R19, R13
	BGE   exceeded_exact1

clear_bit32_exact1:
	LSL   $1, R16, R19             // bitpos = byteIndex << 1
	MOVD  $1, R20
	LSL   R19, R20, R19            // 1 << bitpos
	BIC   R19, R15, R15            // Clear just that bit
	CBNZ  R15, process_syndrome32_exact1
	// No more matches in this 32-byte chunk - fall through to 16-byte or scalar loop

loop16_exact1:
	CMP   $16, R10
	BLT   scalar_exact1

loop16_inner_exact1:
	VLD1.P 16(R11), [V1.B16]
	SUBS   $16, R10, R10
	VCMEQ  V0.B16, V1.B16, V3.B16
	VAND   V5.B16, V3.B16, V3.B16
	VADDP  V3.B16, V3.B16, V3.B16
	VADDP  V3.B16, V3.B16, V3.B16
	VMOV   V3.S[0], R15
	CBNZ   R15, end16_exact1          // match found
	CMP    $16, R10                   // no match - check if we can continue
	BGE    loop16_inner_exact1
	B      scalar_exact1              // not enough bytes for another iteration

end16_exact1:

process16_exact1:
	RBIT  R15, R16
	CLZ   R16, R16
	LSR   $1, R16, R16

	SUB   $16, R11, R17
	ADD   R16, R17, R17
	SUB   R12, R17, R17

	CMP   R9, R17
	BGT   clear16_exact1

	ADD   R0, R17, R8
	MOVD  R8, R19
	MOVD  R2, R20
	MOVD  R3, R21

verify16_exact1:
	CMP   $16, R21
	BLT   verify_tail16_exact1
	VLD1.P 16(R19), [V10.B16]
	VLD1.P 16(R20), [V11.B16]
	VEOR  V10.B16, V11.B16, V12.B16
	WORD  $0x6e30a98c
	FMOVS F12, R6
	CBNZW R6, fail16_exact1
	SUBS  $16, R21, R21
	BGT   verify16_exact1
	B     found_exact1

verify_tail16_exact1:
	CMP   $0, R21
	BLE   found_exact1
	VLD1  (R19), [V10.B16]
	VLD1  (R20), [V11.B16]
	VEOR  V10.B16, V11.B16, V12.B16
	WORD  $0x3cf57b0d               // LDR Q13, [R24, R21, LSL #4]
	VAND  V13.B16, V12.B16, V12.B16
	WORD  $0x6e30a98c
	FMOVS F12, R6
	CBZW  R6, found_exact1

fail16_exact1:
	// Threshold = 32 + (bytes_scanned >> 3)
	SUB   R12, R11, R19
	LSR   $3, R19, R19
	ADD   $32, R19
	ADD   $1, R13
	CMP   R19, R13
	BGE   exceeded_exact1

clear16_exact1:
	LSL   $1, R16, R19             // bitpos = byteIndex << 1
	MOVD  $1, R20
	LSL   R19, R20, R19            // 1 << bitpos
	BIC   R19, R15, R15            // Clear just that bit
	CBNZ  R15, process16_exact1
	CMP   $16, R10
	BGE   loop16_inner_exact1

scalar_exact1:
	CMP   $0, R10
	BLE   not_found_exact1

scalar_loop_exact1:
	MOVBU (R11), R15
	CMP   R5, R15
	BNE   scalar_next_exact1

	SUB   R12, R11, R17
	CMP   R9, R17
	BGT   scalar_next_exact1

	// Verify with NEON
	ADD   R0, R17, R8
	MOVD  R8, R19
	MOVD  R2, R20
	MOVD  R3, R21

scalar_verify_exact1:
	CMP   $16, R21
	BLT   scalar_verify_tail_exact1
	VLD1.P 16(R19), [V10.B16]
	VLD1.P 16(R20), [V11.B16]
	VEOR  V10.B16, V11.B16, V12.B16
	WORD  $0x6e30a98c              // UMAXV B12, V12.B16
	FMOVS F12, R6
	CBNZW R6, scalar_fail_exact1
	SUBS  $16, R21, R21
	BGT   scalar_verify_exact1
	B     found_exact1

scalar_verify_tail_exact1:
	CMP   $0, R21
	BLE   found_exact1
	VLD1  (R19), [V10.B16]
	VLD1  (R20), [V11.B16]
	VEOR  V10.B16, V11.B16, V12.B16
	WORD  $0x3cf57b0d               // LDR Q13, [R24, R21, LSL #4]
	VAND  V13.B16, V12.B16, V12.B16
	WORD  $0x6e30a98c
	FMOVS F12, R6
	CBZW  R6, found_exact1

scalar_fail_exact1:
	// Threshold = 32 + (bytes_scanned >> 3)
	// More lenient than 16 + (bytes >> 4) to handle pathological small inputs
	SUB   R12, R11, R19
	LSR   $3, R19, R19
	ADD   $32, R19
	ADD   $1, R13
	CMP   R19, R13
	BGE   exceeded_exact1

scalar_next_exact1:
	ADD   $1, R11
	SUBS  $1, R10, R10
	BGT   scalar_loop_exact1

not_found_exact1:
	MOVD  $-1, R0
	MOVD  R0, ret+40(FP)
	RET

found_zero_exact1:
	MOVD  ZR, R0
	MOVD  R0, ret+40(FP)
	RET

found_exact1:
	MOVD  R17, R0
	MOVD  R0, ret+40(FP)
	RET

exceeded_exact1:
	MOVD  $0x8000000000000000, R19   // exceeded flag (bit 63)
	ADD   R17, R19, R0              // add position
	MOVD  R0, ret+40(FP)
	RET

// func indexExact2Byte(haystack string, needle string, off1 int, off2Delta int) uint64
