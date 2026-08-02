//go:build (!arm64 && !amd64) || noasm

package utf8

// IndexFold returns the byte index of the first full-Unicode case-folded match
// of needle in haystack. It returns -1 when there is no match and 0 for an
// empty needle. Malformed UTF-8 bytes are compared losslessly byte-for-byte.
func IndexFold(haystack, needle string) int {
	return indexFoldFallback(haystack, needle)
}
