//go:build !noasm && arm64

package ascii

import "testing"

func BenchmarkIndex(b *testing.B) {
	runIndexBenchmarks(b, func(b *testing.B, name, haystack, needle string) {
		// ARM64 NEON Rabin-Karp variants
		b.Run(name+"/impl=RabinKarp_exact", func(b *testing.B) {
			b.SetBytes(int64(len(haystack)))
			for b.Loop() {
				indexExactRabinKarp(haystack, needle)
			}
		})

		b.Run(name+"/impl=RabinKarp_fold", func(b *testing.B) {
			b.SetBytes(int64(len(haystack)))
			for b.Loop() {
				indexFoldRabinKarp(haystack, needle)
			}
		})

		normNeedle := normalizeASCII(needle)
		b.Run(name+"/impl=RabinKarp_prefolded", func(b *testing.B) {
			b.SetBytes(int64(len(haystack)))
			for b.Loop() {
				indexPrefoldedRabinKarp(haystack, normNeedle)
			}
		})

		b.Run(name+"/impl=RabinKarp_go", func(b *testing.B) {
			b.SetBytes(int64(len(haystack)))
			for b.Loop() {
				indexFoldRabinKarpGo(haystack, needle)
			}
		})
	})
}
