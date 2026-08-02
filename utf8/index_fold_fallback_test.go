package utf8

import (
	"strings"
	"testing"
)

func TestIndexFoldFallback(t *testing.T) {
	tests := []struct {
		name             string
		haystack, needle string
		want             int
	}{
		{name: "empty", haystack: "anything", needle: "", want: 0},
		{name: "empty haystack", haystack: "", needle: "a", want: -1},
		{name: "ASCII match", haystack: "Hello, WORLD!", needle: "world", want: 7},
		{name: "ASCII miss", haystack: strings.Repeat("W", 192), needle: "33", want: -1},
		{name: "invariant", haystack: "日本語の検索", needle: "検索", want: 12},
		{name: "sharp s expansion", haystack: "Straße", needle: "STRASSE", want: 0},
		{name: "inside expansion", haystack: "groß", needle: "s", want: 3},
		{name: "ligature", haystack: "ofﬁce", needle: "FFI", want: 1},
		{name: "Greek sigma", haystack: "κόσμος", needle: "ΣΜΟΣ", want: 4},
		{name: "Kelvin", haystack: "10K", needle: "k", want: 2},
		{name: "malformed equal", haystack: "a\xffb", needle: "\xffB", want: 1},
		{name: "malformed invariant", haystack: "a\xffb", needle: "\xff", want: 1},
		{name: "malformed distinct from rune", haystack: "\xfc", needle: "ü", want: -1},
		{
			name:     "rolling Armenian",
			haystack: strings.Repeat("ի", 17) + strings.Repeat("իԻԻի", 3) + "իԻԻխ" + strings.Repeat("ի", 128),
			needle:   strings.Repeat("ի", 15) + "խ",
			want:     34,
		},
		{
			name:     "over ring capacity",
			haystack: strings.Repeat("prefix-", 20) + strings.Repeat("Straße-", 8) + "tail",
			needle:   strings.Repeat("STRASSE-", 8),
			want:     140,
		},
		{
			name:     "over ring capacity inside expansion",
			haystack: "ß" + strings.Repeat("Ab", 20),
			needle:   "s" + strings.Repeat("aB", 20),
			want:     0,
		},
	}

	for _, test := range tests {
		t.Run(test.name, func(t *testing.T) {
			if got := indexFoldFallback(test.haystack, test.needle); got != test.want {
				t.Fatalf("indexFoldFallback(%q, %q) = %d, want %d", test.haystack, test.needle, got, test.want)
			}
		})
	}
}

func TestIndexFoldFallbackMappings(t *testing.T) {
	for source, mapping := range unicodeFullFold {
		var folded strings.Builder
		for index := uint8(0); index < mapping.count; index++ {
			folded.WriteRune(rune(mapping.runes[index]))
		}
		haystack := "\x00" + string(source) + "\x00"
		needles := []string{folded.String()}
		if mapping.count > 1 {
			needles = append(needles, string(rune(mapping.runes[mapping.count-1])))
		}
		for _, needle := range needles {
			if got := indexFoldFallback(haystack, needle); got != 1 {
				t.Fatalf("source U+%04X, needle %q: fallback = %d, want 1", source, needle, got)
			}
		}
	}
}

var indexFoldFallbackBenchmarkSink int

func BenchmarkIndexFoldFallback(b *testing.B) {
	cases := []struct {
		name             string
		haystack, needle string
	}{
		{
			name:     "ASCII short match",
			haystack: strings.Repeat("0123456789_", 128) + "FindMe",
			needle:   "findme",
		},
		{
			name:     "ASCII short miss",
			haystack: strings.Repeat("0123456789_", 128),
			needle:   "absent",
		},
		{
			name:     "Latin expansion match",
			haystack: strings.Repeat("Voilà—", 128) + "Straße",
			needle:   "STRASSE",
		},
		{
			name:     "Greek match",
			haystack: strings.Repeat("αβγδεζηθικλμνξοπρ", 64) + "κόσμος",
			needle:   "ΚΌΣΜΟΣ",
		},
		{
			name:     "Armenian rolling match",
			haystack: strings.Repeat("ի", 31) + strings.Repeat("իԻԻի", 3) + "իԻԻխ" + strings.Repeat("ի", 128),
			needle:   strings.Repeat("ի", 15) + "խ",
		},
		{
			name:     "Long folded match",
			haystack: strings.Repeat("prefix-", 128) + strings.Repeat("Straße-", 16),
			needle:   strings.Repeat("STRASSE-", 16),
		},
		{
			name:     "Invariant byte match",
			haystack: strings.Repeat("日本語の文章。", 128) + "検索対象",
			needle:   "検索対象",
		},
	}

	for _, test := range cases {
		b.Run(test.name, func(b *testing.B) {
			b.ReportAllocs()
			b.SetBytes(int64(len(test.haystack)))
			b.ResetTimer()
			result := 0
			for range b.N {
				result = indexFoldFallback(test.haystack, test.needle)
			}
			indexFoldFallbackBenchmarkSink = result
		})
	}
}
