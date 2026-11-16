package ascii

import "testing"

// forEachKernel runs fn as a subtest for every SIMD kernel this CPU can run,
// by taking the better ones away from the dispatch. Without it the SSE4.1
// kernels are never called where there is AVX2.
func forEachKernel(t *testing.T, fn func(t *testing.T)) {
	avx2, sse41 := hasAVX2, hasSSE41
	defer func() { hasAVX2, hasSSE41 = avx2, sse41 }()

	if avx2 {
		t.Run("avx2", fn)
	}
	hasAVX2 = false
	if sse41 {
		t.Run("sse4.1", fn)
	}
	if !avx2 && !sse41 {
		t.Run("go", fn)
	}
}
