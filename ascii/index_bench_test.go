package ascii

import (
	"strings"
	"testing"
)

type indexBenchCase struct {
	scenario, size, haystack, needle string
}

func indexBenchCases() []indexBenchCase {
	return []indexBenchCase{
		// Pure scan (no match)
		{"notfound", "1KB", strings.Repeat("abcdefghijklmnoprstuvwy ", 43), "quartz"},
		{"notfound", "64KB", strings.Repeat("abcdefghijklmnoprstuvwy ", 2730), "quartz"},
		{"notfound", "1MB", strings.Repeat("abcdefghijklmnoprstuvwy ", 43690), "quartz"},

		// Match positions
		{"match_end", "1KB", strings.Repeat("abcdefghijklmnoprstuvwy ", 42) + "xylophone", "xylophone"},
		{"match_end", "64KB", strings.Repeat("abcdefghijklmnoprstuvwy ", 2728) + "xylophone", "xylophone"},
		{"match_start", "1KB", "xylophone" + strings.Repeat("abcdefghijklmnoprstuvwy ", 42), "xylophone"},
		{"match_mid", "1KB", strings.Repeat("x", 500) + "needle" + strings.Repeat("y", 500), "needle"},

		// High false-positive scenarios
		{"json", "1KB", strings.Repeat(`{"k":"v"},`, 100) + `{"num":1}`, `"num"`},
		{"json", "64KB", strings.Repeat(`{"k":"v"},`, 6500) + `{"num":1}`, `"num"`},
		{"periodic", "1KB", strings.Repeat("abcd", 250) + "abce", "abce"},
		{"samechar", "1KB", strings.Repeat("a", 1000) + "aab", "aab"},
		{"samechar", "64KB", strings.Repeat("a", 64000) + "aab", "aab"},

		// Rare bytes (best case)
		{"rarebyte", "1KB", strings.Repeat("abcdefghijklmnoprstuvwy ", 42) + "quartz", "quartz"},

		// Different needle lengths
		{"needle3", "1KB", strings.Repeat("x", 1000) + "abc", "abc"},
		{"needle8", "1KB", strings.Repeat("x", 1000) + "abcdefgh", "abcdefgh"},
		{"needle16", "1KB", strings.Repeat("x", 1000) + "abcdefghijklmnop", "abcdefghijklmnop"},

		// Domain-specific patterns
		{"logdate", "1KB", strings.Repeat("2024-01-15T10:30:45.123Z INFO Processing request\n", 20) + "2024-99-99", "2024-99"},
		{"hexdata", "1KB", strings.Repeat("0x0000 0x1234 0xABCD 0xFFFF\n", 35) + "0xDEAD", "0xDEAD"},
		{"codebraces", "1KB", strings.Repeat("{\"key\": \"value\"}\n", 60) + "{NOTFOUND}", "{NOTFOUND}"},
		{"dna", "1KB", strings.Repeat("ATCGATCGATCG", 83) + "ZZZZZ", "ZZZZZ"},
		{"digits", "1KB", strings.Repeat("123456789012345678901234567890\n", 32) + "1999999", "1999999"},

		// Torture: long repeating pattern, needle doesn't exist
		{"torture", "6KB", strings.Repeat("ABC", 1<<10) + "123" + strings.Repeat("ABC", 1<<10), strings.Repeat("ABC", 1<<10+1)},

		// Periodic skip patterns (from origin benchmarks)
		{"periodic_skip2", "64KB", strings.Repeat("a ", 1<<15), "aa"},
		{"periodic_skip8", "64KB", strings.Repeat("a"+strings.Repeat(" ", 7), 1<<13), "aa"},
		{"periodic_skip64", "64KB", strings.Repeat("a"+strings.Repeat(" ", 63), 1<<10), "aa"},
	}
}

func runIndexBenchmarks(b *testing.B, extra func(b *testing.B, name, haystack, needle string)) {
	cases := indexBenchCases()

	b.Run("mode=exact", func(b *testing.B) {
		for _, tc := range cases {
			name := "scenario=" + tc.scenario + "/size=" + tc.size

			b.Run(name+"/impl=stdlib", func(b *testing.B) {
				b.SetBytes(int64(len(tc.haystack)))
				for b.Loop() {
					strings.Index(tc.haystack, tc.needle)
				}
			})

			b.Run(name+"/impl=Index", func(b *testing.B) {
				b.SetBytes(int64(len(tc.haystack)))
				for b.Loop() {
					Index(tc.haystack, tc.needle)
				}
			})

			searcher := NewSearcher(tc.needle, true)
			b.Run(name+"/impl=Searcher", func(b *testing.B) {
				b.SetBytes(int64(len(tc.haystack)))
				for b.Loop() {
					searcher.Index(tc.haystack)
				}
			})

			ranks := BuildRankTable(tc.haystack)
			corpusSearcher := NewSearcherWithRanks(tc.needle, ranks[:], true)
			b.Run(name+"/impl=Searcher_corpus", func(b *testing.B) {
				b.SetBytes(int64(len(tc.haystack)))
				for b.Loop() {
					corpusSearcher.Index(tc.haystack)
				}
			})

			if extra != nil {
				extra(b, name, tc.haystack, tc.needle)
			}
		}
	})

	b.Run("mode=fold", func(b *testing.B) {
		for _, tc := range cases {
			name := "scenario=" + tc.scenario + "/size=" + tc.size

			b.Run(name+"/impl=IndexFold", func(b *testing.B) {
				b.SetBytes(int64(len(tc.haystack)))
				for b.Loop() {
					IndexFold(tc.haystack, tc.needle)
				}
			})

			searcher := NewSearcher(tc.needle, false)
			b.Run(name+"/impl=Searcher", func(b *testing.B) {
				b.SetBytes(int64(len(tc.haystack)))
				for b.Loop() {
					searcher.Index(tc.haystack)
				}
			})

			ranks := BuildRankTable(tc.haystack)
			corpusSearcher := NewSearcherWithRanks(tc.needle, ranks[:], false)
			b.Run(name+"/impl=Searcher_corpus", func(b *testing.B) {
				b.SetBytes(int64(len(tc.haystack)))
				for b.Loop() {
					corpusSearcher.Index(tc.haystack)
				}
			})

			if extra != nil {
				extra(b, name, tc.haystack, tc.needle)
			}
		}
	})
}
