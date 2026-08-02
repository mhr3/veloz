//go:build (amd64 || arm64) && !noasm

package utf8

import (
	"strings"
	"testing"
)

const indexFoldKernelFallbackForTest uint8 = 255

const (
	indexFoldKernelASCII         = 1
	indexFoldKernelWesternEurope = 2
	indexFoldKernelCentralEurope = 3
	indexFoldKernelCyrillic      = 4
	indexFoldKernelGreek         = 5
	indexFoldKernelArmenian      = 6
	indexFoldKernelVietnamese    = 7
	indexFoldKernelGeorgian      = 8
)

func TestIndexFoldSIMDKernels(t *testing.T) {
	if !indexFoldSIMDSupportedForTest() {
		t.Skip("CPU does not support the IndexFold SIMD implementation")
	}

	tests := []struct {
		name     string
		haystack string
		needle   string
		want     int
		kernel   uint8
	}{
		{name: "ASCII three probe", haystack: "!aZ3?", needle: "Az3", want: 1, kernel: indexFoldKernelASCII},
		{name: "ASCII four probe", haystack: "!aB3-?", needle: "Ab3-", want: 1, kernel: indexFoldKernelASCII},
		{name: "Western Europe", haystack: "!àéö?", needle: "ÀÉÖ", want: 1, kernel: indexFoldKernelWesternEurope},
		{name: "Central Europe", haystack: "!\u0105\u010d\u0119\u017e?", needle: "\u0104\u010c\u0118\u017d", want: 1, kernel: indexFoldKernelCentralEurope},
		{name: "Cyrillic", haystack: "!привет?", needle: "ПРИВЕТ", want: 1, kernel: indexFoldKernelCyrillic},
		{name: "Greek", haystack: "!κόσμος?", needle: "ΚΌΣΜΟΣ", want: 1, kernel: indexFoldKernelGreek},
		{name: "Armenian", haystack: "!աբգդ?", needle: "ԱԲԳԴ", want: 1, kernel: indexFoldKernelArmenian},
		{name: "Vietnamese", haystack: "!ắệơư?", needle: "ẮỆƠƯ", want: 1, kernel: indexFoldKernelVietnamese},
		{name: "Georgian", haystack: "!Sაბგდ?", needle: "sაბგდ", want: 1, kernel: indexFoldKernelGeorgian},
	}

	for _, test := range tests {
		t.Run(test.name, func(t *testing.T) {
			for _, offset := range []int{0, 1, 15, 16, 29, 30, 31, 32, 33, 63, 64, 95} {
				haystack := strings.Repeat("!", offset) + test.haystack[1:]
				haystack += strings.Repeat("?", indexFoldSIMDMinBytes+64-len(haystack))
				want := offset + test.want - 1

				var metadata indexFoldNeedleMetadata
				if got := indexFoldSIMDRawForTest(haystack, test.needle, &metadata); got != want {
					t.Fatalf("offset %d: raw SIMD result = %d, want %d (kernel %d)", offset, got, want, metadata.kernelID)
				}
				if metadata.kernelID != test.kernel {
					t.Fatalf("offset %d: selected kernel %d, want %d", offset, metadata.kernelID, test.kernel)
				}
				if got := IndexFold(haystack, test.needle); got != want {
					t.Fatalf("offset %d: IndexFold(%q, %q) = %d, want %d", offset, haystack, test.needle, got, want)
				}
			}
		})
	}
}

func TestIndexFoldSIMDDangerAndMisses(t *testing.T) {
	if !indexFoldSIMDSupportedForTest() {
		t.Skip("CPU does not support the IndexFold SIMD implementation")
	}

	tests := []struct {
		name     string
		haystack string
		needle   string
		want     int
	}{
		{name: "sharp s expansion", haystack: "Straße", needle: "STRASSE", want: 0},
		{name: "ligature expansion", haystack: "ofﬁce", needle: "OFFICE", want: 0},
		{name: "chunk boundary miss", haystack: "abcdefghijklmnop", needle: "QRSTUV", want: -1},
		{name: "malformed needle fallback", haystack: "a\xffB", needle: "\xffb", want: 1},
	}

	for _, test := range tests {
		t.Run(test.name, func(t *testing.T) {
			prefix := strings.Repeat("!", 31)
			if len(test.haystack) >= indexFoldSIMDMinBytes {
				prefix = ""
			}
			haystack := prefix + test.haystack
			if padding := indexFoldSIMDMinBytes + 64 - len(haystack); padding > 0 {
				haystack += strings.Repeat("?", padding)
			}
			want := test.want
			if want >= 0 {
				want += len(prefix)
			}
			var metadata indexFoldNeedleMetadata
			if got := indexFoldSIMDRawForTest(haystack, test.needle, &metadata); got != want {
				t.Fatalf("raw SIMD result = %d, want %d (metadata: %+v)", got, want, metadata)
			}
			if got := IndexFold(haystack, test.needle); got != want {
				t.Fatalf("IndexFold(%q, %q) = %d, want %d", haystack, test.needle, got, want)
			}
		})
	}
}

func TestIndexFoldSIMDSerialFallbackRolling(t *testing.T) {
	if !indexFoldSIMDSupportedForTest() {
		t.Skip("CPU does not support the IndexFold SIMD implementation")
	}

	needle := strings.Repeat("ի", 15) + "խ"
	match := strings.Repeat("իԻԻի", 3) + "իԻԻխ"
	for _, runeOffset := range []int{0, 1, 2, 15, 16, 31, 32} {
		haystack := strings.Repeat("ի", runeOffset) + match + strings.Repeat("ի", 128)
		want := runeOffset * len("ի")
		var metadata indexFoldNeedleMetadata
		if got := indexFoldSIMDRawForTest(haystack, needle, &metadata); got != want {
			t.Fatalf("rune offset %d: raw SIMD result = %d, want %d (metadata: %+v)", runeOffset, got, want, metadata)
		}
		if metadata.kernelID != indexFoldKernelFallbackForTest {
			t.Fatalf("rune offset %d: selected kernel %d, want serial fallback", runeOffset, metadata.kernelID)
		}
		if got := IndexFold(haystack, needle); got != want {
			t.Fatalf("rune offset %d: IndexFold result = %d, want %d", runeOffset, got, want)
		}
	}
}
