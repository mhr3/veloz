// Package ascii has SIMD routines for ASCII text. A function takes its text
// as a []byte, and has a form with a String suffix that takes a string.
package ascii

// ByteSet represents a precomputed byte set for fast lookups.
// Build once with NewByteSet, then reuse with bs.Index().
type ByteSet struct {
	bitset [4]uint64
}

// NewByteSet creates a ByteSet from the given characters.
func NewByteSet(chars string) ByteSet {
	var bs ByteSet
	for i := 0; i < len(chars); i++ {
		c := chars[i]
		bs.bitset[c>>6] |= 1 << (c & 63)
	}
	return bs
}

// Match reports whether any byte in s is in the ByteSet.
func (bs ByteSet) Match(s []byte) bool {
	return bs.Index(s) >= 0
}

// MatchString reports whether any byte in s is in the ByteSet.
func (bs ByteSet) MatchString(s string) bool {
	return bs.IndexString(s) >= 0
}

// IndexAny finds the first occurrence of any byte from chars in s.
// Returns -1 if there is none.
func IndexAny(s []byte, chars string) int {
	return NewByteSet(chars).Index(s)
}

// IndexAnyString finds the first occurrence of any byte from chars in s.
// Returns -1 if there is none.
func IndexAnyString(s, chars string) int {
	return NewByteSet(chars).IndexString(s)
}

// ContainsAny reports whether any byte from chars is in data.
func ContainsAny(data []byte, chars string) bool {
	return IndexAny(data, chars) >= 0
}

// ContainsAnyString reports whether any byte from chars is in data.
func ContainsAnyString(data, chars string) bool {
	return IndexAnyString(data, chars) >= 0
}

// IndexNonASCII finds the first non-ASCII byte (>= 0x80) in data.
// Returns -1 if all bytes are ASCII.
func IndexNonASCII(data []byte) int {
	return IndexMask(data, 0x80)
}

// IndexNonASCIIString finds the first non-ASCII byte (>= 0x80) in data.
// Returns -1 if all bytes are ASCII.
func IndexNonASCIIString(data string) int {
	return IndexMaskString(data, 0x80)
}

// indexAnyGo scans s for any byte in the 256-bit bitset.
func indexAnyGo[T string | []byte](s T, bitset *[4]uint64) int {
	for i := 0; i < len(s); i++ {
		c := s[i]
		if bitset[c>>6]&(1<<(c&63)) != 0 {
			return i
		}
	}
	return -1
}

func HasPrefixFold(s, prefix []byte) bool {
	if len(s) < len(prefix) {
		return false
	}
	return EqualFold(s[:len(prefix)], prefix)
}

func HasPrefixFoldString(s, prefix string) bool {
	if len(s) < len(prefix) {
		return false
	}
	return EqualFoldString(s[:len(prefix)], prefix)
}

func HasSuffixFold(s, suffix []byte) bool {
	if len(s) < len(suffix) {
		return false
	}
	return EqualFold(s[len(s)-len(suffix):], suffix)
}

func HasSuffixFoldString(s, suffix string) bool {
	if len(s) < len(suffix) {
		return false
	}
	return EqualFoldString(s[len(s)-len(suffix):], suffix)
}
