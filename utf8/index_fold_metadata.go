//go:build (arm64 || amd64) && !noasm

package utf8

import "unsafe"

// First 64-byte-aligned size where forced SIMD wins over the generic path on
// the equal-weight arithmetic mean across the cross-script benchmark corpus.
const indexFoldSIMDMinBytes = 128

// indexFoldNeedleMetadata mirrors sz_utf8_uncased_needle_metadata_t. It is
// pointer-free scratch space populated by the translated C implementation.
type indexFoldNeedleMetadata struct {
	offsetInUnfolded  uint64
	lengthInUnfolded  uint64
	foldedSlice       [16]byte
	foldedSliceLength uint8
	probeSecond       uint8
	probeThird        uint8
	kernelID          uint8
}

// Keep the Go and C layouts locked to the same 40-byte ABI.
const indexFoldNeedleMetadataSize = unsafe.Sizeof(indexFoldNeedleMetadata{})

var (
	_ [40 - indexFoldNeedleMetadataSize]byte
	_ [indexFoldNeedleMetadataSize - 40]byte
)
