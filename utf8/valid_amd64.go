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

func ValidString(s string) bool {
	if hasAVX512 {
		return utf8_valid_range_avx512(s)
	}

	if hasAVX2 {
		return utf8_valid_range_avx2(s)
	}

	return stdlib.ValidString(s)
}
