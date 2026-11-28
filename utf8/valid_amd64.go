package utf8

import (
	stdlib "unicode/utf8"

	"golang.org/x/sys/cpu"
)

var (
	hasAVX2 = cpu.X86.HasAVX2
	// AVX-512 as the kernel needs it: bytes in the registers and VPERMB
	hasAVX512 = cpu.X86.HasAVX512BW && cpu.X86.HasAVX512VBMI
)

func Valid(p []byte) bool {
	if hasAVX512 {
		return utf8_valid_range_avx512(p)
	}

	if hasAVX2 {
		return utf8_valid_range_avx2(p)
	}

	return stdlib.Valid(p)
}

func ValidString(s string) bool {
	if hasAVX512 {
		return utf8_valid_range_avx512_string(s)
	}

	if hasAVX2 {
		return utf8_valid_range_avx2_string(s)
	}

	return stdlib.ValidString(s)
}
