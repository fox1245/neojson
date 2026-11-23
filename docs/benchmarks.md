# Cross-Architecture JSON Benchmarks

Reproducible single-thread benchmarks comparing `neojson` against six widely used C and C++ JSON libraries on two CPU architectures: **x86-64 (AVX2)** and **ARM64 (NEON)**.

All benchmark binaries live under [`bench/`](../bench/), compile with identical optimization flags (`-O3 -DNDEBUG`, no LTO, no `-march=native`; C++20 benchmark harnesses with `libneojson` built as C++17 per root `CMakeLists.txt`), verify output parity across all 7 libraries via `--verify` before timing, and report the median across **7 alternating paired rounds × 7 measured batches** (>= 25 ms per batch). Throughput uses SI megabytes (`MB/s = 10^6 bytes/s`), and ratios in tables show median latency relative to the `yyjson (C)` baseline (lower ratio = faster; higher `MB/s` or `K/s` = faster).

## Competitor Lineup & Versions

| Target Binary | Library & Version | DOM / Execution Model | Notes |
|---|---|---|---|
| `bench_neojson` | **`neojson`** (vendored `yyjson` v0.13.0) | Ordered mutable DOM (`C++17`/`C++20`) | Insertion-ordered keys, copy-on-write string/node storage, native AVX2 (x86-64) & NEON (ARM64) SIMD parsers |
| `bench_yyjson` | `yyjson` v0.13.0 | Immutable/mutable C DOM | Pure C API baseline (`yyjson_read`, `yyjson_write`, `yyjson_mut_doc`) |
| `bench_simdjson` | `simdjson` v5.3.0 | Read-only tape DOM | Pre-padded `simdjson::padded_string` (padding copy excluded from timed loop), reused `dom::parser`; mutable build N/A |
| `bench_cppyyjson` | `cpp-yyjson` v2.1.0 | Ordered DOM wrapper | Linked against the same vendored `yyjson` v0.13.0 static library |
| `bench_rapidjson` | `RapidJSON` (`297637bc`) | Mutable DOM | Header-only include; baseline SIMD explicitly enabled (`RAPIDJSON_SSE2` on x86-64, `RAPIDJSON_NEON` on ARM64) |
| `bench_nlohmann_json` | `nlohmann::json` v3.11.3 | Unordered mutable DOM | Standard single-header `nlohmann::json` (`std::map`) |
| `bench_nlohmann_ordered` | `nlohmann::ordered_json` v3.11.3 | Ordered mutable DOM | Nearest semantic peer (`nlohmann::fifo_map` insertion order) |

## Corpus (`bench/corpus.json`)

Total corpus size: **7,937,600 B (7.94 MB)**. Each file's SHA-256 digest is verified at configure time by `bench/CMakeLists.txt`.

| File | Size | Workload Profile | SHA-256 Prefix |
|---|---|---|---|
| `canada.json` | 2,251,051 B (2.25 MB) | Dense IEEE-754 float coordinates | `f83b3b354030d5dd...` |
| `citm_catalog.json` | 1,727,204 B (1.73 MB) | Nested arrays, integers & maps | `a73e7a883f6ea8de...` |
| `gsoc-2018.json` | 3,327,831 B (3.33 MB) | Long strings & deep records | `72f1ef4898d88049...` |
| `twitter.json` | 631,514 B (632 KB) | Mixed timeline objects & tweets | `a08b769f32b95f42...` |

## Test Environments

| Architecture | CPU | Logical CPUs | Compiler & Flags | Build Type |
|---|---|---|---|---|
| **x86-64 (AVX2)** | Intel Core i7-8750H (AVX2) | 12 | GCC 13.3.0 (`-O3 -DNDEBUG`, C++20 harnesses / C++17 `libneojson`) | Portable Release (no LTO, no `-march=native`) |
| **ARM64 (NEON)** | ARM Neoverse-N1 (AArch64 NEON) | 4 | GCC 13.3.0 (`-O3 -DNDEBUG`, C++20 harnesses / C++17 `libneojson`) | Portable Release (no LTO, no `-march=native`) |

---

## x86-64 (AVX2) Results

![JSON Throughput on x86-64 AVX2](../assets/bench_overview_x86_64.svg)

### Aggregate Throughput & Latency (x86-64 AVX2)

Each cell reports `throughput (median time, ratio vs yyjson C)`. Ratios `< 1.00×` are faster than C `yyjson`.

| Workload | `neojson` | `yyjson` (C) | `simdjson` (DOM) | `cpp-yyjson` | `RapidJSON` | `nlohmann::json` | `nlohmann::ordered` |
|---|---|---|---|---|---|---|---|
| Parse (Aggregate, 4 files) | **2,557 MB/s (3.10ms, 0.54×)** | 1,393 MB/s (5.70ms, 1.00×) | 1,779 MB/s (4.46ms, 0.78×) | 1,382 MB/s (5.75ms, 1.01×) | 599 MB/s (13.3ms, 2.33×) | 96 MB/s (82.3ms, 14.44×) | 83 MB/s (95.7ms, 16.80×) |
| Parse + Full Traversal Read (4 files) | **1,498 MB/s (5.30ms, 0.79×)** | 1,189 MB/s (6.67ms, 1.00×) | 1,404 MB/s (5.65ms, 0.85×) | 1,196 MB/s (6.64ms, 0.99×) | 564 MB/s (14.1ms, 2.11×) | 94 MB/s (84.4ms, 12.64×) | 81 MB/s (97.5ms, 14.61×) |
| Serialize / Write Minified (4 files) | **2,574 MB/s (3.08ms, 0.70×)** | 1,810 MB/s (4.39ms, 1.00×) | 1,604 MB/s (4.95ms, 1.13×) | 1,755 MB/s (4.52ms, 1.03×) | 716 MB/s (11.1ms, 2.53×) | 277 MB/s (28.6ms, 6.52×) | 321 MB/s (24.7ms, 5.63×) |
| Mutable DOM Build (4 files) | 2,442 MB/s (3.25ms, 1.62×) | 3,962 MB/s (2.00ms, 1.00×) | — *(read-only)* | 4,180 MB/s (1.90ms, 0.95×) | **5,215 MB/s (1.52ms, 0.76×)** | 360 MB/s (22.1ms, 11.02×) | 233 MB/s (34.1ms, 17.04×) |
| Field Lookup (`twitter.json`) | **161.0 K/s (6.21 µs, 0.60×)** | 97.3 K/s (10.28 µs, 1.00×) | 76.2 K/s (13.12 µs, 1.28×) | 88.5 K/s (11.30 µs, 1.09×) | 105.4 K/s (9.49 µs, 0.92×) | 37.1 K/s (26.94 µs, 2.61×) | 123.6 K/s (8.09 µs, 0.78×) |

### Per-File Breakdown (x86-64 AVX2)

![Per-File Breakdown on x86-64 AVX2](../assets/bench_per_file_x86_64.svg)

| Workload | File | Size | `neojson` | `yyjson` (C) | `simdjson` (DOM) | `cpp-yyjson` | `RapidJSON` | `nlohmann::json` | `nlohmann::ordered` |
|---|---|---|---|---|---|---|---|---|---|
| `parse` | `canada.json` | 2.25 MB | **1,230 MB/s (1,830 µs, 0.64×)** | 791 MB/s (2,847 µs, 1.00×) | 827 MB/s (2,722 µs, 0.96×) | 785 MB/s (2,867 µs, 1.01×) | 472 MB/s (4,766 µs, 1.67×) | 53 MB/s (42,148 µs, 14.81×) | 54 MB/s (41,520 µs, 14.58×) |
| `parse` | `citm_catalog.json` | 1.73 MB | **4,012 MB/s (431 µs, 0.42×)** | 1,671 MB/s (1,033 µs, 1.00×) | 2,841 MB/s (608 µs, 0.59×) | 1,677 MB/s (1,030 µs, 1.00×) | 1,225 MB/s (1,410 µs, 1.37×) | 159 MB/s (10,833 µs, 10.48×) | 102 MB/s (16,883 µs, 16.32×) |
| `parse` | `gsoc-2018.json` | 3.33 MB | **5,151 MB/s (646 µs, 0.43×)** | 2,234 MB/s (1,489 µs, 1.00×) | 3,721 MB/s (894 µs, 0.60×) | 2,184 MB/s (1,524 µs, 1.02×) | 555 MB/s (5,991 µs, 4.02×) | 142 MB/s (23,383 µs, 15.71×) | 113 MB/s (29,435 µs, 19.75×) |
| `parse` | `twitter.json` | 632 KB | **3,192 MB/s (198 µs, 0.60×)** | 1,915 MB/s (330 µs, 1.00×) | 2,644 MB/s (239 µs, 0.72×) | 1,945 MB/s (325 µs, 0.99×) | 578 MB/s (1,092 µs, 3.32×) | 107 MB/s (5,929 µs, 17.99×) | 80 MB/s (7,899 µs, 23.95×) |
| `read` | `canada.json` | 2.25 MB | **778 MB/s (2,895 µs, 0.82×)** | 640 MB/s (3,517 µs, 1.00×) | 644 MB/s (3,497 µs, 0.99×) | 646 MB/s (3,484 µs, 0.99×) | 425 MB/s (5,292 µs, 1.50×) | 52 MB/s (43,374 µs, 12.34×) | 53 MB/s (42,705 µs, 12.14×) |
| `read` | `citm_catalog.json` | 1.73 MB | **2,183 MB/s (791 µs, 0.66×)** | 1,448 MB/s (1,193 µs, 1.00×) | 2,106 MB/s (820 µs, 0.69×) | 1,473 MB/s (1,173 µs, 0.98×) | 1,114 MB/s (1,550 µs, 1.30×) | 155 MB/s (11,153 µs, 9.35×) | 101 MB/s (17,142 µs, 14.37×) |
| `read` | `gsoc-2018.json` | 3.33 MB | 2,774 MB/s (1,200 µs, 0.76×) | 2,116 MB/s (1,572 µs, 1.00×) | **3,285 MB/s (1,013 µs, 0.64×)** | 2,089 MB/s (1,593 µs, 1.01×) | 547 MB/s (6,085 µs, 3.87×) | 141 MB/s (23,628 µs, 15.04×) | 112 MB/s (29,641 µs, 18.86×) |
| `read` | `twitter.json` | 632 KB | 1,528 MB/s (413 µs, 1.06×) | 1,617 MB/s (391 µs, 1.00×) | **1,956 MB/s (323 µs, 0.83×)** | 1,629 MB/s (388 µs, 0.99×) | 548 MB/s (1,152 µs, 2.95×) | 102 MB/s (6,221 µs, 15.93×) | 79 MB/s (7,992 µs, 20.45×) |
| `write` | `canada.json` | 2.25 MB | **1,077 MB/s (2,089 µs, 0.78×)** | 838 MB/s (2,686 µs, 1.00×) | 735 MB/s (3,062 µs, 1.14×) | 822 MB/s (2,738 µs, 1.02×) | 273 MB/s (8,250 µs, 3.07×) | 225 MB/s (10,026 µs, 3.73×) | 226 MB/s (9,945 µs, 3.70×) |
| `write` | `citm_catalog.json` | 1.73 MB | **6,553 MB/s (264 µs, 0.93×)** | 6,091 MB/s (284 µs, 1.00×) | 3,452 MB/s (500 µs, 1.76×) | 5,534 MB/s (312 µs, 1.10×) | 2,472 MB/s (699 µs, 2.46×) | 992 MB/s (1,741 µs, 6.14×) | 1,152 MB/s (1,499 µs, 5.29×) |
| `write` | `gsoc-2018.json` | 3.33 MB | **5,416 MB/s (614 µs, 0.49×)** | 2,655 MB/s (1,254 µs, 1.00×) | 2,976 MB/s (1,118 µs, 0.89×) | 2,594 MB/s (1,283 µs, 1.02×) | 2,107 MB/s (1,579 µs, 1.26×) | 223 MB/s (14,914 µs, 11.92×) | 282 MB/s (11,797 µs, 9.41×) |
| `write` | `twitter.json` | 632 KB | **5,404 MB/s (117 µs, 0.72×)** | 3,868 MB/s (163 µs, 1.00×) | 2,351 MB/s (269 µs, 1.64×) | 3,321 MB/s (190 µs, 1.16×) | 1,127 MB/s (560 µs, 3.43×) | 328 MB/s (1,927 µs, 11.80×) | 430 MB/s (1,469 µs, 9.00×) |
| `build` | `canada.json` | 2.25 MB | 1,436 MB/s (1,567 µs, 1.37×) | 1,961 MB/s (1,148 µs, 1.00×) | — | 2,538 MB/s (887 µs, 0.77×) | **3,148 MB/s (715 µs, 0.62×)** | 208 MB/s (10,835 µs, 9.44×) | 208 MB/s (10,822 µs, 9.43×) |
| `build` | `citm_catalog.json` | 1.73 MB | 2,210 MB/s (782 µs, 2.30×) | 5,073 MB/s (340 µs, 1.00×) | — | 4,073 MB/s (424 µs, 1.25×) | **5,553 MB/s (311 µs, 0.91×)** | 398 MB/s (4,338 µs, 12.76×) | 196 MB/s (8,816 µs, 25.91×) |
| `build` | `gsoc-2018.json` | 3.33 MB | 5,896 MB/s (564 µs, 1.57×) | 9,284 MB/s (358 µs, 1.00×) | — | 8,305 MB/s (401 µs, 1.12×) | **9,660 MB/s (344 µs, 0.97×)** | 781 MB/s (4,263 µs, 11.93×) | 327 MB/s (10,171 µs, 28.32×) |
| `build` | `twitter.json` | 632 KB | 1,875 MB/s (337 µs, 2.15×) | 4,035 MB/s (157 µs, 1.00×) | — | 3,373 MB/s (187 µs, 1.20×) | **4,172 MB/s (151 µs, 0.97×)** | 240 MB/s (2,633 µs, 16.92×) | 146 MB/s (4,327 µs, 27.69×) |
| `lookup` | `twitter.json` | 632 KB | **161.0 K/s (6.21 µs, 0.60×)** | 97.3 K/s (10.28 µs, 1.00×) | 76.2 K/s (13.12 µs, 1.28×) | 88.5 K/s (11.30 µs, 1.09×) | 105.4 K/s (9.49 µs, 0.92×) | 37.1 K/s (26.94 µs, 2.61×) | 123.6 K/s (8.09 µs, 0.78×) |

---

## ARM64 (NEON) Results

> **Architecture note:** `neojson` runs its native AArch64 NEON SIMD parser backend (`NeonParserOps` over `SimdOps` in `src/json.cpp`, `#define NEOJSON_ARM_NEON 1`) with 4×16B NEON escape/UTF-8 scanning and SWAR/Eisel-Lemire number parsing. On ARM64, `neojson` leads aggregate Parse at **1,685 MB/s** (**1.47×** faster than C `yyjson`, **1.44×** faster than `simdjson` DOM, **30.9×** faster than `nlohmann::ordered_json`), leads Parse + Full Read (**1,074 MB/s**), leads Write (**1,705 MB/s**, **1.28×** vs `yyjson`), and leads `twitter.json` Field Lookup (**122.2 K/s**, `8.18 µs`).

![JSON Throughput on ARM64 NEON](../assets/bench_arm64_neon.svg)

### Aggregate Throughput & Latency (ARM64 NEON)

| Workload | `neojson` | `yyjson` (C) | `simdjson` (DOM) | `cpp-yyjson` | `RapidJSON` | `nlohmann::json` | `nlohmann::ordered` |
|---|---|---|---|---|---|---|---|
| Parse (Aggregate, 4 files) | **1,685 MB/s (4.71ms, 0.68×)** | 1,145 MB/s (6.93ms, 1.00×) | 1,171 MB/s (6.78ms, 0.98×) | 1,145 MB/s (6.93ms, 1.00×) | 445 MB/s (17.9ms, 2.58×) | 63 MB/s (126ms, 18.22×) | 55 MB/s (145ms, 20.99×) |
| Parse + Full Traversal Read (4 files) | **1,074 MB/s (7.39ms, 0.91×)** | 975 MB/s (8.14ms, 1.00×) | 967 MB/s (8.21ms, 1.01×) | 1,005 MB/s (7.89ms, 0.97×) | 421 MB/s (18.9ms, 2.32×) | 61 MB/s (130ms, 15.93×) | 54 MB/s (148ms, 18.21×) |
| Serialize / Write Minified (4 files) | **1,705 MB/s (4.66ms, 0.78×)** | 1,336 MB/s (5.94ms, 1.00×) | 1,080 MB/s (7.35ms, 1.24×) | 1,341 MB/s (5.92ms, 1.00×) | 561 MB/s (14.1ms, 2.38×) | 257 MB/s (30.9ms, 5.19×) | 264 MB/s (30.1ms, 5.06×) |
| Mutable DOM Build (4 files) | 1,692 MB/s (4.69ms, 2.02×) | 3,422 MB/s (2.32ms, 1.00×) | — *(read-only)* | 3,160 MB/s (2.51ms, 1.08×) | **4,157 MB/s (1.91ms, 0.82×)** | 243 MB/s (32.7ms, 14.08×) | 168 MB/s (47.4ms, 20.41×) |
| Field Lookup (`twitter.json`) | **122.2 K/s (8.18 µs, 0.80×)** | 98.2 K/s (10.19 µs, 1.00×) | 63.7 K/s (15.70 µs, 1.54×) | 80.4 K/s (12.44 µs, 1.22×) | 96.4 K/s (10.38 µs, 1.02×) | 49.0 K/s (20.40 µs, 2.00×) | 106.8 K/s (9.37 µs, 0.92×) |

### Per-File Breakdown (ARM64 NEON)

| Workload | File | Size | `neojson` | `yyjson` (C) | `simdjson` (DOM) | `cpp-yyjson` | `RapidJSON` | `nlohmann::json` | `nlohmann::ordered` |
|---|---|---|---|---|---|---|---|---|---|
| `parse` | `canada.json` | 2.25 MB | **976 MB/s (2,307 µs, 0.66×)** | 646 MB/s (3,486 µs, 1.00×) | 610 MB/s (3,689 µs, 1.06×) | 647 MB/s (3,481 µs, 1.00×) | 417 MB/s (5,400 µs, 1.55×) | 39 MB/s (57,262 µs, 16.46×) | 39 MB/s (58,460 µs, 16.78×) |
| `parse` | `citm_catalog.json` | 1.73 MB | **1,871 MB/s (923 µs, 0.88×)** | 1,653 MB/s (1,045 µs, 1.00×) | 1,671 MB/s (1,034 µs, 0.99×) | 1,650 MB/s (1,047 µs, 1.00×) | 783 MB/s (2,206 µs, 2.11×) | 94 MB/s (18,446 µs, 17.62×) | 67 MB/s (25,848 µs, 24.74×) |
| `parse` | `gsoc-2018.json` | 3.33 MB | **2,896 MB/s (1,149 µs, 0.57×)** | 1,660 MB/s (2,004 µs, 1.00×) | 2,057 MB/s (1,618 µs, 0.81×) | 1,661 MB/s (2,004 µs, 1.00×) | 384 MB/s (8,668 µs, 4.33×) | 81 MB/s (40,981 µs, 20.37×) | 67 MB/s (49,936 µs, 24.96×) |
| `parse` | `twitter.json` | 632 KB | **1,908 MB/s (331 µs, 0.83×)** | 1,588 MB/s (398 µs, 1.00×) | 1,441 MB/s (438 µs, 1.10×) | 1,587 MB/s (398 µs, 1.00×) | 399 MB/s (1,583 µs, 3.98×) | 66 MB/s (9,612 µs, 24.22×) | 56 MB/s (11,234 µs, 28.25×) |
| `read` | `canada.json` | 2.25 MB | **671 MB/s (3,353 µs, 0.77×)** | 514 MB/s (4,380 µs, 1.00×) | 493 MB/s (4,561 µs, 1.04×) | 548 MB/s (4,105 µs, 0.94×) | 376 MB/s (5,988 µs, 1.37×) | 38 MB/s (58,588 µs, 13.39×) | 37 MB/s (60,517 µs, 13.81×) |
| `read` | `citm_catalog.json` | 1.73 MB | 1,247 MB/s (1,386 µs, 1.14×) | **1,423 MB/s (1,214 µs, 1.00×)** | 1,340 MB/s (1,289 µs, 1.06×) | 1,403 MB/s (1,231 µs, 1.01×) | 728 MB/s (2,373 µs, 1.95×) | 89 MB/s (19,516 µs, 16.05×) | 66 MB/s (26,196 µs, 21.60×) |
| `read` | `gsoc-2018.json` | 3.33 MB | 1,635 MB/s (2,036 µs, 0.97×) | 1,592 MB/s (2,090 µs, 1.00×) | **1,822 MB/s (1,826 µs, 0.87×)** | 1,590 MB/s (2,094 µs, 1.00×) | 375 MB/s (8,875 µs, 4.24×) | 81 MB/s (41,170 µs, 19.74×) | 66 MB/s (50,150 µs, 24.00×) |
| `read` | `twitter.json` | 632 KB | 1,020 MB/s (619 µs, 1.35×) | **1,376 MB/s (459 µs, 1.00×)** | 1,182 MB/s (534 µs, 1.16×) | 1,357 MB/s (465 µs, 1.01×) | 385 MB/s (1,638 µs, 3.55×) | 61 MB/s (10,421 µs, 22.61×) | 55 MB/s (11,420 µs, 24.90×) |
| `write` | `canada.json` | 2.25 MB | **756 MB/s (2,978 µs, 0.79×)** | 600 MB/s (3,750 µs, 1.00×) | 458 MB/s (4,915 µs, 1.31×) | 601 MB/s (3,743 µs, 1.00×) | 216 MB/s (10,422 µs, 2.78×) | 198 MB/s (11,365 µs, 3.03×) | 194 MB/s (11,617 µs, 3.10×) |
| `write` | `citm_catalog.json` | 1.73 MB | **5,491 MB/s (315 µs, 0.81×)** | 4,446 MB/s (389 µs, 1.00×) | 2,764 MB/s (625 µs, 1.61×) | 4,447 MB/s (388 µs, 1.00×) | 1,444 MB/s (1,196 µs, 3.06×) | 683 MB/s (2,529 µs, 6.51×) | 693 MB/s (2,494 µs, 6.42×) |
| `write` | `gsoc-2018.json` | 3.33 MB | **2,747 MB/s (1,211 µs, 0.77×)** | 2,116 MB/s (1,573 µs, 1.00×) | 2,286 MB/s (1,455 µs, 0.92×) | 2,134 MB/s (1,560 µs, 0.99×) | 1,755 MB/s (1,896 µs, 1.21×) | 239 MB/s (13,909 µs, 8.84×) | 243 MB/s (13,712 µs, 8.71×) |
| `write` | `twitter.json` | 632 KB | **4,161 MB/s (152 µs, 0.66×)** | 2,742 MB/s (230 µs, 1.00×) | 1,782 MB/s (354 µs, 1.54×) | 2,758 MB/s (229 µs, 0.99×) | 997 MB/s (633 µs, 2.76×) | 207 MB/s (3,058 µs, 13.28×) | 280 MB/s (2,257 µs, 9.80×) |
| `build` | `canada.json` | 2.25 MB | 989 MB/s (2,277 µs, 1.88×) | 1,856 MB/s (1,213 µs, 1.00×) | — | 1,932 MB/s (1,165 µs, 0.96×) | **2,765 MB/s (814 µs, 0.67×)** | 164 MB/s (13,696 µs, 11.30×) | 162 MB/s (13,867 µs, 11.40×) |
| `build` | `citm_catalog.json` | 1.73 MB | 1,568 MB/s (1,102 µs, 2.50×) | 3,920 MB/s (441 µs, 1.00×) | — | 3,147 MB/s (549 µs, 1.25×) | **4,510 MB/s (383 µs, 0.87×)** | 244 MB/s (7,065 µs, 16.07×) | 135 MB/s (12,800 µs, 29.08×) |
| `build` | `gsoc-2018.json` | 3.33 MB | 4,012 MB/s (830 µs, 1.78×) | **7,147 MB/s (466 µs, 1.00×)** | — | 6,056 MB/s (549 µs, 1.18×) | 6,503 MB/s (512 µs, 1.10×) | 501 MB/s (6,638 µs, 14.24×) | 223 MB/s (14,930 µs, 31.99×) |
| `build` | `twitter.json` | 632 KB | 1,304 MB/s (484 µs, 2.42×) | **3,153 MB/s (200 µs, 1.00×)** | — | 2,538 MB/s (249 µs, 1.24×) | 3,146 MB/s (201 µs, 1.01×) | 120 MB/s (5,257 µs, 26.25×) | 110 MB/s (5,753 µs, 28.73×) |
| `lookup` | `twitter.json` | 632 KB | **122.2 K/s (8.18 µs, 0.80×)** | 98.2 K/s (10.19 µs, 1.00×) | 63.7 K/s (15.70 µs, 1.54×) | 80.4 K/s (12.44 µs, 1.22×) | 96.4 K/s (10.38 µs, 1.02×) | 49.0 K/s (20.40 µs, 2.00×) | 106.8 K/s (9.37 µs, 0.92×) |

---

## Why `neojson` Outperforms `yyjson` (C)

A C++ `nlohmann::ordered_json` wrapper beating the C `yyjson` baseline on parse (`1.84×` on x86-64 / `1.47×` on ARM64), full read (`1.26×` x86-64 / `1.10×` ARM64), write (`1.42×` x86-64 / `1.28×` ARM64), and lookup (`1.66×` x86-64 / `1.25×` ARM64) comes from four architectural choices in [`src/json.cpp`](../src/json.cpp) and [`include/neojson/json.hpp`](../include/neojson/json.hpp):

1. **Native AVX2/NEON/SWAR Parsers & Pooled Buffers (Parse)**:
   - For standard JSON inputs (`>= 32 B`, no comments or UTF-8 BOM), `try_parse_native_simd_doc()` dispatches an arch-neutral `parse_native_simd_impl<SimdOps>` core instantiated with `Avx2ParserOps` on x86-64 AVX2 and `NeonParserOps` on ARM64 NEON.
   - Both backends accelerate whitespace skipping, structural scanning, string escape/Keiser-Lemire UTF-8 validation, 8-digit SWAR integer/fraction parsing, Eisel-Lemire float multipliers (`finish_float_noexp`), and tight `,[` coordinate-pair runs directly into a flat `yyjson_doc`-compatible layout.
   - Both native SIMD parses and `yyjson` fallbacks draw large value/string pools from a bounded thread-local buffer cache (`ThreadLargeCache` via `yyjson_parse_alc()`), eliminating repeated OS `malloc`/`free` calls across parses while remaining boundable and reclaimable via `neojson::set_thread_cache_limits()` and `neojson::trim_thread_caches()`.

2. **Lazy Copy-on-Write DOM & Optimized `DocArena` Traversal (Parse & Read)**:
   - `json::parse()` returns a 16-byte lazy handle referencing the flat parsed document rather than eagerly allocating `std::map`/`std::vector`/`std::string` nodes (`nlohmann::ordered_json` allocates per node, hence `83 MB/s` vs `2,557 MB/s` on x86-64).
   - Read-only traversal (`cbegin()`/`cend()`, `const` indexing) materializes a compact bump-allocated `DocArena` view via `lazy_materialize_arena()`, using direct arena traversal, shape reuse (`ArenaShapeCache`), fast string interning (`LongStrInternCache`), and portable object-run builders — beating both C `yyjson` and `simdjson` (DOM) on aggregate `Parse + Full Read` on both x86-64 (`1,498` vs `1,404 MB/s`) and ARM64 (`1,074` vs `975 MB/s`).

3. **Flat Direct Serializer into the Output `std::string` (Write)**:
   - `bench_yyjson` calls `yyjson_write()` (which allocates a C buffer with `malloc`), copies it into `std::string`, and calls `free()`.
   - For unmaterialized lazy objects/arrays, `neojson::json::dump()` runs `try_dump_yyjson_direct()` → `dump_yyjson_direct_flat()`, streaming through an 8 KiB stack staging buffer straight into a pre-reserved `std::string` (sized from `yyjson_doc_get_read_size()`), using `YYJSON_SUBTYPE_NOESC` fast paths, consecutive numeric-pair runs, and AVX2 escape checks on x86-64.
   - On the first dump/arena view of a root parsed document, `neojson` records whether all object keys are duplicate-free (`tree_unique_verified`), so subsequent timed `dump()` calls skip duplicate-key scanning (matching `yyjson_write`, which does not check duplicates).

4. **Contiguous Keys, Open-Addressed Indexing & Compact Mutable Builders (Lookup & Build)**:
   - `yyjson` immutable objects are stored as flat preorder key/value arrays without a hash index, so `yyjson_obj_getn()` performs an `O(N)` linear scan per key lookup.
   - `neojson`'s `ArenaObject` stores keys contiguously (cache-friendly linear scan up to 16 keys, `kOrderedMapHashThreshold`) and lazily builds a compact open-addressed `ArenaHashIndex` above 16 keys — turning `twitter.json` repeated lookups into `O(1)` (`161.0 K/s` / `6.21 µs` on x86-64 and `122.2 K/s` / `8.18 µs` on ARM64).
   - Mutable `json` objects/arrays also use compact `SmallArray2`/`SmallObject4` slabs with NEON 4×16B (`small_obj_verify_neon_mask4`) and SSE2 tag scanning plus single-probe `ordered_map` index insertion (`10.5×` / `10.1×` faster build than `nlohmann::ordered_json` on x86-64 / ARM64).

---

## Methodology & Fairness Details

1. **Process & Symbol Isolation**: `bench/CMakeLists.txt` builds seven independent executables (`bench_neojson`, `bench_yyjson`, `bench_simdjson`, `bench_cppyyjson`, `bench_rapidjson`, `bench_nlohmann_json`, `bench_nlohmann_ordered`) so no binary ever links two copies of `yyjson` or shares allocator/global state across libraries.
2. **Header-Only `RapidJSON` Fetch**: Upstream `RapidJSON`'s `CMakeLists.txt` injects `-march=native` and `-Werror` into global compiler flags. `bench/CMakeLists.txt` downloads and unpacks `RapidJSON`'s headers directly so every benchmark binary compiles under identical `-O3 -DNDEBUG` rules, with `RAPIDJSON_SSE2` defined on x86-64 and `RAPIDJSON_NEON` defined on ARM64.
3. **`simdjson` Padded Input & Read-Only Scope**: `bench/bench.cpp` pre-allocates a reusable `simdjson::padded_string` and `simdjson::dom::parser` per benchmark call (so padding copies do not penalize parse throughput). Because `simdjson::dom` is a read-only tape parser rather than a mutable DOM, `build` is omitted (`N/A`) rather than emulated.
4. **Mutable DOM Build Semantics (`build`)**: Each mutable library constructs a fresh DOM document from a pre-parsed reference document — using builder/insertion APIs (`obj[key] = ...`, `yyjson_mut_obj_add`, `cpp_yyjson` constructors/builders) for `neojson`, `yyjson`, `cpp-yyjson`, and `nlohmann`, and `Document::CopyFrom(doc, alloc, true)` for `RapidJSON`. In `neojson`, copying parsed string leaves increments internal `SharedString` copy-on-write reference counts rather than copying raw string bytes (`RapidJSON`, `yyjson (C)`, and `cpp-yyjson` still lead `build` throughput on both architectures).
5. **Cross-Library Verification Gate (`--verify`)**: `python3 bench/run.py --verify` checks that all 7 executables return identical element counts, scalar checksums, `twitter.json` field lookup matches (`8,284` hits), minified write round-trip verification (`true`), and mutable build round-trip verification before timing runs are collected.

## Reproducing Locally

```bash
cmake -S bench -B build-bench -DCMAKE_BUILD_TYPE=Release
cmake --build build-bench --parallel
python3 bench/run.py --build build-bench --verify
python3 bench/run.py --build build-bench --rounds 7 --json results.json
```
