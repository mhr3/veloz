//go:generate go run github.com/mhr3/gocc/cmd/gocc@v0.30.2 csrc/ascii_sse.c -l -p ascii -o ./ -a amd64 -O3 -msse4.1
//go:generate go run github.com/mhr3/gocc/cmd/gocc@v0.30.2 csrc/ascii_avx2.c -l -p ascii -o ./ -a avx2 -O3
//go:generate go run github.com/mhr3/gocc/cmd/gocc@v0.30.2 csrc/ascii_neon.c -l -p ascii -o ./ -a arm64 -O3

package ascii
