package ascii

import (
	"golang.org/x/sys/cpu"
)

var (
	hasSSE41 = cpu.X86.HasSSE41
	hasAVX2  = cpu.X86.HasAVX2
)

func Valid(p []byte) bool {
	if hasAVX2 {
		return isAsciiAvx(p)
	}

	if hasSSE41 {
		return isAsciiSse(p)
	}

	return isAsciiGo(p)
}

func ValidString(s string) bool {
	if hasAVX2 {
		return isAsciiAvxString(s)
	}

	if hasSSE41 {
		return isAsciiSseString(s)
	}

	return isAsciiGo(s)
}

func IndexMask(p []byte, mask byte) int {
	if hasAVX2 {
		return indexMaskAvx(p, mask)
	}

	return indexMaskGo(p, mask)
}

func IndexMaskString(s string, mask byte) int {
	if hasAVX2 {
		return indexMaskAvxString(s, mask)
	}

	return indexMaskGo(s, mask)
}

func EqualFold(a, b []byte) bool {
	if hasAVX2 && len(a) >= 32 {
		return equalFoldAvx(a, b)
	}

	if hasSSE41 && len(a) >= 16 {
		return equalFoldSse(a, b)
	}

	return equalFoldGo(a, b)
}

func EqualFoldString(a, b string) bool {
	if hasAVX2 && len(a) >= 32 {
		return equalFoldAvxString(a, b)
	}

	if hasSSE41 && len(a) >= 16 {
		return equalFoldSseString(a, b)
	}

	return equalFoldGo(a, b)
}

func IndexFold(a, b []byte) int {
	if hasAVX2 {
		return indexFoldAvx(a, b)
	}

	if hasSSE41 {
		return indexFoldSse(a, b)
	}

	return indexFoldGo(a, b)
}

func IndexFoldString(a, b string) int {
	if hasAVX2 {
		return indexFoldAvxString(a, b)
	}

	if hasSSE41 {
		return indexFoldSseString(a, b)
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

// Index returns the index of the first byte in s that is in the ByteSet,
// or -1 if no such byte exists.
func (bs ByteSet) Index(s []byte) int {
	if bs.bitset == [4]uint64{} {
		return -1
	}
	if hasAVX2 && len(s) >= 32 {
		return indexAnyAvx2(s, &bs.bitset)
	}
	if hasSSE41 && len(s) >= 16 {
		return indexAnySse(s, &bs.bitset)
	}
	return indexAnyGo(s, &bs.bitset)
}

// IndexString returns the index of the first byte in s that is in the
// ByteSet, or -1 if no such byte exists.
func (bs ByteSet) IndexString(s string) int {
	if bs.bitset == [4]uint64{} {
		return -1
	}
	if hasAVX2 && len(s) >= 32 {
		return indexAnyAvx2String(s, &bs.bitset)
	}
	if hasSSE41 && len(s) >= 16 {
		return indexAnySseString(s, &bs.bitset)
	}
	return indexAnyGo(s, &bs.bitset)
}
