package ascii

import (
	"github.com/mhr3/veloz/internal/bytealg"
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

// Index finds the first case-sensitive match of needle in haystack.
func Index(haystack, needle string) int {
	return bytealg.Index(haystack, needle)
}
