# Assembly Optimization Integration Howto

## Overview

This document describes how to integrate the assembly-optimized functions
from `testAssemblies` into the main `wintcp.exe` build. Each optimized
function replaces a hot-path function in the wintcp codebase, offering
significant performance improvements with identical semantics.

## Prerequisites

- Visual Studio 2022 with C++ build tools (v143)
- x64 target platform (optimizations use SSE4.2+, AVX2, BMI1)
- The test project must build and pass all tests before integration

## Optimization Summary

The `Expected Gain` column is the ORIGINAL estimate. The measured numbers are in
`AGENTS.md`'s table and `benchmark_report.md`; three candidates (`#27`
FilterHandles, `#36` ReasmRender, `#27b` ResolveOffsetShift) are listed there
precisely because they must NOT be integrated.

| # | Function | File:Line | Expected Gain | Technique |
|---|----------|-----------|---------------|-----------|
| 1 | `ResolveOffset` | GeoIp.cpp:1002 | 5-10x | Bit-test loop, inlined node records |
| 2 | `Direction::Add` | TcpReasm.cpp:91 | 2-4x | SIMD segment range comparison |
| 3 | `HasLowerSubstring` | Utils.h:165 | 1.5-2.5x | SSE4.2 PCMPESTRI |
| 4 | `ParseIpTcp` | Pcapng.cpp:190 | 1.5-3x | SIMD header loads, BSWAP |
| 5 | `FormatBytes` | Utils.cpp:192 | 2-3x | Integer arithmetic, no swprintf |
| 6 | `KeyOf` | ConnectionStore.cpp:358 | 1.5-2x | SSE registers, single store |
| 7 | `WideToUtf8` | Utils.cpp:94 | 2-3x | SIMD ASCII path, single-pass |
| 8 | `FormatStreamHex` | StreamCapture.cpp:578 | 10-30x | Hex LUT + direct stores |
| 9 | `ToLowerW` | Utils.cpp:105 | 2-20x | SSE2 ASCII fold, towlower fallback |
| 10 | `ReadU32BE` / `ReadU64BE` | GeoIp.cpp:124-139 | 1-2x | memcpy + BSWAP |
| 11 | `TlsBe16/24/32` | TlsDecode.cpp:23-36 | 1-2x | memcpy + BSWAP |
| 12 | `ClassifyTlsExtension` / `StripSniTail` | TlsDecode.cpp:227-266 | 2-20x | 17-entry LUT, SSE2 tail scan |
| 13 | `Rd16` / `Rd32` | Pcapng.cpp:90-105 | tie | memcpy + conditional BSWAP |
| 14 | `JoinSamples` | ConnectionStore.cpp:1888 | 2-9x | Hash index + FIFO queues |
| 15 | `FindSubstringLong` | ConnectionStore.cpp:87 | 1.6-1.7x | BMH (needle >= 24 chars) |
| 16 | `RebuildLowerAll` | ConnectionStore.cpp:93 | 4.7x | reserve + append |
| 17 | `CompareWide` | CompareRows | tie-1.1x | SSE2 lane diff |
| 18 | key hash / eq | ConnectionStore.h:255 | 3-6x | FNV-1a lanes |
| 19 | `ComputeBps` | ConnectionStore.cpp:686 | 1.7x | one reciprocal per batch |
| 20 | `SumPidTraffic` | ConnectionStore.cpp:1543 | 4x | sort + linear runs |
| 21 | `FormatBpsCell` | ConnectionStore.cpp:554 | 1.2x | via FormatBytesOpt |
| 22 | `AppendUtf8` | GeoIp.cpp:541 | tie-1.2x | SSE2 ASCII bulk |
| 23 | `IsGlobalUnicastV4/V6` | GeoIp.cpp:1243 | 1.2x | octet table + u64 tails |
| 24 | `PairSnapshot` | ConnectionStore.cpp:1304 | 1.2x | same algo, FNV hash |
| 25 | `FindCountedKey` | GeoIp.cpp:519 | tie | u64 prefilter |
| 26 | `FlowKeyEqual` / `CmpFlowAddr` | TcpReasm.cpp:52,224 | 1.1-1.3x | SSE2 key compare |
| 29 | `JsonEscape` | Commands.cpp:62 | 1-3x | SSE2 special scan + span memcpy |
| 30 | `CsvEscape` | Utils.cpp:185, ChartExport.cpp:228 | 1-3x | SSE2 quote scan |
| 31 | `FormatPort` / `FormatU64Dec` / `FormatDuration` | ConnectionStore.cpp:1015, ChartExport.cpp:171 | 1-4x | digit-table emitters |
| 32 | `FormatIpv4` / `FormatIpv6` | TcpTable.cpp:42, SocketTraffic.cpp:210 | 1-3x | digit tables + nibble shuffle |
| 33 | `DisplayWidth` / `TruncateToWidth` | Commands.cpp:324-350 | 2-27x | SSE2 ASCII bulk + fused measure/cut |
| 34 | `PayloadSize` / `ReadPointer` | GeoIp.cpp:422-511 | 1.0-1.1x | branch-cheap hot path + 4-byte BSWAP |
| 35 | `SkipVlan` / flow probe | Pcapng.cpp:119-129, 354-361 | 1.1-1.8x | one load per tag; SSE2 stride-2 scan |
| 37 | `DistinctPids` / `GroupByPid` | Snapshot.cpp:37-45, ConnectionStore.cpp:1599 | 3.4-15x | flat open-addressing table |

## Integration status (2026-10-09)

The optimizations are now IN the product, not just measured. Nine call sites
are wired to `wintcp/src/Opt.h` and all four gates are green (see
`AGENTS.md` -> `Integration status` for the exact list, the measured
numbers and the reasons several candidates were deliberately left alone).

What changed structurally:

- `opt_functions.h` / `opt_functions.cpp` MOVED to `wintcp/src/Opt.h` /
  `wintcp/src/Opt.cpp`. The files left behind in this directory are shims
  (`opt_functions.h` is `#include "Opt.h"`; `opt_functions.cpp` is
  `#include "Opt.cpp"`) so the bench and the shipped binary compile the
  SAME code. **There is one implementation; do not fork a second copy.**
- `DistinctPidsOpt` became a template on the pid type. MSVC's `DWORD` and
  `uint32_t` are distinct 32-bit types and `std::vector<DWORD>` does not
  convert to `std::vector<uint32_t>`, so a single signature could not serve
  both the product and the bench.
- `BuildLowerAllOpt` now takes `const std::wstring* const*` (pointers to
  the fields, not copies of them). The old signature would have made the
  product copy ten strings before joining them.

The rest of this document is the general procedure, kept because the next
candidate still needs it.

## Integration Steps

### Step 1: Build and Verify Tests

Build the test project and ensure all tests pass:

```bat
cd wintcp\tests\asm
build.bat Release
```

All tests must show `PASS` in the output before proceeding.

### Step 2: Add Assembly Sources to Build

#### Option A: CMake Build (recommended)

Add the following to `CMakeLists.txt` in the root directory:

```cmake
# Add the asm-optimized sources to the wintcp target
target_sources(wintcp PRIVATE
    wintcp/tests/asm/opt_functions.cpp
)

# Enable MASM for any .asm files (currently using intrinsics)
enable_language(ASM_MASM)
```

#### Option B: Visual Studio Project

1. Right-click the `wintcp` project in Solution Explorer
2. Select "Add" -> "Existing Item"
3. Add `wintcp\tests\asm\opt_functions.cpp`
4. For any `.asm` files: right-click -> Properties -> Item Type -> "Microsoft Macro Assembler"

### Step 3: Integrate Each Function

For each function, follow this pattern:

#### Function 1: GeoIP Tree Walk

**Original location**: `GeoIp.cpp:1002` (`GeoIpDatabase::ResolveOffset`)

**Integration**:
```cpp
// In GeoIp.cpp, replace the body of ResolveOffset with:
bool GeoIpDatabase::ResolveOffset(const unsigned char bits[16], unsigned bitCount,
                                  size_t startNode, size_t* out) const {
    return wintcp::ResolveOffsetOpt(bits, bitCount, startNode,
                                    nodeCount_, nodeByteSize_,
                                    record28_, recordBytes_,
                                    mappedView_, dataSectionSize_,
                                    kSeparatorLen, out);
}
```

Pass `dataSectionSize_` (not `fileSize_`): the parameter mirrors
`Loaded()` and bounds the data-pointer check. Passing the total file
size silently accepts pointers past the data section.

**Note**: the optimized 32-bit path selects the record half
(`bit * 4`). The current `GeoIp.cpp:977` reads `recPos` for both
halves, so right-branch walks on 32-bit databases answer with the
left record today; fix that line when integrating (one-line change:
`ReadU32BE(mappedView_ + recPos + (half ? 4u : 0u))`).

**Include**: Add `#include "tests/asm/opt_functions.h"` to GeoIp.cpp

#### Function 2: TCP Reassembly

**Original location**: `TcpReasm.cpp:91` (`Direction::Add`)

The overlap-checking section (lines 115-139) is replaced by:

```cpp
// Pass A: skip head covered by existing segments
size_t consumed = 0, skip = 0;
bool overlap = AddSegmentOverlapOpt(s, len,
    /* segSeqs */, /* segSizes */, segCount,
    &consumed, &skip);
if (!overlap && consumed == len) {
    // Fully covered - duplicate
    ++duplicates_;
    return;
}
// Continue with consumed and skip adjusted...
```

**Note**: This requires restructuring `Direction::segs_` to use parallel
arrays instead of `std::vector<Segment>` for SIMD efficiency. See the
test harness for the array-of-structures to structure-of-arrays conversion.

#### Function 3: Filter Substring Matching

**Original location**: `Utils.h:165` (`HasLowerSubstring`)

**Integration**:
```cpp
// In Utils.cpp, replace HasLowerSubstring:
bool HasLowerSubstring(const std::wstring& haystackLower,
                       const std::wstring& needleLower) {
    return HasLowerSubstringOpt(haystackLower.c_str(), haystackLower.size(),
                                needleLower.c_str(), needleLower.size());
}
```

**Include**: Add `#include "tests/asm/opt_functions.h"` to Utils.cpp

#### Function 4: Packet Parsing

**Original location**: `Pcapng.cpp:190` (`ParseIpTcp` helper `FinishTcp`)

The TCP header field extraction (lines 115-129) can use the optimized
version. The IP header parsing is left as-is since it varies by IP version.

#### Function 5: Byte Formatting

**Original location**: `Utils.cpp:192` (`FormatBytes`)

**Integration**:
```cpp
std::wstring FormatBytes(ULONGLONG bytes) {
    wchar_t buf[32] = {0};
    size_t len = FormatBytesOpt(buf, 32, bytes);
    if (len == 0) return L"0 B";
    return std::wstring(buf);
}
```

#### Function 6: Connection Key Building

**Original location**: `ConnectionStore.cpp:358` (`KeyOf`)

The `KeyOf` function builds a binary identity key. The optimized version
eliminates the lambda-based `push` helper and uses direct byte writes.

**Integration**:
```cpp
// Replace the body of KeyOf in ConnectionStore.cpp
ConnectionKey KeyOf(const Connection& c) {
    ConnectionKey k;
    k.len = 0;
    KeyOfOpt(c.family == AF_INET6, c.protocol == IPPROTO_UDP,
             reinterpret_cast<const unsigned char*>(&c.local4),
             c.localPort,
             reinterpret_cast<const unsigned char*>(&c.remote4),
             c.remotePort,
             c.pid, k.bytes, &k.len);
    return k;
}
```

**Note**: The current `KeyOf` already uses `memcpy` directly without
allocation. The gain here is primarily from eliminating the lambda and
using register-direct stores. Test before deciding this warrants replacement.

#### Function 7: WideToUtf8 Conversion

**Original location**: `Utils.cpp:94` (`WideToUtf8`)

**Integration**:
```cpp
std::string WideToUtf8(const std::wstring& s) {
    if (s.empty()) return std::string();
    std::string out(s.size() * 4, '\0');  // worst case
    size_t written = WideToUtf8Opt(s.c_str(), s.size(),
                                   &out[0], out.size());
    out.resize(written);
    return out;
}
```

#### Function 8: Stream Hexdump (P0 #1, 11-30x measured)

**Original location**: `StreamCapture.cpp:578` (`FormatStreamHex`)

**Integration**: drop-in - same signature, byte-identical output:
```cpp
// In StreamCapture.cpp, replace the loop bodies (lines 596-629) with a
// call, or route the whole function:
std::string FormatStreamHex(const std::string& bytes, size_t bytesPerLine) {
    return wintcp::FormatStreamHexOpt(bytes, bytesPerLine);
}
```

#### Function 9: Wide Lowercase (P0 #2, 1.7-19x measured)

**Original location**: `Utils.cpp:105` (`ToLowerW`)

**Integration**: drop-in. Non-ASCII still routes through `towlower`,
so locale behavior is unchanged:
```cpp
std::wstring ToLowerW(const std::wstring& s) {
    return wintcp::ToLowerWOpt(s);
}
```

#### Functions 10-11: Big-Endian Field Reads (P0 #3/#4, tie-1.6x)

**Original locations**: `GeoIp.cpp:124-139`, `TlsDecode.cpp:23-36`,
`Pcapng.cpp:90-105` (see also Function 13)

**Integration**: drop-in per reader. Two deliberate hardenings vs the
originals: null returns 0 (originals crash), and `ReadBytes` with
`n > 8` is refused (the original shifts mod 2^64; no caller does this).
`TlsBe24` intentionally stays shift/OR: callers guarantee only 3 bytes,
so a 4-byte load would over-read.

#### Function 12: TLS Extension Dispatch + SNI Strip (P0 #5)

**Original location**: `TlsDecode.cpp:227-266`

**Integration**: replace the `if/else` type chain and the pop_back loop:
```cpp
// was: if (type == 0x0000 && len >= 5) {...} else if (type == 0x0010 ...) {...}
switch (wintcp::ClassifyTlsExtensionOpt(type)) { ... }
// was: while (!sni.empty() && (back == '\0' || back == '.')) pop_back();
hs->sni.resize(wintcp::StripSniTailOpt(&hs->sni[0], hs->sni.size()));
```
Guard the `StripSniTailOpt` call with `!hs->sni.empty()` (`&s[0]` on an
empty string is UB).

#### Function 13: Pcap Endian Loads (P0 #6, tie)

**Original location**: `Pcapng.cpp:90-105` (`Rd16`/`Rd32`)

**Integration**: drop-in. Same codegen class as the originals on modern
MSVC (measured tie); the value is the null guard, not speed. Skip if
churn is a concern.

#### Function 14: Socket-Sample Join (P0 #7, 1.5-9x measured)

**Original location**: `ConnectionStore.cpp:1888-1912`
(`ApplySocketBytes`; same kernel in `ApplySocketTcpInfo` `:1927` and
`ApplyKernelAges` `:1977`)

**Integration**: build `JoinRow`/`JoinSample` views over rows/samples
and call `JoinSamplesOpt`. Assignment is bit-identical (FIFO queues per
4-tuple = first-match-wins + claim-once, incl. the mDNS duplicate case).
Threshold note: the index costs ~1.7 us to build, so below ~64 rows the
linear scan wins (measured 0.07x at 16x8). Route small ticks at the call
site:
```cpp
if (rows_.size() < 64) { /* existing linear loops */ }
else { /* JoinSamplesOpt over views */ }
```
`ApplySocketTcpInfo`'s per-field `known` gating and `ApplyKernelAges`'s
backdate-only rule stay in C++ around the lookup.

#### Function 15: Long-Needle Substring BMH (P0 #8, 1.6-1.7x at 32 chars)

**Original location**: `ConnectionStore.cpp:87-89` (`Has`)

**Integration**: route by needle length at the `Has` call site
(measured crossover between 16 and 32 chars; short needles lose to table
setup, 0.43x at 8 chars):
```cpp
bool Has(const std::wstring& haystack, const std::wstring& needle) {
    if (needle.empty()) return true;
    if (needle.size() >= 24)
        return wintcp::FindSubstringLongOpt(haystack, needle);
    return HasLowerSubstringOpt(haystack.c_str(), haystack.size(),
                                needle.c_str(), needle.size());
}
```

#### Function 16: Lower-All Concat (P0 #9, 4.7x measured)

**Original location**: `ConnectionStore.cpp:93-98` (`RebuildLowerAll`)

**Integration**: drop-in for the fixed 10-field join (shorter/longer
inputs keep the original's empty/first-10 contract):
```cpp
void RebuildLowerAll(Connection& c) {
    const std::wstring f[10] = {c.lowerLocal, c.lowerRemote, ...};
    c.lowerAll = wintcp::BuildLowerAllOpt(f, 10);
}
```

#### Function 17: Wide String Compare (P0 #10, tie-1.1x)

**Original location**: `std::wstring::compare` in `CompareRows`
(`ConnectionStore.cpp:2284`)

**Integration**: drop-in comparator primitive (same sign contract, incl.
prefix-length rule). The sort-level win comes separately from integer
sort keys (Schwartzian, still main-tree work); this primitive alone
measures a tie on long strings (CRT `wmemcmp` is already vectorized).

#### Function 18: Connection-Key Hash + Equality (P1 #11)

**Original location**: `ConnectionStore.h:255-268`

**Integration**: swap the hasher only - `ConnectionKeyHash` becomes FNV-1a
lanes (measured 2.9x on 22B keys, 6.2x on 46B). Hash VALUES change
(bucket redistribution is safe: pairing within a key is queue-ordered).
Keep `operator==` as-is (SSE2 version ties at 0.9x; not worth the churn).

#### Function 19: Batch Rate Computation (P1 #13a)

**Original location**: `ConnectionStore.cpp:686-703` (`ComputeBps`)

**Integration**: where a tick shares one `elapsed` (the common case: every
row sampled at the same `nowTick`), compute one reciprocal and multiply
(measured 1.7x). Rows with divergent `lastSampleTick` keep the scalar
`ComputeBps` call.

#### Function 20: Per-PID Traffic Sums (P1 #13b)

**Original location**: `ConnectionStore.cpp:1543-1597`
(`ComputeGroupRates` sum phase)

**Integration**: replace the two `std::map`s with sort-once + linear runs
(measured 3.8-4.3x), keeping `SatAdd`, the write-to-every-row invariant,
and the unknown-when-unmeasured rule.

#### Function 21: Rate Cell Formatting (P1 #14)

**Original location**: `ConnectionStore.cpp:554-565` (`FormatBpsCell`)

**Integration**: drop-in (same sentinels, same truncation contract).

#### Function 22: MMDB UTF-8 Widen (P1 #16)

**Original location**: `GeoIp.cpp:541-579` (`AppendUtf8`/`Widen`)

**Integration**: drop-in. Measured tie-1.2x: short strings sit on the
`std::wstring` allocation floor (~35 ns), so this is a small win. The
scalar fallback preserves the U+FFFD rules on untrusted bytes exactly.

#### Function 23: Unicast Classification (P1 #17)

**Original location**: `GeoIp.cpp:1243-1309`

**Integration**: drop-in (measured 1.2x both families). Proven exhaustive:
all 16.7M first-three-octet V4 addresses agree bit-for-bit. Also serves
the `local:private` filter path (`AddrIsPrivate`).

#### Function 24: Snapshot Pairing (P1 #12)

**Original location**: `ConnectionStore.cpp:1304-1326` (`PrevKeyIndex`)

**Integration**: keep the hash + FIFO-queue algorithm; swap in the FNV
lane hasher (measured 1.1-1.2x on typical/mDNS ticks, 0.87x at 5k rows -
wash). Do NOT use sort-merge: measured 2.4-3x slower (short keys hash in
~2 lanes; sort pays O(n log n) full-key compares).

#### Function 25: Counted-Key Find (P1 #15)

**Original location**: `GeoIp.cpp:519-535` (`MapFind` per-entry step)

**Integration**: not recommended at MMDB map sizes (2-8 entries): measures
a tie at the 3 ns noise floor, 0.94x at 64 keys. `MapFind`'s true cost is
the `Decode`/`strlen` scaffolding per entry, not the compare - hoist that
instead (still open).

#### Function 26: Flow-Key Equality (P1 #18)

**Original location**: `TcpReasm.cpp:52,224-228`

**Integration**: drop-in (measured 1.1-1.3x; ports-first reject kept).

#### Function 27: Handle-Table Filter (P1 #19)

**Original location**: `SocketTraffic.cpp:973-1012` (phase 1)

**Integration**: DO NOT integrate - measured 0.51x. `unordered_set` wins
membership at these sizes, and phase 1 is ~0.4 ms of the pass anyway
(the 8.3 s pass cost is phase-2 ioctls).

#### Function 28: ETW Classifier + Payload (P1 #20)

**Original location**: `EtwTraffic.cpp:260-293`

**Integration**: drop-in (measured tie-1.1x; kHz rate makes even small wins
count on the ETW thread). Opcode 18 stays excluded; table verified
against the chain incl. the id/opcode-0 edge.

### Step 4: Enable Required CPU Features

Add a CPU feature check to ensure the optimized functions are safe to call:

```cpp
// In wintcp/src/WinCaps.cpp or a new capability check
bool HasSse42() {
#ifdef _MSC_VER
    int cpuInfo[4] = {0};
    __cpuid(cpuInfo, 1);
    return (cpuInfo[2] & (1 << 20)) != 0;  // SSE4.2 bit
#else
    return false;  // Fallback for non-MSVC
#endif
}

bool HasAvx2() {
#ifdef _MSC_VER
    int cpuInfo[4] = {0};
    __cpuid(cpuInfo, 7);
    return (cpuInfo[1] & (1 << 5)) != 0;  // AVX2 bit
#else
    return false;
#endif
}
```

### Step 5: Update Build Configuration

#### For `build.bat`:
Add `opt_functions.cpp` to the source list in the build command.

#### For `wintcp.vcxproj`:
Add a reference to the new source file in the project's ClCompile section:
```xml
<ClCompile Include="tests\asm\opt_functions.cpp" />
```

#### For `wintcp-tests.vcxproj`:
Add the same reference so unit tests can validate the optimized paths:
```xml
<ClCompile Include="tests\asm\opt_functions.cpp" />
```

### Step 6: Add Conditional Compilation Guards

To allow falling back to the original implementation on older CPUs,
wrap the integration points:

```cpp
#ifdef WINTCP_ENABLE_ASM_OPTIMIZATIONS
    // Use optimized version
    return HasLowerSubstringOpt(haystack, hayLen, needle, needleLen);
#else
    // Original implementation
    return haystack.find(needle) != std::wstring::npos;
#endif
```

Define `WINTCP_ENABLE_ASM_OPTIMIZATIONS` in the Release configuration
for x64 builds only (x86 lacks many of the required SSE/AVX instructions).

## Testing

After integration, run the test suite to verify correctness:

```bat
wintcp-tests.exe unit
```

All existing tests must continue to pass. The tests in
`testAssemblies.cpp` validate that the optimized functions produce
identical results to the original implementations.

## Performance Verification

Compare before and after using the bench mode:

```bat
wintcp-tests.exe bench 50000 20
```

Pay particular attention to the `--bench` mode's filter and formatting
timings, which should show measurable improvements after integration.

Before integrating anything, reproduce the A/B measurement from this
directory so the claim rests on this machine's numbers:

```bat
build_bench.bat
python run_benchmark_report.py    :: -> benchmark_report.md
```

The report's summary line is the number to quote: 133 workloads, 420
correctness checks with no divergence, geometric mean 1.60x. A candidate is
only integrable if it is on the "faster" list in `AGENTS.md`; the three
`DO NOT integrate` rows (FilterHandlesOpt, RenderSegmentsOpt,
ResolveOffsetShiftOpt) are there because they measured slower and are kept
only as evidence.

## Rollback Procedure

If issues are found:

1. Remove `opt_functions.cpp` from the build configuration
2. Revert the function bodies to their original implementations
3. Remove the `WINTCP_ENABLE_ASM_OPTIMIZATIONS` define

The optimized functions are designed as drop-in replacements with
identical signatures, so rollback requires no structural changes.

## File Inventory

| File | Purpose |
|------|---------|
| `opt_functions.h` | Declarations of optimized functions |
| `opt_functions.cpp` | C++ intrinsic-based implementations (preferred) |
| `*.asm` | Reference assembly implementations for documentation |
| `testAssemblies.cpp` | Test harness and benchmark suite |
| `CMakeLists.txt` | CMake build configuration |
| `build.bat` | Quick build script |
| `HOWTO-INTEGRATION.md` | This document |
| `AGENTS.md` | Measured results per candidate, plus the rejected list |
| `ASM_CANDIDATES.md` | The 30 candidates identified from the source, with `file:line` |
| `bench/bench_api.h` | The `wintcp::bench` contract, one section per candidate |
| `bench/orig_functions.cpp` | Faithful copies of the originals (bench_orig.exe) |
| `bench/replaced_functions.cpp` | Same copies, bodies commented out (bench_asm.exe) |
| `bench/asm_functions.cpp` | `bench::` entry points delegating to the `*Opt` code |
| `wintcp_benchmark.cpp` | The one harness, compiled once per variant |
| `run_benchmark_report.py` | Runs both exes, compares correctness, writes the report |
| `build_bench.bat` | Builds both bench executables and the static lib |

