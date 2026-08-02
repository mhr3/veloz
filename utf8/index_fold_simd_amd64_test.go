//go:build amd64 && !noasm

package utf8

import "golang.org/x/sys/cpu"

func indexFoldSIMDSupportedForTest() bool {
	return cpu.X86.HasAVX2
}

func indexFoldSIMDRawForTest(haystack, needle string, metadata *indexFoldNeedleMetadata) int {
	var matchedLength uint64
	return indexFoldAVX2(haystack, needle, metadata, &matchedLength)
}
