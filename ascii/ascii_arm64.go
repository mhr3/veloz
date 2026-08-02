//go:build !noasm && arm64

package ascii

// Index returns the index of the first byte in s that is in the ByteSet,
// or -1 if no such byte exists.
func (bs ByteSet) Index(s string) int {
	if bs.bitset == [4]uint64{} {
		return -1
	}
	if len(s) < 16 {
		return indexAnyGoBitset(s, &bs.bitset)
	}
	return indexAnyNeonBitset(s, bs.bitset[0], bs.bitset[1], bs.bitset[2], bs.bitset[3])
}

// Match reports whether any byte in s is in the ByteSet.
func (bs ByteSet) Match(s string) bool {
	return bs.Index(s) >= 0
}

// IndexAny finds the first occurrence of any byte from chars in data.
// Returns -1 if no match is found.
// Dispatch: Go (<16B data) → NEON bitset (default)
func IndexAny(data, chars string) int {
	return NewByteSet(chars).Index(data)
}
