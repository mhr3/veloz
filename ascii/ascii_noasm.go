//go:build noasm && arm64

package ascii

import "github.com/mhr3/veloz/internal/bytealg"

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

// Index finds the first case-sensitive match of needle in haystack.
func Index(haystack, needle string) int {
	return bytealg.Index(haystack, needle)
}
