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
				equalFoldGo(s1, s2)
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

func TestSearcherCaseSensitive(t *testing.T) {
	tests := []struct {
		haystack, needle string
		want             int
	}{
		{"", "", 0},
		{"", "a", -1},
		{"a", "", 0},
		{"abc", "a", 0},
		{"abc", "A", -1}, // case-sensitive: 'A' not found
		{"abc", "b", 1},
		{"abc", "B", -1}, // case-sensitive
		{"abc", "c", 2},
		{"abc", "d", -1},
		{"hello world", "world", 6},
		{"Hello World", "hello", -1}, // case-sensitive: 'hello' not found
		{"Hello World", "Hello", 0},
		{"The Quick Brown Fox", "Quick", 4},
		{"The Quick Brown Fox", "quick", -1}, // case-sensitive
		{"The Quick Brown Fox", "Fox", 16},
		{"The Quick Brown Fox", "xyz", -1},
		// Longer strings
		{strings.Repeat("a", 100) + "NEEDLE" + strings.Repeat("b", 100), "NEEDLE", 100},
		{strings.Repeat("a", 100) + "NEEDLE" + strings.Repeat("b", 100), "needle", -1}, // case-sensitive
		{strings.Repeat("x", 1000) + "QuIcK", "QuIcK", 1000},
		{strings.Repeat("x", 1000) + "QuIcK", "quick", -1}, // case-sensitive
		// Edge cases
		{strings.Repeat("x", 20) + "needle", "needle", 20},
		{strings.Repeat("x", 17) + "QZ", "QZ", 17},
		{strings.Repeat("x", 25) + "abc", "abc", 25},
		{strings.Repeat("y", 31) + "z", "z", 31},
	}

	for _, tt := range tests {
		s := NewSearcher(tt.needle, true) // case-sensitive
		if got := s.Index(tt.haystack); got != tt.want {
			t.Errorf("Searcher(%q, true).Index(%q) = %d, want %d", tt.needle, tt.haystack, got, tt.want)
		}
		// Verify against strings.Index
		if want := strings.Index(tt.haystack, tt.needle); want != tt.want {
			t.Errorf("strings.Index(%q, %q) = %d, want %d", tt.haystack, tt.needle, want, tt.want)
		}
	}
}

func TestSearcherCaseInsensitive(t *testing.T) {
	tests := []struct {
		haystack, needle string
		want             int
	}{
		{"", "", 0},
		{"", "a", -1},
		{"a", "", 0},
		{"abc", "a", 0},
		{"abc", "A", 0},
		{"abc", "b", 1},
		{"abc", "B", 1},
		{"hello world", "WORLD", 6},
		{"Hello World", "hello", 0},
		{"The Quick Brown Fox", "quick", 4},
		// Longer strings
		{strings.Repeat("a", 100) + "NEEDLE" + strings.Repeat("b", 100), "needle", 100},
		{strings.Repeat("x", 1000) + "QuIcK", "quick", 1000},
		// Edge cases
		{strings.Repeat("x", 127) + "needle", "needle", 127},
		{strings.Repeat("x", 128) + "needle", "needle", 128},
		{strings.Repeat("x", 129) + "needle", "needle", 129},
	}

	for _, tt := range tests {
		s := NewSearcher(tt.needle, false) // case-insensitive
		if got := s.Index(tt.haystack); got != tt.want {
			t.Errorf("Searcher(%q, false).Index(%q) = %d, want %d", tt.needle, tt.haystack, got, tt.want)
		}
	}
}
