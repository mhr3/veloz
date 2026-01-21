//go:build !noasm && arm64

package ascii

// Staged NEON kernels for case-insensitive substring search.
// Architecture:
//   Stage 1: 1-byte filter (rare byte at off1)
//   Stage 2: 2-byte filter (rare bytes at off1 and off2)
//   Stage 3: SIMD Rabin-Karp (guaranteed linear)

// Assembly kernel declarations

//go:noescape
func indexFold1ByteRaw(haystack string, needle string, off1 int) uint64

//go:noescape
func indexFold2ByteRaw(haystack string, needle string, off1 int, off2Delta int) uint64

const (
	exceededFlag = 1 << 63
)

func resultExceeded(r uint64) bool {
	// RESULT_NOT_FOUND is 0xFFFFFFFFFFFFFFFF which has bit 63 set,
	// but it's NOT an exceeded result - it means "not found"
	if r == ^uint64(0) {
		return false
	}
	return r&exceededFlag != 0
}

func resultPosition(r uint64) int {
	pos := int64(r &^ exceededFlag)
	if pos == int64(^uint64(0)&^exceededFlag) {
		return -1
	}
	return int(pos)
}

// IndexFold finds the first case-insensitive match of needle in haystack.
// Uses staged SIMD kernels with adaptive rare-byte filtering.
func IndexFold(haystack, needle string) int {
	n := len(needle)
	if n == 0 {
		return 0
	}
	if len(haystack) < n {
		return -1
	}

	// Quick check for position-0 match - avoids SIMD setup overhead
	first := toLower(needle[0])
	if toLower(haystack[0]) == first && EqualFold(haystack[:n], needle) {
		return 0
	}

	// Find the two rarest bytes in needle for filtering.
	off1, off2 := findRarePairForFilter(needle)
	filterByte1 := toLower(needle[off1])

	// For long needles with common filter bytes, use Rabin-Karp directly.
	if n > 64 && byteRank[filterByte1] > 180 {
		return indexFoldRabinKarp(haystack, needle)
	}

	// Skip 1-byte filter:
	// - small inputs (< 2KB): 2-byte filter is more robust
	// - filter byte is very common (rank > 200): too many false positives
	skip1Byte := len(haystack) < 2048 || byteRank[filterByte1] > 200

	var result uint64
	var resumePos int

	if !skip1Byte {
		// Stage 1: 1-byte filter on rarest byte
		result = indexFold1ByteRaw(haystack, needle, off1)
		if !resultExceeded(result) {
			return resultPosition(result)
		}
		resumePos = resultPosition(result)
		if resumePos > 0 {
			haystack = haystack[resumePos:]
		}
	}

	// Stage 2: 2-byte filter using both rare bytes
	result = indexFold2ByteRaw(haystack, needle, off1, off2-off1)
	if !resultExceeded(result) {
		pos := resultPosition(result)
		if pos >= 0 {
			return pos + resumePos
		}
		return -1
	}

	// Stage 3: Rabin-Karp fallback
	resumePos2 := resultPosition(result)
	if resumePos2 > 0 {
		haystack = haystack[resumePos2:]
		resumePos += resumePos2
	}

	pos := indexFoldRabinKarp(haystack, needle)
	if pos >= 0 {
		return pos + resumePos
	}
	return -1
}

// findRarePairForFilter finds two rare byte offsets for SIMD filtering.
// Returns (off1, off2) where off1 <= off2, picking bytes with lowest byteRank.
func findRarePairForFilter(needle string) (off1, off2 int) {
	n := len(needle)
	if n <= 2 {
		return 0, n - 1
	}

	// Find the two rarest distinct bytes using byteRank
	off1, off2 = 0, n-1
	best1Rank := byteRank[toLower(needle[0])]
	best2Rank := byte(255)
	best1Char := toLower(needle[0])

	for i := 1; i < n; i++ {
		c := toLower(needle[i])
		r := byteRank[c]
		if r < best1Rank {
			// New rarest - demote old best1 to best2 if different char
			if c != best1Char {
				off2, best2Rank = off1, best1Rank
			}
			off1, best1Rank, best1Char = i, r, c
		} else if c != best1Char && r < best2Rank {
			off2, best2Rank = i, r
		}
	}

	// Ensure off1 <= off2 for positive delta
	if off1 > off2 {
		off1, off2 = off2, off1
		best1Rank, best2Rank = best2Rank, best1Rank
	}

	// If offsets are adjacent and both bytes are common, use first+last spread instead.
	if off2-off1 <= 1 && best1Rank > 200 && best2Rank > 200 {
		return 0, n - 1
	}

	return off1, off2
}
