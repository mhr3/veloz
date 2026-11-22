package utf8

// forEachKernel calls fn for every kernel of ValidString this CPU can run, by
// taking the better ones away from the dispatch. Without it the AVX2 kernel
// is never called where there is AVX-512.
func forEachKernel(fn func(kernel string)) {
	avx512, avx2 := hasAVX512, hasAVX2
	defer func() { hasAVX512, hasAVX2 = avx512, avx2 }()

	if avx512 {
		fn("avx512")
	}
	hasAVX512 = false
	if avx2 {
		fn("avx2")
	}
	if !avx512 && !avx2 {
		fn("go")
	}
}
