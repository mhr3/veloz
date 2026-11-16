# veloz

Veloz is a high-performance SIMD-accelerated library for ASCII and UTF-8 string operations in Go. It provides fast validation and case-insensitive string matching, leveraging SIMD instructions on supported architectures for significant performance improvements over standard library implementations.

While amd64 SIMD optimizations are becoming common in the Go ecosystem, arm64 (NEON) support is often overlooked. Veloz focuses on providing first-class SIMD acceleration for arm64, making it ideal for deployment on ARM-based servers like AWS Graviton, Apple Silicon, and other ARM platforms.

Another motivation for veloz is maintainability. Many Go packages rely on hand-rolled assembly for performance-critical code, which is notoriously difficult to maintain, debug, and extend. By writing SIMD implementations in C and transpiling them to Go assembly using [gocc](https://github.com/mhr3/gocc), veloz keeps the source code readable and maintainable while still delivering native performance.

## Features

- High-speed ASCII string validation
- Case-insensitive ASCII string comparison (`EqualFold`)
- Case-insensitive ASCII substring search (`IndexFold`)
- Fast UTF-8 validation
- SIMD support for amd64 (AVX2, SSE4.1) and arm64 (NEON)
- Pure Go fallback for other architectures

## Installation

To install the library, use `go get`:

```sh
go get github.com/mhr3/veloz
```

## Usage

### ASCII Operations

The `ascii` package provides functions for validating and searching ASCII strings:

```go
package main

import (
    "fmt"

    "github.com/mhr3/veloz/ascii"
)

func main() {
    // Check if a string contains only ASCII characters
    fmt.Println(ascii.ValidString("Hello, World!"))  // true
    fmt.Println(ascii.ValidString("Hello, 世界!"))   // false

    // Case-insensitive string comparison
    fmt.Println(ascii.EqualFold("Hello", "HELLO"))   // true
    fmt.Println(ascii.EqualFold("Hello", "World"))   // false

    // Case-insensitive substring search
    fmt.Println(ascii.IndexFold("Hello, World!", "WORLD"))  // 7
    fmt.Println(ascii.IndexFold("Hello, World!", "foo"))    // -1
}
```

### UTF-8 Validation

The `utf8` package provides fast UTF-8 string validation:

```go
package main

import (
    "fmt"

    "github.com/mhr3/veloz/utf8"
)

func main() {
    // Validate UTF-8 strings
    fmt.Println(utf8.ValidString("Hello, 世界!"))           // true
    fmt.Println(utf8.ValidString("Valid UTF-8 string"))    // true
    fmt.Println(utf8.ValidString(string([]byte{0xff})))    // false (invalid UTF-8)
}
```

## Benchmarks

| Function           | CPU        | naive (MB/s) | veloz (MB/s) | Speedup |
|--------------------|------------|--------------|--------------|---------|
| ascii.ValidString  | AMD Zen 3  | 8,875        | 118,663      | 13.4x   |
| ascii.EqualFold    | AMD Zen 3  | 2,269        | 33,270       | 14.7x   |
| ascii.IndexFold    | AMD Zen 3  | 5,771        | 45,545       | 7.9x    |
| ascii.IndexAny     | AMD Zen 3  | 3,254        | 8,077        | 2.5x    |
| utf8.ValidString   | AMD Zen 3  | 1,532        | 11,330       | 7.4x    |
| ascii.ValidString  | Graviton 2 | 5,330        | 33,768       | 6.3x    |
| ascii.EqualFold    | Graviton 2 |   708        | 10,963       | 15.5x   |
| ascii.IndexFold    | Graviton 2 | 2,638        | 12,250       | 4.6x    |
| ascii.IndexAny     | Graviton 2 | 1,203        | 8,896        | 7.4x    |
| utf8.ValidString   | Graviton 2 |   678        | 3,076        | 4.5x    |
| ascii.ValidString  | Graviton 4 | 10,825       | 74,576       | 6.9x    |
| ascii.EqualFold    | Graviton 4 | 1,323        | 21,530       | 16.3x   |
| ascii.IndexFold    | Graviton 4 | 6,042        | 27,300       | 4.5x    |
| ascii.IndexAny     | Graviton 4 | 2,419        | 17,381       | 7.2x    |
| utf8.ValidString   | Graviton 4 | 1,638        | 6,075        | 3.7x    |
| ascii.ValidString  | Apple M2   | 12,132       | 88,602       | 7.3x    |
| ascii.EqualFold    | Apple M2   | 1,899        | 31,959       | 16.8x   |
| ascii.IndexFold    | Apple M2   | 7,024        | 42,335       | 6.0x    |
| ascii.IndexAny     | Apple M2   | 2,866        | 25,698       | 9.0x    |
| utf8.ValidString   | Apple M2   | 2,009        | 9,902        | 4.9x    |
