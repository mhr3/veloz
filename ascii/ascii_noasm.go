//go:build noasm && arm64

package ascii

func ValidString(s string) bool {
	return indexMaskGo(s, 0x80) == -1
}

func IndexMask(s string, mask byte) int {
	return indexMaskGo(s, mask)
}

func EqualFold(a, b string) bool {
	return equalFoldGo(a, b)
}

func IndexFold(a, b string) int {
	return indexFoldGo(a, b)
}

func indexFoldRabinKarp(a, b string) int {
	return indexFoldGo(a, b)
}
