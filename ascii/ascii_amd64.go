package ascii

import (
	"golang.org/x/sys/cpu"
)

var (
	hasSSE41 = cpu.X86.HasSSE41
	hasAVX2  = cpu.X86.HasAVX2
)

func ValidString(s string) bool {
	if hasAVX2 {
		return isAsciiAvx(s)
	}

	if hasSSE41 {
		return isAsciiSse(s)
	}

	return isAsciiGo(s)
}

func IndexMask(s string, mask byte) int {
	if hasAVX2 {
		return indexMaskAvx(s, mask)
	}

	return indexMaskGo(s, mask)
}

func EqualFold(a, b string) bool {
	if hasAVX2 && len(a) >= 32 {
		return equalFoldAvx(a, b)
	}

	if hasSSE41 && len(a) >= 16 {
		return equalFoldSse(a, b)
	}

	return equalFoldGo(a, b)
}

func IndexFold(a, b string) int {
	if hasAVX2 {
		return indexFoldAvx(a, b)
	}

	if hasSSE41 {
		return indexFoldSse(a, b)
	}

	return indexFoldGo(a, b)
}

func indexFoldRabinKarp(a, b string) int {
	if hasAVX2 {
		return indexFoldRabinKarpAvx(a, b)
	}

	if hasSSE41 {
		return indexFoldRabinKarpSse(a, b)
	}

	return indexFoldGo(a, b)
}
