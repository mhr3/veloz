package ascii

import "math/bits"

// byteRank is a frequency table for bytes based on corpus analysis.
// Lower rank = rarer byte = better candidate for rare-byte search.
// Derived from BYTE_FREQUENCIES table (corpus: CIA World Factbook,
// rustc source, Septuaginta). UTF-8 prefix bytes (0xC0-0xFF) forced to 255
// since continuation bytes are more discriminating.
var byteRank = [256]byte{
	// 0x00-0x0F: control characters (mostly rare)
	55, 52, 51, 50, 49, 48, 47, 46, 45, 103, 242, 66, 67, 229, 44, 43,
	// 0x10-0x1F: more control characters
	42, 41, 40, 39, 38, 37, 36, 35, 34, 33, 56, 32, 31, 30, 29, 28,
	// 0x20-0x2F: space and punctuation
	255, // ' ' - most common
	148, // '!'
	164, // '"' - common in JSON
	149, // '#'
	136, // '$'
	160, // '%'
	155, // '&'
	173, // '\''
	221, // '('
	222, // ')'
	134, // '*'
	122, // '+'
	232, // ',' - common
	202, // '-'
	215, // '.'
	224, // '/'
	// 0x30-0x39: digits
	208, // '0'
	220, // '1'
	204, // '2'
	187, // '3'
	183, // '4'
	179, // '5'
	177, // '6'
	168, // '7'
	178, // '8'
	200, // '9'
	// 0x3A-0x40: more punctuation
	226, // ':' - common in JSON
	195, // ';'
	154, // '<'
	184, // '='
	174, // '>'
	126, // '?'
	120, // '@'
	// 0x41-0x5A: uppercase A-Z
	191, // 'A'
	157, // 'B'
	194, // 'C'
	170, // 'D'
	189, // 'E'
	162, // 'F'
	161, // 'G'
	150, // 'H'
	193, // 'I'
	142, // 'J'
	137, // 'K'
	171, // 'L'
	176, // 'M'
	185, // 'N'
	167, // 'O'
	186, // 'P'
	112, // 'Q' - rare
	175, // 'R'
	192, // 'S'
	188, // 'T'
	156, // 'U'
	140, // 'V'
	143, // 'W'
	123, // 'X' - rare
	133, // 'Y'
	128, // 'Z' - rare
	// 0x5B-0x60: brackets and punctuation
	147, // '['
	138, // '\\'
	146, // ']'
	114, // '^'
	223, // '_'
	151, // '`'
	// 0x61-0x7A: lowercase a-z
	249, // 'a'
	216, // 'b'
	238, // 'c'
	236, // 'd'
	253, // 'e' - most common letter
	227, // 'f'
	218, // 'g'
	230, // 'h'
	247, // 'i'
	135, // 'j' - rare
	180, // 'k'
	241, // 'l'
	233, // 'm'
	246, // 'n'
	244, // 'o'
	231, // 'p'
	139, // 'q' - rare
	245, // 'r'
	243, // 's'
	251, // 't'
	235, // 'u'
	201, // 'v'
	196, // 'w'
	240, // 'x'
	214, // 'y'
	152, // 'z' - rare
	// 0x7B-0x7F: braces and control
	182, // '{'
	205, // '|'
	181, // '}'
	127, // '~'
	27,  // DEL
	// 0x80-0xBF: UTF-8 continuation bytes (varied frequency in real UTF-8 text)
	212, 211, 210, 213, 228, 197, 169, 159, 131, 172, 105, 80, 98, 96, 97, 81,
	207, 145, 116, 115, 144, 130, 153, 121, 107, 132, 109, 110, 124, 111, 82, 108,
	118, 141, 113, 129, 119, 125, 165, 117, 92, 106, 83, 72, 99, 93, 65, 79,
	166, 237, 163, 199, 190, 225, 209, 203, 198, 217, 219, 206, 234, 248, 158, 239,
	// 0xC0-0xFF: UTF-8 prefix bytes (force to 255 = most common)
	255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255,
	255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255,
	255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255,
	255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255,
}

// toLower converts ASCII uppercase to lowercase.
func toLower(b byte) byte {
	if b >= 'A' && b <= 'Z' {
		return b + 0x20
	}
	return b
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
// If caseSensitive is true, uses byte ranks directly.
// If caseSensitive is false, uses case-folded ranks (A and a have same rank).
// If ranks is nil, uses the default frequency table.
func getRankTable(ranks []byte, caseSensitive bool) *[256]uint16 {
	if caseSensitive {
		// Case-sensitive: use byte ranks directly
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
	// Case-insensitive: use case-folded ranks
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
// Returns the two rarest bytes and their offsets, with off1 < off2.
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
	// (same as Index function). This maximizes spread and avoids corpus assumptions.
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

	// Scan all positions to find the two rarest distinct bytes
	best1Byte, best2Byte := normalize(pattern[0]), byte(0)
	best1Off, best2Off := 0, -1
	best1Rank := rankTable[best1Byte]
	best2Rank := uint16(0xFFFF)

	for i := 1; i < n; i++ {
		c := normalize(pattern[i])
		r := rankTable[c]
		if r < best1Rank {
			// New rarest - shift old best1 to best2 if different byte
			if c != best1Byte {
				best2Byte, best2Off, best2Rank = best1Byte, best1Off, best1Rank
			}
			best1Byte, best1Off, best1Rank = c, i, r
		} else if c != best1Byte && r < best2Rank {
			// New second-rarest (must be different byte from best1)
			best2Byte, best2Off, best2Rank = c, i, r
		}
	}

	// Fallback if no distinct second byte found
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

// NewSearcher creates a Searcher for repeated substring searches.
// If caseSensitive is false, searches are case-insensitive (ASCII letters only).
// Uses O(n) full scan for optimal rare byte selection (cost amortized over many searches).
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

// BuildRankTable builds a byte frequency table from a corpus sample.
// Lower rank values indicate rarer bytes (better for filtering).
// Pass the result to NewSearcherWithRanks for corpus-optimized searching.
//
// The function case-folds letters (a-z → A-Z) so the table works for both
// case-sensitive and case-insensitive searches.
func BuildRankTable(corpus string) [256]byte {
	var counts [256]int
	for i := 0; i < len(corpus); i++ {
		c := corpus[i]
		if c >= 'a' && c <= 'z' {
			c -= 0x20 // uppercase
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

// NewSearcherWithRanks creates a Searcher using a custom byte frequency table.
// The ranks slice must have 256 entries where lower values indicate rarer bytes.
// If caseSensitive is false, searches are case-insensitive (ASCII letters only).
//
// Use this for specialized corpora where byte frequencies differ from English:
//   - JSON logs (where ", :, { are common)
//   - DNA sequences (A, C, G, T equally common)
//   - Hex dumps (0-9, A-F equally common)
//
// Build a rank table with BuildRankTable:
//
//	ranks := ascii.BuildRankTable(corpusSample)
//	searcher := ascii.NewSearcherWithRanks(`"trace_id":`, ranks[:], true)
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

func indexMaskGo[T string | []byte](s T, mask byte) int {
	mask32 := uint32(mask)
	mask32 |= mask32 << 8
	mask32 |= mask32 << 16

	pos := 0
	// use all go tricks to make this fast
	for ; len(s) >= 8; pos, s = pos+8, s[8:] {
		_ = s[7]
		first32 := uint32(s[0]) | uint32(s[1])<<8 | uint32(s[2])<<16 | uint32(s[3])<<24
		second32 := uint32(s[4]) | uint32(s[5])<<8 | uint32(s[6])<<16 | uint32(s[7])<<24
		if (first32|second32)&mask32 != 0 {
			first32 &= mask32
			if first32 != 0 {
				return pos + bits.TrailingZeros32(first32)/8
			}
			second32 &= mask32
			return pos + 4 + bits.TrailingZeros32(second32)/8
		}
	}

	for i := 0; i < len(s); i++ {
		b := s[i]
		if b&mask != 0 {
			return pos + i
		}
	}
	return -1
}

func isAsciiGo[T string | []byte](s T) bool {
	return indexMaskGo(s, 0x80) == -1
}

// based on https://graphics.stanford.edu/~seander/bithacks.html#HasBetweenInWord
func hasLowercaseAsciiByte(x uint64) uint64 {
	const mult = ^uint64(0) / 255
	const m, n = 'a' - 1, 'z' + 1

	A := mult * (127 + n)
	B := x & (mult * 127)
	C := ^x
	D := mult * (127 - m)
	return (A - B) & C & (B + D) & (mult * 128)
}

func asciiFoldWord(x uint64) uint64 {
	mask := hasLowercaseAsciiByte(x)
	mask >>= 2
	return x - mask
}

func equalFoldGo(a, b string) bool {
	if len(a) != len(b) {
		return false
	}

	for len(a) >= 8 {
		_ = a[7]
		_ = b[7]

		// the compiler should be able to optimize this to two 64-bit loads
		a64 := uint64(a[0]) | uint64(a[1])<<8 | uint64(a[2])<<16 | uint64(a[3])<<24 |
			uint64(a[4])<<32 | uint64(a[5])<<40 | uint64(a[6])<<48 | uint64(a[7])<<56
		b64 := uint64(b[0]) | uint64(b[1])<<8 | uint64(b[2])<<16 | uint64(b[3])<<24 |
			uint64(b[4])<<32 | uint64(b[5])<<40 | uint64(b[6])<<48 | uint64(b[7])<<56

		if a64 != b64 {
			a64 = asciiFoldWord(a64)
			b64 = asciiFoldWord(b64)
			if a64 != b64 {
				return false
			}
		}
		a = a[8:]
		b = b[8:]
	}

	var a0, a1, b0, b1 uint32
	switch len(a) {
	case 7:
		fallthrough
	case 6:
		fallthrough
	case 5:
		// get the data using four 32-bit loads
		_, _ = a[3], b[3]
		a0 = uint32(a[0]) | uint32(a[1])<<8 | uint32(a[2])<<16 | uint32(a[3])<<24
		b0 = uint32(b[0]) | uint32(b[1])<<8 | uint32(b[2])<<16 | uint32(b[3])<<24

		idx := len(a) - 4
		a, b = a[idx:], b[idx:]
		_, _ = a[3], b[3]
		a1 = uint32(a[0]) | uint32(a[1])<<8 | uint32(a[2])<<16 | uint32(a[3])<<24
		b1 = uint32(b[0]) | uint32(b[1])<<8 | uint32(b[2])<<16 | uint32(b[3])<<24
	case 4:
		_ = b[3]
		a0 = uint32(a[0]) | uint32(a[1])<<8 | uint32(a[2])<<16 | uint32(a[3])<<24
		b0 = uint32(b[0]) | uint32(b[1])<<8 | uint32(b[2])<<16 | uint32(b[3])<<24
	case 3:
		a0 = uint32(a[0]) | uint32(a[1])<<8 | uint32(a[2])<<16
		_ = b[2]
		b0 = uint32(b[0]) | uint32(b[1])<<8 | uint32(b[2])<<16
	case 2:
		a0 = uint32(a[0]) | uint32(a[1])<<8
		_ = b[1]
		b0 = uint32(b[0]) | uint32(b[1])<<8
	case 1:
		a0 = uint32(a[0])
		b0 = uint32(b[0])
	case 0:
		return true
	}

	a64 := uint64(a0) | uint64(a1)<<32
	b64 := uint64(b0) | uint64(b1)<<32
	if a64 == b64 {
		return true
	}

	return asciiFoldWord(a64) == asciiFoldWord(b64)
}

func indexFoldGo[T string | []byte](s T, substr T) int {
	if len(substr) == 0 {
		return 0
	} else if len(substr) > len(s) {
		return -1
	}

	first := substr[0]
	complement := first
	if first >= 'A' && first <= 'Z' {
		complement += 0x20
	} else if first >= 'a' && first <= 'z' {
		complement -= 0x20
	}

	for i := 0; i <= len(s)-len(substr); i++ {
		b := byte(s[i])
		if b == first || b == complement {
			prefix := s[i:]
			if equalFoldGo(string(prefix[:len(substr)]), string(substr)) {
				return i
			}
		}
	}
	return -1
}

// indexFoldRabinKarpGo is a scalar Rabin-Karp for case-insensitive search.
// Uses rolling hash with antisigma trick, calls into SIMD EqualFold for verification.
// Kept for benchmarking comparison against SIMD implementations.
func indexFoldRabinKarpGo(haystack, needle string) int {
	n := len(needle)
	if n == 0 {
		return 0
	}
	h := len(haystack)
	if h < n {
		return -1
	}

	// Precompute constants
	searchLen := h - n + 1
	powW := powPrimeScalar(n)
	antisigma := -powW // -B^w mod 2^32 (unsigned overflow is fine)

	// Compute needle hash (case-folded)
	var targetHash uint32
	for i := 0; i < n; i++ {
		targetHash = targetHash*primeRK + uint32(foldTable[needle[i]])
	}

	// Compute initial hash at position 0
	var hash uint32
	for i := 0; i < n; i++ {
		hash = hash*primeRK + uint32(foldTable[haystack[i]])
	}

	// Check position 0
	if hash == targetHash && EqualFold(haystack[:n], needle) {
		return 0
	}

	// Roll through remaining positions
	for i := 1; i < searchLen; i++ {
		// Rolling hash: hash = hash*primeRK + new_char + old_char*antisigma
		hash = hash*primeRK + uint32(foldTable[haystack[i+n-1]]) + antisigma*uint32(foldTable[haystack[i-1]])
		if hash == targetHash && EqualFold(haystack[i:i+n], needle) {
			return i
		}
	}

	return -1
}

// foldTable is a lookup table for case folding: 'a'-'z' -> 'A'-'Z', others unchanged.
var foldTable = [256]byte{
	0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17, 18, 19, 20, 21, 22, 23, 24, 25, 26, 27, 28, 29, 30, 31,
	32, 33, 34, 35, 36, 37, 38, 39, 40, 41, 42, 43, 44, 45, 46, 47, 48, 49, 50, 51, 52, 53, 54, 55, 56, 57, 58, 59, 60, 61, 62, 63,
	64, 65, 66, 67, 68, 69, 70, 71, 72, 73, 74, 75, 76, 77, 78, 79, 80, 81, 82, 83, 84, 85, 86, 87, 88, 89, 90, 91, 92, 93, 94, 95,
	96, 65, 66, 67, 68, 69, 70, 71, 72, 73, 74, 75, 76, 77, 78, 79, 80, 81, 82, 83, 84, 85, 86, 87, 88, 89, 90, 123, 124, 125, 126, 127,
	128, 129, 130, 131, 132, 133, 134, 135, 136, 137, 138, 139, 140, 141, 142, 143, 144, 145, 146, 147, 148, 149, 150, 151, 152, 153, 154, 155, 156, 157, 158, 159,
	160, 161, 162, 163, 164, 165, 166, 167, 168, 169, 170, 171, 172, 173, 174, 175, 176, 177, 178, 179, 180, 181, 182, 183, 184, 185, 186, 187, 188, 189, 190, 191,
	192, 193, 194, 195, 196, 197, 198, 199, 200, 201, 202, 203, 204, 205, 206, 207, 208, 209, 210, 211, 212, 213, 214, 215, 216, 217, 218, 219, 220, 221, 222, 223,
	224, 225, 226, 227, 228, 229, 230, 231, 232, 233, 234, 235, 236, 237, 238, 239, 240, 241, 242, 243, 244, 245, 246, 247, 248, 249, 250, 251, 252, 253, 254, 255,
}

// primeRK is the same prime used by Go stdlib for Rabin-Karp
const primeRK = 16777619

// powPrimeScalar computes primeRK^n mod 2^32 using repeated squaring.
func powPrimeScalar(n int) uint32 {
	result := uint32(1)
	base := uint32(primeRK)
	for n > 0 {
		if n&1 != 0 {
			result *= base
		}
		base *= base
		n >>= 1
	}
	return result
}

func indexAnyGo(s, chars string) int {
	if len(chars) == 0 {
		return -1
	}
	// Build 256-bit set (8 uint32s) for O(1) lookup per byte
	var set [8]uint32
	for i := 0; i < len(chars); i++ {
		c := chars[i]
		set[c>>5] |= 1 << (c & 31)
	}
	// O(n) scan
	for i := 0; i < len(s); i++ {
		c := s[i]
		if set[c>>5]&(1<<(c&31)) != 0 {
			return i
		}
	}
	return -1
}
