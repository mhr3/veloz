//go:build !arm64

package bytealg

import "strings"

// Index finds the first case-sensitive match of needle in haystack.
func Index(haystack, needle string) int {
	return strings.Index(haystack, needle)
}
