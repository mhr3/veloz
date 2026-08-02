//go:build arm64 && !noasm

package utf8

// IndexFold returns the byte index of the first full-Unicode case-folded match
// of needle in haystack. The arm64 implementation includes script-specific
// NEON classifiers and kernels generated from the C reference by gocc.
func IndexFold(haystack, needle string) int {
	if len(needle) == 0 {
		return 0
	}
	if len(haystack) == 0 {
		return -1
	}
	if len(haystack) < indexFoldSIMDMinBytes {
		return indexFoldFallback(haystack, needle)
	}
	return indexFoldSIMD(haystack, needle)
}

func indexFoldSIMD(haystack, needle string) int {
	var metadata indexFoldNeedleMetadata
	var matchedLength uint64

	return indexFoldNeon(haystack, needle, &metadata, &matchedLength)
}
