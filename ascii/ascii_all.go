package ascii

// CharSet represents a precomputed character set for fast IndexAny lookups.
// Build once with NewCharSet, then reuse with cs.IndexAny().
type CharSet struct {
	bitset [4]uint64
}

// NewCharSet creates a CharSet from the given characters.
func NewCharSet(chars string) CharSet {
	var cs CharSet
	for i := 0; i < len(chars); i++ {
		c := chars[i]
		cs.bitset[c>>6] |= 1 << (c & 63)
	}
	return cs
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

// ContainsAny reports whether any byte from chars is in data.
func ContainsAny(data, chars string) bool {
	return IndexAny(data, chars) >= 0
}

// IndexNonASCII finds the first non-ASCII byte (>= 0x80) in the string.
// Returns -1 if all bytes are ASCII.
func IndexNonASCII(data string) int {
	return IndexMask(data, 0x80)
}
