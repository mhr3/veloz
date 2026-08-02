//go:generate go run github.com/mhr3/goruntool@v0.1.1 github.com/mhr3/gocc/cmd/gocc@testing csrc/range_avx2.c -l -p utf8 -o ./ -a avx2 -O3
//go:generate go run github.com/mhr3/goruntool@v0.1.1 github.com/mhr3/gocc/cmd/gocc@testing csrc/range_neon.c -l -p utf8 -o ./ -a arm64 -O3
//go:generate go run github.com/mhr3/goruntool@v0.1.1 github.com/mhr3/gocc/cmd/gocc@testing csrc/utf8_uncased_search_avx2.c -l -p utf8 -o ./ -a avx2 -O3 --with-internal-functions
//go:generate go run github.com/mhr3/goruntool@v0.1.1 github.com/mhr3/gocc/cmd/gocc@testing csrc/utf8_uncased_search_neon.c -l -p utf8 -o ./ -a arm64 -O3 --with-internal-functions

package utf8
