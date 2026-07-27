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
| ascii.ValidString  | AMD Zen 3  | 8,715        | 117,592      | 13.5x   |
| ascii.EqualFold    | AMD Zen 3  | 1,742        | 33,300       | 19.1x   |
| ascii.IndexFold    | AMD Zen 3  | 5,828        | 19,372       | 3.3x    |
| ascii.IndexAny     | AMD Zen 3  | 1,716        | 8,775        | 5.1x    |
| utf8.ValidString   | AMD Zen 3  | 1,184        | 11,291       | 9.5x    |
| ascii.ValidString  | Graviton 2 | 4,902        | 33,642       | 6.9x    |
| ascii.EqualFold    | Graviton 2 |   601        | 10,839       | 18.0x   |
| ascii.IndexFold    | Graviton 2 | 2,728        | 8,431        | 3.1x    |
| ascii.IndexAny     | Graviton 2 |   698        | 9,488        | 13.6x   |
| utf8.ValidString   | Graviton 2 |   618        | 3,091        | 5.0x    |
| ascii.ValidString  | Apple M2   | 12,256       | 89,227       | 7.3x    |
| ascii.EqualFold    | Apple M2   | 1,949        | 31,804       | 16.3x   |
| ascii.IndexFold    | Apple M2   | 7,117        | 29,046       | 4.1x    |
| ascii.IndexAny     | Apple M2   | 1,950        | 28,527       | 14.6x   |
| utf8.ValidString   | Apple M2   | 1,673        | 10,014       | 6.0x    |
