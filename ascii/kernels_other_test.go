//go:build !amd64

package ascii

import "testing"

// forEachKernel runs fn for every SIMD kernel this CPU can run, which is a
// single one here.
func forEachKernel(t *testing.T, fn func(t *testing.T)) {
	fn(t)
}
