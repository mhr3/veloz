//go:generate go run github.com/mhr3/gocc/cmd/gocc@v0.30.2 csrc/range_avx2.c -l -p utf8 -o ./ -a avx2 -O3
//go:generate go run github.com/mhr3/gocc/cmd/gocc@v0.30.2 csrc/range_avx512.c -l -p utf8 -o ./ -a avx512 -O3 -mavx512bw -mavx512vbmi
//go:generate go run github.com/mhr3/gocc/cmd/gocc@v0.30.2 csrc/range_neon.c -l -p utf8 -o ./ -a arm64 -O3
//go:generate go run github.com/mhr3/gocc/cmd/gocc@v0.30.2 csrc/utf8_uncased_search_avx2.c -l -p utf8 -o ./ -a avx2 -O3 --with-internal-functions
//go:generate go run github.com/mhr3/gocc/cmd/gocc@v0.30.2 csrc/utf8_uncased_search_neon.c -l -p utf8 -o ./ -a arm64 -O3 --with-internal-functions

package utf8
