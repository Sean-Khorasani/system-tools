# AGENTS.md

This directory (`wintcp/tests/asm`) is a standalone test project that validates
assembly-optimized drop-in replacements for hot-path functions in the main wintcp.exe.
The optimized implementations use MSVC compiler intrinsics (`<intrin.h>`) that emit
SSE2 and BSWAP instructions, rather than hand-written MASM.

## Project at a Glance

| Aspect | Detail |
|---|---|
| Language | C++17 (MSVC); reference MASM in `.asm` |
| Architecture target | x64 only |
| Build system | CMake (primary) or `build.bat` |
| Test runner | `testAssemblies.exe` — runs all tests then benchmarks |
| License header | `SPDX-License-Identifier: Apache-2.0` |

## File Inventory

| File | Purpose |
|---|---|
| `CMakeLists.txt` | CMake build configuration (C++17, MSVC flags, links) |
| `build.bat` | Quick-build script with cmake-first strategy + `cl.exe` fallback |
| `opt_functions.h` | Declarations of optimized functions (namespace `wintcp`) |
| `opt_functions.cpp` | Intrinsic-based implementations of the optimized functions |
| `testAssemblies.cpp` | Test harness (unit tests + benchmarks) with `main()` |
| `geoip_lookup.asm` | Reference MASM documentation for the GeoIP tree walk (conceptual; not built) |
| `HOWTO-INTEGRATION.md` | Guide for integrating these functions into the main wintcp.exe build |

## Build & Run

### Prerequisites
- Visual Studio 2022 with C++ build tools (v143), x64
- Run from **Developer Command Prompt for VS 2022** (or call `vcvarsall.bat`)

### Primary build (CMake + VS 2022 generator)
```bat
cd wintcp\tests\asm
cmake -S . -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release
.\build\bin\Release\testAssemblies.exe
```

### Ninja build (faster incremental)
```bat
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
.\build\bin\testAssemblies.exe
```

### One-shot build + test (Windows)
```bat
build.bat Release
```
`build.bat` tries CMake first; if `cmake` is not on PATH it falls back to a direct
`cl.exe` invocation with the same flags. It builds and immediately runs the test
executable. Pass `Debug` for a debug build (`build.bat Debug`).

### Direct `cl.exe` invocation
```bat
cl /std:c++17 /EHsc /utf-8 /permissive- /Zc:__cplusplus /W4 /MT /O2 ^
   /I "..\..\src" ^
   /DWIN32_LEAN_AND_MEAN /D_UNICODE /DUNICODE /D_WIN32_WINNT=0x0601 ^
   /Fe:"build\testAssemblies.exe" ^
   /Fo:"build\obj\\" ^
   testAssemblies.cpp opt_functions.cpp ^
   /link /MANIFEST:NO iphlpapi.lib ws2_32.lib comctl32.lib user32.lib
```

## Compiler Flags & Notes
- `/permissive-` and `/Zc:__cplusplus` are required for correct C++ standard conformance.
- `/utf-8` ensures source files are interpreted as UTF-8.
- `/MANIFEST:NO` produces a self-contained binary (static CRT).
- Static CRT (`/MT` or `/MTd`) is set in CMake via `MSVC_RUNTIME_LIBRARY`.
- The project links `iphlpapi`, `ws2_32`, `comctl32`, `user32` (mirroring the main wintcp build).
- `_WIN32_WINNT=0x0601` targets Windows 7+; `WIN32_LEAN_AND_MEAN` excludes rarely-used headers.

## Architecture & Code Flow

### Module structure
- **`opt_functions.h` / `opt_functions.cpp`**: declare and implement seven optimized
  functions inside `namespace wintcp`. Each header comment documents the original
  source location it replaces and the optimization technique.
- **`testAssemblies.cpp`**: a single-file test harness using only `<cstdio>`/`printf`
  (no external test framework). `main()` calls each `Test*` function sequentially.

### Test harness pattern
Each test function follows this structure:
1. Build synthetic test data (GeoIP trees, TCP segments, packets, etc.).
2. Call the optimized function with hand-crafted inputs.
3. `ReportTest(name, condition, detail)` — prints `PASS`/`FAIL` and updates
   `testsPassed`/`testsFailed` globals.
4. A benchmark loop using `std::chrono::high_resolution_clock` reports `ns/op`.

`ReportTest` uses `printf` (not a framework). The process exits with code `1` if any
test failed, `0` otherwise — suitable for CI exit-code checks.

## Optimized Functions (and their origins)

The header comment for each function names its **original** location in the wintcp
codebase, and `HOWTO-INTEGRATION.md` carries the per-candidate procedure for
wiring one in.

Two cautions about the table below, both learned the hard way:

1. **"Replaces (original)" is the ORIGIN, not the status.** A row names where the
   function would go, not where it has gone. This table was read as a done list
   for months, which is why the Status column now exists and must be filled in
   for every row. Trust the wire count, not the row number.
2. **The Status column is maintained by hand and it decays.** Re-derive it from
   the product sources before relying on it (see the audit command in
   HOWTO-INTEGRATION.md), exactly as the gate counts are re-measured rather than
   quoted.

| # | Optimized function | Replaces (original) | Status | Technique |
|---|---|---|---|---|
| 1 | `wintcp::ResolveOffsetOpt` | `GeoIp.cpp` `ResolveOffset` | measured, not wired | Bit-test loop, inlined 24/28/32-bit node records |
| 2 | `wintcp::AddSegmentOverlapOpt` | `TcpReasm.cpp` `Direction::Add` | measured 0.74x-1.01x, do not wire | Scalar two-pass scan, overflow-safe |
| 3 | `wintcp::HasLowerSubstringOpt` | `Utils.h` `HasLowerSubstring` | measured, not wired - needs a needle-length threshold (2.56x short, 0.71x at 64k) | SSE2 first-char scan, memcmp verify |
| 4 | `wintcp::ParseTcpHeaderOpt` | `Pcapng.cpp` `ParseIpTcp` | measured 0.52x/0.30x, do not wire | SIMD header loads, BSWAP |
| 5 | `wintcp::FormatBytesOpt` | `Utils.cpp` `FormatBytes` | measured winner, READY to wire | Integer arithmetic for bytes, `swprintf_s` fallback |
| 6 | `wintcp::KeyOfOpt` | `ConnectionStore.cpp` `KeyOf` | measured 0.77x-0.81x, do not wire | Direct byte writes, single store |
| 7 | `wintcp::WideToUtf8Opt` | `Utils.cpp` `WideToUtf8` | measured, not wired | ASCII SIMD fast path, single-pass UTF-8 |
| 8 | `wintcp::FormatStreamHexOpt` | `StreamCapture.cpp` `FormatStreamHex` | measured 11x-30x, READY to wire | Hex LUT + direct stores |
| 9 | `wintcp::ToLowerWOpt` | `Utils.cpp:105` `ToLowerW` | **WIRED** | SSE2 ASCII fold, towlower fallback (1.7-19x) |
| 10 | `wintcp::ReadU32BEOpt`/`ReadU64BEOpt`/`ReadBytesOpt` | `GeoIp.cpp` read helpers | tie-1.6x, not wired | memcpy + BSWAP, size switch |
| 11 | `wintcp::TlsBe16Opt`/`TlsBe24Opt`/`TlsBe32Opt` | `TlsDecode.cpp` BE readers | tie, not wired | memcpy + BSWAP; Be24 stays shift/OR |
| 12 | `wintcp::ClassifyTlsExtensionOpt`/`StripSniTailOpt` | `TlsDecode.cpp` | **WIRED** | 17-entry LUT; SSE2 tail scan (1.3x/17x) |
| 13 | `wintcp::Rd16Opt`/`Rd32Opt` | `Pcapng.cpp` readers | tie, not wired | memcpy + conditional BSWAP |
| 14 | `wintcp::JoinSamplesOpt` | `ConnectionStore.cpp` `ApplySocketBytes` | measured winner, READY but needs a size threshold (8.23x at 2k/2k, 0.07x on `join_small` = 14x SLOWER below ~64 rows) | Hash index + FIFO queues |
| 15 | `wintcp::FindSubstringLongOpt` | `ConnectionStore.cpp` `Has` | measured, not wired - needs a needle-length threshold (1.7x at 32 chars, 0.43x at 8) | BMH, route needles >= 24 chars here |
| 16 | `wintcp::BuildLowerAllOpt` | `ConnectionStore.cpp:93` `RebuildLowerAll` | **WIRED** | reserve + append (4.7x) |
| 17 | `wintcp::CompareWideOpt` | `CompareRows` `wstring::compare` | measured 0.94x-0.99x, do not wire | SSE2 lane diff |
| 18 | `wintcp::HashConnKeyOpt` / `EqualConnKeyOpt` | `ConnectionStore.h` key hash/eq | **BLOCKED - DO NOT WIRE.** Changing the hash changes `unordered_map` bucket order and therefore row order. Saved by policy, not by measurement (2.9x-6.2x is real and irrelevant) | FNV-1a lanes |
| 19 | `wintcp::ComputeBpsBatchOpt` | `ConnectionStore.cpp` `ComputeBps` | measured 1.7x, not wired - no call site shares one elapsed across a batch | one reciprocal per batch |
| 20 | `wintcp::SumPidTrafficOpt` | `ConnectionStore.cpp` group sums | **WIRED** (2026-10-10) | sort + linear runs (3.8-4.3x) |
| 21 | `wintcp::FormatBpsCellOpt` | `ConnectionStore.cpp` `FormatBpsCell` | measured 1.2x, READY to wire via `FormatBytesOpt` | via FormatBytesOpt + splice |
| 22 | `wintcp::WidenUtf8Opt` | `GeoIp.cpp` `AppendUtf8` | measured tie-1.2x, READY to wire | SSE2 ASCII bulk |
| 23 | `wintcp::IsGlobalUnicastV4Opt` / `V6Opt` | `GeoIp.cpp:1243` | **WIRED** (2026-10-10) | octet table + u64 tails (1.2x) |
| 24 | `wintcp::PairSnapshotOpt` | `ConnectionStore.cpp` `PrevKeyIndex` | measured 0.88x-1.03x, do not wire | same algo, FNV hash; sort lost 2.4x |
| 25 | `wintcp::FindCountedKeyOpt` | `GeoIp.cpp` `MapFind` step | measured 0.93x-0.96x, do not wire | u64 prefilter |
| 26 | `wintcp::FlowKeyEqualOpt` / `CmpFlowAddrOpt` | `TcpReasm.cpp` | measured 1.1x-1.3x, not wired | SSE2 key compare |
| 27 | `wintcp::FilterHandlesOpt` | `SocketTraffic.cpp` phase 1 | **REJECTED 0.53x - DO NOT WIRE.** The hash wins; this phase is 0.4 ms | sorted+binsearch |
| 28 | `wintcp::ClassifyEventOpt` / `ParseEventPayloadOpt` | `EtwTraffic.cpp` | measured 0.90x, do not wire | u64 GUID discrim. + opcode table |
| 29 | `wintcp::JsonEscapeOpt` | `Commands.cpp` `JsonEscapeA` | **WIRED** | SSE2 special scan + span memcpy |
| 30 | `wintcp::CsvEscapeOpt` | `Utils.cpp` `CsvEscapeUtf8` | **WIRED** (2026-10-10) | SSE2 quote scan. Held the CSV span-copy defect once - the 16-byte input ceiling in the tests is why it hid |
| 31 | `wintcp::FormatPortOpt` / `FormatU64DecOpt` / `FormatDurationOpt` | `Utils.cpp` `FormatPort`, `ConnectionStore.cpp` `FormatDuration` | `FormatPortOpt` and `FormatDurationOpt` **WIRED** (2026-10-10); `FormatU64DecOpt` measured 5.35x, READY to wire | digit-table emitters |
| 32 | `wintcp::FormatIpv4Opt` / `FormatIpv6Opt` | `TcpTable.cpp` `PrintIpv4`/`PrintIpv6` | **WIRED** | digit tables, nibble shuffle + RFC 5952 `::` |
| 33 | `wintcp::DisplayWidthOpt` / `TruncateToWidthOpt` | `Commands.cpp` | **WIRED** | SSE2 ASCII bulk + fused measure/cut (2.2-27x) |
| 33b | `wintcp::CpWidthOpt` | original `NextCp` chain | **REJECTED 0.62x-0.78x** - the 12 KB LUT lost | 12 KB table |
| 34 | `wintcp::PayloadSizeOpt` / `ReadPointerOpt` | `GeoIp.cpp` | measured 0.96x / 0.99x, do not wire | hot path stays branch-cheap |
| 35 | `wintcp::SkipVlanOpt` / `FlowProbeOpt` | `Pcapng.cpp` | **WIRED** | one 2-byte load per tag (1.15-1.25x); SSE2 stride-2 flow scan |
| 36 | `wintcp::RenderSegmentsOpt` | `TcpReasm.cpp` `Render` | **DO NOT integrate - measured tie, four rewrites all lost** (see below) | |
| 37 | `wintcp::DistinctPidsOpt` / `GroupByPidOpt` | `Snapshot.cpp`, `ConnectionStore.cpp` | `DistinctPidsOpt` **WIRED**; `GroupByPidOpt` BLOCKED (needs the `pidRows_` type and 3 read sites changed) | flat open-addressing table (3.4-15x dedup) |
| 27b | `wintcp::ResolveOffsetShiftOpt` | `GeoIp.cpp` | **DO NOT integrate - measured 0.69x-0.94x** | MSB-first shift register |

**Wire count, last re-derived 2026-10-10:** 18 of the 57 `*Opt` declarations in
`Opt.h` are called from product sources. Everything not marked **WIRED** above is
uncalled: either measured and rejected, measured and blocked, or a measured
winner still waiting to be wired. An unmarked row is not evidence either way -
go and count the calls.

## Integration status (2026-10-09)

`Opt.h` / `Opt.cpp` now live in the PRODUCT tree (`wintcp/src/`), registered
in all three build paths, and the bench compiles that one copy. So a number this
page quotes is the number the shipped binary gets.

Integrated and gated green. Gate numbers from the **2026-10-10 run** - they
widen as the suites grow, so re-measure rather than quote (the last known
`examples.bat` figure was 97 commands and is now 116, and `unit` was 810 and is
now 1004):

`build.bat` from clean, `cli.bat` 268/0, `examples.bat` 116/0, `gui.bat` 54/0,
`wintcp-tests.exe unit` 1004/0, `bench` 431/431 both variants:

| Call site replaced | By | Measured |
|---|---|---|
| `Snapshot.cpp:37` `DistinctPids` | `DistinctPidsOpt` | 3.3x - 15x |
| `Commands.cpp:324` `DisplayWidth` / `:336` `TruncateToWidth` | `DisplayWidthOpt` / `TruncateToWidthOpt` | 2.2x - 28.7x |
| `Pcapng.cpp:119` `SkipVlan` / `:352` flow probe | `SkipVlanOpt` / `FlowProbeOpt` | 1.13x - 1.75x |
| `Utils.cpp:105` `ToLowerW` | `ToLowerWOpt` | 1.8x - 19.2x |
| `ConnectionStore.cpp:93` `RebuildLowerAll` | `BuildLowerAllOpt` | 4.9x |
| `TlsDecode.cpp:227` ext classify + SNI tail strip | `ClassifyTlsExtensionOpt` / `StripSniTailOpt` | 1.26x / 17.0x |
| `Commands.cpp:63` `JsonEscapeA` | `JsonEscapeOpt` | 1.0x - 1.9x |
| `Utils.cpp:195` `CsvEscapeUtf8` | `CsvEscapeOpt` | see report |
| `TcpTable.cpp:42` `PrintIpv4` / `:55` `PrintIpv6` | `FormatIpv4Opt` / `FormatIpv6Opt` | 7.9x / 13.7-21.6x |
| `Utils.cpp` `FormatPort` | `FormatPortOpt` | 6.30x (443) / 4.09x (65535) |
| `ConnectionStore.cpp` `FormatDuration` | `FormatDurationOpt` | 7.75x - the `%02llu` padding is the easy step to lose |
| `ConnectionStore.cpp` `ComputeGroupRates` group sums | `SumPidTrafficOpt` | 3.63x (500 rows) / 4.24x (5k rows) |
| `GeoIp.cpp` `IsGlobalUnicastV4` / `IsGlobalUnicastV6` | `IsGlobalUnicastV4Opt` / `IsGlobalUnicastV6Opt` | 1.24x |

The last four landed in commit `c2cb108`. The V4 octet table was re-derived
from the original's range tests before wiring, because a table can lie in one
place: a first octet whose original test fell through to the multicast check.
For 172 the later tests can never fire (the address is masked to
`0xB0000000`), so the table and the original agree - but that is the kind of
detail to re-check rather than assume.

**Signature change that came with it:** `BuildLowerAllOpt` now takes
`const std::wstring* const*` (an array of pointers) instead of
`const std::wstring*`. The product call site passes the ten fields by
address; the old form would have copied all ten before joining, which is most
of what the optimization removed.

Deliberately NOT integrated, with the reason - do not re-attempt without
reading it first:

- **`ConnectionKeyHash`** (`ConnectionStore.h:263`) - `HashConnKeyOpt`
  measures 2.9x - 6.2x, but it is FNV-1a where the original is
  `std::hash<string_view>`. Swapping it changes every bucket and therefore
  the iteration order of `PrevKeyIndex` and `pidRows_`, which is
  observable in row ordering. The win does not justify a visible behaviour
  change.
- **`GroupByPidOpt`** into `RebuildIndexes` - 1.0x - 1.6x, and it requires
  changing `pidRows_`'s type (an `unordered_map<DWORD, vector<size_t>>`)
  plus its three `find` sites. `DistinctPids` already removed the larger
  PID cost.
- **`ComputeBpsBatchOpt`** - needs a batch shape (one reciprocal per
  uniform elapsed tick) that the two per-row call sites do not have; each row
  carries its own `elapsed`.
- **`ComputeBpsBatchOpt`** - needs a batch shape (one reciprocal per
  uniform elapsed tick) that the two per-row call sites do not have; each row
  carries its own `elapsed`.
- **Everything in the rejected table below.**
- **`SumPidTrafficOpt` into `ComputeGroupRates`** - WAS BLOCKED HERE, now
  wired in `c2cb108` (2026-10-10). The block was real: it needed the
  `std::map<DWORD, PidTraffic>` accumulator and the `anyCounted` map replaced
  together with the two read sites. The wiring flattens rows into
  `PidTrafficRow[]`, calls Opt once, then reads the sums back by PID. The
  invariant that kept the effort honest: a PID whose rows were all ETW-fed
  must come back with `hasSum == false` rather than a zero sum, because
  "unknown" and "measured zero" are different answers and the em-dash is what
  the reader should see.

### Measured and rejected (kept so nobody re-derives them)

Four candidates were implemented, measured, and deliberately abandoned. The
bench harness keeps the entries so the report carries the evidence.

| Candidate | Approach | Measured | Verdict |
|---|---|---|---|
| `CpWidth` (33) | 12 KB flat LUT for cp < 0x3000, generated from the original chain by a constexpr table | 0.62x / 0.78x | The chain is 4 well-predicted compares for the common low code points; no table load or binary search beat it. ASCII fast path in front of the table did not close the gap (0.90x). |
| `ResolveOffset` shift register (27b) | address bits as two byte-swapped `u64` words consumed MSB-first, three geometry-specific straight-line walkers, 4-byte load + BSWAP per record | 0.69x - 0.94x across all tree sizes | `ResolveOffsetOpt` (#1) already wins by avoiding the per-bit division/modulo; the register cannot also remove the per-bit branch. 24-bit halves also cannot be taken from one 4-byte load - the right half is `p[3..5]`, which starts past the load, so it needs a second overlapping load. |
| `ReasmRender` (36) | 4-pass 16-bit radix sort on `u64` seq | 0.02x (96 segs) - 0.67x (4096) | The 65536-entry histogram costs more than `n log n` until n is in the tens of thousands. The digit width, not a threshold, was the problem. |
| `ReasmRender` (36) | packed `{seq, index}` key + `std::sort` | 0.60x - 0.85x | Removes two pointer chases per compare but adds 12 bytes of memory traffic per element - a net loss at these sizes. |
| `ReasmRender` (36) | `resize(bytes_)` instead of `reserve()` | 0.73x - 0.93x | `resize` value-initialises, so it adds a full zero pass over the stream for nothing. |
| `ReasmRender` (36) | already-in-order fast path | 0.80x - 0.87x | The O(n) check costs as much as the sort it avoids below ~4000 segments. |

**Why #36 is a tie and stays in the bench**: `Render` is dominated by the byte
copy, which is memory-bandwidth bound and identical in every rewrite. The
report's `ReasmRender` rows read 1.01-1.06x - that is noise around a deliberate
no-op, not a win. `RenderSegmentsOpt` is a faithful copy of the original
algorithm on purpose, so the entry exists to say "do not touch this function"
rather than to claim a speedup.

### Implementation gotchas
- **GeoIP records**: supports three record widths — 24-bit (3 bytes), 32-bit (4 bytes),
  and the packed 28-bit format where two records share a middle byte
  (`record28=true`). For 28-bit databases the real `recordBytes_` is 3
  (GeoIp.cpp:825-826); callers that pass 0 with `record28=true` are also
  accepted. The byte layout for 28-bit is:
  `left = high nibble of byte3 | byte0..byte2`, `right = low nibble of byte3 | byte4..byte6`.
- **GeoIP sizes**: the `dataSectionSize` parameter is the DATA SECTION size
  (it mirrors `Loaded()`), not the total file size. Passing the file size
  accepts data pointers past the section and un-breaks the unloaded-tree
  refusal — the integration snippet passes `dataSectionSize_`.
- **GeoIP 32-bit halves**: the optimized 32-bit path selects the half
  (`bit * 4`). `GeoIp.cpp:977` reads `recPos` for both halves, so the
  original answers 32-bit right-branch walks with the left record; the
  benchmark keeps that bug in `orig_functions.cpp` and probes it.
- **Key encoding**: the key begins with a family byte (`'4'` or `'6'`) and protocol
  byte (`'T'` or `'U'`), followed by addresses and ports written little-endian
  (4 bytes each). IPv6 = 46 bytes; IPv4 = 22 bytes.
- **FormatBytes precision**: KB/MB use `%.1f`, GB/TB use `%.2f`, matching the
  original. The KB/MB/GB/TB path renders into a stack buffer first so a
  short caller buffer truncates quietly (return 0, empty buffer) instead of
  invoking the CRT invalid-parameter handler (modal dialog in Debug).
- **WideToUtf8**: surrogate pairs (0xD800–0xDBFF high, 0xDC00–0xDFFF low) are encoded
  as 4-byte UTF-8; unpaired surrogates (high without low, lone low) emit
  U+FFFD (3 bytes). Truncation stops before a character that would not fit.
- **Display width**: `CpWidth`'s wide ranges are NOT all contiguous - `0x303F` and
  `0x3040` sit between `0x303E` and `0x3041` and are width 1, so any range table
  must include the holes. `TruncateToWidth`'s budget is `width - 1` because the
  `U+2026` marker owns the last column, and a value whose width already fits is
  returned unchanged with no marker.
- **MMDB 24-bit halves**: the left record is bytes 0..2 of the node and the right
  is bytes 3..5, so a single 4-byte load at the node start does NOT yield the
  right half - it needs a second load at `recPos + 3` (still inside the node, so
  in bounds). The same trap applies to any hand-rolled reader.
- **MMDB code 29**: the check is `pos >= size`, not `pos + 1 > size`; they happen
  to be equivalent, but the base is 29 + the byte, not just the byte.
- **Flow-record probe**: the candidates are the EVEN offsets 12..38 (stride 2),
  and one is admitted only when `k + 5 <= capLen`. An SSE2 block that scans
  lanes 0..15 of a 16-byte load must mask with `0x5555` (even lanes only) AND
  stop before the block would reach an offset >= 40, or it reports a
  "signature" the original refuses.
- **SSE2 `movemask` + stride**: when the per-lane verdict already reads the
  byte two past the lane (a load at `k + 2`), do NOT also shift the vector by
  two - that double-counts the stride and loses every candidate.
- **A `std::vector::resize` over `reserve`**: `resize` value-initialises, so it
  writes a full pass of zeroes over the new elements. For a multi-megabyte
  render buffer that is pure waste - use `reserve` and grow.
- **A span-copy SIMD loop must EMIT the clean block, not just skip it.**
  `CsvEscapeOpt` had `if (m == 0) { i += 16; continue; }` - it consumed 16 bytes
  and appended nothing, so every quoted field longer than 16 bytes lost its
  first 16 characters. `list --format csv` (and `export --out x.csv`) turned
  `RpcEptMapper, RpcSs` into `cSs` and `EFS, KeyIso, SamSs, VaultSvc` into
  `Ss, VaultSvc`. The A/B bench passed 420/420 through it because EVERY csv
  test input was under 16 bytes (`"a,b"`, `"a\"b"`, ...) and never entered the
  SIMD loop. `JsonEscapeOpt` is correct because it appends the whole span
  (`out.append(p + i, j - i)`) after the scan instead of per block.
  **The lesson generalises: a differential test whose inputs are shorter than
  the SIMD width proves nothing about the SIMD path.** Both files now carry
  16/17/32/33-byte and multi-block cases plus a length x position x special
  sweep (81 x 80 x 4). Found by running the documented CLI example, not by a
  test - which is the argument for running them.

## Integration Workflow (summary)

The full procedure is in `HOWTO-INTEGRATION.md`. Short version:
1. Build and pass all tests in this directory (`build.bat Release`).
2. Add `opt_functions.cpp` to the main `wintcp` CMake/vcxproj sources.
3. Replace each target function's body with a call to its `*Opt` equivalent
   (include `"tests/asm/opt_functions.h"`).
4. Add a CPU feature guard (`WINTCP_ENABLE_ASM_OPTIMIZATIONS`, x64-Release only) so
   older CPUs fall back to the original implementation.
5. Run `wintcp-tests.exe unit` and `wintcp-tests.exe bench` to verify.

## Important Notes
- The `.asm` file is **reference documentation only** — it contains a stub `PROC` and
  is not assembled or linked by either build system. The real implementations are the
  C++ intrinsics in `opt_functions.cpp`.
- `build.bat` writes the executable to `build\testAssemblies.exe` (no `bin` subdir)
  in the direct-`cl` path, but to `build\bin\Release\testAssemblies.exe` in the
  CMake path. Be aware of the different output locations.
- This is a **standalone** test project — it does not include or link against the
  rest of wintcp. It only reads headers from `../../src` (include path) for any
  shared type definitions the optimized functions depend on.

## The A/B Bench Project (`bench/`)

Alongside the unit harness there is a two-executable A/B benchmark that measures
every candidate against a copy of the original wintcp code.

| File | Role |
|---|---|
| `wintcp_benchmark.cpp` | ONE harness, compiled once per variant; `--json`, `--phase=`, `--rounds`, `--time-ms`, `--variant=` |
| `bench/bench_api.h` | `namespace wintcp::bench` contract, one numbered section per candidate |
| `bench/orig_functions.cpp` | faithful copies of the original C++, each citing `file:line` |
| `bench/replaced_functions.cpp` | the same copies with the bodies commented out |
| `bench/asm_functions.cpp` | `bench::` functions that delegate to the `*Opt` implementations |
| `run_benchmark_report.py` | runs both exes, compares correctness test-by-test, computes stats |
| `build_bench.bat` | builds `bench_orig.exe` and `bench_asm.exe` (auto-locates `vcvars64`) |
| `opt_functions.cpp` -> `build_bench/wintcp_asm_opt.lib` | the optimized code as a static lib |

Two executables from ONE harness source: `bench_asm.exe` links
`replaced_functions.cpp` + `asm_functions.cpp` + the lib, `bench_orig.exe` links
`orig_functions.cpp`. **Only the code under test differs.**

### Per-candidate procedure (proven 37 times)

1. Copy the original into `bench/orig_functions.cpp`, citing `file:line`.
2. Comment the body out in `bench/replaced_functions.cpp`.
3. Declare the `*Opt` in `opt_functions.h`, implement in `opt_functions.cpp`.
4. Add a `bench::` wrapper in `bench/asm_functions.cpp` and the struct/entry in
   `bench/bench_api.h`.
5. Add correctness + workload tests to `wintcp_benchmark.cpp`; unit tests to
   `testAssemblies.cpp`.
6. `build_bench.bat` -> both exes must be 100% correctness-identical.
7. Run `run_benchmark_report.py`; record the ratio above.

```bat
build_bench.bat                    :: build both bench exes + the lib
.\build_bench\bench_orig.exe --json --phase=correctness > o.json
.\build_bench\bench_asm.exe  --json --phase=correctness > a.json
python run_benchmark_report.py     :: -> benchmark_report.md
```

**Latest measured state (2026-10-09, after the CSV span-copy fix):** 431
correctness checks, 0 divergences; 133 workloads, geometric mean **1.60x**.
`testAssemblies.exe`: **248 passed / 0 failed**.

The correction matters: the earlier 420/420 and 238/238 runs were both green
while `CsvEscapeOpt` was silently corrupting every quoted field longer than 16
bytes, because no test input was that long. See the span-copy gotcha above.
