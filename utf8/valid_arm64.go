package utf8

import "github.com/mhr3/veloz/ascii"

func Valid(p []byte) bool {
	// speed up the common case
	idx := ascii.IndexMask(p, 0x80)
	if idx == -1 {
		return true
	}

	return utf8_valid_range(p[idx:])
}

func ValidString(s string) bool {
	// speed up the common case
	idx := ascii.IndexMaskString(s, 0x80)
	if idx == -1 {
		return true
	}

	return utf8_valid_range_string(s[idx:])
}
