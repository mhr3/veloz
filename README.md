# veloz

Veloz is a high-performance SIMD-accelerated library for ASCII and UTF-8 string operations in Go. It provides fast validation and case-insensitive string matching, leveraging SIMD instructions on supported architectures for significant performance improvements over standard library implementations.

While amd64 SIMD optimizations are becoming common in the Go ecosystem, arm64 (NEON) support is often overlooked. Veloz focuses on providing first-class SIMD acceleration for arm64, making it ideal for deployment on ARM-based servers like AWS Graviton, Apple Silicon, and other ARM platforms.

Another motivation for veloz is maintainability. Many Go packages rely on hand-rolled assembly for performance-critical code, which is notoriously difficult to maintain, debug, and extend. By writing SIMD implementations in C and transpiling them to Go assembly using [gocc](https://github.com/mhr3/gocc), veloz keeps the source code readable and maintainable while still delivering native performance.

## Features

- High-speed ASCII string validation
- Case-insensitive ASCII string comparison (`EqualFold`)
- Case-insensitive ASCII substring search (`IndexFold`)
- Fast UTF-8 validation
- SIMD support for amd64 (AVX-512, AVX2, SSE4.1) and arm64 (NEON)
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
| ascii.ValidString  | AMD Zen 3  | 8,905        | 119,152      | 13.4x   |
| ascii.EqualFold    | AMD Zen 3  | 2,268        | 33,342       | 14.7x   |
| ascii.IndexFold    | AMD Zen 3  | 6,359        | 45,606       | 7.2x    |
| ascii.IndexAny     | AMD Zen 3  | 3,316        | 8,061        | 2.4x    |
| utf8.ValidString   | AMD Zen 3  | 1,535        | 14,387       | 9.4x    |
| ascii.ValidString  | AMD Zen 5  | 11,899       | 185,774      | 15.6x   |
| ascii.EqualFold    | AMD Zen 5  | 3,344        | 51,082       | 15.3x   |
| ascii.IndexFold    | AMD Zen 5  | 9,007        | 68,491       | 7.6x    |
| ascii.IndexAny     | AMD Zen 5  | 3,999        | 10,682       | 2.7x    |
| utf8.ValidString   | AMD Zen 5  | 2,338        | 78,415       | 33.5x   |
| ascii.ValidString  | Graviton 2 | 5,330        | 33,768       | 6.3x    |
| ascii.EqualFold    | Graviton 2 |   708        | 10,963       | 15.5x   |
| ascii.IndexFold    | Graviton 2 | 2,638        | 12,250       | 4.6x    |
| ascii.IndexAny     | Graviton 2 | 1,203        | 8,896        | 7.4x    |
| utf8.ValidString   | Graviton 2 |   678        | 4,514        | 6.7x    |
| ascii.ValidString  | Apple M2   | 12,187       | 90,323       | 7.4x    |
| ascii.EqualFold    | Apple M2   | 1,950        | 31,952       | 16.4x   |
| ascii.IndexFold    | Apple M2   | 7,251        | 41,168       | 5.7x    |
| ascii.IndexAny     | Apple M2   | 2,852        | 25,595       | 9.0x    |
| utf8.ValidString   | Apple M2   | 2,000        | 12,558       | 6.3x    |
