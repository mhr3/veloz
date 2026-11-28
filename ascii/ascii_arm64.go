//go:build !noasm && arm64

package ascii

// Index returns the index of the first byte in s that is in the ByteSet,
// or -1 if no such byte exists.
func (bs ByteSet) Index(s []byte) int {
	if bs.bitset == [4]uint64{} {
		return -1
	}
	if len(s) < 16 {
		return indexAnyGo(s, &bs.bitset)
	}
	return indexAnyNeon(s, &bs.bitset)
}

// IndexString returns the index of the first byte in s that is in the
// ByteSet, or -1 if no such byte exists.
func (bs ByteSet) IndexString(s string) int {
	if bs.bitset == [4]uint64{} {
		return -1
	}
	if len(s) < 16 {
		return indexAnyGo(s, &bs.bitset)
	}
	return indexAnyNeonString(s, &bs.bitset)
}
