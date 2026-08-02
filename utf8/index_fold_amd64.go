//go:build amd64 && !noasm

package utf8

import (
	"golang.org/x/sys/cpu"
)

// IndexFold returns the byte index of the first full-Unicode case-folded match
// of needle in haystack. On capable amd64 CPUs, long inputs use the generated
// AVX2 script-specific classifiers and folding kernels.
func IndexFold(haystack, needle string) int {
	if len(needle) == 0 {
		return 0
	}
	if len(haystack) == 0 {
		return -1
	}
	if !cpu.X86.HasAVX2 {
		return indexFoldFallback(haystack, needle)
	}
	if len(haystack) < indexFoldSIMDMinBytes {
		return indexFoldFallback(haystack, needle)
	}
	return indexFoldSIMD(haystack, needle)
}

func indexFoldSIMD(haystack, needle string) int {
	var metadata indexFoldNeedleMetadata
	var matchedLength uint64

	return indexFoldAVX2(haystack, needle, &metadata, &matchedLength)
}
