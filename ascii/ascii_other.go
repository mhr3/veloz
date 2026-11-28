//go:build (!amd64 && !arm64) || (noasm && arm64)

package ascii

func Valid(p []byte) bool {
	return isAsciiGo(p)
}

func ValidString(s string) bool {
	return isAsciiGo(s)
}

func IndexMask(p []byte, mask byte) int {
	return indexMaskGo(p, mask)
}

func IndexMaskString(s string, mask byte) int {
	return indexMaskGo(s, mask)
}

func EqualFold(a, b []byte) bool {
	return equalFoldGo(a, b)
}

func EqualFoldString(a, b string) bool {
	return equalFoldGo(a, b)
}

func IndexFold(a, b []byte) int {
	return indexFoldGo(a, b)
}

func IndexFoldString(a, b string) int {
	return indexFoldGo(a, b)
}

func indexFoldRabinKarp(a, b string) int {
	return indexFoldGo(a, b)
}

// Index returns the index of the first byte in s that is in the ByteSet,
// or -1 if no such byte exists.
func (bs ByteSet) Index(s []byte) int {
	if bs.bitset == [4]uint64{} {
		return -1
	}
	return indexAnyGo(s, &bs.bitset)
}

// IndexString returns the index of the first byte in s that is in the
// ByteSet, or -1 if no such byte exists.
func (bs ByteSet) IndexString(s string) int {
	if bs.bitset == [4]uint64{} {
		return -1
	}
	return indexAnyGo(s, &bs.bitset)
}
