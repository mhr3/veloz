# veloz

Veloz is a high-performance SIMD-accelerated library for ASCII and UTF-8 string operations in Go. It provides fast validation, case-insensitive string matching, and substring search, leveraging SIMD instructions on supported architectures.

While amd64 SIMD optimizations are common in the Go ecosystem, arm64 (NEON) support is often overlooked. Veloz provides first-class SIMD acceleration for arm64, ideal for AWS Graviton, Apple Silicon, and other ARM platforms.

## Features

- **Validation**: `ValidString` - high-speed ASCII string validation
- **Comparison**: `EqualFold` - case-insensitive ASCII string comparison
- **Substring search**: 
  - `Index` - case-sensitive search
  - `IndexFold` - case-insensitive search  
  - `Searcher` - precomputed pattern for repeated searches
- **Character set search**: `IndexAny`, `ContainsAny` - find any byte from a set
- **UTF-8**: `utf8.ValidString` - fast UTF-8 validation
- SIMD support for amd64 (AVX2, SSE4.1) and arm64 (NEON)
- Pure Go fallback for other architectures

## Installation

```sh
go get github.com/mhr3/veloz
```

## Usage

### Basic Substring Search

```go
import "github.com/mhr3/veloz/ascii"

// Case-sensitive search
idx := ascii.Index("Hello, World!", "World")  // 7

// Case-insensitive search  
idx := ascii.IndexFold("Hello, World!", "WORLD")  // 7
```

### Searcher for Repeated Searches

`Searcher` precomputes rare-byte offsets from the pattern, amortizing the analysis cost across many searches. Use it when searching for the same needle in multiple haystacks.

```go
// Create a case-insensitive searcher (false = case-insensitive)
searcher := ascii.NewSearcher("error", false)

// Reuse for many searches - no per-call pattern analysis
for _, line := range logLines {
    if idx := searcher.Index(line); idx >= 0 {
        // found
    }
}

// Case-sensitive searcher (true = case-sensitive)
exact := ascii.NewSearcher(`"trace_id":`, true)
```

The `caseSensitive` boolean parameter controls matching behavior:
- `true` - exact byte matching (like `strings.Index`)
- `false` - ASCII case-insensitive matching (like `strings.EqualFold`)

### Corpus-Specific Optimization

For specialized data where byte frequencies differ from typical English text (JSON logs, hex dumps, DNA sequences), use `BuildRankTable` and `NewSearcherWithRanks`:

```go
// Build rank table from a corpus sample (do once at init)
ranks := ascii.BuildRankTable(corpusSample)

// Create optimized searcher
searcher := ascii.NewSearcherWithRanks(`"trace_id":`, ranks[:], true)
```

This is particularly effective for JSON data where characters like `"`, `:`, and `{` are common - the default English-derived frequency table treats these as rare, causing false positives.

### Character Set Search

```go
// Find first occurrence of any character from set
idx := ascii.IndexAny("hello world", " \t\n")  // 5 (space)

// Check if any character exists
found := ascii.ContainsAny("hello", "aeiou")  // true

// Precompute CharSet for repeated searches
cs := ascii.NewCharSet(" \t\n\r")
for _, line := range lines {
    if idx := cs.IndexAny(line); idx >= 0 {
        // found whitespace
    }
}
```

### Validation and Comparison

```go
ascii.ValidString("Hello, World!")   // true
ascii.ValidString("Hello, 世界!")    // false (contains non-ASCII)

ascii.EqualFold("Hello", "HELLO")    // true
ascii.HasPrefixFold("Hello", "HE")   // true
ascii.HasSuffixFold("Hello", "LO")   // true
```

### UTF-8 Validation

```go
import "github.com/mhr3/veloz/utf8"

utf8.ValidString("Hello, 世界!")         // true
utf8.ValidString(string([]byte{0xff}))  // false
```

## API Reference

### Substring Search

| Function | Description |
|----------|-------------|
| `Index(haystack, needle)` | Case-sensitive substring search |
| `IndexFold(haystack, needle)` | Case-insensitive substring search |
| `NewSearcher(pattern, caseSensitive)` | Create precomputed Searcher |
| `BuildRankTable(corpus)` | Build byte frequency table from corpus |
| `NewSearcherWithRanks(pattern, ranks, caseSensitive)` | Searcher with custom byte frequency table |
| `Searcher.Index(haystack)` | Search using precomputed pattern |

### Character Set Search

| Function | Description |
|----------|-------------|
| `IndexAny(s, chars)` | Find first byte from chars |
| `ContainsAny(s, chars)` | Check if any byte from chars exists |
| `NewCharSet(chars)` | Precompute character set |
| `CharSet.IndexAny(s)` | Search with precomputed CharSet |
| `CharSet.ContainsAny(s)` | Check with precomputed CharSet |

### Validation and Comparison

| Function | Description |
|----------|-------------|
| `ValidString(s)` | Check if string is valid ASCII |
| `EqualFold(a, b)` | Case-insensitive equality |
| `HasPrefixFold(s, prefix)` | Case-insensitive prefix check |
| `HasSuffixFold(s, suffix)` | Case-insensitive suffix check |
| `IndexNonASCII(s)` | Find first non-ASCII byte |

## Benchmarks

All substring search benchmarks use the "json" scenario (searching for `"name":` in JSON-like data with high false-positive rates). This represents real-world workloads where stdlib performance degrades.

Raw benchmark data: [ascii/bench/](ascii/bench/)
- [m3_max.txt](ascii/bench/m3_max.txt), [m3_max_indexany.txt](ascii/bench/m3_max_indexany.txt) — Apple M3 Max
- [graviton4.txt](ascii/bench/graviton4.txt), [graviton4_indexany.txt](ascii/bench/graviton4_indexany.txt) — AWS Graviton 4
- [graviton3.txt](ascii/bench/graviton3.txt), [graviton3_indexany.txt](ascii/bench/graviton3_indexany.txt) — AWS Graviton 3

### Substring Search: Index vs stdlib (1KB, case-sensitive)

| Scenario | strings.Index | ascii.Index | Speedup |
|----------|-------------:|------------:|--------:|
| json | 309 ns (3.3 GB/s) | 24 ns (42 GB/s) | **12.9x** |
| samechar | 394 ns (2.5 GB/s) | 25 ns (40 GB/s) | **15.8x** |
| periodic | 315 ns (3.2 GB/s) | 25 ns (40 GB/s) | **12.6x** |
| logdate | 533 ns (1.9 GB/s) | 177 ns (5.6 GB/s) | **3.0x** |
| codebraces | 510 ns (2.0 GB/s) | 147 ns (7.0 GB/s) | **3.5x** |
| hexdata | 275 ns (3.6 GB/s) | 192 ns (5.1 GB/s) | **1.4x** |
| digits | 320 ns (3.1 GB/s) | 193 ns (5.2 GB/s) | **1.7x** |
| match_end | 16 ns (63 GB/s) | 17 ns (60 GB/s) | 0.9x |
| match_mid | 11 ns (91 GB/s) | 16 ns (63 GB/s) | 0.7x |
| rarebyte | 16 ns (63 GB/s) | 17 ns (60 GB/s) | 0.9x |
| needle3 | 16 ns (63 GB/s) | 24 ns (42 GB/s) | 0.7x |
| dna | 16 ns (63 GB/s) | 16 ns (63 GB/s) | 1.0x |
| notfound | 14 ns (74 GB/s) | 14 ns (74 GB/s) | 1.0x |

*Apple M3 Max. Scenarios where stdlib is faster have speedup < 1.0x.*

**When veloz wins**: High false-positive patterns (json, samechar, periodic) where the first byte of the needle appears frequently in the haystack. The staged rare-byte filtering avoids verifying every candidate.

**When stdlib wins**: Short haystacks with early matches (match_mid, needle3) where SIMD setup overhead exceeds the scan time. For these cases, stdlib's simple loop is faster.

### Case-Insensitive Search: IndexFold

| Platform | 1KB | 64KB |
|----------|----:|-----:|
| Apple M3 Max | 43 ns (23 GB/s) | 1.8 µs (35 GB/s) |
| AWS Graviton 4 | 74 ns (14 GB/s) | 3.6 µs (18 GB/s) |
| AWS Graviton 3 | 79 ns (13 GB/s) | 4.2 µs (16 GB/s) |

*"json" scenario*

### Searcher with Corpus-Tuned Ranks

For JSON data, `Searcher_corpus` (using corpus-derived byte ranks) is **2x faster** than default rare-byte selection:

| Platform | Index | Searcher | Searcher_corpus | Corpus Gain |
|----------|------:|---------:|----------------:|------------:|
| Apple M3 Max | 1.3 µs | 1.4 µs | **0.6 µs** | **2.3x** |
| AWS Graviton 4 | 2.7 µs | 2.9 µs | **1.6 µs** | **1.8x** |
| AWS Graviton 3 | 2.7 µs | 3.0 µs | **1.5 µs** | **2.0x** |

*64KB JSON input, case-sensitive search*

**Why corpus tuning helps**: The default frequency table is derived from English text, where `"`, `:`, and `{` are rare. In JSON, these appear constantly. Corpus-tuned ranks correctly identify them as common, choosing better filter bytes.

### IndexAny: SIMD vs Pure Go

| Chars in set | Pure Go | ascii.IndexAny | Speedup |
|-------------:|--------:|---------------:|--------:|
| 1 | 381 ns (2.7 GB/s) | 32 ns (32 GB/s) | **12x** |
| 16 | 404 ns (2.5 GB/s) | 45 ns (23 GB/s) | **9x** |
| 64 | 490 ns (2.1 GB/s) | 143 ns (7 GB/s) | **3.4x** |

*Apple M3 Max, 1KB input, match not found*

### Core Functions (1KB input)

| Function | M3 Max | Graviton 4 | Graviton 3 |
|----------|-------:|-----------:|-----------:|
| `ValidString` | 107 GB/s | 61 GB/s | 54 GB/s |
| `EqualFold` | 26 GB/s | 14 GB/s | 11 GB/s |

## Implementation

The substring search uses an adaptive three-stage approach combining techniques from [memchr](https://github.com/BurntSushi/memchr) (rare-byte selection) and [Sneller](https://github.com/SnellerInc/sneller) (SIMD string matching):

1. **Stage 1: Single rare-byte filter** - Fast SIMD scan for one rare byte from the pattern. Exits early if match density is low.

2. **Stage 2: Two-byte filter** - SIMD scan for two rare bytes simultaneously. More selective when Stage 1 hits too many false positives.

3. **Stage 3: Rabin-Karp fallback** - Rolling hash with SIMD verification. Guaranteed linear time for pathological patterns (e.g., `"aaa"` in `"aaaa...a"`).

The staged approach adapts to data characteristics at runtime, avoiding worst-case behavior that affects simpler algorithms.

### Rare-Byte Selection

The search algorithm's performance depends on finding rare bytes in the pattern to minimize false positives. The default frequency table is derived from the CIA World Factbook, rustc source, and Septuaginta—representative of English text.

For domain-specific data, `NewSearcherWithRanks` allows custom frequency tables. This is critical for JSON where `"`, `:`, and `{` appear frequently but are treated as rare by English-derived tables.

## License

MIT
