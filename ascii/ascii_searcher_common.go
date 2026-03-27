package ascii

// caseFoldRank is a case-insensitive rank table for rare-byte selection.
// For letters, rank = rankUpper + rankLower (sum models P(upper OR lower)).
// For non-letters, rank = original rank.
// Lower value = rarer = better for filtering.
var caseFoldRank [256]uint16

func init() {
	// Initialize with original ranks
	for b := 0; b < 256; b++ {
		caseFoldRank[b] = uint16(byteRank[b])
	}
	// For letters, use sum of upper+lower ranks (models case-insensitive frequency)
	for b := byte('A'); b <= 'Z'; b++ {
		lower := b + 0x20
		sum := uint16(byteRank[b]) + uint16(byteRank[lower])
		caseFoldRank[b] = sum
		caseFoldRank[lower] = sum
	}
}

// normalizeASCII converts a string to lowercase ASCII.
func normalizeASCII(s string) string {
	for i := 0; i < len(s); i++ {
		if s[i] >= 'A' && s[i] <= 'Z' {
			goto normalize
		}
	}
	return s

normalize:
	b := make([]byte, len(s))
	for i := 0; i < len(s); i++ {
		b[i] = toLower(s[i])
	}
	return string(b)
}

// getRankTable returns the appropriate rank table for rare byte selection.
func getRankTable(ranks []byte, caseSensitive bool) *[256]uint16 {
	if caseSensitive {
		var directRanks [256]uint16
		if ranks == nil {
			for i := 0; i < 256; i++ {
				directRanks[i] = uint16(byteRank[i])
			}
		} else {
			for i := 0; i < 256; i++ {
				directRanks[i] = uint16(ranks[i])
			}
		}
		return &directRanks
	}
	if ranks == nil {
		return &caseFoldRank
	}
	var customFolded [256]uint16
	for i := 0; i < 256; i++ {
		customFolded[i] = uint16(ranks[i])
	}
	for b := byte('A'); b <= 'Z'; b++ {
		lower := b + 0x20
		sum := uint16(ranks[b]) + uint16(ranks[lower])
		customFolded[b] = sum
		customFolded[lower] = sum
	}
	return &customFolded
}

// selectRarePair finds two rare bytes by scanning the entire pattern.
// O(n) complexity - used by NewSearcher where cost is amortized over many searches.
func selectRarePair(pattern string, ranks []byte, caseSensitive bool) (rare1 byte, off1 int, rare2 byte, off2 int) {
	n := len(pattern)
	if n == 0 {
		return 0, 0, 0, 0
	}

	if n == 1 {
		b := pattern[0]
		if !caseSensitive {
			b = toLower(b)
		}
		return b, 0, b, 0
	}

	// For case-sensitive search without corpus ranks, use first+last byte strategy
	if caseSensitive && ranks == nil {
		first := pattern[0]
		last := pattern[n-1]
		off2 = n - 1
		if n > 2 && first == last {
			off2 = n / 2
		}
		return first, 0, pattern[off2], off2
	}

	normalize := toLower
	if caseSensitive {
		normalize = func(b byte) byte { return b }
	}

	rankTable := getRankTable(ranks, caseSensitive)

	best1Byte, best2Byte := normalize(pattern[0]), byte(0)
	best1Off, best2Off := 0, -1
	best1Rank := rankTable[best1Byte]
	best2Rank := uint16(0xFFFF)

	for i := 1; i < n; i++ {
		c := normalize(pattern[i])
		r := rankTable[c]
		if r < best1Rank {
			if c != best1Byte {
				best2Byte, best2Off, best2Rank = best1Byte, best1Off, best1Rank
			}
			best1Byte, best1Off, best1Rank = c, i, r
		} else if c != best1Byte && r < best2Rank {
			best2Byte, best2Off, best2Rank = c, i, r
		}
	}

	if best2Off == -1 {
		return normalize(pattern[0]), 0, normalize(pattern[n-1]), n - 1
	}

	off1, off2 = best1Off, best2Off
	rare1, rare2 = best1Byte, best2Byte

	if off1 > off2 {
		off1, off2 = off2, off1
		rare1, rare2 = rare2, rare1
	}

	return rare1, off1, rare2, off2
}

// BuildRankTable builds a byte frequency table from a corpus sample.
func BuildRankTable(corpus string) [256]byte {
	var counts [256]int
	for i := 0; i < len(corpus); i++ {
		c := corpus[i]
		if c >= 'a' && c <= 'z' {
			c -= 0x20
		}
		counts[c]++
	}

	maxCount := 1
	for _, c := range counts {
		if c > maxCount {
			maxCount = c
		}
	}

	var ranks [256]byte
	for i := range ranks {
		ranks[i] = byte((counts[i] * 255) / maxCount)
	}
	return ranks
}
