//go:build !amd64

package utf8

// forEachKernel calls fn for every kernel of ValidString this CPU can run,
// which is a single one here.
func forEachKernel(fn func(kernel string)) {
	fn("neon")
}
