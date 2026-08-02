package utf8

import (
	"unicode"
	stdutf8 "unicode/utf8"
)

// equalFoldRune compares a and b runes whether they fold equally.
//
// The code comes from strings.EqualFold, but shortened to only one rune.
func equalFoldRune(sr, tr rune) bool {
	if sr == tr {
		return true
	}
	// Make sr < tr to simplify what follows.
	if tr < sr {
		sr, tr = tr, sr
	}
	// Fast check for ASCII.
	if tr < stdutf8.RuneSelf && 'A' <= sr && sr <= 'Z' {
		// ASCII, and sr is upper case.  tr must be lower case.
		if tr == sr+'a'-'A' {
			return true
		}
		return false
	}

	// General case.  SimpleFold(x) returns the next equivalent rune > x
	// or wraps around to smaller values.
	r := unicode.SimpleFold(sr)
	for r != sr && r < tr {
		r = unicode.SimpleFold(r)
	}
	if r == tr {
		return true
	}
	return false
}

func hasPrefixFoldSimple(s, prefix string) bool {
	if prefix == "" {
		return true
	}
	for _, pr := range prefix {
		if s == "" {
			return false
		}
		// step with s, too
		sr, size := stdutf8.DecodeRuneInString(s)
		if sr == stdutf8.RuneError {
			return false
		}
		s = s[size:]
		if !equalFoldRune(sr, pr) {
			return false
		}
	}
	return true
}

func indexFoldSimple(haystack, needle string) int {
	if needle == "" {
		return 0
	}
	if haystack == "" {
		return -1
	}
	firstRune := rune(needle[0])
	if firstRune >= stdutf8.RuneSelf {
		firstRune, _ = stdutf8.DecodeRuneInString(needle)
	}
	for i, rune := range haystack {
		if equalFoldRune(rune, firstRune) && hasPrefixFoldSimple(haystack[i:], needle) {
			return i
		}
	}
	return -1
}
