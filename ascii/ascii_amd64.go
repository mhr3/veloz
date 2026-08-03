package ascii

import (
	"golang.org/x/sys/cpu"
)

var (
	hasSSE41 = cpu.X86.HasSSE41
	hasAVX2  = cpu.X86.HasAVX2
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
	if hasAVX2 && len(a) >= 32 {
		return equalFoldAvx(a, b)
	}

	if hasSSE41 && len(a) >= 16 {
		return equalFoldSse(a, b)
	}

	return equalFoldGo(a, b)
}

func IndexFold(a, b string) int {
	if hasAVX2 {
		return indexFoldAvx(a, b)
	}

	if hasSSE41 {
		return indexFoldSse(a, b)
	}

	return indexFoldGo(a, b)
}

func indexFoldRabinKarp(a, b string) int {
	if hasAVX2 {
		return indexFoldRabinKarpAvx(a, b)
	}

	if hasSSE41 {
		return indexFoldRabinKarpSse(a, b)
	}

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
	if hasAVX2 && len(s) >= 32 {
		return indexAnyAvxBitset(s, bs.bitset[0], bs.bitset[1], bs.bitset[2], bs.bitset[3])
	}
	return indexAnyGoBitset(s, &bs.bitset)
}

// Match reports whether any byte in s is in the ByteSet.
func (bs ByteSet) Match(s string) bool {
	return bs.Index(s) >= 0
}
