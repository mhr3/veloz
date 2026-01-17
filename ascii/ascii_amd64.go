package ascii

import (
	"strings"

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

func IndexFold(haystack, needle string) int {
	return indexFoldGo(haystack, needle)
}

// Index finds the first case-sensitive match of needle in haystack.
func Index(haystack, needle string) int {
	return strings.Index(haystack, needle)
}

func indexFoldRabinKarp(a, b string) int {
	return indexFoldGo(a, b)
}

func IndexAny(s, chars string) int {
	return indexAnyGo(s, chars)
}

// IndexAny returns the index of the first byte in s that is in the CharSet,
// or -1 if no such byte exists.
func (cs CharSet) IndexAny(s string) int {
	if cs.bitset == [4]uint64{} {
		return -1
	}
	for i := 0; i < len(s); i++ {
		c := s[i]
		if cs.bitset[c>>6]&(1<<(c&63)) != 0 {
			return i
		}
	}
	return -1
}

// ContainsAny reports whether any byte in s is in the CharSet.
func (cs CharSet) ContainsAny(s string) bool {
	return cs.IndexAny(s) >= 0
}

// Index finds the first occurrence of the pattern in haystack.
// Uses the case sensitivity specified when the Searcher was created.
// On amd64, this falls back to Go implementation (no SIMD acceleration yet).
func (s Searcher) Index(haystack string) int {
	if s.caseSensitive {
		return strings.Index(haystack, s.raw)
	}
	return indexFoldGo(haystack, s.raw)
}
