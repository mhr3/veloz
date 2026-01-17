//go:build !noasm && arm64

package ascii

// IndexAny returns the index of the first byte in s that is in the CharSet,
// or -1 if no such byte exists.
func (cs CharSet) IndexAny(s string) int {
	if cs.bitset == [4]uint64{} {
		return -1
	}
	if len(s) < 16 {
		return cs.indexAnyGo(s)
	}
	return indexAnyNeonBitset(s, cs.bitset[0], cs.bitset[1], cs.bitset[2], cs.bitset[3])
}

// ContainsAny reports whether any byte in s is in the CharSet.
func (cs CharSet) ContainsAny(s string) bool {
	return cs.IndexAny(s) >= 0
}

// indexAnyGo is a Go fallback for small data.
func (cs CharSet) indexAnyGo(s string) int {
	for i := 0; i < len(s); i++ {
		c := s[i]
		if cs.bitset[c>>6]&(1<<(c&63)) != 0 {
			return i
		}
	}
	return -1
}

// IndexAny finds the first occurrence of any byte from chars in data.
// Returns -1 if no match is found.
// Dispatch: Go (<16B data) → SVE2 (>32 chars) → NEON bitset (default)
func IndexAny(data, chars string) int {
	if len(chars) == 0 {
		return -1
	}
	// For very small data, Go is faster (bitset building overhead dominates)
	if len(data) < 16 {
		return indexAnyGo(data, chars)
	}
	// Note: SVE2 MATCH was removed - use NEON bitset for all char set sizes
	// Build 256-bit bitset from chars
	var bitset [4]uint64
	for i := 0; i < len(chars); i++ {
		c := chars[i]
		bitset[c>>6] |= 1 << (c & 63)
	}
	return indexAnyNeonBitset(data, bitset[0], bitset[1], bitset[2], bitset[3])
}

// indexFoldRabinKarp is now generated via gocc in ascii_neon.go
