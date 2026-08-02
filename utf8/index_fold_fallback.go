package utf8

import (
	"strings"
	stdutf8 "unicode/utf8"
)

const indexFoldFallbackRingCapacity = 32

const malformedRuneTag = uint32(0x80000000)

type foldMapping struct {
	runes [3]uint32
	count uint8
}

type casedRange struct {
	low, high rune
}

// indexFoldFallback is the native Go counterpart of
// sz_utf8_uncased_search_serial. It searches the folded rune stream without
// materializing either folded string.
func indexFoldFallback(haystack, needle string) int {
	if needle == "" {
		return 0
	}

	if len(haystack) == 0 {
		return -1
	}

	// Match the serial implementation's byte-search fast path. This includes
	// malformed UTF-8, whose bytes are compared losslessly.
	if isFoldInvariant(needle) {
		return strings.Index(haystack, needle)
	}

	if len(needle) <= 12 {
		var folded [4]uint32
		iterator := newIndexFoldFallbackIterator(needle)
		foldedCount := 0
		for foldedCount < len(folded) {
			value, _, _, ok := iterator.next()
			if !ok {
				break
			}
			folded[foldedCount] = value
			foldedCount++
		}
		if foldedCount >= 1 && foldedCount <= 3 {
			return indexFoldFallbackShort(haystack, folded[:foldedCount])
		}
	}

	return indexFoldFallbackRolling(haystack, needle)
}

func isFoldInvariant(value string) bool {
	for offset := 0; offset < len(value); {
		r, size := stdutf8.DecodeRuneInString(value[offset:])
		if r == stdutf8.RuneError && size == 1 {
			offset++
			continue
		}
		if isCasedRune(r) {
			return false
		}
		offset += size
	}
	return true
}

func isCasedRune(r rune) bool {
	for _, candidate := range unicodeCasedRanges {
		if r < candidate.low {
			return false
		}
		if r <= candidate.high {
			return true
		}
	}
	return false
}

// indexFoldFallbackIterator streams full case-folded runes while retaining
// the source byte offset and position within a one-to-many expansion.
type indexFoldFallbackIterator struct {
	value  string
	offset int

	pending       [3]uint32
	pendingCount  uint8
	pendingIndex  uint8
	pendingSource int
}

func newIndexFoldFallbackIterator(value string) indexFoldFallbackIterator {
	return indexFoldFallbackIterator{value: value}
}

// next returns a folded rune, its source byte offset, and the number of
// earlier folded runes emitted from that source rune.
func (iterator *indexFoldFallbackIterator) next() (
	value uint32, sourceStart, expansionOffset int, ok bool,
) {
	if iterator.pendingIndex < iterator.pendingCount {
		expansionOffset = int(iterator.pendingIndex)
		value = iterator.pending[iterator.pendingIndex]
		iterator.pendingIndex++
		return value, iterator.pendingSource, expansionOffset, true
	}

	if iterator.offset >= len(iterator.value) {
		return 0, 0, 0, false
	}

	sourceStart = iterator.offset
	lead := iterator.value[iterator.offset]
	if lead < stdutf8.RuneSelf {
		iterator.offset++
		if lead >= 'A' && lead <= 'Z' {
			lead += 'a' - 'A'
		}
		iterator.pendingCount = 0
		iterator.pendingIndex = 0
		return uint32(lead), sourceStart, 0, true
	}

	r, size := stdutf8.DecodeRuneInString(iterator.value[iterator.offset:])
	if r == stdutf8.RuneError && size == 1 {
		iterator.offset++
		iterator.pendingCount = 0
		iterator.pendingIndex = 0
		return malformedRuneTag | uint32(lead), sourceStart, 0, true
	}

	iterator.offset += size
	mapping, mapped := unicodeFullFold[r]
	if !mapped {
		iterator.pendingCount = 0
		iterator.pendingIndex = 0
		return uint32(r), sourceStart, 0, true
	}

	iterator.pending = mapping.runes
	iterator.pendingCount = mapping.count
	iterator.pendingIndex = 1
	iterator.pendingSource = sourceStart
	return iterator.pending[0], sourceStart, 0, true
}

// indexFoldFallbackShort is the hash-free path used for needles that fold to
// one, two, or three runes.
func indexFoldFallbackShort(haystack string, needle []uint32) int {
	var window [3]uint32
	var sources [3]int
	windowCount := 0
	iterator := newIndexFoldFallbackIterator(haystack)

	for {
		value, source, _, ok := iterator.next()
		if !ok {
			return -1
		}

		if windowCount < len(needle) {
			window[windowCount] = value
			sources[windowCount] = source
			windowCount++
		} else {
			copy(window[:len(needle)-1], window[1:len(needle)])
			copy(sources[:len(needle)-1], sources[1:len(needle)])
			window[len(needle)-1] = value
			sources[len(needle)-1] = source
		}

		if windowCount != len(needle) {
			continue
		}
		matched := true
		for index := range needle {
			if window[index] != needle[index] {
				matched = false
				break
			}
		}
		if matched {
			return sources[0]
		}
	}
}

func indexFoldFallbackRolling(haystack, needle string) int {
	var needleRunes [indexFoldFallbackRingCapacity]uint32
	needleIterator := newIndexFoldFallbackIterator(needle)
	needlePrefixCount := 0
	needleHash := uint64(0)
	for needlePrefixCount < len(needleRunes) {
		value, _, _, ok := needleIterator.next()
		if !ok {
			break
		}
		needleRunes[needlePrefixCount] = value
		needleHash = needleHash*257 + uint64(value)
		needlePrefixCount++
	}
	needleTotalCount := needlePrefixCount
	for {
		_, _, _, ok := needleIterator.next()
		if !ok {
			break
		}
		needleTotalCount++
	}
	if needlePrefixCount == 0 {
		return -1
	}

	hashMultiplier := uint64(1)
	for index := 1; index < needlePrefixCount; index++ {
		hashMultiplier *= 257
	}

	var windowRunes [indexFoldFallbackRingCapacity]uint32
	var windowSources [indexFoldFallbackRingCapacity]int
	var windowSkipCounts [indexFoldFallbackRingCapacity]uint8
	windowHash := uint64(0)
	windowCount := 0
	haystackIterator := newIndexFoldFallbackIterator(haystack)
	for windowCount < needlePrefixCount {
		value, source, expansionOffset, ok := haystackIterator.next()
		if !ok {
			return -1
		}
		windowRunes[windowCount] = value
		windowSources[windowCount] = source
		windowSkipCounts[windowCount] = uint8(expansionOffset)
		windowHash = windowHash*257 + uint64(value)
		windowCount++
	}

	ringHead := 0
	for {
		if windowHash == needleHash &&
			indexFoldFallbackWindowEqual(&windowRunes, ringHead, needleRunes[:needlePrefixCount]) {
			windowStart := windowSources[ringHead]
			if needleTotalCount <= indexFoldFallbackRingCapacity ||
				indexFoldFallbackVerifyLong(
					haystack[windowStart:], needle, int(windowSkipCounts[ringHead]),
				) {
				return windowStart
			}
		}

		value, source, expansionOffset, ok := haystackIterator.next()
		if !ok {
			return -1
		}
		windowHash -= uint64(windowRunes[ringHead]) * hashMultiplier
		windowHash = windowHash*257 + uint64(value)
		windowRunes[ringHead] = value
		windowSources[ringHead] = source
		windowSkipCounts[ringHead] = uint8(expansionOffset)
		ringHead++
		if ringHead == needlePrefixCount {
			ringHead = 0
		}
	}
}

func indexFoldFallbackWindowEqual(
	window *[indexFoldFallbackRingCapacity]uint32,
	ringHead int,
	needle []uint32,
) bool {
	firstSegment := len(needle) - ringHead
	for index := 0; index < firstSegment; index++ {
		if window[ringHead+index] != needle[index] {
			return false
		}
	}
	for index := 0; index < ringHead; index++ {
		if window[index] != needle[firstSegment+index] {
			return false
		}
	}
	return true
}

func indexFoldFallbackVerifyLong(haystack, needle string, skipRunes int) bool {
	haystackIterator := newIndexFoldFallbackIterator(haystack)
	for range skipRunes {
		if _, _, _, ok := haystackIterator.next(); !ok {
			return false
		}
	}

	needleIterator := newIndexFoldFallbackIterator(needle)
	for {
		needleRune, _, _, haveNeedle := needleIterator.next()
		if !haveNeedle {
			return true
		}
		haystackRune, _, _, haveHaystack := haystackIterator.next()
		if !haveHaystack || needleRune != haystackRune {
			return false
		}
	}
}
