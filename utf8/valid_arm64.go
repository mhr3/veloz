package utf8

import (
	"unsafe"

	"github.com/mhr3/veloz/ascii"
)

func Valid(p []byte) bool {
	// speed up the common case; IndexMask has no []byte form, and it only
	// reads the bytes
	idx := ascii.IndexMask(unsafe.String(unsafe.SliceData(p), len(p)), 0x80)
	if idx == -1 {
		return true
	}

	return utf8_valid_range(p[idx:])
}

func ValidString(s string) bool {
	// speed up the common case
	idx := ascii.IndexMask(s, 0x80)
	if idx == -1 {
		return true
	}

	return utf8_valid_range_string(s[idx:])
}
