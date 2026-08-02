//go:build arm64 && !noasm

package utf8

func indexFoldSIMDSupportedForTest() bool {
	return true
}

func indexFoldSIMDRawForTest(haystack, needle string, metadata *indexFoldNeedleMetadata) int {
	var matchedLength uint64
	return indexFoldNeon(haystack, needle, metadata, &matchedLength)
}
