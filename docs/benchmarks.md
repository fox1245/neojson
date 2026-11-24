# Cross-Architecture JSON Benchmarks

Reproducible single-thread benchmarks comparing `neojson` against six widely used C and C++ JSON libraries on two CPU architectures: **x86-64 (AVX2)** and **ARM64 (NEON)**.

All benchmark binaries live under [`bench/`](../bench/), compile with identical optimization flags (`-O3 -DNDEBUG`, no LTO, no `-march=native`; C++20 benchmark harnesses with `libneojson` built as C++17 per root `CMakeLists.txt`), verify output parity across all 7 libraries via `--verify` before timing, and report the median across **7 alternating paired rounds × 7 measured batches** (>= 25 ms per batch). Throughput uses SI megabytes (`MB/s = 10^6 bytes/s`), and ratios in tables show latency relative to the `yyjson (C)` baseline (aggregate ratios divide summed corpus medians; per-file ratios report the median of paired round ratios; lower ratio = faster; higher `MB/s` or `K/s` = faster).

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
| Parse (Aggregate, 4 files) | **2,575 MB/s (3.08ms, 0.54×)** | 1,392 MB/s (5.70ms, 1.00×) | 1,779 MB/s (4.46ms, 0.78×) | 1,382 MB/s (5.74ms, 1.01×) | 599 MB/s (13.3ms, 2.33×) | 96 MB/s (82.4ms, 14.45×) | 83 MB/s (95.8ms, 16.81×) |
| Parse + Full Traversal Read (4 files) | **1,539 MB/s (5.16ms, 0.77×)** | 1,191 MB/s (6.67ms, 1.00×) | 1,405 MB/s (5.65ms, 0.85×) | 1,196 MB/s (6.64ms, 1.00×) | 564 MB/s (14.1ms, 2.11×) | 94 MB/s (84.4ms, 12.66×) | 81 MB/s (97.6ms, 14.64×) |
| Serialize / Write Minified (4 files) | **2,783 MB/s (2.85ms, 0.65×)** | 1,809 MB/s (4.39ms, 1.00×) | 1,604 MB/s (4.95ms, 1.13×) | 1,755 MB/s (4.52ms, 1.03×) | 716 MB/s (11.1ms, 2.53×) | 277 MB/s (28.6ms, 6.52×) | 321 MB/s (24.7ms, 5.63×) |
| Mutable DOM Build (4 files) | 2,569 MB/s (3.09ms, 1.54×) | 3,962 MB/s (2.00ms, 1.00×) | — *(read-only)* | 4,171 MB/s (1.90ms, 0.95×) | **5,197 MB/s (1.53ms, 0.76×)** | 359 MB/s (22.1ms, 11.05×) | 233 MB/s (34.1ms, 17.02×) |
| Field Lookup (`twitter.json`) | **165.7 K/s (6.03 µs, 0.59×)** | 97.1 K/s (10.30 µs, 1.00×) | 76.5 K/s (13.08 µs, 1.27×) | 88.8 K/s (11.26 µs, 1.09×) | 104.7 K/s (9.55 µs, 0.93×) | 37.0 K/s (27.06 µs, 2.63×) | 124.8 K/s (8.01 µs, 0.78×) |

### Per-File Breakdown (x86-64 AVX2)

![Per-File Breakdown on x86-64 AVX2](../assets/bench_per_file_x86_64.svg)

| Workload | File | Size | `neojson` | `yyjson` (C) | `simdjson` (DOM) | `cpp-yyjson` | `RapidJSON` | `nlohmann::json` | `nlohmann::ordered` |
|---|---|---|---|---|---|---|---|---|---|
| `parse` | `canada.json` | 2.25 MB | **1,216 MB/s (1,852 µs, 0.65×)** | 790 MB/s (2,848 µs, 1.00×) | 827 MB/s (2,721 µs, 0.96×) | 785 MB/s (2,868 µs, 1.01×) | 472 MB/s (4,769 µs, 1.67×) | 53 MB/s (42,172 µs, 14.80×) | 54 MB/s (41,522 µs, 14.58×) |
| `parse` | `citm_catalog.json` | 1.73 MB | **4,116 MB/s (420 µs, 0.41×)** | 1,672 MB/s (1,033 µs, 1.00×) | 2,850 MB/s (606 µs, 0.59×) | 1,678 MB/s (1,029 µs, 1.00×) | 1,227 MB/s (1,408 µs, 1.36×) | 159 MB/s (10,852 µs, 10.50×) | 102 MB/s (16,896 µs, 16.37×) |
| `parse` | `gsoc-2018.json` | 3.33 MB | **5,420 MB/s (614 µs, 0.41×)** | 2,232 MB/s (1,491 µs, 1.00×) | 3,715 MB/s (896 µs, 0.60×) | 2,186 MB/s (1,522 µs, 1.02×) | 556 MB/s (5,990 µs, 4.02×) | 142 MB/s (23,389 µs, 15.70×) | 113 MB/s (29,491 µs, 19.77×) |
| `parse` | `twitter.json` | 632 KB | **3,201 MB/s (197 µs, 0.60×)** | 1,918 MB/s (329 µs, 1.00×) | 2,639 MB/s (239 µs, 0.73×) | 1,948 MB/s (324 µs, 0.98×) | 579 MB/s (1,090 µs, 3.31×) | 106 MB/s (5,944 µs, 18.09×) | 80 MB/s (7,909 µs, 24.00×) |
| `read` | `canada.json` | 2.25 MB | **791 MB/s (2,846 µs, 0.81×)** | 641 MB/s (3,514 µs, 1.00×) | 644 MB/s (3,494 µs, 0.99×) | 646 MB/s (3,483 µs, 0.99×) | 425 MB/s (5,295 µs, 1.51×) | 52 MB/s (43,353 µs, 12.34×) | 53 MB/s (42,728 µs, 12.17×) |
| `read` | `citm_catalog.json` | 1.73 MB | **2,283 MB/s (757 µs, 0.63×)** | 1,450 MB/s (1,191 µs, 1.00×) | 2,102 MB/s (822 µs, 0.69×) | 1,472 MB/s (1,173 µs, 0.98×) | 1,115 MB/s (1,550 µs, 1.30×) | 155 MB/s (11,173 µs, 9.38×) | 101 MB/s (17,159 µs, 14.41×) |
| `read` | `gsoc-2018.json` | 3.33 MB | 2,890 MB/s (1,151 µs, 0.73×) | 2,119 MB/s (1,570 µs, 1.00×) | **3,289 MB/s (1,012 µs, 0.64×)** | 2,091 MB/s (1,592 µs, 1.01×) | 547 MB/s (6,087 µs, 3.88×) | 141 MB/s (23,616 µs, 15.04×) | 112 MB/s (29,717 µs, 18.94×) |
| `read` | `twitter.json` | 632 KB | 1,572 MB/s (402 µs, 1.03×) | 1,617 MB/s (391 µs, 1.00×) | **1,956 MB/s (323 µs, 0.83×)** | 1,629 MB/s (388 µs, 0.99×) | 549 MB/s (1,151 µs, 2.95×) | 101 MB/s (6,233 µs, 15.99×) | 79 MB/s (8,001 µs, 20.49×) |
| `write` | `canada.json` | 2.25 MB | **1,120 MB/s (2,011 µs, 0.75×)** | 838 MB/s (2,687 µs, 1.00×) | 736 MB/s (3,060 µs, 1.14×) | 822 MB/s (2,738 µs, 1.02×) | 273 MB/s (8,246 µs, 3.07×) | 225 MB/s (10,019 µs, 3.73×) | 226 MB/s (9,951 µs, 3.70×) |
| `write` | `citm_catalog.json` | 1.73 MB | **6,541 MB/s (264 µs, 0.93×)** | 6,088 MB/s (284 µs, 1.00×) | 3,451 MB/s (501 µs, 1.76×) | 5,537 MB/s (312 µs, 1.10×) | 2,469 MB/s (700 µs, 2.47×) | 990 MB/s (1,744 µs, 6.15×) | 1,151 MB/s (1,501 µs, 5.28×) |
| `write` | `gsoc-2018.json` | 3.33 MB | **7,090 MB/s (469 µs, 0.37×)** | 2,653 MB/s (1,254 µs, 1.00×) | 2,973 MB/s (1,119 µs, 0.89×) | 2,594 MB/s (1,283 µs, 1.02×) | 2,109 MB/s (1,578 µs, 1.26×) | 223 MB/s (14,928 µs, 11.90×) | 282 MB/s (11,800 µs, 9.41×) |
| `write` | `twitter.json` | 632 KB | **5,839 MB/s (108 µs, 0.66×)** | 3,871 MB/s (163 µs, 1.00×) | 2,348 MB/s (269 µs, 1.65×) | 3,319 MB/s (190 µs, 1.17×) | 1,125 MB/s (561 µs, 3.44×) | 327 MB/s (1,929 µs, 11.83×) | 430 MB/s (1,470 µs, 9.00×) |
| `build` | `canada.json` | 2.25 MB | 1,515 MB/s (1,486 µs, 1.29×) | 1,961 MB/s (1,148 µs, 1.00×) | — | 2,537 MB/s (887 µs, 0.77×) | **3,136 MB/s (718 µs, 0.63×)** | 208 MB/s (10,831 µs, 9.43×) | 208 MB/s (10,837 µs, 9.44×) |
| `build` | `citm_catalog.json` | 1.73 MB | 2,312 MB/s (747 µs, 2.19×) | 5,070 MB/s (341 µs, 1.00×) | — | 4,072 MB/s (424 µs, 1.25×) | **5,551 MB/s (311 µs, 0.91×)** | 397 MB/s (4,354 µs, 12.81×) | 196 MB/s (8,817 µs, 25.94×) |
| `build` | `gsoc-2018.json` | 3.33 MB | 6,202 MB/s (537 µs, 1.50×) | 9,301 MB/s (358 µs, 1.00×) | — | 8,241 MB/s (404 µs, 1.12×) | **9,597 MB/s (347 µs, 0.97×)** | 775 MB/s (4,294 µs, 11.97×) | 329 MB/s (10,125 µs, 28.30×) |
| `build` | `twitter.json` | 632 KB | 1,973 MB/s (320 µs, 2.04×) | 4,022 MB/s (157 µs, 1.00×) | — | 3,369 MB/s (187 µs, 1.19×) | **4,170 MB/s (151 µs, 0.97×)** | 238 MB/s (2,655 µs, 16.82×) | 146 MB/s (4,328 µs, 27.57×) |
| `lookup` | `twitter.json` | 632 KB | **165.7 K/s (6.03 µs, 0.58×)** | 97.1 K/s (10.30 µs, 1.00×) | 76.5 K/s (13.08 µs, 1.27×) | 88.8 K/s (11.26 µs, 1.10×) | 104.7 K/s (9.55 µs, 0.92×) | 37.0 K/s (27.06 µs, 2.62×) | 124.8 K/s (8.01 µs, 0.78×) |

---

## ARM64 (NEON) Results

> **Architecture note:** `neojson` runs its native AArch64 NEON SIMD parser backend (`NeonParserOps` over `SimdOps` in `src/json.cpp`, `#define NEOJSON_ARM_NEON 1`) with 4×16B NEON escape/UTF-8 scanning, outlined 64B zero-waste long-string unrolling (`parse_string_val_long`), value-returned whitespace skipping (`skip_spaces_for_key`), SWAR/Eisel-Lemire number parsing, and NEON movemask/verbatim escape scanning on write. On ARM64, `neojson` leads aggregate Parse at **1,885 MB/s** (**1.65×** faster than C `yyjson`, **1.61×** faster than `simdjson` DOM, **34.5×** faster than `nlohmann::ordered_json`), leads Parse + Full Read (**1,155 MB/s**, **1.19×** vs both `yyjson` C and `simdjson` DOM, #1 on 3/4 files), leads Write (**1,918 MB/s**, **1.44×** vs `yyjson`), and leads `twitter.json` Field Lookup (**123.9 K/s**, `8.07 µs`).

![JSON Throughput on ARM64 NEON](../assets/bench_arm64_neon.svg)

### Aggregate Throughput & Latency (ARM64 NEON)

| Workload | `neojson` | `yyjson` (C) | `simdjson` (DOM) | `cpp-yyjson` | `RapidJSON` | `nlohmann::json` | `nlohmann::ordered` |
|---|---|---|---|---|---|---|---|
| Parse (Aggregate, 4 files) | **1,885 MB/s (4.21ms, 0.61×)** | 1,144 MB/s (6.94ms, 1.00×) | 1,170 MB/s (6.78ms, 0.98×) | 1,143 MB/s (6.95ms, 1.00×) | 444 MB/s (17.9ms, 2.58×) | 62 MB/s (128ms, 18.51×) | 55 MB/s (145ms, 20.95×) |
| Parse + Full Traversal Read (4 files) | **1,155 MB/s (6.87ms, 0.84×)** | 974 MB/s (8.15ms, 1.00×) | 967 MB/s (8.21ms, 1.01×) | 1,004 MB/s (7.91ms, 0.97×) | 420 MB/s (18.9ms, 2.32×) | 60 MB/s (132ms, 16.24×) | 53 MB/s (149ms, 18.22×) |
| Serialize / Write Minified (4 files) | **1,918 MB/s (4.14ms, 0.69×)** | 1,330 MB/s (5.97ms, 1.00×) | 1,075 MB/s (7.38ms, 1.24×) | 1,332 MB/s (5.96ms, 1.00×) | 550 MB/s (14.4ms, 2.42×) | 257 MB/s (30.9ms, 5.18×) | 263 MB/s (30.1ms, 5.05×) |
| Mutable DOM Build (4 files) | 1,691 MB/s (4.70ms, 2.02×) | 3,422 MB/s (2.32ms, 1.00×) | — *(read-only)* | 3,158 MB/s (2.51ms, 1.08×) | **4,151 MB/s (1.91ms, 0.82×)** | 235 MB/s (33.7ms, 14.54×) | 167 MB/s (47.6ms, 20.54×) |
| Field Lookup (`twitter.json`) | **123.9 K/s (8.07 µs, 0.79×)** | 98.2 K/s (10.18 µs, 1.00×) | 63.5 K/s (15.75 µs, 1.55×) | 80.5 K/s (12.43 µs, 1.22×) | 96.4 K/s (10.38 µs, 1.02×) | 49.0 K/s (20.39 µs, 2.00×) | 106.7 K/s (9.38 µs, 0.92×) |

### Per-File Breakdown (ARM64 NEON)

| Workload | File | Size | `neojson` | `yyjson` (C) | `simdjson` (DOM) | `cpp-yyjson` | `RapidJSON` | `nlohmann::json` | `nlohmann::ordered` |
|---|---|---|---|---|---|---|---|---|---|
| `parse` | `canada.json` | 2.25 MB | **979 MB/s (2,298 µs, 0.66×)** | 645 MB/s (3,488 µs, 1.00×) | 610 MB/s (3,689 µs, 1.06×) | 645 MB/s (3,490 µs, 1.00×) | 415 MB/s (5,430 µs, 1.56×) | 39 MB/s (57,285 µs, 16.47×) | 39 MB/s (58,452 µs, 16.76×) |
| `parse` | `citm_catalog.json` | 1.73 MB | **2,728 MB/s (633 µs, 0.61×)** | 1,650 MB/s (1,047 µs, 1.00×) | 1,670 MB/s (1,034 µs, 0.99×) | 1,645 MB/s (1,050 µs, 1.00×) | 780 MB/s (2,215 µs, 2.11×) | 90 MB/s (19,253 µs, 18.41×) | 67 MB/s (25,853 µs, 24.76×) |
| `parse` | `gsoc-2018.json` | 3.33 MB | **3,506 MB/s (949 µs, 0.47×)** | 1,659 MB/s (2,006 µs, 1.00×) | 2,054 MB/s (1,620 µs, 0.81×) | 1,656 MB/s (2,009 µs, 1.00×) | 383 MB/s (8,695 µs, 4.33×) | 80 MB/s (41,615 µs, 20.77×) | 67 MB/s (49,659 µs, 24.70×) |
| `parse` | `twitter.json` | 632 KB | **1,917 MB/s (329 µs, 0.83×)** | 1,583 MB/s (399 µs, 1.00×) | 1,441 MB/s (438 µs, 1.10×) | 1,591 MB/s (397 µs, 1.00×) | 406 MB/s (1,557 µs, 3.91×) | 61 MB/s (10,274 µs, 25.83×) | 55 MB/s (11,393 µs, 28.53×) |
| `read` | `canada.json` | 2.25 MB | **671 MB/s (3,353 µs, 0.77×)** | 514 MB/s (4,378 µs, 1.00×) | 493 MB/s (4,562 µs, 1.04×) | 547 MB/s (4,117 µs, 0.94×) | 375 MB/s (6,008 µs, 1.38×) | 38 MB/s (58,716 µs, 13.41×) | 37 MB/s (60,488 µs, 13.83×) |
| `read` | `citm_catalog.json` | 1.73 MB | **1,587 MB/s (1,089 µs, 0.89×)** | 1,421 MB/s (1,216 µs, 1.00×) | 1,342 MB/s (1,287 µs, 1.06×) | 1,402 MB/s (1,232 µs, 1.01×) | 727 MB/s (2,375 µs, 1.96×) | 83 MB/s (20,692 µs, 17.08×) | 66 MB/s (26,347 µs, 21.66×) |
| `read` | `gsoc-2018.json` | 3.33 MB | **1,835 MB/s (1,814 µs, 0.87×)** | 1,587 MB/s (2,096 µs, 1.00×) | 1,823 MB/s (1,826 µs, 0.87×) | 1,589 MB/s (2,094 µs, 1.00×) | 374 MB/s (8,905 µs, 4.24×) | 79 MB/s (41,953 µs, 20.01×) | 66 MB/s (50,135 µs, 23.88×) |
| `read` | `twitter.json` | 632 KB | 1,021 MB/s (618 µs, 1.34×) | **1,368 MB/s (462 µs, 1.00×)** | 1,181 MB/s (535 µs, 1.16×) | 1,359 MB/s (465 µs, 1.01×) | 387 MB/s (1,631 µs, 3.54×) | 57 MB/s (11,018 µs, 23.87×) | 55 MB/s (11,568 µs, 25.20×) |
| `write` | `canada.json` | 2.25 MB | **748 MB/s (3,008 µs, 0.80×)** | 598 MB/s (3,761 µs, 1.00×) | 456 MB/s (4,935 µs, 1.31×) | 598 MB/s (3,763 µs, 1.00×) | 214 MB/s (10,502 µs, 2.79×) | 198 MB/s (11,347 µs, 3.01×) | 194 MB/s (11,613 µs, 3.09×) |
| `write` | `citm_catalog.json` | 1.73 MB | **5,498 MB/s (314 µs, 0.81×)** | 4,432 MB/s (390 µs, 1.00×) | 2,770 MB/s (623 µs, 1.61×) | 4,434 MB/s (390 µs, 1.00×) | 1,413 MB/s (1,222 µs, 3.15×) | 683 MB/s (2,530 µs, 6.51×) | 691 MB/s (2,498 µs, 6.42×) |
| `write` | `gsoc-2018.json` | 3.33 MB | **4,869 MB/s (683 µs, 0.43×)** | 2,098 MB/s (1,587 µs, 1.00×) | 2,261 MB/s (1,472 µs, 0.92×) | 2,109 MB/s (1,578 µs, 0.99×) | 1,687 MB/s (1,972 µs, 1.22×) | 239 MB/s (13,923 µs, 8.77×) | 242 MB/s (13,770 µs, 8.68×) |
| `write` | `twitter.json` | 632 KB | **4,732 MB/s (133 µs, 0.58×)** | 2,754 MB/s (229 µs, 1.00×) | 1,783 MB/s (354 µs, 1.54×) | 2,747 MB/s (230 µs, 1.00×) | 851 MB/s (742 µs, 3.24×) | 201 MB/s (3,137 µs, 13.66×) | 279 MB/s (2,261 µs, 9.85×) |
| `build` | `canada.json` | 2.25 MB | 986 MB/s (2,284 µs, 1.88×) | 1,854 MB/s (1,214 µs, 1.00×) | — | 1,932 MB/s (1,165 µs, 0.96×) | **2,754 MB/s (817 µs, 0.67×)** | 163 MB/s (13,796 µs, 11.38×) | 162 MB/s (13,878 µs, 11.43×) |
| `build` | `citm_catalog.json` | 1.73 MB | 1,570 MB/s (1,100 µs, 2.50×) | 3,932 MB/s (439 µs, 1.00×) | — | 3,144 MB/s (549 µs, 1.25×) | **4,508 MB/s (383 µs, 0.87×)** | 240 MB/s (7,208 µs, 16.41×) | 134 MB/s (12,851 µs, 29.17×) |
| `build` | `gsoc-2018.json` | 3.33 MB | 4,010 MB/s (830 µs, 1.79×) | **7,148 MB/s (466 µs, 1.00×)** | — | 6,049 MB/s (550 µs, 1.19×) | 6,503 MB/s (512 µs, 1.09×) | 483 MB/s (6,892 µs, 14.80×) | 220 MB/s (15,141 µs, 32.42×) |
| `build` | `twitter.json` | 632 KB | 1,310 MB/s (482 µs, 2.40×) | 3,147 MB/s (201 µs, 1.00×) | — | 2,537 MB/s (249 µs, 1.24×) | **3,159 MB/s (200 µs, 1.00×)** | 108 MB/s (5,834 µs, 29.10×) | 109 MB/s (5,774 µs, 28.77×) |
| `lookup` | `twitter.json` | 632 KB | **123.9 K/s (8.07 µs, 0.79×)** | 98.2 K/s (10.18 µs, 1.00×) | 63.5 K/s (15.75 µs, 1.55×) | 80.5 K/s (12.43 µs, 1.22×) | 96.4 K/s (10.38 µs, 1.02×) | 49.0 K/s (20.39 µs, 2.00×) | 106.7 K/s (9.38 µs, 0.92×) |

---

## Why `neojson` Outperforms `yyjson` (C)

A C++ `nlohmann::ordered_json` wrapper beating the C `yyjson` baseline on parse (`1.85×` on x86-64 / `1.65×` on ARM64), full read (`1.29×` x86-64 / `1.19×` ARM64), write (`1.54×` x86-64 / `1.44×` ARM64), and lookup (`1.71×` x86-64 / `1.26×` ARM64) comes from four architectural choices in [`src/json.cpp`](../src/json.cpp) and [`include/neojson/json.hpp`](../include/neojson/json.hpp):

1. **Native AVX2/NEON/SWAR Parsers & Pooled Buffers (Parse)**:
   - For standard JSON inputs (`>= 32 B`, no comments or UTF-8 BOM), `try_parse_native_simd_doc()` dispatches an arch-neutral `parse_native_simd_impl<SimdOps>` core instantiated with `Avx2ParserOps` on x86-64 AVX2 and `NeonParserOps` on ARM64 NEON.
   - Both backends accelerate whitespace skipping (including `skip_spaces_for_key` returning `KeySpaceRes` by value on wide-indented lines to eliminate stack-canary spills), structural scanning, string escape/Keiser-Lemire UTF-8 validation, outlined zero-waste 128 B AVX2 / 64 B NEON long-string unrolling (`parse_string_val_long`), 8-digit SWAR integer/fraction parsing, Eisel-Lemire float multipliers (`finish_float_noexp`), and tight `,[` coordinate-pair runs directly into a flat `yyjson_doc`-compatible layout.
   - Both native SIMD parses and `yyjson` fallbacks draw large value/string pools from a bounded thread-local buffer cache (`ThreadLargeCache` via `yyjson_parse_alc()`), eliminating repeated OS `malloc`/`free` calls across parses while remaining boundable and reclaimable via `neojson::set_thread_cache_limits()` and `neojson::trim_thread_caches()`.

2. **Lazy Copy-on-Write DOM & Optimized `DocArena` Traversal (Parse & Read)**:
   - `json::parse()` returns a 16-byte lazy handle referencing the flat parsed document rather than eagerly allocating `std::map`/`std::vector`/`std::string` nodes (`nlohmann::ordered_json` allocates per node, hence `83 MB/s` vs `2,575 MB/s` on x86-64).
   - Read-only traversal (`cbegin()`/`cend()`, `const` indexing) materializes a compact bump-allocated `DocArena` view via `lazy_materialize_arena()`, using direct arena traversal, shape caching (`ArenaShapeCache` with `init_arena_slot_with_sibling_hint`), batched ARM64 NEON shape verification (`try_build_arena_object_with_entry_neon` + `shape_key_eq_padded32_neon`), doc-stamped `ShortStr7Cache` + way-1 MRU `LongStrInternCache` string interning, overlapping/AVX2 block string copies (`copy_str_ge16`, `copy_str_ge16_avx2`), bounded scratch reuse (`MedBufGuard` / `ScratchGuard`), and portable object-run builders — beating both C `yyjson` and `simdjson` (DOM) on aggregate `Parse + Full Read` on both x86-64 (`1,539` vs `1,405 MB/s`) and ARM64 (`1,155` vs `974 MB/s`).

3. **Fused Single-Pass Serializer into the Output `std::string` (Write)**:
   - `bench_yyjson` calls `yyjson_write()` (which allocates a C buffer with `malloc`), copies it into `std::string`, and calls `free()`.
   - For unmaterialized lazy objects/arrays, `neojson::json::dump()` runs `try_dump_yyjson_direct()` → `dump_yyjson_direct_flat()`, streaming through an 8 KiB stack staging buffer straight into a pre-reserved `std::string` (sized from `yyjson_doc_get_read_size()`), using `YYJSON_SUBTYPE_NOESC` fast paths (`write_long_noesc_flat`), consecutive numeric-pair runs, and fused single-pass UTF-8 validation/escape batching (`write_valid_utf8_escaped_flat_avx2` / `_sse2` / `_neon` / `_scalar` with `neon_movemask_u8` / `find_non_verbatim_*_neon` on ARM64).
   - On the first dump/arena view of a root parsed document, `neojson` records whether all object keys are duplicate-free (`tree_unique_verified`), so subsequent timed `dump()` calls skip duplicate-key scanning (matching `yyjson_write`, which does not check duplicates).

4. **Contiguous Keys, Open-Addressed Indexing & Compact Mutable Builders (Lookup & Build)**:
   - `yyjson` immutable objects are stored as flat preorder key/value arrays without a hash index, so `yyjson_obj_getn()` performs an `O(N)` linear scan per key lookup.
   - `neojson`'s `ArenaObject` stores keys contiguously (cache-friendly linear scan up to 16 keys, `kOrderedMapHashThreshold`) and lazily builds a compact open-addressed `ArenaHashIndex` above 16 keys — turning `twitter.json` repeated lookups into `O(1)` (`165.7 K/s` / `6.03 µs` on x86-64 and `123.9 K/s` / `8.07 µs` on ARM64).
   - Mutable `json` objects/arrays also use compact `SmallArray2`/`SmallObject4` slabs with NEON 4×16B (`small_obj_verify_neon_mask4`) and SSE2 tag scanning plus single-probe `ordered_map` index insertion (`11.0×` / `10.1×` faster build than `nlohmann::ordered_json` on x86-64 / ARM64).

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
