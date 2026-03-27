//go:build !noasm && arm64

package ascii

import "github.com/mhr3/veloz/internal/bytealg"

// Searcher performs fast repeated substring searches.
// Construct once with NewSearcher, then call Index on multiple haystacks.
// Amortizes pattern analysis cost across many searches.
type Searcher struct {
	raw           string // original pattern
	norm          string // lowercase pattern (for case-insensitive verification)
	rare1         byte   // first rare byte (lowercase for case-insensitive)
	off1          int    // offset in pattern
	rare2         byte   // second rare byte (lowercase for case-insensitive)
	off2          int    // offset in pattern
	caseSensitive bool   // if true, use exact matching
}

// NewSearcher creates a Searcher for repeated substring searches.
// If caseSensitive is false, searches are case-insensitive (ASCII letters only).
func NewSearcher(pattern string, caseSensitive bool) Searcher {
	rare1, off1, rare2, off2 := selectRarePair(pattern, nil, caseSensitive)
	norm := ""
	if !caseSensitive {
		norm = normalizeASCII(pattern)
	}
	return Searcher{
		raw:           pattern,
		norm:          norm,
		rare1:         rare1,
		off1:          off1,
		rare2:         rare2,
		off2:          off2,
		caseSensitive: caseSensitive,
	}
}

// NewSearcherWithRanks creates a Searcher using a custom byte frequency table.
func NewSearcherWithRanks(pattern string, ranks []byte, caseSensitive bool) Searcher {
	if len(ranks) != 256 {
		panic("ranks must have exactly 256 entries")
	}
	rare1, off1, rare2, off2 := selectRarePair(pattern, ranks, caseSensitive)
	norm := ""
	if !caseSensitive {
		norm = normalizeASCII(pattern)
	}
	return Searcher{
		raw:           pattern,
		norm:          norm,
		rare1:         rare1,
		off1:          off1,
		rare2:         rare2,
		off2:          off2,
		caseSensitive: caseSensitive,
	}
}

// Assembly kernel declarations for Searcher (use pre-computed offsets)

//go:noescape
func indexFold1Byte(haystack string, needle string, off1 int) uint64

//go:noescape
func indexFold2Byte(haystack string, needle string, off1 int, off2Delta int) uint64

//go:noescape
func indexPrefoldedRabinKarp(haystack string, needle string) int

// Index finds the first occurrence of the pattern in haystack.
func (s Searcher) Index(haystack string) int {
	if len(s.raw) == 0 {
		return 0
	}
	if len(haystack) < len(s.raw) {
		return -1
	}

	if s.caseSensitive {
		return indexExactWithOffsets(haystack, s.raw, s.off1, s.off2)
	}
	return indexFoldWithOffsets(haystack, s.norm, s.off1, s.off2)
}

func indexFoldWithOffsets(haystack, normNeedle string, off1, off2 int) int {
	skip1Byte := off1 == off2

	var result uint64
	var resumePos int

	if !skip1Byte {
		result = indexFold1Byte(haystack, normNeedle, off1)
		if !resultExceeded(result) {
			return resultPosition(result)
		}
		resumePos = resultPosition(result)
		if resumePos > 0 {
			haystack = haystack[resumePos:]
		}
	}

	result = indexFold2Byte(haystack, normNeedle, off1, off2-off1)
	if !resultExceeded(result) {
		pos := resultPosition(result)
		if pos >= 0 {
			return pos + resumePos
		}
		return -1
	}

	resumePos2 := resultPosition(result)
	if resumePos2 > 0 {
		haystack = haystack[resumePos2:]
		resumePos += resumePos2
	}

	var pos int
	if len(normNeedle) <= 8 {
		pos = indexFoldBruteForce(haystack, normNeedle)
	} else {
		pos = indexPrefoldedRabinKarp(haystack, normNeedle)
	}
	if pos >= 0 {
		return pos + resumePos
	}
	return -1
}

func indexFoldBruteForce(haystack, normNeedle string) int {
	n := len(normNeedle)
	for i := 0; i <= len(haystack)-n; i++ {
		if EqualFold(haystack[i:i+n], normNeedle) {
			return i
		}
	}
	return -1
}

func indexExactWithOffsets(haystack, needle string, off1, off2 int) int {
	// For case-sensitive search, delegate to bytealg.Index which has the
	// optimized staged SIMD implementation.
	return bytealg.Index(haystack, needle)
}
