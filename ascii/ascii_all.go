package ascii

// ByteSet represents a precomputed byte set for fast IndexAny lookups.
// Build once with NewByteSet, then reuse with bs.IndexAny().
type ByteSet struct {
	bitset [4]uint64
}

// NewByteSet creates a ByteSet from the given characters.
func NewByteSet(chars string) ByteSet {
	var bs ByteSet
	for i := 0; i < len(chars); i++ {
		c := chars[i]
		bs.bitset[c>>6] |= 1 << (c & 63)
	}
	return bs
}

// ContainsAny reports whether any byte from chars is in data.
func ContainsAny(data, chars string) bool {
	return IndexAny(data, chars) >= 0
}

// IndexNonASCII finds the first non-ASCII byte (>= 0x80) in the string.
// Returns -1 if all bytes are ASCII.
func IndexNonASCII(data string) int {
	return IndexMask(data, 0x80)
}

// indexAnyGoBitset scans s for any byte in the 256-bit bitset.
func indexAnyGoBitset(s string, bitset *[4]uint64) int {
	for i := 0; i < len(s); i++ {
		c := s[i]
		if bitset[c>>6]&(1<<(c&63)) != 0 {
			return i
		}
	}
	return -1
}

func HasPrefixFold(s, prefix string) bool {
	if len(s) < len(prefix) {
		return false
	}
	return EqualFold(s[:len(prefix)], prefix)
}

func HasSuffixFold(s, suffix string) bool {
	if len(s) < len(suffix) {
		return false
	}
	return EqualFold(s[len(s)-len(suffix):], suffix)
}
