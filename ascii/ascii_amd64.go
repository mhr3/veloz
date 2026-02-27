package ascii

import (
	"golang.org/x/sys/cpu"
)

var (
	hasSSE41 = cpu.X86.HasSSE41
	hasAVX2  = cpu.X86.HasAVX
)

func ValidString(s string) bool {
	if hasAVX2 {
		return isAsciiAvx(s)
	}

	if hasSSE41 {
		return isAsciiSse(s)
	}

	return isAsciiGo(s)
}

func IndexMask(s string, mask byte) int {
	if hasAVX2 {
		return indexMaskAvx(s, mask)
	}

	return indexMaskGo(s, mask)
}

func EqualFold(a, b string) bool {
	if len(a) < 32 || !hasAVX2 {
		return equalFoldGo(a, b)
	}

	return equalFoldAvx(a, b)
}

func IndexFold(a, b string) int {
	// TODO: implement acceleration for this
	return indexFoldGo(a, b)
}

func indexFoldRabinKarp(a, b string) int {
	// FIXME: definitely not Rabin-Karp
	return indexFoldGo(a, b)
}

// IndexAny finds the first occurrence of any byte from chars in data.
func IndexAny(s, chars string) int {
	return NewByteSet(chars).IndexAny(s)
}

// IndexAny returns the index of the first byte in s that is in the ByteSet,
// or -1 if no such byte exists.
func (bs ByteSet) IndexAny(s string) int {
	if bs.bitset == [4]uint64{} {
		return -1
	}
	return indexAnyGoBitset(s, &bs.bitset)
}

// ContainsAny reports whether any byte in s is in the ByteSet.
func (bs ByteSet) ContainsAny(s string) bool {
	return bs.IndexAny(s) >= 0
}
