package utf8

import (
	stdlib "unicode/utf8"

	"golang.org/x/sys/cpu"
)

var hasAVX2 = cpu.X86.HasAVX2

func ValidString(s string) bool {
	if !hasAVX2 {
		return stdlib.ValidString(s)
	}

	return utf8_valid_range_avx2(s)
}
