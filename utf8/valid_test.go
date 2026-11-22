package utf8

import (
	"bytes"
	"strings"
	"testing"
	stdlib "unicode/utf8"

	"github.com/stretchr/testify/assert"
)

var valid1k = bytes.Repeat([]byte("0123456789日本語日本語日本語日abcdefghijklmnopqrstuvwx"), 16)
var valid1M = bytes.Repeat(valid1k, 1024)
var someutf8 = []byte("\xF4\x8F\xBF\xBF")

type byteRange struct {
	Low  byte
	High byte
}

func one(b byte) byteRange {
	return byteRange{b, b}
}

func genExamples(current string, ranges []byteRange) []string {
	if len(ranges) == 0 {
		return []string{string(current)}
	}
	r := ranges[0]
	var all []string

	elements := []byte{r.Low, r.High}

	mid := (r.High + r.Low) / 2
	if mid != r.Low && mid != r.High {
		elements = append(elements, mid)
	}

	for _, x := range elements {
		s := current + string(x)
		all = append(all, genExamples(s, ranges[1:])...)
		if x == r.High {
			break
		}
	}
	return all
}

func TestValid(t *testing.T) {
	var examples = []string{
		// Tests copied from the stdlib
		"",
		"a",
		"abc",
		"Ж",
		"ЖЖ",
		"брэд-ЛГТМ",
		"☺☻☹",

		// overlong
		"\xE0\x80",
		// unfinished continuation
		"aa\xE2",

		string([]byte{66, 250}),

		string([]byte{66, 250, 67}),

		"a\uFFFDb",

		"\xF4\x8F\xBF\xBF", // U+10FFFF

		"\xF4\x90\x80\x80", // U+10FFFF+1; out of range
		"\xF7\xBF\xBF\xBF", // 0x1FFFFF; out of range

		"\xFB\xBF\xBF\xBF\xBF", // 0x3FFFFFF; out of range

		"\xc0\x80",     // U+0000 encoded in two bytes: incorrect
		"\xed\xa0\x80", // U+D800 high surrogate (sic)
		"\xed\xbf\xbf", // U+DFFF low surrogate (sic)

		// valid at boundary
		strings.Repeat("a", 32+28) + "☺☻☹",
		strings.Repeat("a", 32+29) + "☺☻☹",
		strings.Repeat("a", 32+30) + "☺☻☹",
		strings.Repeat("a", 32+31) + "☺☻☹",
		// invalid at boundary
		strings.Repeat("a", 32+31) + "\xE2a",
		strings.Repeat("a", 14) + "☺" + strings.Repeat("a", 13) + "\xE2",

		// same inputs as benchmarks
		"0123456789",
		"日本語日本語日本語日",
		"\xF4\x8F\xBF\xBF",

		// bugs found with fuzzing
		"0000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000\xc60",
		"000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000\xc300",
		"߀0000000000000000000000000000訨",
		"0000000000000000000000000000000˂00000000000000000000000000000000",
	}

	any := byteRange{0, 0xFF}
	ascii := byteRange{0, 0x7F}
	cont := byteRange{0x80, 0xBF}

	rangesToTest := [][]byteRange{
		{one(0x20), ascii, ascii, ascii},

		// 2-byte sequences
		{one(0xC2)},
		{one(0xC2), ascii},
		{one(0xC2), cont},
		{one(0xC2), {0xC0, 0xFF}},
		{one(0xC2), cont, cont},
		{one(0xC2), cont, cont, cont},

		// 3-byte sequences
		{one(0xE1)},
		{one(0xE1), cont},
		{one(0xE1), cont, cont},
		{one(0xE1), cont, cont, ascii},
		{one(0xE1), cont, ascii},
		{one(0xE1), cont, cont, cont},

		// 4-byte sequences
		{one(0xF1)},
		{one(0xF1), cont},
		{one(0xF1), cont, cont},
		{one(0xF1), cont, cont, cont},
		{one(0xF1), cont, cont, ascii},
		{one(0xF1), cont, cont, cont, ascii},

		// overlong
		{{0xC0, 0xC1}, any},
		{{0xC0, 0xC1}, any, any},
		{{0xC0, 0xC1}, any, any, any},
		{one(0xE0), {0x0, 0x9F}, cont},
		{one(0xE0), {0xA0, 0xBF}, cont},
	}

	for _, r := range rangesToTest {
		examples = append(examples, genExamples("", r)...)
	}

	for _, i := range []int{300, 316} {
		d := bytes.Repeat(someutf8, i/len(someutf8))
		examples = append(examples, string(d))
	}

	for _, tt := range examples {
		t.Run(tt, func(t *testing.T) {
			expected := stdlib.ValidString(tt)
			assert.Equal(t, expected, ValidString(tt))
		})
	}
}

// An empty string does not have to point at anything: the kernels get a nil
// pointer for one that never had data.
func TestValidEmpty(t *testing.T) {
	var noData []byte

	for _, s := range []string{"", string(noData), "a"[:0]} {
		if !ValidString(s) {
			t.Fatalf("ValidString(%q) = false, want true", s)
		}
	}
}

// The kernels take the input in blocks and carry what a block ends with over
// to the next one, and they step over ASCII: the NEON one after a block that
// ends in ASCII, the AVX2 one at the start of the input and after 512 bytes
// of it. Put every kind of sequence at every place in the blocks, behind
// ASCII and behind longer characters, with nothing, a little and a lot of
// ASCII after it. All of that with a character in front as well, for the
// ASCII not to be the start of the input, which on arm64 the kernel does not
// get to see.
func TestValidBlocks(t *testing.T) {
	sequences := []string{
		"é", "日", "😀",
		// cut short, or without a lead byte
		"\x80", "\xc2", "\xe6\x97", "\xf0\x9f\x98",
		// never valid
		"\xc0\x80", "\xe0\x9f\xbf", "\xed\xa0\x80", "\xf4\x90\x80\x80", "\xff",
	}

	for _, start := range []string{"é", ""} {
		for _, filler := range []string{"a", "é", "日"} {
			for at := 0; at < 800; at++ {
				front := start + strings.Repeat("a", at%len(filler)) + strings.Repeat(filler, at/len(filler))

				for _, seq := range sequences {
					for _, after := range []int{0, 9, 150} {
						s := front + seq + strings.Repeat("a", after)
						if got, want := ValidString(s), stdlib.ValidString(s); got != want {
							t.Fatalf("ValidString(%q) = %v, want %v", s, got, want)
						}
					}
				}
			}
		}
	}
}

func FuzzValidString(f *testing.F) {
	f.Add("0123456789日本語日本語日本語日abcdefghijklmnopqrstuvwx", uint16(0), uint16(0))
	f.Add("\xf4\x8f\xbf\xbf0123456789abcdef\xed\xa0\x80", uint16(61), uint16(70))
	f.Add("\xe6\x97", uint16(60), uint16(700))

	f.Fuzz(func(t *testing.T, s string, before, between uint16) {
		// the input as it is, and twice in a longer text, with ASCII in front
		// of it and in between: at any place in the blocks of the kernels,
		// and with ASCII for them to skip, behind a character and at the
		// start
		long := "é" + strings.Repeat("a", int(before%1000)) + s + strings.Repeat("a", int(between%1000)) + s

		for _, in := range []string{s, long, long[len("é"):]} {
			if got, want := ValidString(in), stdlib.ValidString(in); got != want {
				t.Fatalf("ValidString(%q) = %v, want %v", in, got, want)
			}
		}
	})
}

var ascii100000 = strings.Repeat("0123456789", 10000)
var longStringMostlyASCII string // ~100KB, ~97% ASCII
var longStringJapanese string    // ~100KB, non-ASCII

func init() {
	const japanese = "日本語日本語日本語日"
	var b strings.Builder
	for i := 0; b.Len() < 100_000; i++ {
		if i%100 == 0 {
			b.WriteString(japanese)
		} else {
			b.WriteString("0123456789")
		}
	}
	longStringMostlyASCII = b.String()
	longStringJapanese = strings.Repeat(japanese, 100_000/len(japanese))
}

func BenchmarkValidStringTenASCIIChars(b *testing.B) {
	b.Run("std", func(b *testing.B) {
		for i := 0; i < b.N; i++ {
			stdlib.ValidString("0123456789")
		}
	})

	b.Run("simd", func(b *testing.B) {
		for i := 0; i < b.N; i++ {
			ValidString("0123456789")
		}
	})
}

func BenchmarkValidString100KASCIIChars(b *testing.B) {
	b.Run("std", func(b *testing.B) {
		for i := 0; i < b.N; i++ {
			stdlib.ValidString(ascii100000)
		}
	})

	b.Run("simd", func(b *testing.B) {
		for i := 0; i < b.N; i++ {
			ValidString(ascii100000)
		}
	})
}

func BenchmarkValidStringTenJapaneseChars(b *testing.B) {
	b.Run("std", func(b *testing.B) {
		for i := 0; i < b.N; i++ {
			stdlib.ValidString("日本語日本語日本語日")
		}
	})

	b.Run("simd", func(b *testing.B) {
		for i := 0; i < b.N; i++ {
			ValidString("日本語日本語日本語日")
		}
	})
}

func BenchmarkValidStringLongMostlyASCII(b *testing.B) {
	b.Run("std", func(b *testing.B) {
		b.SetBytes(int64(len(longStringMostlyASCII)))
		for i := 0; i < b.N; i++ {
			stdlib.ValidString(longStringMostlyASCII)
		}
	})

	b.Run("simd", func(b *testing.B) {
		b.SetBytes(int64(len(longStringMostlyASCII)))
		for i := 0; i < b.N; i++ {
			ValidString(longStringMostlyASCII)
		}
	})
}

func BenchmarkValidStringLongJapanese(b *testing.B) {
	b.Run("std", func(b *testing.B) {
		b.SetBytes(int64(len(longStringJapanese)))
		for i := 0; i < b.N; i++ {
			stdlib.ValidString(longStringJapanese)
		}
	})

	b.Run("simd", func(b *testing.B) {
		b.SetBytes(int64(len(longStringJapanese)))
		for i := 0; i < b.N; i++ {
			ValidString(longStringJapanese)
		}
	})
}

func BenchmarkInvalidStringLong(b *testing.B) {
	invalidLongString := "\xe2" + longStringMostlyASCII

	b.Run("std", func(b *testing.B) {
		for i := 0; i < b.N; i++ {
			stdlib.ValidString(invalidLongString)
		}
	})

	b.Run("simd", func(b *testing.B) {
		for i := 0; i < b.N; i++ {
			ValidString(invalidLongString)
		}
	})
}
