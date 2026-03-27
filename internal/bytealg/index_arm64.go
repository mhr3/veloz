//go:build !noasm && arm64

package bytealg

//go:noescape
func indexExact1Byte(haystack string, needle string, off1 int) uint64

//go:noescape
func indexExact2Byte(haystack string, needle string, off1 int, off2Delta int) uint64

//go:noescape
func indexExactRabinKarp(haystack string, needle string) int

// Index finds the first case-sensitive match of needle in haystack.
// Uses staged SIMD kernels with adaptive rare-byte filtering.
func Index(haystack, needle string) int {
	n := len(needle)
	if n == 0 {
		return 0
	}
	if len(haystack) < n {
		return -1
	}

	// Quick check for position-0 match - avoids SIMD setup overhead
	if haystack[0] == needle[0] && haystack[:n] == needle {
		return 0
	}

	// Use first + last byte (max spread), or first + middle if first==last
	first := needle[0]
	last := needle[n-1]
	off2 := n - 1
	if n > 2 && first == last {
		off2 = n / 2
	}

	// For long needles with common filter bytes, use Rabin-Karp directly.
	if n > 64 && byteRank[first] > 180 && byteRank[needle[off2]] > 180 {
		return indexExactRabinKarp(haystack, needle)
	}

	// Skip 1-byte filter for pathological patterns:
	// - first byte is very common (rank > 240: space, e, t, a, i, n, s, o, l, r)
	// - first == last AND first is moderately common (rank > 160)
	skip1Byte := byteRank[first] > 240 || (first == last && byteRank[first] > 160)

	var result uint64
	var resumePos int

	if !skip1Byte {
		// Stage 1: 1-byte filter (fast scan, adaptive threshold)
		result = indexExact1Byte(haystack, needle, 0)
		if !resultExceeded(result) {
			return resultPosition(result)
		}
		resumePos = resultPosition(result)
		if resumePos > 0 {
			haystack = haystack[resumePos:]
		}
	}

	// Stage 2: 2-byte filter (more selective)
	result = indexExact2Byte(haystack, needle, 0, off2)
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

	pos := indexExactRabinKarp(haystack, needle)
	if pos >= 0 {
		return pos + resumePos
	}
	return -1
}
