package ascii

import (
	"fmt"
	"math/rand"
	"strings"
	"testing"
	"unicode"

	segAscii "github.com/segmentio/asm/ascii"
)

func makeASCII(n int) []byte {
	data := make([]byte, n)
	for i := range data {
		data[i] = byte(rand.Uint32() & 0x7f)
	}
	return data
}

type ValidTest struct {
	in  string
	exp bool
}

var validTests = []ValidTest{
	{"", true},
	{"a", true},
	{"abc", true},
	{"Ж", false},
	{"ЖЖ", false},
	{"брэд-ЛГТМ", false},
	{"☺☻☹", false},
	{"aa\xe2", false},
	{string([]byte{66, 250}), false},
	{string([]byte{66, 250, 67}), false},
	{"a\uFFFDb", false},
	{string("\xF4\x8F\xBF\xBF"), false},     // U+10FFFF
	{string("\xF4\x90\x80\x80"), false},     // U+10FFFF+1; exp of range
	{string("\xF7\xBF\xBF\xBF"), false},     // 0x1FFFFF; exp of range
	{string("\xFB\xBF\xBF\xBF\xBF"), false}, // 0x3FFFFFF; exp of range
	{string("\xc0\x80"), false},             // U+0000 encoded in two bytes: incorrect
	{string("\xed\xa0\x80"), false},         // U+D800 high surrogate (sic)
	{string("\xed\xbf\xbf"), false},         // U+DFFF low surrogate (sic)
	{"hellowo\xff", false},
	{"hellowor", true},
}

func TestAscii(t *testing.T) {
	for _, vt := range validTests {
		if ValidString(vt.in) != vt.exp {
			t.Errorf("ValidString(%q) = %v; want %v", vt.in, !vt.exp, vt.exp)
		}
	}

	for _, vt := range validTests {
		pt := "0123456789ab" + vt.in
		if ValidString(pt) != vt.exp {
			t.Errorf("ValidString(%q) = %v; want %v", pt, !vt.exp, vt.exp)
		}
	}
}

func TestIndexMask(t *testing.T) {
	for i := 4; i < 6400; i++ {
		data := makeASCII(i)
		if ValidString(string(data)) != true {
			t.Errorf("ValidString(%q) = false; want true", data)
		}
		if res := IndexMask(string(data), 0x80); res != -1 {
			t.Errorf("IndexMask([%d]) = %d; want %d", len(data), res, -1)
		}

		idx := rand.Intn(i)
		data[idx] |= 0x80
		if ValidString(string(data)) != false {
			t.Errorf("ValidString(%q) = true; want false", data)
		}
		if res := IndexMask(string(data), 0x80); res != idx {
			t.Errorf("IndexMask([%d]) = %d; want %d", len(data), res, idx)
		}
	}
}

func containsFold(s, substr string) bool {
	return IndexFold(s, substr) != -1
}

func TestContainsFold(t *testing.T) {
	containsTests := []struct {
		str, substr string
		expected    bool
	}{
		{"abc", "bc", true},
		{"abc", "bcd", false},
		{"abc", "", true},
		{"", "a", false},
		{"0123abcd", "B", true},
		// 2-byte needle
		{"xxxxxx", "01", false},
		{"01xxxx", "01", true},
		{"xx01xx", "01", true},
		{"xxxx01", "01", true},
		{"01xxxxx"[1:], "01", false},
		{"xxxxx01"[:6], "01", false},
		// 3-byte needle
		{"xxxxxxx", "012", false},
		{"012xxxx", "012", true},
		{"xx012xx", "012", true},
		{"xxxx012", "012", true},
		{"012xxxxx"[1:], "012", false},
		{"xxxxx012"[:7], "012", false},
		// 4-byte needle
		{"xxxxxxxx", "0123", false},
		{"0123xxxx", "0123", true},
		{"xx0123xx", "0123", true},
		{"xxxx0123", "0123", true},
		{"0123xxxxx"[1:], "0123", false},
		{"xxxxx0123"[:8], "0123", false},
		// 5-7-byte needle
		{"xxxxxxxxx", "01234", false},
		{"01234xxxx", "01234", true},
		{"xx01234xx", "01234", true},
		{"xxxx01234", "01234", true},
		{"01234xxxxx"[1:], "01234", false},
		{"xxxxx01234"[:9], "01234", false},
		// 8-byte needle
		{"xxxxxxxxxxxx", "01234567", false},
		{"01234567xxxx", "01234567", true},
		{"xx01234567xx", "01234567", true},
		{"xxxx01234567", "01234567", true},
		{"01234567xxxxx"[1:], "01234567", false},
		{"xxxxx01234567"[:12], "01234567", false},
		// 9-15-byte needle
		{"xxxxxxxxxxxxx", "012345678", false},
		{"012345678xxxx", "012345678", true},
		{"xx012345678xx", "012345678", true},
		{"xxxx012345678", "012345678", true},
		{"012345678xxxxx"[1:], "012345678", false},
		{"xxxxx012345678"[:13], "012345678", false},
		// 16-byte needle
		{"xxxxxxxxxxxxxxxxxxxx", "0123456789ABCDEF", false},
		{"0123456789ABCDEFxxxx", "0123456789ABCDEF", true},
		{"xx0123456789ABCDEFxx", "0123456789ABCDEF", true},
		{"xxxx0123456789ABCDEF", "0123456789ABCDEF", true},
		{"0123456789ABCDEFxxxxx"[1:], "0123456789ABCDEF", false},
		{"xxxxx0123456789ABCDEF"[:20], "0123456789ABCDEF", false},
		// 17-31-byte needle
		{"xxxxxxxxxxxxxxxxxxxxx", "0123456789ABCDEFG", false},
		{"0123456789ABCDEFGxxxx", "0123456789ABCDEFG", true},
		{"xx0123456789ABCDEFGxx", "0123456789ABCDEFG", true},
		{"xxxx0123456789ABCDEFG", "0123456789ABCDEFG", true},
		{"0123456789ABCDEFGxxxxx"[1:], "0123456789ABCDEFG", false},
		{"xxxxx0123456789ABCDEFG"[:21], "0123456789ABCDEFG", false},

		// partial match cases
		{"xx01x", "012", false},                             // 3
		{"xx0123x", "01234", false},                         // 5-7
		{"xx01234567x", "012345678", false},                 // 9-15
		{"xx0123456789ABCDEFx", "0123456789ABCDEFG", false}, // 17-31, issue 15679
		// 2 byte needle, 16byte haystack
		{"xxxxxxxxxxxxxxxx", "01", false},
		{"01xxxxxxxxxxxxxx", "01", true},
		{"xx01xxxxxxxxxxxx", "01", true},
		{"xxxxxxxxxxxxx01x", "01", true},
		{"xxxxxxxxxxxxxxx01xxxxxxx", "01", true},
		{"01xxxxxxxxxxxxxxx"[1:], "01", false},
		// 3 byte needle, 32byte haystack
		{"xyyyyyyyyyyyyyyyyxxxxxxxxxxxxxxx", "yyy", true},
		// 5 bytes needle, 21byte haystack
		{"xxxxxxxxxxxxxxxxxxxxx", "01234", false},
		{"01234xxxxxxxxxxxxxxxx", "01234", true},
		{"xx01234xxxxxxxxxxxxxx", "01234", true},
		{"xxxxxxxxxxx01234xxxxx", "01234", true},
		{"xxxxxxxxxxx01x34xxxxx", "01234", false},
		{"0101x340123401234xxxx", "01234", true},
		// fuzzed cases
		{"000", "0\x00", false},
		{"00000000000000000", "0`", false},
		{"0000", "\x00\x00\x00", false},
	}

	for _, ct := range containsTests {
		if containsFold(ct.str, ct.substr) != ct.expected {
			t.Errorf("ContainsFold(%s, %s) = %v, want %v",
				ct.str, ct.substr, !ct.expected, ct.expected)
		}
		want := indexFoldGo(ct.str, ct.substr)
		if idx := IndexFold(ct.str, ct.substr); idx != want {
			t.Errorf("IndexFold(%s, %s) = %v, want %v",
				ct.str, ct.substr, idx, want)
		}
		if idx := indexFoldRabinKarp(ct.str, ct.substr); idx != want {
			t.Errorf("indexFoldRabinKarp(%s, %s) = %v, want %v",
				ct.str, ct.substr, idx, want)
		}
	}
}

// Periodic inputs make the first2/last2 prefilter produce a candidate at almost
// every position while each verification runs the length of the needle, which is
// what drives the SIMD implementations onto their Rabin-Karp fallback.
//
// The leading run is kept shorter than the needle so no candidate in it can
// succeed, and a genuine occurrence is planted in the trailing run - the
// fallback has to find it and report it in the original coordinates.
func TestIndexFoldPeriodic(t *testing.T) {
	for _, period := range []string{"ab", "abc", "aBcD", "abcdefg"} {
		for _, reps := range []int{2, 3, 5, 17, 64} {
			needle := strings.Repeat(period, reps)

			for _, lead := range []int{0, 1, 2, reps - 1} {
				if lead < 0 {
					continue
				}
				head := strings.Repeat(period, lead)

				for _, poison := range []string{"\x00", "123", strings.Repeat("z", 40)} {
					for _, hay := range []string{
						// match reachable only after the poison
						head + poison + strings.Repeat(period, reps),
						head + poison + strings.Repeat(strings.ToUpper(period), reps+8),
						// no match at all
						head + poison + strings.Repeat(period, reps-1),
						// poison in the middle of every otherwise matching window
						head + needle[:len(needle)/2] + poison + needle[len(needle)/2:] + head,
					} {
						want := indexFoldGo(hay, needle)
						if idx := IndexFold(hay, needle); idx != want {
							t.Fatalf("IndexFold(%q, %q) = %v, want %v", hay, needle, idx, want)
						}
						if idx := indexFoldRabinKarp(hay, needle); idx != want {
							t.Fatalf("indexFoldRabinKarp(%q, %q) = %v, want %v", hay, needle, idx, want)
						}
					}
				}
			}
		}
	}
}

func TestEqualFold(t *testing.T) {
	equalFoldTests := []struct {
		s, t string
		out  bool
	}{
		{"", "", true},
		{"abc", "abc", true},
		{"ABcd", "ABcd", true},
		{"123abc", "123ABC", true},
		{"abc", "xyz", false},
		{"abc", "XYZ", false},
		{"abcdefghijk", "abcdefghijX", false},
		{"1", "2", false},
		{"utf-8", "US-ASCII", false},
		{"hello", "Hello", true},
		{"oh hello there!!", "oh hello there!!", true},
		{"oh hello there!!", "oh HELLO there!!", true},
		{"oh hello there!!", "oh HELLO there !", false},
		{"oh hello there!! friend!", "oh HELLO there!! FRIEND!", true},
	}

	for _, tt := range equalFoldTests {
		if out := EqualFold(tt.s, tt.t); out != tt.out {
			t.Errorf("EqualFold(%#q, %#q) = %v, want %v", tt.s, tt.t, out, tt.out)
		}
		if out := EqualFold(tt.t, tt.s); out != tt.out {
			t.Errorf("EqualFold(%#q, %#q) = %v, want %v", tt.t, tt.s, out, tt.out)
		}
	}
}

func TestHasPrefixFold(t *testing.T) {
	tests := []struct {
		s, prefix string
		want      bool
	}{
		// Empty cases
		{"", "", true},
		{"abc", "", true},
		{"", "a", false},

		// Exact match cases
		{"abc", "abc", true},
		{"abc", "ab", true},
		{"abc", "a", true},

		// Case insensitive matches
		{"ABC", "abc", true},
		{"abc", "ABC", true},
		{"Hello World", "hello", true},
		{"hello world", "HELLO", true},
		{"HeLLo", "hElLo", true},

		// Non-matches
		{"abc", "xyz", false},
		{"abc", "bc", false},
		{"hello", "world", false},
		{"abc", "abcd", false}, // prefix longer than string

		// Various lengths
		{"abcdefghijklmnop", "ABCDEFGH", true},
		{"abcdefghijklmnop", "ABCDEFGHIJKLMNOP", true},
		{"abcdefghijklmnop", "ABCDEFGHIJKLMNOPQ", false},
		{"0123456789", "0123", true},
		{"0123456789", "0123456789", true},
		{"0123456789", "01onal", false},
	}

	for _, tt := range tests {
		if got := HasPrefixFold(tt.s, tt.prefix); got != tt.want {
			t.Errorf("HasPrefixFold(%q, %q) = %v, want %v", tt.s, tt.prefix, got, tt.want)
		}
	}
}

func TestHasSuffixFold(t *testing.T) {
	tests := []struct {
		s, suffix string
		want      bool
	}{
		// Empty cases
		{"", "", true},
		{"abc", "", true},
		{"", "a", false},

		// Exact match cases
		{"abc", "abc", true},
		{"abc", "bc", true},
		{"abc", "c", true},

		// Case insensitive matches
		{"ABC", "abc", true},
		{"abc", "ABC", true},
		{"Hello World", "WORLD", true},
		{"hello world", "World", true},
		{"HeLLo", "hElLo", true},

		// Non-matches
		{"abc", "xyz", false},
		{"abc", "ab", false},
		{"hello", "world", false},
		{"abc", "zabc", false}, // suffix longer than string

		// Various lengths
		{"abcdefghijklmnop", "IJKLMNOP", true},
		{"abcdefghijklmnop", "ABCDEFGHIJKLMNOP", true},
		{"abcdefghijklmnop", "XABCDEFGHIJKLMNOP", false},
		{"0123456789", "6789", true},
		{"0123456789", "0123456789", true},
		{"0123456789", "onal6789", false},
	}

	for _, tt := range tests {
		if got := HasSuffixFold(tt.s, tt.suffix); got != tt.want {
			t.Errorf("HasSuffixFold(%q, %q) = %v, want %v", tt.s, tt.suffix, got, tt.want)
		}
	}
}

func BenchmarkAsciiValid(b *testing.B) {
	for _, n := range []int{1, 7, 15, 44, 100, 1000} {
		asciiBuf := makeASCII(n)
		asciiStr := string(asciiBuf)

		b.Run(fmt.Sprintf("go-%d", n), func(b *testing.B) {
			b.SetBytes(int64(len(asciiStr)))
			for i := 0; i < b.N; i++ {
				isAsciiGo(asciiBuf)
			}
		})

		b.Run(fmt.Sprintf("segment-%d", n), func(b *testing.B) {
			b.SetBytes(int64(len(asciiStr)))
			for i := 0; i < b.N; i++ {
				segAscii.ValidString(asciiStr)
			}
		})

		b.Run(fmt.Sprintf("simd-%d", n), func(b *testing.B) {
			b.SetBytes(int64(len(asciiStr)))
			for i := 0; i < b.N; i++ {
				ValidString(asciiStr)
			}
		})
	}
}

func BenchmarkIndexMask(b *testing.B) {
	for _, n := range []int{1, 7, 15, 44, 100, 1000} {
		asciiBuf := makeASCII(n)
		idx := rand.Intn(n)
		asciiBuf[idx] |= 0x80

		asciiStr := string(asciiBuf)

		b.Run(fmt.Sprintf("go-%d", n), func(b *testing.B) {
			b.SetBytes(int64(len(asciiStr)))
			for i := 0; i < b.N; i++ {
				indexMaskGo(asciiBuf, 0x80)
			}
		})

		b.Run(fmt.Sprintf("simd-%d", n), func(b *testing.B) {
			b.SetBytes(int64(len(asciiStr)))
			for i := 0; i < b.N; i++ {
				IndexMask(asciiStr, 0x80)
			}
		})
	}
}

func BenchmarkAsciiEqualFold(b *testing.B) {
	for _, n := range []int{1, 7, 15, 44, 100, 1000} {
		asciiBuf := makeASCII(n)
		s1 := string(asciiBuf)

		// try to flip as least one byte
		for k := 0; k < 3; k++ {
			idx := rand.Intn(n)
			if unicode.IsUpper(rune(asciiBuf[idx])) {
				asciiBuf[idx] = byte(unicode.ToLower(rune(asciiBuf[idx])))
			} else if unicode.IsLower(rune(asciiBuf[idx])) {
				asciiBuf[idx] = byte(unicode.ToUpper(rune(asciiBuf[idx])))
			}
		}
		s2 := string(asciiBuf)

		b.Run(fmt.Sprintf("go-%d", n), func(b *testing.B) {
			b.SetBytes(int64(len(s1)))
			for i := 0; i < b.N; i++ {
				strings.EqualFold(s1, s2)
			}
		})

		b.Run(fmt.Sprintf("segment-%d", n), func(b *testing.B) {
			b.SetBytes(int64(len(s1)))
			for i := 0; i < b.N; i++ {
				segAscii.EqualFoldString(s1, s2)
			}
		})

		b.Run(fmt.Sprintf("simd-%d", n), func(b *testing.B) {
			b.SetBytes(int64(len(s1)))
			for i := 0; i < b.N; i++ {
				EqualFold(s1, s2)
			}
		})
	}
}

func BenchmarkAsciiIndexFold(b *testing.B) {
	rnd := rand.New(rand.NewSource(0))

	for _, n := range []int{1, 7, 15, 44, 100, 1000} {
		asciiBuf := makeASCII(n)
		s1 := string(asciiBuf)

		// try to flip as least one byte
		for k := 0; k < 3; k++ {
			idx := rnd.Intn(n)
			if unicode.IsUpper(rune(asciiBuf[idx])) {
				asciiBuf[idx] = byte(unicode.ToLower(rune(asciiBuf[idx])))
			} else if unicode.IsLower(rune(asciiBuf[idx])) {
				asciiBuf[idx] = byte(unicode.ToUpper(rune(asciiBuf[idx])))
			}
		}

		s2 := string(asciiBuf[rnd.Intn(n):])
		if len(s2) > 3 {
			s2 = s2[:rnd.Intn(len(s2))]
		}
		b.Logf("haystack len: %d, needle len: %d", len(s1), len(s2))

		b.Run(fmt.Sprintf("go-%d", n), func(b *testing.B) {
			b.SetBytes(int64(len(s1)))
			for i := 0; i < b.N; i++ {
				indexFoldGo(s1, s2)
			}
		})

		b.Run(fmt.Sprintf("simd-%d", n), func(b *testing.B) {
			b.SetBytes(int64(len(s1)))
			for i := 0; i < b.N; i++ {
				IndexFold(s1, s2)
			}
		})
	}
}

var benchInputTorture = strings.Repeat("ABC", 1<<10) + "123" + strings.Repeat("ABC", 1<<10)
var benchNeedleTorture = strings.Repeat("ABC", 1<<10+1)

func BenchmarkIndexTorture(b *testing.B) {
	b.Run("go", func(b *testing.B) {
		for i := 0; i < b.N; i++ {
			strings.Index(benchInputTorture, benchNeedleTorture)
		}
	})

	b.Run("simd", func(b *testing.B) {
		for i := 0; i < b.N; i++ {
			IndexFold(benchInputTorture, benchNeedleTorture)
		}
	})
}

func BenchmarkIndexPeriodic(b *testing.B) {
	key := "aa"

	for _, skip := range [...]int{2, 4, 8, 16, 32, 64} {
		b.Run(fmt.Sprintf("go-%d", skip), func(b *testing.B) {
			s := strings.Repeat("a"+strings.Repeat(" ", skip-1), 1<<16/skip)
			for i := 0; i < b.N; i++ {
				strings.Index(s, key)
			}
		})

		b.Run(fmt.Sprintf("simd-%d", skip), func(b *testing.B) {
			s := strings.Repeat("a"+strings.Repeat(" ", skip-1), 1<<16/skip)
			for i := 0; i < b.N; i++ {
				IndexFold(s, key)
			}
		})
	}
}

func FuzzEqualFold(f *testing.F) {
	f.Add("01234567", "01234567")
	f.Add("abcd", "ABCD")
	f.Add("EqualFold", "equalFold")

	f.Fuzz(func(t *testing.T, in1, in2 string) {
		if !ValidString(in1) || !ValidString(in2) {
			t.Skip()
		}

		res := EqualFold(in1, in2)
		stdRes := strings.EqualFold(in1, in2)
		if res != stdRes {
			t.Fatalf("EqualFold(%q, %q) = %v; want %v", in1, in2, res, stdRes)
		}
		goRes := equalFoldGo(in1, in2)
		if goRes != stdRes {
			t.Fatalf("equalFoldGo(%q, %q) = %v; want %v", in1, in2, goRes, stdRes)
		}
	})
}

func FuzzIndexFold(f *testing.F) {
	f.Add("01234567", "01234567")
	f.Add("abcdefghijklmnopqrstuvwxyz01234567890", "klmno")
	f.Add("abcdefghijklmnopqrstuvwxyz01234567890", "12")
	f.Add("abcdefghABCDEFGH01234567890", "H")
	f.Add("000000000000000B0", "B0")
	f.Add("EqualFold", "fold")
	f.Add("Lorem ipsum dolor sit amet, consectetur adipiscing elit, sed do eiusmod tempor...", " ELIT")

	f.Fuzz(func(t *testing.T, istr, isubstr string) {
		if !ValidString(isubstr) {
			t.Skip()
		}

		res := IndexFold(istr, isubstr)
		goRes := indexFoldGo(istr, isubstr)
		if res != goRes {
			t.Fatalf("IndexFold(%q, %q) = %v; want %v", istr, isubstr, res, goRes)
		}

		res = indexFoldRabinKarp(istr, isubstr)
		if res != goRes {
			t.Fatalf("indexFoldRabinKarp(%q, %q) = %v; want %v", istr, isubstr, res, goRes)
		}
	})
}

func TestIndexAny(t *testing.T) {
	tests := []struct {
		s, chars string
		want     int
	}{
		{"", "", -1},
		{"", "a", -1},
		{"a", "", -1},
		{"abc", "a", 0},
		{"abc", "b", 1},
		{"abc", "c", 2},
		{"abc", "d", -1},
		{"abc", "cb", 1},
		{"abc", "dc", 2},
		{"abc", "xyz", -1},
		{"abcdefghijklmnop", "p", 15},
		{"abcdefghijklmnop", "op", 14},
		{"abcdefghijklmnop", "xyz", -1},
		{"hello world", " ", 5},
		{"hello world", "\t\n ", 5},
		{"hello\tworld", "\t\n ", 5},
		{"hello\nworld", "\t\n ", 5},
		// Longer strings to test SIMD paths
		{strings.Repeat("x", 100) + "y", "y", 100},
		{strings.Repeat("x", 100) + "y", "yz", 100},
		{strings.Repeat("x", 1000) + "abc", "abc", 1000},
		{strings.Repeat("x", 1000), "abc", -1},
		// Multiple chars in set
		{"the quick brown fox", "aeiou", 2}, // 'e' in 'the'
		{"xyz", "aeiou", -1},
		// Edge cases: duplicates in chars (should still work)
		{"abc", "aaa", 0},
		{"abc", "bbb", 1},
		{"abc", "aaabbbccc", 0},
		// Edge case: >16 chars
		{"abcdefghijklmnopqrstuvwxyz", "1234567890!@#$%^&*()z", 25},
		// Edge case: >64 chars (falls back to Go)
		{"test", strings.Repeat("x", 65) + "t", 0},
		// Edge case: single char strings
		{"a", "a", 0},
		{"a", "b", -1},
		{"b", "abc", 0},
		// Edge case: non-ASCII in haystack (should still find ASCII chars)
		{"hello\x80world", " ", -1},
		{"hello\x80world", "w", 6},
		{"\xff\xfe\xfd", "abc", -1},
		// Edge case: match at various positions within vector
		{strings.Repeat("a", 15) + "x", "x", 15},
		{strings.Repeat("a", 16) + "x", "x", 16},
		{strings.Repeat("a", 17) + "x", "x", 17},
		{strings.Repeat("a", 31) + "x", "x", 31},
		{strings.Repeat("a", 32) + "x", "x", 32},
		{strings.Repeat("a", 33) + "x", "x", 33},
	}

	for _, tt := range tests {
		if got := IndexAny(tt.s, tt.chars); got != tt.want {
			t.Errorf("IndexAny(%q, %q) = %d, want %d", tt.s, tt.chars, got, tt.want)
		}
	}
}

func TestByteSetIndex(t *testing.T) {
	tests := []struct {
		s, chars string
		want     int
	}{
		{"", "", -1},
		{"", "a", -1},
		{"a", "", -1},
		{"abc", "a", 0},
		{"abc", "b", 1},
		{"abc", "c", 2},
		{"abc", "d", -1},
		{"abc", "cb", 1},
		{"abc", "dc", 2},
		{"abc", "xyz", -1},
		{"abcdefghijklmnop", "p", 15},
		{"abcdefghijklmnop", "op", 14},
		{"hello world", " ", 5},
		// Small data (Go fallback path)
		{"a", "a", 0},
		{"ab", "b", 1},
		{"abcdefghij", "j", 9},
		{"abcdefghijklmno", "o", 14}, // exactly 15 bytes
		// Larger data (NEON path)
		{strings.Repeat("x", 100) + "y", "y", 100},
		{strings.Repeat("x", 1000) + "abc", "abc", 1000},
		{strings.Repeat("x", 1000), "abc", -1},
	}

	for _, tt := range tests {
		bs := NewByteSet(tt.chars)
		if got := bs.Index(tt.s); got != tt.want {
			t.Errorf("ByteSet(%q).Index(%q) = %d, want %d", tt.chars, tt.s, got, tt.want)
		}
		// Verify matches IndexAny
		if got := IndexAny(tt.s, tt.chars); got != tt.want {
			t.Errorf("IndexAny(%q, %q) = %d, want %d", tt.s, tt.chars, got, tt.want)
		}
	}
}

func TestByteSetMatch(t *testing.T) {
	tests := []struct {
		s, chars string
		want     bool
	}{
		{"", "", false},
		{"", "a", false},
		{"a", "", false},
		{"abc", "a", true},
		{"abc", "d", false},
		{"hello world", " ", true},
		{"helloworld", " ", false},
	}

	for _, tt := range tests {
		bs := NewByteSet(tt.chars)
		if got := bs.Match(tt.s); got != tt.want {
			t.Errorf("ByteSet(%q).Match(%q) = %v, want %v", tt.chars, tt.s, got, tt.want)
		}
	}
}

func TestContainsAny(t *testing.T) {
	tests := []struct {
		s, chars string
		want     bool
	}{
		{"", "", false},
		{"", "a", false},
		{"a", "", false},
		{"abc", "a", true},
		{"abc", "d", false},
		{"hello world", " ", true},
		{"helloworld", " ", false},
	}

	for _, tt := range tests {
		if got := ContainsAny(tt.s, tt.chars); got != tt.want {
			t.Errorf("ContainsAny(%q, %q) = %v, want %v", tt.s, tt.chars, got, tt.want)
		}
	}
}

func TestIndexNonASCII(t *testing.T) {
	tests := []struct {
		s    string
		want int
	}{
		{"", -1},
		{"hello", -1},
		{"hello world", -1},
		{"hello\x80world", 5},
		{"\x80hello", 0},
		{"hello\x80", 5},
		{strings.Repeat("x", 100) + "\x80", 100},
		{strings.Repeat("x", 1000) + "日本語", 1000},
	}

	for _, tt := range tests {
		if got := IndexNonASCII(tt.s); got != tt.want {
			t.Errorf("IndexNonASCII(%q) = %d, want %d", tt.s, got, tt.want)
		}
	}
}

func BenchmarkIndexAny(b *testing.B) {
	chars := " \t\n\r"
	bs := NewByteSet(chars)
	for _, n := range []int{16, 64, 256, 1024} {
		data := strings.Repeat("x", n-1) + " "

		b.Run(fmt.Sprintf("stdlib-%d", n), func(b *testing.B) {
			b.SetBytes(int64(len(data)))
			for i := 0; i < b.N; i++ {
				strings.IndexAny(data, chars)
			}
		})

		b.Run(fmt.Sprintf("byteset-%d", n), func(b *testing.B) {
			b.SetBytes(int64(len(data)))
			for i := 0; i < b.N; i++ {
				bs.Index(data)
			}
		})

		b.Run(fmt.Sprintf("indexany-%d", n), func(b *testing.B) {
			b.SetBytes(int64(len(data)))
			for i := 0; i < b.N; i++ {
				IndexAny(data, chars)
			}
		})
	}
}

func BenchmarkByteSetIndex(b *testing.B) {
	chars := " \t\n\r"
	bs := NewByteSet(chars)

	for _, n := range []int{16, 64, 256, 1024} {
		data := strings.Repeat("x", n-1) + " "

		b.Run(fmt.Sprintf("per-call-%d", n), func(b *testing.B) {
			b.SetBytes(int64(len(data)))
			for i := 0; i < b.N; i++ {
				IndexAny(data, chars)
			}
		})

		b.Run(fmt.Sprintf("byteset-%d", n), func(b *testing.B) {
			b.SetBytes(int64(len(data)))
			for i := 0; i < b.N; i++ {
				bs.Index(data)
			}
		})
	}
}

// indexAnyNaive is a trivially-correct reference for validating IndexAny.
func indexAnyNaive(s, chars string) int {
	for i := 0; i < len(s); i++ {
		for j := 0; j < len(chars); j++ {
			if s[i] == chars[j] {
				return i
			}
		}
	}
	return -1
}

func FuzzIndexAny(f *testing.F) {
	f.Add("hello world", " ")
	f.Add("abcdefghij", "xyz")
	f.Add(strings.Repeat("a", 100), "b")
	// Edge cases for high bytes
	f.Add("abc\x80def", "\x80")
	f.Add("\xff\xfe\xfd", "\xfd")
	f.Add(strings.Repeat("x", 17)+"\x00", "\x00")

	f.Fuzz(func(t *testing.T, s, chars string) {
		want := indexAnyNaive(s, chars)

		got := IndexAny(s, chars)
		if got != want {
			t.Fatalf("IndexAny(%q, %q) = %d, want %d", s, chars, got, want)
		}
	})
}
