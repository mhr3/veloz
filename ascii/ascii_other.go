//go:build (!amd64 && !arm64) || (noasm && arm64)

package ascii

func ValidString(s string) bool {
	return indexMaskGo(s, 0x80) == -1
}

func IndexMask(s string, mask byte) int {
	return indexMaskGo(s, mask)
}

func EqualFold(a, b string) bool {
	return equalFoldGo(a, b)
}

func IndexFold(a, b string) int {
	return indexFoldGo(a, b)
}

func indexFoldRabinKarp(a, b string) int {
	return indexFoldGo(a, b)
}

// IndexAny finds the first occurrence of any byte from chars in data.
func IndexAny(s, chars string) int {
	return NewByteSet(chars).Index(s)
}

// Index returns the index of the first byte in s that is in the ByteSet,
// or -1 if no such byte exists.
func (bs ByteSet) Index(s string) int {
	if bs.bitset == [4]uint64{} {
		return -1
	}
	return indexAnyGoBitset(s, &bs.bitset)
}

// Match reports whether any byte in s is in the ByteSet.
func (bs ByteSet) Match(s string) bool {
	return bs.Index(s) >= 0
}
