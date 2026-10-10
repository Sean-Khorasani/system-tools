# WinTCP Assembly Optimization Benchmark Report

**Generated:** 2026-10-09 17:49:33
**Original:** `build_bench\bench_orig.exe`
**Optimized:** `build_bench\bench_asm.exe`
**Runs per variant:** 3

---

## Executive Summary

Both variants pass **all** correctness tests. 
The optimized implementations are functionally equivalent to the original C++ code.


**Overall speedup (geometric mean): 1.59x**
- 71 workload(s) faster (optimized)
- 35 workload(s) slower (optimized)
- 27 tied (within 5%)

---

## Environment

| Property | Value |
|----------|-------|
| Compiler | MSVC 194435228 |
| Architecture | x64 |
| CPU | unknown |
| RAM | 0 MB |
| OS | unknown |
| CPU Features | sse42=True, avx2=True, bmi1=True, bmi2=True, popcnt=True |
| Benchmark config | 5 rounds x 80.0 ms (warmup: 20.0 ms) |

- Original executable: `bench_orig.exe`
- Optimized executable: `bench_asm.exe`

---

## Correctness Comparison

| Variant | Passed | Failed | Total |
|---------|--------|--------|-------|
| original (run 1) | 431 | 0 | 431 |
| original (run 2) | 431 | 0 | 431 |
| original (run 3) | 431 | 0 | 431 |
| asm (run 1) | 431 | 0 | 431 |
| asm (run 2) | 431 | 0 | 431 |
| asm (run 3) | 431 | 0 | 431 |

### Per-Test Results

| Test | Group | Original | Optimized | Match |
|------|-------|----------|-----------|-------|
| chain30 walk (24-bit) finds data at depth 29 | geoip | PASS | PASS | OK |
| | | _orig: found=1 offset=0_ | | |
| | | | _asm: found=1 offset=0_ | |
| chain40 walk exhausts 32 bits (no data) | geoip | PASS | PASS | OK |
| | | _orig: found=0_ | | |
| | | | _asm: found=0_ | |
| data at depth 0 via left record | geoip | PASS | PASS | OK |
| | | _orig: found=1 offset=5_ | | |
| | | | _asm: found=1 offset=5_ | |
| data at depth 0 via right record | geoip | PASS | PASS | OK |
| | | _orig: found=1 offset=7_ | | |
| | | | _asm: found=1 offset=7_ | |
| record == nodeCount means no data | geoip | PASS | PASS | OK |
| | | _orig: found=0_ | | |
| | | | _asm: found=0_ | |
| record == nodeCount+16 is data offset 0 | geoip | PASS | PASS | OK |
| | | _orig: found=1 offset=0_ | | |
| | | | _asm: found=1 offset=0_ | |
| record == nodeCount+16+dataSize-1 is last byte | geoip | PASS | PASS | OK |
| | | _orig: found=1 offset=1023_ | | |
| | | | _asm: found=1 offset=1023_ | |
| record == nodeCount+16+dataSize is out of range | geoip | PASS | PASS | OK |
| | | _orig: found=0 offset=999_ | | |
| | | | _asm: found=0 offset=999_ | |
| chain30 walk (28-bit) finds data | geoip | PASS | PASS | OK |
| | | _orig: found=1 offset=0_ | | |
| | | | _asm: found=1 offset=0_ | |
| chain30 walk (32-bit) finds data | geoip | PASS | PASS | OK |
| | | _orig: found=1 offset=0_ | | |
| | | | _asm: found=1 offset=0_ | |
| PROBE 32-bit right-branch (known orig half bug) | geoip | PASS | PASS | OK |
| | | _orig: found=0 offset=999_ | | |
| | | | _asm: found=1 offset=0_ | |
| IPv6 128-bit walk down a 64-node chain | geoip | PASS | PASS | OK |
| | | _orig: found=1 offset=3_ | | |
| | | | _asm: found=1 offset=3_ | |
| random 65k-tree walk consistency (64 addresses) | geoip | PASS | PASS | OK |
| | | _orig: T1T0T1T1T1T1T1T7T3T1T1T0T1T3T4T1T7T1T7T1T1T3T0T1T2T3T1T1T3T0T1T1T1T1T1T1FT7T0T1T1T1T1T1T2T7T2T1T1T7T1T7T7T7T1T1T3T1FT0T7T1T1T1_ | | |
| | | | _asm: T1T0T1T1T1T1T1T7T3T1T1T0T1T3T4T1T7T1T7T1T1T3T0T1T2T3T1T1T3T0T1T1T1T1T1T1FT7T0T1T1T1T1T1T2T7T2T1T1T7T1T7T7T7T1T1T3T1FT0T7T1T1T1_ | |
| PROBE startNode past the node table (divergence) | geoip | PASS | PASS | OK |
| | | _orig: found=0 offset=999_ | | |
| | | | _asm: found=0 offset=999_ | |
| PROBE data pointer past the data section (divergence) | geoip | PASS | PASS | OK |
| | | _orig: found=0 offset=999_ | | |
| | | | _asm: found=0 offset=999_ | |
| unloaded tree (dataSectionSize=0) refuses | geoip | PASS | PASS | OK |
| | | _orig: found=0_ | | |
| | | | _asm: found=0_ | |
| chain30 24-bit: 200 random walks agree with the per-bit walker | shiftwalk | PASS | PASS | OK |
| chain40 24-bit: 200 random walks agree with the per-bit walker | shiftwalk | PASS | PASS | OK |
| dense 24-bit: 200 random walks agree with the per-bit walker | shiftwalk | PASS | PASS | OK |
| dense 28-bit: 200 random walks agree with the per-bit walker | shiftwalk | PASS | PASS | OK |
| dense 32-bit: 200 random walks agree with the per-bit walker | shiftwalk | PASS | PASS | OK |
| ipv6 128-bit: 200 random walks agree with the per-bit walker | shiftwalk | PASS | PASS | OK |
| MSB-first order matches (depth 0) | shiftwalk | PASS | PASS | OK |
| | | _orig: found=0_ | | |
| | | | _asm: found=0_ | |
| MSB-first order matches (depth 0, left) | shiftwalk | PASS | PASS | OK |
| | | _orig: found=1 offset=3_ | | |
| | | | _asm: found=1 offset=3_ | |
| empty store: full segment is new | reasm | PASS | PASS | OK |
| | | _orig: consumed=0 skip=50 hasNew=1_ | | |
| | | | _asm: consumed=0 skip=50 hasNew=1_ | |
| append after existing segment | reasm | PASS | PASS | OK |
| | | _orig: consumed=0 skip=50 hasNew=1_ | | |
| | | | _asm: consumed=0 skip=50 hasNew=1_ | |
| fully covered segment is a duplicate | reasm | PASS | PASS | OK |
| | | _orig: consumed=50 skip=0 hasNew=0_ | | |
| | | | _asm: consumed=50 skip=0 hasNew=0_ | |
| head trim: 70 consumed, 30 new | reasm | PASS | PASS | OK |
| | | _orig: consumed=70 skip=30 hasNew=1_ | | |
| | | | _asm: consumed=70 skip=30 hasNew=1_ | |
| zero-length segment | reasm | PASS | PASS | OK |
| | | _orig: consumed=0 skip=0 hasNew=0_ | | |
| | | | _asm: consumed=0 skip=0 hasNew=0_ | |
| gap between segments: tail is new | reasm | PASS | PASS | OK |
| | | _orig: consumed=30 skip=10 hasNew=1_ | | |
| | | | _asm: consumed=30 skip=10 hasNew=1_ | |
| out-of-order store: consumed+skip <= len | reasm | PASS | PASS | OK |
| | | _orig: consumed=170 skip=50 hasNew=1_ | | |
| | | | _asm: consumed=170 skip=50 hasNew=1_ | |
| boundary-adjacent segment (gEnd == s) | reasm | PASS | PASS | OK |
| | | _orig: consumed=0 skip=10 hasNew=1_ | | |
| | | | _asm: consumed=0 skip=10 hasNew=1_ | |
| single-byte full duplicate | reasm | PASS | PASS | OK |
| | | _orig: consumed=1 skip=0 hasNew=0_ | | |
| | | | _asm: consumed=1 skip=0 hasNew=0_ | |
| 8192-segment randomized store (invariants) | reasm | PASS | PASS | OK |
| | | _orig: consumed=957 skip=0 hasNew=0_ | | |
| | | | _asm: consumed=957 skip=0 hasNew=0_ | |
| empty needle matches | substring | PASS | PASS | OK |
| needle longer than haystack | substring | PASS | PASS | OK |
| match at start | substring | PASS | PASS | OK |
| match in middle | substring | PASS | PASS | OK |
| match at end | substring | PASS | PASS | OK |
| no match | substring | PASS | PASS | OK |
| identical strings | substring | PASS | PASS | OK |
| single-char needle at end | substring | PASS | PASS | OK |
| repeated-char haystack, absent needle | substring | PASS | PASS | OK |
| | | _orig: haystack=4096_ | | |
| | | | _asm: haystack=4096_ | |
| 4K haystack, needle at end | substring | PASS | PASS | OK |
| | | _orig: haystack=4102_ | | |
| | | | _asm: haystack=4102_ | |
| 64K haystack, absent needle | substring | PASS | PASS | OK |
| | | _orig: haystack=65536_ | | |
| | | | _asm: haystack=65536_ | |
| 20-byte header: all fields match | parse | PASS | PASS | OK |
| | | _orig: ok=1 sport=8080 dport=80 seq=0x11223344 ack=0x55667788 flags=0x18_ | | |
| | | | _asm: ok=1 sport=8080 dport=80 seq=0x11223344 ack=0x55667788 flags=0x18_ | |
| 32-byte header with options and 8-byte payload | parse | PASS | PASS | OK |
| | | _orig: ok=1 payloadLen=8_ | | |
| | | | _asm: ok=1 payloadLen=8_ | |
| zero-length payload (dataOff == avail) | parse | PASS | PASS | OK |
| | | _orig: ok=1_ | | |
| | | | _asm: ok=1_ | |
| truncated header (< 20 bytes) rejected | parse | PASS | PASS | OK |
| | | _orig: ok=0_ | | |
| | | | _asm: ok=0_ | |
| data offset 0 rejected | parse | PASS | PASS | OK |
| | | _orig: ok=0_ | | |
| | | | _asm: ok=0_ | |
| data offset beyond avail rejected | parse | PASS | PASS | OK |
| | | _orig: ok=0_ | | |
| | | | _asm: ok=0_ | |
| 0 bytes | format | PASS | PASS | OK |
| | | _orig: got="0 B" want="0 B"_ | | |
| | | | _asm: got="0 B" want="0 B"_ | |
| 512 bytes | format | PASS | PASS | OK |
| | | _orig: got="512 B" want="512 B"_ | | |
| | | | _asm: got="512 B" want="512 B"_ | |
| 1023 bytes | format | PASS | PASS | OK |
| | | _orig: got="1023 B" want="1023 B"_ | | |
| | | | _asm: got="1023 B" want="1023 B"_ | |
| 1024 bytes | format | PASS | PASS | OK |
| | | _orig: got="1.0 KB" want="1.0 KB"_ | | |
| | | | _asm: got="1.0 KB" want="1.0 KB"_ | |
| 1536 bytes (1.5 KB) | format | PASS | PASS | OK |
| | | _orig: got="1.5 KB" want="1.5 KB"_ | | |
| | | | _asm: got="1.5 KB" want="1.5 KB"_ | |
| 5 MiB | format | PASS | PASS | OK |
| | | _orig: got="5.0 MB" want="5.0 MB"_ | | |
| | | | _asm: got="5.0 MB" want="5.0 MB"_ | |
| PROBE 1 GiB (%.2f GB) | format | PASS | PASS | OK |
| | | _orig: got="1.00 GB" want="1.00 GB"_ | | |
| | | | _asm: got="1.00 GB" want="1.00 GB"_ | |
| PROBE 1 TiB (%.2f TB) | format | PASS | PASS | OK |
| | | _orig: got="1.00 TB" want="1.00 TB"_ | | |
| | | | _asm: got="1.00 TB" want="1.00 TB"_ | |
| PROBE 2 TiB (%.2f TB) | format | PASS | PASS | OK |
| | | _orig: got="2.00 TB" want="2.00 TB"_ | | |
| | | | _asm: got="2.00 TB" want="2.00 TB"_ | |
| buffer too small truncates identically | format | PASS | PASS | OK |
| | | _orig: n=0 got=""_ | | |
| | | | _asm: n=0 got=""_ | |
| IPv4 TCP key: exact 22 bytes | key | PASS | PASS | OK |
| | | _orig: len=22 key=3454c0a8010a3ac9000008080808bb01000092100000_ | | |
| | | | _asm: len=22 key=3454c0a8010a3ac9000008080808bb01000092100000_ | |
| IPv4 UDP key: protocol byte | key | PASS | PASS | OK |
| | | _orig: len=22 key=3455c0a8010a3500000008080808e914000001000000_ | | |
| | | | _asm: len=22 key=3455c0a8010a3500000008080808e914000001000000_ | |
| IPv6 TCP key: exact 46 bytes | key | PASS | PASS | OK |
| | | _orig: len=46 key=365420010db80102030405060708090a0b0c50000000fe800102030405060708090a0b0c0d0ebb01000007000000_ | | |
| | | | _asm: len=46 key=365420010db80102030405060708090a0b0c50000000fe800102030405060708090a0b0c0d0ebb01000007000000_ | |
| IPv4 key length is 22 | key | PASS | PASS | OK |
| | | _orig: len=22_ | | |
| | | | _asm: len=22_ | |
| ASCII string | utf8 | PASS | PASS | OK |
| | | _orig: n=5_ | | |
| | | | _asm: n=5_ | |
| empty string | utf8 | PASS | PASS | OK |
| | | _orig: n=0_ | | |
| | | | _asm: n=0_ | |
| 2-byte character (U+00C0) | utf8 | PASS | PASS | OK |
| | | _orig: n=2 hex=c380_ | | |
| | | | _asm: n=2 hex=c380_ | |
| 3-byte character (U+3042) | utf8 | PASS | PASS | OK |
| | | _orig: n=3 hex=e38182_ | | |
| | | | _asm: n=3 hex=e38182_ | |
| mixed ASCII + 2-byte + 3-byte | utf8 | PASS | PASS | OK |
| | | _orig: n=6_ | | |
| | | | _asm: n=6_ | |
| surrogate pair (U+1F600) encodes as 4 bytes | utf8 | PASS | PASS | OK |
| | | _orig: n=4 hex=f09f9880_ | | |
| | | | _asm: n=4 hex=f09f9880_ | |
| PROBE unpaired surrogate (divergence) | utf8 | PASS | PASS | OK |
| | | _orig: n=3 hex=efbfbd_ | | |
| | | | _asm: n=4 hex=f0908080_ | |
| PROBE truncation mid-character (divergence) | utf8 | PASS | PASS | OK |
| | | _orig: n=2 hex=61c3_ | | |
| | | | _asm: n=1 hex=61_ | |
| 4K ASCII string | utf8 | PASS | PASS | OK |
| | | _orig: n=4096_ | | |
| | | | _asm: n=4096_ | |
| empty input | hex | PASS | PASS | OK |
| single byte exact | hex | PASS | PASS | OK |
| | | _orig: n=64_ | | |
| | | | _asm: n=64_ | |
| full 16-byte line exact | hex | PASS | PASS | OK |
| short final line | hex | PASS | PASS | OK |
| | | _orig: n=147_ | | |
| | | | _asm: n=147_ | |
| gutter mapping | hex | PASS | PASS | OK |
| | | _orig:  | ~....|_ | | |
| | | | _asm:  | ~....|_ | |
| odd width gap | hex | PASS | PASS | OK |
| bytesPerLine clamps to 16 | hex | PASS | PASS | OK |
| 64-wide single line | hex | PASS | PASS | OK |
| | | _orig: n=272_ | | |
| | | | _asm: n=272_ | |
| empty string | utf8 | PASS | PASS | OK |
| | | _orig: n=0_ | | |
| | | | _asm: n=0_ | |
| ASCII fold only A-Z | lower | PASS | PASS | OK |
| already lower unchanged | lower | PASS | PASS | OK |
| fallback around non-ASCII | lower | PASS | PASS | OK |
| | | _orig: n=3_ | | |
| | | | _asm: n=3_ | |
| surrogate halves preserved | lower | PASS | PASS | OK |
| embedded NUL preserved | lower | PASS | PASS | OK |
| 600-char ASCII | lower | PASS | PASS | OK |
| u32 aligned | beread | PASS | PASS | OK |
| u32 unaligned | beread | PASS | PASS | OK |
| u64 aligned | beread | PASS | PASS | OK |
| u64 unaligned | beread | PASS | PASS | OK |
| bytes n=0..8 | beread | PASS | PASS | OK |
| PROBE bytes n=9 (wrap vs refuse) | beread | PASS | PASS | OK |
| | | _orig: found=1 v=0x23456789ABCDEF11_ | | |
| | | | _asm: found=0 v=0x0_ | |
| u16 version | tlsbe | PASS | PASS | OK |
| u16 length unaligned | tlsbe | PASS | PASS | OK |
| u24 handshake len | tlsbe | PASS | PASS | OK |
| u24 0x010203 | tlsbe | PASS | PASS | OK |
| u32 | tlsbe | PASS | PASS | OK |
| record header walk | tlsbe | PASS | PASS | OK |
| server_name -> SNI | tlsext | PASS | PASS | OK |
| ALPN -> ALPN | tlsext | PASS | PASS | OK |
| others skip | tlsext | PASS | PASS | OK |
| strip dots | tlsext | PASS | PASS | OK |
| no strip | tlsext | PASS | PASS | OK |
| interior NUL kept | tlsext | PASS | PASS | OK |
| all-strip to zero | tlsext | PASS | PASS | OK |
| empty | tlsext | PASS | PASS | OK |
| u16 LE | rd | PASS | PASS | OK |
| u16 BE | rd | PASS | PASS | OK |
| u16 unaligned BE | rd | PASS | PASS | OK |
| u32 LE | rd | PASS | PASS | OK |
| u32 BE | rd | PASS | PASS | OK |
| u32 unaligned BE | rd | PASS | PASS | OK |
| ports walk | rd | PASS | PASS | OK |
| empty | tlsext | PASS | PASS | OK |
| single match | join | PASS | PASS | OK |
| | | _orig: n=1_ | | |
| | | | _asm: n=1_ | |
| unknown skipped | join | PASS | PASS | OK |
| non-TCP row skipped | join | PASS | PASS | OK |
| duplicate keys assign in order | join | PASS | PASS | OK |
| | | _orig: n=2_ | | |
| | | | _asm: n=2_ | |
| second-row match | join | PASS | PASS | OK |
| excess sample dropped | join | PASS | PASS | OK |
| empty needle | findlong | PASS | PASS | OK |
| needle longer | findlong | PASS | PASS | OK |
| at start | findlong | PASS | PASS | OK |
| in middle | findlong | PASS | PASS | OK |
| at end | findlong | PASS | PASS | OK |
| absent | findlong | PASS | PASS | OK |
| identical | findlong | PASS | PASS | OK |
| single char hit | findlong | PASS | PASS | OK |
| single char miss | findlong | PASS | PASS | OK |
| 32-char needle present | findlong | PASS | PASS | OK |
| 32-char needle absent | findlong | PASS | PASS | OK |
| repeated prefix | findlong | PASS | PASS | OK |
| 10 fields exact | lowerall | PASS | PASS | OK |
| | | _orig: n=86_ | | |
| | | | _asm: n=86_ | |
| all empty | lowerall | PASS | PASS | OK |
| short count empty | lowerall | PASS | PASS | OK |
| over count joins first 10 | lowerall | PASS | PASS | OK |
| equal | cmpwide | PASS | PASS | OK |
| | | _orig: want=0 got=0_ | | |
| | | | _asm: want=0 got=0_ | |
| both empty | cmpwide | PASS | PASS | OK |
| | | _orig: want=0 got=0_ | | |
| | | | _asm: want=0 got=0_ | |
| empty vs non-empty | cmpwide | PASS | PASS | OK |
| | | _orig: want=-1 got=-1_ | | |
| | | | _asm: want=-1 got=-1_ | |
| prefix shorter | cmpwide | PASS | PASS | OK |
| | | _orig: want=-1 got=-1_ | | |
| | | | _asm: want=-1 got=-1_ | |
| prefix longer | cmpwide | PASS | PASS | OK |
| | | _orig: want=1 got=1_ | | |
| | | | _asm: want=1 got=1_ | |
| diff at 0 | cmpwide | PASS | PASS | OK |
| | | _orig: want=-1 got=-1_ | | |
| | | | _asm: want=-1 got=-1_ | |
| diff at 7 (in-lane) | cmpwide | PASS | PASS | OK |
| | | _orig: want=-1 got=-1_ | | |
| | | | _asm: want=-1 got=-1_ | |
| diff at 8 (lane edge) | cmpwide | PASS | PASS | OK |
| | | _orig: want=-1 got=-1_ | | |
| | | | _asm: want=-1 got=-1_ | |
| diff at 15/16 | cmpwide | PASS | PASS | OK |
| | | _orig: want=-1 got=-1_ | | |
| | | | _asm: want=-1 got=-1_ | |
| high chars | cmpwide | PASS | PASS | OK |
| | | _orig: want=-1 got=-1_ | | |
| | | | _asm: want=-1 got=-1_ | |
| late diff 256 | cmpwide | PASS | PASS | OK |
| | | _orig: want=-1 got=-1_ | | |
| | | | _asm: want=-1 got=-1_ | |
| equal 256 | cmpwide | PASS | PASS | OK |
| | | _orig: want=0 got=0_ | | |
| | | | _asm: want=0 got=0_ | |
| equal keys equal hash | keyhash | PASS | PASS | OK |
| distinct keys distinct hash | keyhash | PASS | PASS | OK |
| equal bytes | keyhash | PASS | PASS | OK |
| length differs | keyhash | PASS | PASS | OK |
| last byte differs | keyhash | PASS | PASS | OK |
| first byte differs | keyhash | PASS | PASS | OK |
| basic rate | bps | PASS | PASS | OK |
| | | _orig: ok=2_ | | |
| | | | _asm: ok=2_ | |
| zero elapsed | bps | PASS | PASS | OK |
| backwards rejected | bps | PASS | PASS | OK |
| flat counter | bps | PASS | PASS | OK |
| sums + uncounted pid | pidsum | PASS | PASS | OK |
| saturates | pidsum | PASS | PASS | OK |
| uncounted row rides sums | pidsum | PASS | PASS | OK |
| unknown | bpscell | PASS | PASS | OK |
| idle | bpscell | PASS | PASS | OK |
| normal | bpscell | PASS | PASS | OK |
| | | _orig: ? 1.5 KB/s  ? 2.9 MB/s_ | | |
| | | | _asm: ? 1.5 KB/s  ? 2.9 MB/s_ | |
| half boundary | bpscell | PASS | PASS | OK |
| | | _orig: ? 1 B/s  ? 0 B/s_ | | |
| | | | _asm: ? 1 B/s  ? 0 B/s_ | |
| truncates | bpscell | PASS | PASS | OK |
| empty | tlsext | PASS | PASS | OK |
| ascii | widen | PASS | PASS | OK |
| 2+3 byte | widen | PASS | PASS | OK |
| overlong | widen | PASS | PASS | OK |
| stray + 4-byte | widen | PASS | PASS | OK |
| | | _orig: n=5_ | | |
| | | | _asm: n=5_ | |
| 40-char ASCII | widen | PASS | PASS | OK |
| mixed | widen | PASS | PASS | OK |
| public 8.8.8.8 | unicast | PASS | PASS | OK |
| | | _orig: allow_ | | |
| | | | _asm: allow_ | |
| public 1.1.1.1 | unicast | PASS | PASS | OK |
| | | _orig: allow_ | | |
| | | | _asm: allow_ | |
| 0/8 | unicast | PASS | PASS | OK |
| | | _orig: deny_ | | |
| | | | _asm: deny_ | |
| 10/8 | unicast | PASS | PASS | OK |
| | | _orig: deny_ | | |
| | | | _asm: deny_ | |
| 127/8 | unicast | PASS | PASS | OK |
| | | _orig: deny_ | | |
| | | | _asm: deny_ | |
| 100.64/10 in | unicast | PASS | PASS | OK |
| | | _orig: deny_ | | |
| | | | _asm: deny_ | |
| 100.127/10 edge in | unicast | PASS | PASS | OK |
| | | _orig: deny_ | | |
| | | | _asm: deny_ | |
| 100.128/10 out | unicast | PASS | PASS | OK |
| | | _orig: allow_ | | |
| | | | _asm: allow_ | |
| 100.63/10 out | unicast | PASS | PASS | OK |
| | | _orig: allow_ | | |
| | | | _asm: allow_ | |
| 169.254/16 | unicast | PASS | PASS | OK |
| | | _orig: deny_ | | |
| | | | _asm: deny_ | |
| 169.253 out | unicast | PASS | PASS | OK |
| | | _orig: allow_ | | |
| | | | _asm: allow_ | |
| 172.16/12 low | unicast | PASS | PASS | OK |
| | | _orig: deny_ | | |
| | | | _asm: deny_ | |
| 172.31/12 high | unicast | PASS | PASS | OK |
| | | _orig: deny_ | | |
| | | | _asm: deny_ | |
| 172.15 out | unicast | PASS | PASS | OK |
| | | _orig: allow_ | | |
| | | | _asm: allow_ | |
| 172.32 out | unicast | PASS | PASS | OK |
| | | _orig: allow_ | | |
| | | | _asm: allow_ | |
| 192.0.0/24 | unicast | PASS | PASS | OK |
| | | _orig: deny_ | | |
| | | | _asm: deny_ | |
| 192.0.2 out (no carve-out) | unicast | PASS | PASS | OK |
| | | _orig: allow_ | | |
| | | | _asm: allow_ | |
| 192.88.99/24 | unicast | PASS | PASS | OK |
| | | _orig: deny_ | | |
| | | | _asm: deny_ | |
| 192.168/16 | unicast | PASS | PASS | OK |
| | | _orig: deny_ | | |
| | | | _asm: deny_ | |
| 198.18/15 low | unicast | PASS | PASS | OK |
| | | _orig: deny_ | | |
| | | | _asm: deny_ | |
| 198.19/15 high | unicast | PASS | PASS | OK |
| | | _orig: deny_ | | |
| | | | _asm: deny_ | |
| 198.20 out | unicast | PASS | PASS | OK |
| | | _orig: allow_ | | |
| | | | _asm: allow_ | |
| 198.51.100/24 | unicast | PASS | PASS | OK |
| | | _orig: deny_ | | |
| | | | _asm: deny_ | |
| 203.0.113/24 | unicast | PASS | PASS | OK |
| | | _orig: deny_ | | |
| | | | _asm: deny_ | |
| 224/4 | unicast | PASS | PASS | OK |
| | | _orig: deny_ | | |
| | | | _asm: deny_ | |
| 240/4 | unicast | PASS | PASS | OK |
| | | _orig: deny_ | | |
| | | | _asm: deny_ | |
| broadcast | unicast | PASS | PASS | OK |
| | | _orig: deny_ | | |
| | | | _asm: deny_ | |
| 186 ordinary | unicast | PASS | PASS | OK |
| | | _orig: allow_ | | |
| | | | _asm: allow_ | |
| 204 ordinary | unicast | PASS | PASS | OK |
| | | _orig: allow_ | | |
| | | | _asm: allow_ | |
| 176 ordinary | unicast | PASS | PASS | OK |
| | | _orig: allow_ | | |
| | | | _asm: allow_ | |
| exhaustive V4 (16.7M addrs) | unicast | PASS | PASS | OK |
| | | _orig: h=0xE32990961491F3CF_ | | |
| | | | _asm: h=0xE32990961491F3CF_ | |
| V6 sweep (168 addrs) | unicast | PASS | PASS | OK |
| | | _orig: 011111111111111111111111011111111111111111111111111100011111111111111111000000000000000000000000000000000000000000000000111111111111111100000000000000000000000000000000_ | | |
| | | | _asm: 011111111111111111111111011111111111111111111111111100011111111111111111000000000000000000000000000000000000000000000000111111111111111100000000000000000000000000000000_ | |
| V6 ::1 allowed | unicast | PASS | PASS | OK |
| V6 :: denied | unicast | PASS | PASS | OK |
| V6 2001:db8 denied | unicast | PASS | PASS | OK |
| V6 public allowed | unicast | PASS | PASS | OK |
| V6 v4-mapped allowed | unicast | PASS | PASS | OK |
| empty | tlsext | PASS | PASS | OK |
| single | pair | PASS | PASS | OK |
| newcomer | pair | PASS | PASS | OK |
| duplicates FIFO | pair | PASS | PASS | OK |
| interleaved | pair | PASS | PASS | OK |
| | | _orig: n=3_ | | |
| | | | _asm: n=3_ | |
| ghost ignored | pair | PASS | PASS | OK |
| hit middle | countedkey | PASS | PASS | OK |
| hit first | countedkey | PASS | PASS | OK |
| same-len miss | countedkey | PASS | PASS | OK |
| long miss | countedkey | PASS | PASS | OK |
| short miss | countedkey | PASS | PASS | OK |
| null | countedkey | PASS | PASS | OK |
| equal | cmpwide | PASS | PASS | OK |
| | | _orig: want=0 got=0_ | | |
| | | | _asm: want=0 got=0_ | |
| port differs | flow | PASS | PASS | OK |
| addr late differs | flow | PASS | PASS | OK |
| addr early differs | flow | PASS | PASS | OK |
| cmp equal | flow | PASS | PASS | OK |
| cmp high-vs-low | flow | PASS | PASS | OK |
| cmp reverse | flow | PASS | PASS | OK |
| empty | tlsext | PASS | PASS | OK |
| pid+type filter | handles | PASS | PASS | OK |
| types unknown skips check | handles | PASS | PASS | OK |
| skip pair | handles | PASS | PASS | OK |
| wide pid skipped | handles | PASS | PASS | OK |
| capped writes | handles | PASS | PASS | OK |
| tcp send | etw | PASS | PASS | OK |
| tcp recv | etw | PASS | PASS | OK |
| tcp send26 | etw | PASS | PASS | OK |
| udp recv27 | etw | PASS | PASS | OK |
| foreign guid | etw | PASS | PASS | OK |
| opcode 18 excluded | etw | PASS | PASS | OK |
| zero opcode+id | etw | PASS | PASS | OK |
| id fallback | etw | PASS | PASS | OK |
| null guid | etw | PASS | PASS | OK |
| payload | etw | PASS | PASS | OK |
| idle pid rejected | etw | PASS | PASS | OK |
| short payload | etw | PASS | PASS | OK |
| empty | tlsext | PASS | PASS | OK |
| plain | json | PASS | PASS | OK |
| quotes | json | PASS | PASS | OK |
| backslash | json | PASS | PASS | OK |
| newline | json | PASS | PASS | OK |
| cr-tab | json | PASS | PASS | OK |
| control 0x01 | json | PASS | PASS | OK |
| control 0x1F | json | PASS | PASS | OK |
| DEL passes | json | PASS | PASS | OK |
| utf8 passthrough | json | PASS | PASS | OK |
| mixed row | json | PASS | PASS | OK |
| empty | tlsext | PASS | PASS | OK |
| plain | json | PASS | PASS | OK |
| comma | csv | PASS | PASS | OK |
| quote | csv | PASS | PASS | OK |
| crlf | csv | PASS | PASS | OK |
| all specials | csv | PASS | PASS | OK |
| comma past the first block | csv | PASS | PASS | OK |
| | | _orig: "RpcEptMapper, RpcSs"_ | | |
| | | | _asm: "RpcEptMapper, RpcSs"_ | |
| 16-byte comma field | csv | PASS | PASS | OK |
| 17-byte comma field | csv | PASS | PASS | OK |
| 32-byte comma field | csv | PASS | PASS | OK |
| 33-byte comma field | csv | PASS | PASS | OK |
| 40-byte plain field | csv | PASS | PASS | OK |
| quote in the second block | csv | PASS | PASS | OK |
| | | _orig: "0123456789abcdef""ghij"_ | | |
| | | | _asm: "0123456789abcdef""ghij"_ | |
| quotes across blocks | csv | PASS | PASS | OK |
| trailing comma after blocks | csv | PASS | PASS | OK |
| crlf after the first block | csv | PASS | PASS | OK |
| differential sweep (len x pos x special) | csv | PASS | PASS | OK |
| port 0 | intfmt | PASS | PASS | OK |
| port 80 | intfmt | PASS | PASS | OK |
| port 443 | intfmt | PASS | PASS | OK |
| port 8080 | intfmt | PASS | PASS | OK |
| port max | intfmt | PASS | PASS | OK |
| uint max | intfmt | PASS | PASS | OK |
| port truncates | intfmt | PASS | PASS | OK |
| u64 0 | intfmt | PASS | PASS | OK |
| u64 7 | intfmt | PASS | PASS | OK |
| u64 9digits | intfmt | PASS | PASS | OK |
| u64 1e9 | intfmt | PASS | PASS | OK |
| u64 max | intfmt | PASS | PASS | OK |
| u64 20digits | intfmt | PASS | PASS | OK |
| dur 0s | intfmt | PASS | PASS | OK |
| dur 59s | intfmt | PASS | PASS | OK |
| dur 60s | intfmt | PASS | PASS | OK |
| dur 61s | intfmt | PASS | PASS | OK |
| dur 3599s | intfmt | PASS | PASS | OK |
| dur 3600s | intfmt | PASS | PASS | OK |
| dur 3661s | intfmt | PASS | PASS | OK |
| dur 86399s | intfmt | PASS | PASS | OK |
| dur 86400s | intfmt | PASS | PASS | OK |
| dur 90061s | intfmt | PASS | PASS | OK |
| v4 private | ipfmt | PASS | PASS | OK |
| v4 zeros | ipfmt | PASS | PASS | OK |
| v4 max | ipfmt | PASS | PASS | OK |
| v4 dns | ipfmt | PASS | PASS | OK |
| v6 unspecified | ipfmt | PASS | PASS | OK |
| v6 loopback | ipfmt | PASS | PASS | OK |
| v6 full | ipfmt | PASS | PASS | OK |
| v6 single zero kept | ipfmt | PASS | PASS | OK |
| v6 trailing run | ipfmt | PASS | PASS | OK |
| v6 tie picks first | ipfmt | PASS | PASS | OK |
| v6-mapped | ipfmt | PASS | PASS | OK |
| CpWidth matches at every range boundary | width | PASS | PASS | OK |
| CpWidth exhaustive sweep (169692 code points) | width | PASS | PASS | OK |
| NextCp ascii | width | PASS | PASS | OK |
| NextCp 2-byte | width | PASS | PASS | OK |
| NextCp 3-byte | width | PASS | PASS | OK |
| NextCp 4-byte | width | PASS | PASS | OK |
| NextCp truncated lead is a 1-byte stray | width | PASS | PASS | OK |
| NextCp malformed continuation resyncs | width | PASS | PASS | OK |
| NextCp matches the reference at every byte | width | PASS | PASS | OK |
| empty is width 0 | width | PASS | PASS | OK |
| ascii width == length | width | PASS | PASS | OK |
| long ascii (bulk path) | width | PASS | PASS | OK |
| ascii across 16-byte blocks | width | PASS | PASS | OK |
| 2-byte chars | width | PASS | PASS | OK |
| 4-byte emoji is width 2 | width | PASS | PASS | OK |
| combining accent is width 0 | width | PASS | PASS | OK |
| cjk is width 2 | width | PASS | PASS | OK |
| mixed ascii+cjk 600 chars | width | PASS | PASS | OK |
| | | _orig: got=600_ | | |
| | | | _asm: got=600_ | |
| cjk straddling a block boundary | width | PASS | PASS | OK |
| | | _orig: got=202_ | | |
| | | | _asm: got=202_ | |
| codepoint split across a block boundary | width | PASS | PASS | OK |
| | | _orig: got=23_ | | |
| | | | _asm: got=23_ | |
| truncate fits -> unchanged | width | PASS | PASS | OK |
| truncate exact fit -> unchanged | width | PASS | PASS | OK |
| truncate one short adds the marker | width | PASS | PASS | OK |
| truncate width 6 | width | PASS | PASS | OK |
| truncate width 1 | width | PASS | PASS | OK |
| truncate width 0 is empty | width | PASS | PASS | OK |
| truncate empty stays empty | width | PASS | PASS | OK |
| truncate multi-byte is not split | width | PASS | PASS | OK |
| truncate sweep over every width | width | PASS | PASS | OK |
| PayloadSize code 0 | mmdb | PASS | PASS | OK |
| | | _orig: ok=1 size=0 pos=16_ | | |
| | | | _asm: ok=1 size=0 pos=16_ | |
| PayloadSize code 1 | mmdb | PASS | PASS | OK |
| | | _orig: ok=1 size=1 pos=16_ | | |
| | | | _asm: ok=1 size=1 pos=16_ | |
| PayloadSize code 2 | mmdb | PASS | PASS | OK |
| | | _orig: ok=1 size=2 pos=16_ | | |
| | | | _asm: ok=1 size=2 pos=16_ | |
| PayloadSize code 3 | mmdb | PASS | PASS | OK |
| | | _orig: ok=1 size=3 pos=16_ | | |
| | | | _asm: ok=1 size=3 pos=16_ | |
| PayloadSize code 4 | mmdb | PASS | PASS | OK |
| | | _orig: ok=1 size=4 pos=16_ | | |
| | | | _asm: ok=1 size=4 pos=16_ | |
| PayloadSize code 5 | mmdb | PASS | PASS | OK |
| | | _orig: ok=1 size=5 pos=16_ | | |
| | | | _asm: ok=1 size=5 pos=16_ | |
| PayloadSize code 6 | mmdb | PASS | PASS | OK |
| | | _orig: ok=1 size=6 pos=16_ | | |
| | | | _asm: ok=1 size=6 pos=16_ | |
| PayloadSize code 7 | mmdb | PASS | PASS | OK |
| | | _orig: ok=1 size=7 pos=16_ | | |
| | | | _asm: ok=1 size=7 pos=16_ | |
| PayloadSize code 8 | mmdb | PASS | PASS | OK |
| | | _orig: ok=1 size=8 pos=16_ | | |
| | | | _asm: ok=1 size=8 pos=16_ | |
| PayloadSize code 9 | mmdb | PASS | PASS | OK |
| | | _orig: ok=1 size=9 pos=16_ | | |
| | | | _asm: ok=1 size=9 pos=16_ | |
| PayloadSize code 10 | mmdb | PASS | PASS | OK |
| | | _orig: ok=1 size=10 pos=16_ | | |
| | | | _asm: ok=1 size=10 pos=16_ | |
| PayloadSize code 11 | mmdb | PASS | PASS | OK |
| | | _orig: ok=1 size=11 pos=16_ | | |
| | | | _asm: ok=1 size=11 pos=16_ | |
| PayloadSize code 12 | mmdb | PASS | PASS | OK |
| | | _orig: ok=1 size=12 pos=16_ | | |
| | | | _asm: ok=1 size=12 pos=16_ | |
| PayloadSize code 13 | mmdb | PASS | PASS | OK |
| | | _orig: ok=1 size=13 pos=16_ | | |
| | | | _asm: ok=1 size=13 pos=16_ | |
| PayloadSize code 14 | mmdb | PASS | PASS | OK |
| | | _orig: ok=1 size=14 pos=16_ | | |
| | | | _asm: ok=1 size=14 pos=16_ | |
| PayloadSize code 15 | mmdb | PASS | PASS | OK |
| | | _orig: ok=1 size=15 pos=16_ | | |
| | | | _asm: ok=1 size=15 pos=16_ | |
| PayloadSize code 16 | mmdb | PASS | PASS | OK |
| | | _orig: ok=1 size=16 pos=16_ | | |
| | | | _asm: ok=1 size=16 pos=16_ | |
| PayloadSize code 17 | mmdb | PASS | PASS | OK |
| | | _orig: ok=1 size=17 pos=16_ | | |
| | | | _asm: ok=1 size=17 pos=16_ | |
| PayloadSize code 18 | mmdb | PASS | PASS | OK |
| | | _orig: ok=1 size=18 pos=16_ | | |
| | | | _asm: ok=1 size=18 pos=16_ | |
| PayloadSize code 19 | mmdb | PASS | PASS | OK |
| | | _orig: ok=1 size=19 pos=16_ | | |
| | | | _asm: ok=1 size=19 pos=16_ | |
| PayloadSize code 20 | mmdb | PASS | PASS | OK |
| | | _orig: ok=1 size=20 pos=16_ | | |
| | | | _asm: ok=1 size=20 pos=16_ | |
| PayloadSize code 21 | mmdb | PASS | PASS | OK |
| | | _orig: ok=1 size=21 pos=16_ | | |
| | | | _asm: ok=1 size=21 pos=16_ | |
| PayloadSize code 22 | mmdb | PASS | PASS | OK |
| | | _orig: ok=1 size=22 pos=16_ | | |
| | | | _asm: ok=1 size=22 pos=16_ | |
| PayloadSize code 23 | mmdb | PASS | PASS | OK |
| | | _orig: ok=1 size=23 pos=16_ | | |
| | | | _asm: ok=1 size=23 pos=16_ | |
| PayloadSize code 24 | mmdb | PASS | PASS | OK |
| | | _orig: ok=1 size=24 pos=16_ | | |
| | | | _asm: ok=1 size=24 pos=16_ | |
| PayloadSize code 25 | mmdb | PASS | PASS | OK |
| | | _orig: ok=1 size=25 pos=16_ | | |
| | | | _asm: ok=1 size=25 pos=16_ | |
| PayloadSize code 26 | mmdb | PASS | PASS | OK |
| | | _orig: ok=1 size=26 pos=16_ | | |
| | | | _asm: ok=1 size=26 pos=16_ | |
| PayloadSize code 27 | mmdb | PASS | PASS | OK |
| | | _orig: ok=1 size=27 pos=16_ | | |
| | | | _asm: ok=1 size=27 pos=16_ | |
| PayloadSize code 28 | mmdb | PASS | PASS | OK |
| | | _orig: ok=1 size=28 pos=16_ | | |
| | | | _asm: ok=1 size=28 pos=16_ | |
| PayloadSize code 29 | mmdb | PASS | PASS | OK |
| | | _orig: ok=1 size=45 pos=17_ | | |
| | | | _asm: ok=1 size=45 pos=17_ | |
| PayloadSize code 30 | mmdb | PASS | PASS | OK |
| | | _orig: ok=1 size=4398 pos=18_ | | |
| | | | _asm: ok=1 size=4398 pos=18_ | |
| PayloadSize code 31 | mmdb | PASS | PASS | OK |
| | | _orig: ok=1 size=1118767 pos=19_ | | |
| | | | _asm: ok=1 size=1118767 pos=19_ | |
| PayloadSize code 29 last byte | mmdb | PASS | PASS | OK |
| | | _orig: ok=1_ | | |
| | | | _asm: ok=1_ | |
| PayloadSize code 29 past the end | mmdb | PASS | PASS | OK |
| PayloadSize code 30 needs 2 bytes | mmdb | PASS | PASS | OK |
| PayloadSize code 31 needs 3 bytes | mmdb | PASS | PASS | OK |
| PayloadSize code 29 adds its base | mmdb | PASS | PASS | OK |
| | | _orig: size=284_ | | |
| | | | _asm: size=284_ | |
| PayloadSize code 30 max | mmdb | PASS | PASS | OK |
| | | _orig: size=65820_ | | |
| | | | _asm: size=65820_ | |
| PayloadSize code 31 max | mmdb | PASS | PASS | OK |
| | | _orig: size=16843036_ | | |
| | | | _asm: size=16843036_ | |
| ReadPointer size 0 | mmdb | PASS | PASS | OK |
| | | _orig: ok=1 out=1824_ | | |
| | | | _asm: ok=1 out=1824_ | |
| ReadPointer size 1 | mmdb | PASS | PASS | OK |
| | | _orig: ok=1 out=469025_ | | |
| | | | _asm: ok=1 out=469025_ | |
| ReadPointer size 2 | mmdb | PASS | PASS | OK |
| | | _orig: ok=1 out=120072482_ | | |
| | | | _asm: ok=1 out=120072482_ | |
| ReadPointer size 3 | mmdb | PASS | PASS | OK |
| | | _orig: ok=1 out=539042339_ | | |
| | | | _asm: ok=1 out=539042339_ | |
| ReadPointer size 3 ignores the low bits | mmdb | PASS | PASS | OK |
| | | _orig: got=539042339_ | | |
| | | | _asm: got=539042339_ | |
| ReadPointer size 0 packs the control bits | mmdb | PASS | PASS | OK |
| | | _orig: got=1832_ | | |
| | | | _asm: got=1832_ | |
| ReadPointer size 1 adds 2048 | mmdb | PASS | PASS | OK |
| | | _orig: got=79921_ | | |
| | | | _asm: got=79921_ | |
| ReadPointer size 2 adds 526336 | mmdb | PASS | PASS | OK |
| | | _orig: got=20461874_ | | |
| | | | _asm: got=20461874_ | |
| ReadPointer refuses a partial payload | mmdb | PASS | PASS | OK |
| | | _orig: got=1_ | | |
| | | | _asm: got=1_ | |
| ReadPointer boundary sweep (size x pos x psz) | mmdb | PASS | PASS | OK |
| PayloadSize boundary sweep (size x pos x code) | mmdb | PASS | PASS | OK |
| plain ethernet: off 12 -> 14 | vlan | PASS | PASS | OK |
| single 802.1Q: off 12 -> 18 | vlan | PASS | PASS | OK |
| single 802.1ad: off 12 -> 18 | vlan | PASS | PASS | OK |
| QinQ: off 12 -> 22 | vlan | PASS | PASS | OK |
| truncated tag chain stops at the bound | vlan | PASS | PASS | OK |
| too short for a tag: off 12 -> 14 | vlan | PASS | PASS | OK |
| 0x0800 is not a tag | vlan | PASS | PASS | OK |
| signature at candidate 12 | flowprobe | PASS | PASS | OK |
| signature at candidate 38 | flowprobe | PASS | PASS | OK |
| candidate 38 needs 5 bytes (43 is enough) | flowprobe | PASS | PASS | OK |
| capLen 42 refuses candidate 38 | flowprobe | PASS | PASS | OK |
| version 6 is not a flow record | flowprobe | PASS | PASS | OK |
| 0x0801 is not a flow record | flowprobe | PASS | PASS | OK |
| odd offset 13 is not a candidate | flowprobe | PASS | PASS | OK |
| differential sweep (capLen x offset x version) | flowprobe | PASS | PASS | OK |
| no signature -> false | flowprobe | PASS | PASS | OK |
| empty frame | flowprobe | PASS | PASS | OK |
| 3-byte frame | flowprobe | PASS | PASS | OK |
| contiguous: matches the reference | render | PASS | PASS | OK |
| contiguous: no gap, no missing | render | PASS | PASS | OK |
| | | _orig: gap=0 missing=0_ | | |
| | | | _asm: gap=0 missing=0_ | |
| contiguous: bytes concatenated in seq order | render | PASS | PASS | OK |
| duplicates + truncated are carried | render | PASS | PASS | OK |
| sort puts the stream in seq order | render | PASS | PASS | OK |
| gap: hole recorded, hasGap set | render | PASS | PASS | OK |
| | | _orig: missing=95_ | | |
| | | | _asm: missing=95_ | |
| gap: segments still concatenated | render | PASS | PASS | OK |
| touching segments are contiguous | render | PASS | PASS | OK |
| empty: no segments, flags carried | render | PASS | PASS | OK |
| zero-length segments are skipped | render | PASS | PASS | OK |
| | | _orig: bytes=8_ | | |
| | | | _asm: bytes=8_ | |
| differential sweep across the sort threshold | render | PASS | PASS | OK |
| empty | tlsext | PASS | PASS | OK |
| single | pair | PASS | PASS | OK |
| all duplicates collapse | pids | PASS | PASS | OK |
| first-appearance order preserved | pids | PASS | PASS | OK |
| | | _orig: got 3 entries_ | | |
| | | | _asm: got 3 entries_ | |
| pid 0 and 0xFFFFFFFF are real keys | pids | PASS | PASS | OK |
| 2000 sockets, 12 processes -> 12 distinct | pids | PASS | PASS | OK |
| | | _orig: got 12_ | | |
| | | | _asm: got 12_ | |
| differential sweep (length x multiplicity) | pids | PASS | PASS | OK |
| group: pids in first-appearance order | pids | PASS | PASS | OK |
| | | _orig: 3_ | | |
| | | | _asm: 3_ | |
| group: row indices per pid | pids | PASS | PASS | OK |
| group: matches the reference exactly | pids | PASS | PASS | OK |
| group: differential sweep | pids | PASS | PASS | OK |

### No Correctness Divergences

Both variants pass all tests identically. The optimized implementations are functionally equivalent to the originals.

---

## Performance Comparison

| Function | Workload | Original (ns/op) | Optimized (ns/op) | Speedup | Winner |
|----------|----------|-------------------|--------------------|---------|--------|
| GeoIpWalk | tree_256_24bit | 3.574 | 5.102 | 0.70x | original (1.43x slower) |
| GeoIpWalk | tree_65k_24bit | 10.405 | 10.224 | 1.02x | **optimized** (1.02x) |
| GeoIpWalk | tree_1m_24bit | 8.757 | 8.740 | 1.00x | **optimized** (1.00x) |
| GeoIpWalk | tree_65k_28bit | 3.591 | 5.262 | 0.68x | original (1.47x slower) |
| GeoIpWalk | tree_65k_32bit | 3.590 | 5.117 | 0.70x | original (1.43x slower) |
| GeoIpWalk | tree_65k_ipv6 | 6.972 | 7.446 | 0.94x | original (1.07x slower) |
| GeoIpWalkShift | tree_256_24bit | 3.612 | 5.173 | 0.70x | original (1.43x slower) |
| GeoIpWalkShift | tree_65k_24bit | 5.684 | 6.171 | 0.92x | original (1.09x slower) |
| GeoIpWalkShift | tree_65k_28bit | 6.374 | 7.171 | 0.89x | original (1.13x slower) |
| GeoIpWalkShift | tree_65k_32bit | 5.218 | 6.806 | 0.77x | original (1.30x slower) |
| SkipVlan | vlan_plain | 1.406 | 1.223 | 1.15x | **optimized** (1.15x) |
| SkipVlan | vlan_qinq | 2.115 | 1.743 | 1.21x | **optimized** (1.21x) |
| FlowProbe | flowprobe_hit | 1.464 | 1.623 | 0.90x | original (1.11x slower) |
| FlowProbe | flowprobe_miss | 4.997 | 3.453 | 1.45x | **optimized** (1.45x) |
| FlowProbe | flowprobe_short | 3.011 | 1.709 | 1.76x | **optimized** (1.76x) |
| ReasmRender | render_64_segs | 970.280 | 1024.215 | 0.95x | original (1.06x slower) |
| ReasmRender | render_96_segs | 1507.512 | 1583.373 | 0.95x | original (1.05x slower) |
| ReasmRender | render_300_segs | 5180.511 | 5371.009 | 0.96x | original (1.04x slower) |
| ReasmRender | render_4096_segs | 431090.137 | 430101.270 | 1.00x | **optimized** (1.00x) |
| ReasmRender | render_1024_contig | 17468.418 | 17384.414 | 1.00x | **optimized** (1.00x) |
| DistinctPids | distinctpids_2000x12 | 5250.586 | 1502.524 | 3.49x | **optimized** (3.49x) |
| GroupByPid | distinctpids_2000x12 | 13087.549 | 11478.097 | 1.14x | **optimized** (1.14x) |
| DistinctPids | distinctpids_200x200 | 4425.656 | 311.363 | 14.21x | **optimized** (14.21x) |
| GroupByPid | distinctpids_200x200 | 13810.400 | 8986.784 | 1.54x | **optimized** (1.54x) |
| DistinctPids | distinctpids_20kx400 | 70451.904 | 19372.090 | 3.64x | **optimized** (3.64x) |
| GroupByPid | distinctpids_20kx400 | 355971.973 | 335654.395 | 1.06x | **optimized** (1.06x) |
| ReasmOverlap | reasm_4 | 2.964 | 4.014 | 0.74x | original (1.35x slower) |
| ReasmOverlap | reasm_64 | 10.435 | 12.384 | 0.84x | original (1.19x slower) |
| ReasmOverlap | reasm_1024 | 115.120 | 131.139 | 0.88x | original (1.14x slower) |
| ReasmOverlap | reasm_8192 | 1183.807 | 1168.521 | 1.01x | **optimized** (1.01x) |
| ReasmOverlap | reasm_64_dup | 2.534 | 3.378 | 0.75x | original (1.33x slower) |
| HasLowerSubstring | sub_16_present | 9.954 | 8.643 | 1.15x | **optimized** (1.15x) |
| HasLowerSubstring | sub_16_absent | 7.096 | 2.770 | 2.56x | **optimized** (2.56x) |
| HasLowerSubstring | sub_256_absent | 47.730 | 44.807 | 1.07x | **optimized** (1.07x) |
| HasLowerSubstring | sub_4k_absent | 684.475 | 695.525 | 0.98x | original (1.02x slower) |
| HasLowerSubstring | sub_4k_present | 965.606 | 892.027 | 1.08x | **optimized** (1.08x) |
| HasLowerSubstring | sub_64k_absent | 12475.600 | 17673.203 | 0.71x | original (1.42x slower) |
| ParseTcpHeader | tcp_20 | 3.380 | 6.563 | 0.52x | original (1.94x slower) |
| ParseTcpHeader | tcp_52_payload | 3.382 | 6.554 | 0.52x | original (1.94x slower) |
| ParseTcpHeader | tcp_12_invalid | 1.710 | 5.684 | 0.30x | original (3.32x slower) |
| FormatBytes | fmt_small | 105.949 | 88.180 | 1.20x | **optimized** (1.20x) |
| FormatBytes | fmt_mb | 181.820 | 183.950 | 0.99x | original (1.01x slower) |
| FormatBytes | fmt_gb | 176.058 | 180.171 | 0.98x | original (1.02x slower) |
| FormatBytes | fmt_tb | 176.657 | 182.768 | 0.97x | original (1.03x slower) |
| KeyOf | key_v4 | 2.780 | 3.429 | 0.81x | original (1.23x slower) |
| KeyOf | key_v6 | 2.768 | 3.614 | 0.77x | original (1.31x slower) |
| WideToUtf8 | utf8_ascii | 29.025 | 6.405 | 4.53x | **optimized** (4.53x) |
| WideToUtf8 | utf8_mixed | 244.941 | 118.278 | 2.07x | **optimized** (2.07x) |
| WideToUtf8 | utf8_emoji | 231.698 | 105.018 | 2.21x | **optimized** (2.21x) |
| WideToUtf8 | utf8_4k | 872.693 | 269.500 | 3.24x | **optimized** (3.24x) |
| GeoRead | be_u32_unaligned | 1.284 | 1.327 | 0.97x | original (1.03x slower) |
| GeoRead | be_u64_unaligned | 1.459 | 1.344 | 1.09x | **optimized** (1.09x) |
| GeoRead | be_bytes_14 | 1.761 | 2.052 | 0.86x | original (1.17x slower) |
| GeoRead | be_bytes_58 | 2.130 | 1.405 | 1.52x | **optimized** (1.52x) |
| TlsBe | tls_rec_hdr | 1.737 | 1.732 | 1.00x | **optimized** (1.00x) |
| TlsBe | tls_u24 | 1.283 | 1.303 | 0.98x | original (1.02x slower) |
| FindSubstringLong | findlong_8 | 688.579 | 1610.585 | 0.43x | original (2.34x slower) |
| FindSubstringLong | findlong_16 | 766.217 | 920.484 | 0.83x | original (1.20x slower) |
| FindSubstringLong | findlong_32_absent | 676.073 | 391.353 | 1.73x | **optimized** (1.73x) |
| FindSubstringLong | findlong_32_present | 750.867 | 471.607 | 1.59x | **optimized** (1.59x) |
| BuildLowerAll | lowerall_10 | 274.478 | 60.158 | 4.56x | **optimized** (4.56x) |
| CompareWide | cmpwide_eq_256 | 16.040 | 17.079 | 0.94x | original (1.06x slower) |
| CompareWide | cmpwide_late_256 | 18.315 | 18.468 | 0.99x | original (1.01x slower) |
| CompareWide | cmpwide_short | 2.549 | 2.344 | 1.09x | **optimized** (1.09x) |
| ConnKey | keyhash_22 | 7.241 | 2.453 | 2.95x | **optimized** (2.95x) |
| ConnKey | keyhash_46 | 19.554 | 3.264 | 5.99x | **optimized** (5.99x) |
| ConnKey | keyeq_46 | 3.862 | 3.981 | 0.97x | original (1.03x slower) |
| ConnKey | keyeq_46_diff | 3.947 | 4.115 | 0.96x | original (1.04x slower) |
| ComputeBps | bps_1k | 853.053 | 516.449 | 1.65x | **optimized** (1.65x) |
| SumPidTraffic | pidsum_500_50 | 13456.494 | 3705.438 | 3.63x | **optimized** (3.63x) |
| SumPidTraffic | pidsum_5k_700 | 882065.137 | 207956.152 | 4.24x | **optimized** (4.24x) |
| FormatBpsCell | bpscell_hot | 398.085 | 332.427 | 1.20x | **optimized** (1.20x) |
| FormatBpsCell | bpscell_idle | 2.110 | 2.551 | 0.83x | original (1.21x slower) |
| WidenUtf8 | widen_org_34 | 54.560 | 47.321 | 1.15x | **optimized** (1.15x) |
| WidenUtf8 | widen_iso | 4.414 | 4.279 | 1.03x | **optimized** (1.03x) |
| WidenUtf8 | widen_mixed_15 | 36.425 | 35.903 | 1.01x | **optimized** (1.01x) |
| IsGlobalV4 | unicast_v4_mixed | 1.976 | 1.593 | 1.24x | **optimized** (1.24x) |
| IsGlobalV6 | unicast_v6_mixed | 2.522 | 2.133 | 1.18x | **optimized** (1.18x) |
| PairSnapshot | pair_500 | 35699.837 | 34534.245 | 1.03x | **optimized** (1.03x) |
| PairSnapshot | pair_5k | 507999.219 | 576830.469 | 0.88x | original (1.14x slower) |
| PairSnapshot | pair_mdns | 5342.181 | 4445.296 | 1.20x | **optimized** (1.20x) |
| FindCountedKey | findkey_short_hit | 3.164 | 3.397 | 0.93x | original (1.07x slower) |
| FindCountedKey | findkey_short_miss | 2.535 | 2.762 | 0.92x | original (1.09x slower) |
| FindCountedKey | findkey_long_hit | 2.774 | 2.974 | 0.93x | original (1.07x slower) |
| FindCountedKey | findkey_64_hit | 141.932 | 149.639 | 0.95x | original (1.05x slower) |
| FindCountedKey | findkey_64_miss | 141.883 | 148.560 | 0.96x | original (1.05x slower) |
| FlowKeyEqual | flowkey_hit | 1.386 | 1.284 | 1.08x | **optimized** (1.08x) |
| FlowKeyEqual | flowkey_miss | 1.553 | 1.354 | 1.15x | **optimized** (1.15x) |
| CmpFlowAddr | cmpaddr_late | 1.811 | 1.383 | 1.31x | **optimized** (1.31x) |
| FilterHandles | handles_100k | 406293.164 | 760045.410 | 0.53x | original (1.87x slower) |
| ClassifyEvent | etw_mixed | 1.635 | 1.816 | 0.90x | original (1.11x slower) |
| ParseEventPayload | etw_payload | 1.512 | 1.268 | 1.19x | **optimized** (1.19x) |
| JsonEscape | json_proc | 60.633 | 62.604 | 0.97x | original (1.03x slower) |
| JsonEscape | json_row | 193.479 | 102.901 | 1.88x | **optimized** (1.88x) |
| JsonEscape | json_64_quotes | 170.835 | 90.906 | 1.88x | **optimized** (1.88x) |
| CsvEscape | csv_plain_64 | 35.707 | 35.987 | 0.99x | original (1.01x slower) |
| CsvEscape | csv_quoted | 69.555 | 45.122 | 1.54x | **optimized** (1.54x) |
| FormatPort | port_443 | 33.210 | 5.269 | 6.30x | **optimized** (6.30x) |
| FormatPort | port_65535 | 34.765 | 8.496 | 4.09x | **optimized** (4.09x) |
| FormatU64Dec | u64_20digit | 70.032 | 13.097 | 5.35x | **optimized** (5.35x) |
| FormatDuration | dur_hms | 65.971 | 8.516 | 7.75x | **optimized** (7.75x) |
| FormatIpv4 | ipv4 | 227.579 | 28.832 | 7.89x | **optimized** (7.89x) |
| FormatIpv6 | ipv6_full | 928.071 | 42.858 | 21.65x | **optimized** (21.65x) |
| FormatIpv6 | ipv6_zip | 162.728 | 12.139 | 13.41x | **optimized** (13.41x) |
| CpWidth | cp_boundaries | 2.439 | 2.005 | 1.22x | **optimized** (1.22x) |
| CpWidth | cp_random_sweep | 6.248 | 6.781 | 0.92x | original (1.09x slower) |
| DisplayWidth | width_ascii_47 | 42.510 | 4.866 | 8.74x | **optimized** (8.74x) |
| DisplayWidth | width_mixed_30 | 61.003 | 28.412 | 2.15x | **optimized** (2.15x) |
| DisplayWidth | width_ascii_256 | 175.223 | 6.248 | 28.04x | **optimized** (28.04x) |
| DisplayWidth | width_mixed_320 | 478.378 | 270.276 | 1.77x | **optimized** (1.77x) |
| TruncateToWidth | trunc_fits | 65.371 | 32.335 | 2.02x | **optimized** (2.02x) |
| TruncateToWidth | trunc_cut | 94.712 | 33.647 | 2.81x | **optimized** (2.81x) |
| TruncateToWidth | trunc_wide_cut | 129.780 | 53.780 | 2.41x | **optimized** (2.41x) |
| PayloadSize | size_code_mix | 2.141 | 2.241 | 0.96x | original (1.05x slower) |
| PayloadSize | size_inline_only | 1.688 | 1.604 | 1.05x | **optimized** (1.05x) |
| ReadPointer | ptr_size_mix | 2.299 | 2.317 | 0.99x | original (1.01x slower) |
| TlsExt | ext_classify | 1.825 | 1.465 | 1.25x | **optimized** (1.25x) |
| TlsExt | sni_strip_all | 36.276 | 2.116 | 17.14x | **optimized** (17.14x) |
| TlsExt | sni_strip_tail | 3.463 | 4.234 | 0.82x | original (1.22x slower) |
| Rd | rd16_mixed | 1.265 | 1.326 | 0.95x | original (1.05x slower) |
| Rd | rd32_mixed | 1.326 | 1.287 | 1.03x | **optimized** (1.03x) |
| JoinSamples | join_small | 123.158 | 1731.084 | 0.07x | original (14.06x slower) |
| JoinSamples | join_500_300 | 80117.383 | 57071.875 | 1.40x | **optimized** (1.40x) |
| JoinSamples | join_2k_2k | 2191956.445 | 266275.684 | 8.23x | **optimized** (8.23x) |
| JoinSamples | join_mdns | 75750.684 | 29334.342 | 2.58x | **optimized** (2.58x) |
| FormatStreamHex | hex_1460_16 | 105308.984 | 7066.675 | 14.90x | **optimized** (14.90x) |
| FormatStreamHex | hex_64k_64 | 4347238.574 | 158260.645 | 27.47x | **optimized** (27.47x) |
| FormatStreamHex | hex_40_16 | 3130.023 | 288.518 | 10.85x | **optimized** (10.85x) |
| ToLowerW | lower_proc | 59.499 | 33.648 | 1.77x | **optimized** (1.77x) |
| ToLowerW | lower_path | 857.695 | 63.844 | 13.43x | **optimized** (13.43x) |
| ToLowerW | lower_4k | 9536.806 | 505.537 | 18.86x | **optimized** (18.86x) |
| ToLowerW | lower_mixed | 1379.225 | 85.174 | 16.19x | **optimized** (16.19x) |
| Pipeline | packet_hotpath | 79.796 | 45.537 | 1.75x | **optimized** (1.75x) |

---

## Statistical Analysis

### Summary

| Metric | Value |
|--------|-------|
| Total benchmarks | 133 |
| Faster (optimized wins) | 81 |
| Slower (original wins) | 52 |
| Tied (within 1%) | 7 |
| Geometric mean speedup | 1.59x |
| Min speedup | 0.07x |
| Max speedup | 28.04x |
| Median speedup | 1.08x |
| Mean speedup | 3.02x |
| Stdev of speedup | 4.982 |

### Fastest Optimized Functions

| Rank | Function | Workload | Original | Optimized | Speedup |
|------|----------|----------|----------|-----------|---------|
| 1 | DisplayWidth | width_ascii_256 | 175.223 ns | 6.248 ns | 28.04x |
| 2 | FormatStreamHex | hex_64k_64 | 4347.239 us | 158.261 us | 27.47x |
| 3 | FormatIpv6 | ipv6_full | 928.071 ns | 42.858 ns | 21.65x |
| 4 | ToLowerW | lower_4k | 9.537 us | 505.537 ns | 18.86x |
| 5 | TlsExt | sni_strip_all | 36.276 ns | 2.116 ns | 17.14x |
| 6 | ToLowerW | lower_mixed | 1.379 us | 85.174 ns | 16.19x |
| 7 | FormatStreamHex | hex_1460_16 | 105.309 us | 7.067 us | 14.90x |
| 8 | DistinctPids | distinctpids_200x200 | 4.426 us | 311.363 ns | 14.21x |
| 9 | ToLowerW | lower_path | 857.695 ns | 63.844 ns | 13.43x |
| 10 | FormatIpv6 | ipv6_zip | 162.728 ns | 12.139 ns | 13.41x |
| 11 | FormatStreamHex | hex_40_16 | 3.130 us | 288.518 ns | 10.85x |
| 12 | DisplayWidth | width_ascii_47 | 42.510 ns | 4.866 ns | 8.74x |
| 13 | JoinSamples | join_2k_2k | 2191.956 us | 266.276 us | 8.23x |
| 14 | FormatIpv4 | ipv4 | 227.579 ns | 28.832 ns | 7.89x |
| 15 | FormatDuration | dur_hms | 65.971 ns | 8.516 ns | 7.75x |
| 16 | FormatPort | port_443 | 33.210 ns | 5.269 ns | 6.30x |
| 17 | ConnKey | keyhash_46 | 19.554 ns | 3.264 ns | 5.99x |
| 18 | FormatU64Dec | u64_20digit | 70.032 ns | 13.097 ns | 5.35x |
| 19 | BuildLowerAll | lowerall_10 | 274.478 ns | 60.158 ns | 4.56x |
| 20 | WideToUtf8 | utf8_ascii | 29.025 ns | 6.405 ns | 4.53x |
| 21 | SumPidTraffic | pidsum_5k_700 | 882.065 us | 207.956 us | 4.24x |
| 22 | FormatPort | port_65535 | 34.765 ns | 8.496 ns | 4.09x |
| 23 | DistinctPids | distinctpids_20kx400 | 70.452 us | 19.372 us | 3.64x |
| 24 | SumPidTraffic | pidsum_500_50 | 13.456 us | 3.705 us | 3.63x |
| 25 | DistinctPids | distinctpids_2000x12 | 5.251 us | 1.503 us | 3.49x |
| 26 | WideToUtf8 | utf8_4k | 872.693 ns | 269.500 ns | 3.24x |
| 27 | ConnKey | keyhash_22 | 7.241 ns | 2.453 ns | 2.95x |
| 28 | TruncateToWidth | trunc_cut | 94.712 ns | 33.647 ns | 2.81x |
| 29 | JoinSamples | join_mdns | 75.751 us | 29.334 us | 2.58x |
| 30 | HasLowerSubstring | sub_16_absent | 7.096 ns | 2.770 ns | 2.56x |
| 31 | TruncateToWidth | trunc_wide_cut | 129.780 ns | 53.780 ns | 2.41x |
| 32 | WideToUtf8 | utf8_emoji | 231.698 ns | 105.018 ns | 2.21x |
| 33 | DisplayWidth | width_mixed_30 | 61.003 ns | 28.412 ns | 2.15x |
| 34 | WideToUtf8 | utf8_mixed | 244.941 ns | 118.278 ns | 2.07x |
| 35 | TruncateToWidth | trunc_fits | 65.371 ns | 32.335 ns | 2.02x |
| 36 | JsonEscape | json_row | 193.479 ns | 102.901 ns | 1.88x |
| 37 | JsonEscape | json_64_quotes | 170.835 ns | 90.906 ns | 1.88x |
| 38 | DisplayWidth | width_mixed_320 | 478.378 ns | 270.276 ns | 1.77x |
| 39 | ToLowerW | lower_proc | 59.499 ns | 33.648 ns | 1.77x |
| 40 | FlowProbe | flowprobe_short | 3.011 ns | 1.709 ns | 1.76x |
| 41 | Pipeline | packet_hotpath | 79.796 ns | 45.537 ns | 1.75x |
| 42 | FindSubstringLong | findlong_32_absent | 676.073 ns | 391.353 ns | 1.73x |
| 43 | ComputeBps | bps_1k | 853.053 ns | 516.449 ns | 1.65x |
| 44 | FindSubstringLong | findlong_32_present | 750.867 ns | 471.607 ns | 1.59x |
| 45 | CsvEscape | csv_quoted | 69.555 ns | 45.122 ns | 1.54x |
| 46 | GroupByPid | distinctpids_200x200 | 13.810 us | 8.987 us | 1.54x |
| 47 | GeoRead | be_bytes_58 | 2.130 ns | 1.405 ns | 1.52x |
| 48 | FlowProbe | flowprobe_miss | 4.997 ns | 3.453 ns | 1.45x |
| 49 | JoinSamples | join_500_300 | 80.117 us | 57.072 us | 1.40x |
| 50 | CmpFlowAddr | cmpaddr_late | 1.811 ns | 1.383 ns | 1.31x |
| 51 | TlsExt | ext_classify | 1.825 ns | 1.465 ns | 1.25x |
| 52 | IsGlobalV4 | unicast_v4_mixed | 1.976 ns | 1.593 ns | 1.24x |
| 53 | CpWidth | cp_boundaries | 2.439 ns | 2.005 ns | 1.22x |
| 54 | SkipVlan | vlan_qinq | 2.115 ns | 1.743 ns | 1.21x |
| 55 | PairSnapshot | pair_mdns | 5.342 us | 4.445 us | 1.20x |
| 56 | FormatBytes | fmt_small | 105.949 ns | 88.180 ns | 1.20x |
| 57 | FormatBpsCell | bpscell_hot | 398.085 ns | 332.427 ns | 1.20x |
| 58 | ParseEventPayload | etw_payload | 1.512 ns | 1.268 ns | 1.19x |
| 59 | IsGlobalV6 | unicast_v6_mixed | 2.522 ns | 2.133 ns | 1.18x |
| 60 | WidenUtf8 | widen_org_34 | 54.560 ns | 47.321 ns | 1.15x |
| 61 | HasLowerSubstring | sub_16_present | 9.954 ns | 8.643 ns | 1.15x |
| 62 | SkipVlan | vlan_plain | 1.406 ns | 1.223 ns | 1.15x |
| 63 | FlowKeyEqual | flowkey_miss | 1.553 ns | 1.354 ns | 1.15x |
| 64 | GroupByPid | distinctpids_2000x12 | 13.088 us | 11.478 us | 1.14x |
| 65 | CompareWide | cmpwide_short | 2.549 ns | 2.344 ns | 1.09x |
| 66 | GeoRead | be_u64_unaligned | 1.459 ns | 1.344 ns | 1.09x |
| 67 | HasLowerSubstring | sub_4k_present | 965.606 ns | 892.027 ns | 1.08x |
| 68 | FlowKeyEqual | flowkey_hit | 1.386 ns | 1.284 ns | 1.08x |
| 69 | HasLowerSubstring | sub_256_absent | 47.730 ns | 44.807 ns | 1.07x |
| 70 | GroupByPid | distinctpids_20kx400 | 355.972 us | 335.654 us | 1.06x |
| 71 | PayloadSize | size_inline_only | 1.688 ns | 1.604 ns | 1.05x |
| 72 | PairSnapshot | pair_500 | 35.700 us | 34.534 us | 1.03x |
| 73 | WidenUtf8 | widen_iso | 4.414 ns | 4.279 ns | 1.03x |
| 74 | Rd | rd32_mixed | 1.326 ns | 1.287 ns | 1.03x |
| 75 | GeoIpWalk | tree_65k_24bit | 10.405 ns | 10.224 ns | 1.02x |
| 76 | WidenUtf8 | widen_mixed_15 | 36.425 ns | 35.903 ns | 1.01x |
| 77 | ReasmOverlap | reasm_8192 | 1.184 us | 1.169 us | 1.01x |
| 78 | ReasmRender | render_1024_contig | 17.468 us | 17.384 us | 1.00x |
| 79 | TlsBe | tls_rec_hdr | 1.737 ns | 1.732 ns | 1.00x |
| 80 | ReasmRender | render_4096_segs | 431.090 us | 430.101 us | 1.00x |
| 81 | GeoIpWalk | tree_1m_24bit | 8.757 ns | 8.740 ns | 1.00x |

### Slowest (Regression) Functions

| Rank | Function | Workload | Original | Optimized | Speedup |
|------|----------|----------|----------|-----------|---------|
| 1 | JoinSamples | join_small | 123.158 ns | 1.731 us | 0.07x |
| 2 | ParseTcpHeader | tcp_12_invalid | 1.710 ns | 5.684 ns | 0.30x |
| 3 | FindSubstringLong | findlong_8 | 688.579 ns | 1.611 us | 0.43x |
| 4 | ParseTcpHeader | tcp_20 | 3.380 ns | 6.563 ns | 0.52x |
| 5 | ParseTcpHeader | tcp_52_payload | 3.382 ns | 6.554 ns | 0.52x |
| 6 | FilterHandles | handles_100k | 406.293 us | 760.045 us | 0.53x |
| 7 | GeoIpWalk | tree_65k_28bit | 3.591 ns | 5.262 ns | 0.68x |
| 8 | GeoIpWalkShift | tree_256_24bit | 3.612 ns | 5.173 ns | 0.70x |
| 9 | GeoIpWalk | tree_256_24bit | 3.574 ns | 5.102 ns | 0.70x |
| 10 | GeoIpWalk | tree_65k_32bit | 3.590 ns | 5.117 ns | 0.70x |
| 11 | HasLowerSubstring | sub_64k_absent | 12.476 us | 17.673 us | 0.71x |
| 12 | ReasmOverlap | reasm_4 | 2.964 ns | 4.014 ns | 0.74x |
| 13 | ReasmOverlap | reasm_64_dup | 2.534 ns | 3.378 ns | 0.75x |
| 14 | KeyOf | key_v6 | 2.768 ns | 3.614 ns | 0.77x |
| 15 | GeoIpWalkShift | tree_65k_32bit | 5.218 ns | 6.806 ns | 0.77x |
| 16 | KeyOf | key_v4 | 2.780 ns | 3.429 ns | 0.81x |
| 17 | TlsExt | sni_strip_tail | 3.463 ns | 4.234 ns | 0.82x |
| 18 | FormatBpsCell | bpscell_idle | 2.110 ns | 2.551 ns | 0.83x |
| 19 | FindSubstringLong | findlong_16 | 766.217 ns | 920.484 ns | 0.83x |
| 20 | ReasmOverlap | reasm_64 | 10.435 ns | 12.384 ns | 0.84x |
| 21 | GeoRead | be_bytes_14 | 1.761 ns | 2.052 ns | 0.86x |
| 22 | ReasmOverlap | reasm_1024 | 115.120 ns | 131.139 ns | 0.88x |
| 23 | PairSnapshot | pair_5k | 507.999 us | 576.830 us | 0.88x |
| 24 | GeoIpWalkShift | tree_65k_28bit | 6.374 ns | 7.171 ns | 0.89x |
| 25 | ClassifyEvent | etw_mixed | 1.635 ns | 1.816 ns | 0.90x |
| 26 | FlowProbe | flowprobe_hit | 1.464 ns | 1.623 ns | 0.90x |
| 27 | FindCountedKey | findkey_short_miss | 2.535 ns | 2.762 ns | 0.92x |
| 28 | GeoIpWalkShift | tree_65k_24bit | 5.684 ns | 6.171 ns | 0.92x |
| 29 | CpWidth | cp_random_sweep | 6.248 ns | 6.781 ns | 0.92x |
| 30 | FindCountedKey | findkey_short_hit | 3.164 ns | 3.397 ns | 0.93x |
| 31 | FindCountedKey | findkey_long_hit | 2.774 ns | 2.974 ns | 0.93x |
| 32 | GeoIpWalk | tree_65k_ipv6 | 6.972 ns | 7.446 ns | 0.94x |
| 33 | CompareWide | cmpwide_eq_256 | 16.040 ns | 17.079 ns | 0.94x |
| 34 | ReasmRender | render_64_segs | 970.280 ns | 1.024 us | 0.95x |
| 35 | FindCountedKey | findkey_64_hit | 141.932 ns | 149.639 ns | 0.95x |
| 36 | ReasmRender | render_96_segs | 1.508 us | 1.583 us | 0.95x |
| 37 | Rd | rd16_mixed | 1.265 ns | 1.326 ns | 0.95x |
| 38 | FindCountedKey | findkey_64_miss | 141.883 ns | 148.560 ns | 0.96x |
| 39 | PayloadSize | size_code_mix | 2.141 ns | 2.241 ns | 0.96x |
| 40 | ConnKey | keyeq_46_diff | 3.947 ns | 4.115 ns | 0.96x |
| 41 | ReasmRender | render_300_segs | 5.181 us | 5.371 us | 0.96x |
| 42 | FormatBytes | fmt_tb | 176.657 ns | 182.768 ns | 0.97x |
| 43 | GeoRead | be_u32_unaligned | 1.284 ns | 1.327 ns | 0.97x |
| 44 | JsonEscape | json_proc | 60.633 ns | 62.604 ns | 0.97x |
| 45 | ConnKey | keyeq_46 | 3.862 ns | 3.981 ns | 0.97x |
| 46 | FormatBytes | fmt_gb | 176.058 ns | 180.171 ns | 0.98x |
| 47 | HasLowerSubstring | sub_4k_absent | 684.475 ns | 695.525 ns | 0.98x |
| 48 | TlsBe | tls_u24 | 1.283 ns | 1.303 ns | 0.98x |
| 49 | FormatBytes | fmt_mb | 181.820 ns | 183.950 ns | 0.99x |
| 50 | CompareWide | cmpwide_late_256 | 18.315 ns | 18.468 ns | 0.99x |
| 51 | CsvEscape | csv_plain_64 | 35.707 ns | 35.987 ns | 0.99x |
| 52 | ReadPointer | ptr_size_mix | 2.299 ns | 2.317 ns | 0.99x |

---

## Per-Round Timing Data

### GeoIpWalk / tree_256_24bit

#### Original

| Run | ns/op | ns/min | ns/p90 | ns/mean | stdev | iterations |
|-----|-------|--------|--------|---------|-------|------------|
| 1 | 3.615 | 3.580 | 3.632 | 3.610 | 0.018 | 110802944 |
| | rounds: [3.601, 3.622, 3.632, 3.615, 3.580] | | | | | |
| 2 | 3.565 | 3.554 | 3.606 | 3.571 | 0.018 | 112012288 |
| | rounds: [3.606, 3.565, 3.554, 3.566, 3.565] | | | | | |
| 3 | 3.574 | 3.559 | 3.675 | 3.600 | 0.043 | 111116288 |
| | rounds: [3.571, 3.623, 3.574, 3.559, 3.675] | | | | | |

#### Optimized

| Run | ns/op | ns/min | ns/p90 | ns/mean | stdev | iterations |
|-----|-------|--------|--------|---------|-------|------------|
| 1 | 5.162 | 5.136 | 5.256 | 5.189 | 0.052 | 77101056 |
| | rounds: [5.136, 5.144, 5.256, 5.162, 5.247] | | | | | |
| 2 | 5.058 | 5.029 | 5.135 | 5.073 | 0.041 | 78852096 |
| | rounds: [5.135, 5.109, 5.058, 5.037, 5.029] | | | | | |
| 3 | 5.102 | 5.071 | 5.161 | 5.106 | 0.030 | 78350336 |
| | rounds: [5.088, 5.105, 5.102, 5.071, 5.161] | | | | | |

### GeoIpWalk / tree_65k_24bit

#### Original

| Run | ns/op | ns/min | ns/p90 | ns/mean | stdev | iterations |
|-----|-------|--------|--------|---------|-------|------------|
| 1 | 10.440 | 10.346 | 10.602 | 10.454 | 0.088 | 38268928 |
| | rounds: [10.393, 10.488, 10.346, 10.602, 10.440] | | | | | |
| 2 | 10.405 | 10.094 | 10.489 | 10.332 | 0.147 | 38726656 |
| | rounds: [10.405, 10.234, 10.489, 10.094, 10.438] | | | | | |
| 3 | 10.259 | 10.178 | 10.462 | 10.279 | 0.097 | 38920192 |
| | rounds: [10.223, 10.178, 10.462, 10.273, 10.259] | | | | | |

#### Optimized

| Run | ns/op | ns/min | ns/p90 | ns/mean | stdev | iterations |
|-----|-------|--------|--------|---------|-------|------------|
| 1 | 10.347 | 10.080 | 10.406 | 10.284 | 0.128 | 38903808 |
| | rounds: [10.396, 10.406, 10.190, 10.080, 10.347] | | | | | |
| 2 | 10.224 | 10.126 | 10.401 | 10.243 | 0.090 | 39056384 |
| | rounds: [10.401, 10.126, 10.224, 10.203, 10.261] | | | | | |
| 3 | 10.162 | 9.936 | 10.417 | 10.174 | 0.155 | 39328768 |
| | rounds: [10.220, 10.417, 9.936, 10.133, 10.162] | | | | | |

### GeoIpWalk / tree_1m_24bit

#### Original

| Run | ns/op | ns/min | ns/p90 | ns/mean | stdev | iterations |
|-----|-------|--------|--------|---------|-------|------------|
| 1 | 8.853 | 8.806 | 8.994 | 8.873 | 0.063 | 45087744 |
| | rounds: [8.994, 8.861, 8.806, 8.848, 8.853] | | | | | |
| 2 | 8.678 | 8.653 | 8.781 | 8.711 | 0.058 | 45925376 |
| | rounds: [8.653, 8.781, 8.678, 8.661, 8.781] | | | | | |
| 3 | 8.757 | 8.676 | 8.893 | 8.777 | 0.087 | 45578240 |
| | rounds: [8.699, 8.893, 8.862, 8.676, 8.757] | | | | | |

#### Optimized

| Run | ns/op | ns/min | ns/p90 | ns/mean | stdev | iterations |
|-----|-------|--------|--------|---------|-------|------------|
| 1 | 8.869 | 8.763 | 9.007 | 8.860 | 0.089 | 45154304 |
| | rounds: [8.889, 8.770, 9.007, 8.763, 8.869] | | | | | |
| 2 | 8.699 | 8.646 | 8.903 | 8.747 | 0.106 | 45737984 |
| | rounds: [8.903, 8.842, 8.646, 8.647, 8.699] | | | | | |
| 3 | 8.740 | 8.683 | 8.809 | 8.741 | 0.045 | 45763584 |
| | rounds: [8.772, 8.740, 8.703, 8.809, 8.683] | | | | | |

### GeoIpWalk / tree_65k_28bit

#### Original

| Run | ns/op | ns/min | ns/p90 | ns/mean | stdev | iterations |
|-----|-------|--------|--------|---------|-------|------------|
| 1 | 3.603 | 3.585 | 3.626 | 3.603 | 0.013 | 111029248 |
| | rounds: [3.626, 3.585, 3.604, 3.597, 3.603] | | | | | |
| 2 | 3.574 | 3.546 | 3.639 | 3.582 | 0.031 | 111693824 |
| | rounds: [3.574, 3.546, 3.568, 3.580, 3.639] | | | | | |
| 3 | 3.591 | 3.559 | 3.632 | 3.589 | 0.025 | 111444992 |
| | rounds: [3.569, 3.632, 3.591, 3.559, 3.597] | | | | | |

#### Optimized

| Run | ns/op | ns/min | ns/p90 | ns/mean | stdev | iterations |
|-----|-------|--------|--------|---------|-------|------------|
| 1 | 5.269 | 5.209 | 5.341 | 5.271 | 0.042 | 75887616 |
| | rounds: [5.266, 5.272, 5.209, 5.269, 5.341] | | | | | |
| 2 | 5.226 | 5.195 | 5.332 | 5.254 | 0.055 | 76140544 |
| | rounds: [5.332, 5.195, 5.226, 5.309, 5.209] | | | | | |
| 3 | 5.262 | 5.221 | 5.396 | 5.281 | 0.061 | 75754496 |
| | rounds: [5.221, 5.262, 5.243, 5.284, 5.396] | | | | | |

### GeoIpWalk / tree_65k_32bit

#### Original

| Run | ns/op | ns/min | ns/p90 | ns/mean | stdev | iterations |
|-----|-------|--------|--------|---------|-------|------------|
| 1 | 3.590 | 3.560 | 3.617 | 3.589 | 0.024 | 111474688 |
| | rounds: [3.590, 3.560, 3.613, 3.617, 3.563] | | | | | |
| 2 | 3.577 | 3.551 | 3.600 | 3.578 | 0.017 | 111802368 |
| | rounds: [3.600, 3.569, 3.591, 3.577, 3.551] | | | | | |
| 3 | 3.599 | 3.548 | 3.611 | 3.590 | 0.023 | 111412224 |
| | rounds: [3.588, 3.611, 3.599, 3.607, 3.548] | | | | | |

#### Optimized

| Run | ns/op | ns/min | ns/p90 | ns/mean | stdev | iterations |
|-----|-------|--------|--------|---------|-------|------------|
| 1 | 5.183 | 5.150 | 5.296 | 5.204 | 0.052 | 76871680 |
| | rounds: [5.183, 5.296, 5.224, 5.150, 5.169] | | | | | |
| 2 | 5.117 | 5.041 | 5.240 | 5.129 | 0.067 | 78009344 |
| | rounds: [5.117, 5.041, 5.155, 5.091, 5.240] | | | | | |
| 3 | 5.112 | 5.096 | 5.180 | 5.131 | 0.033 | 77964288 |
| | rounds: [5.105, 5.096, 5.161, 5.180, 5.112] | | | | | |

### GeoIpWalk / tree_65k_ipv6

#### Original

| Run | ns/op | ns/min | ns/p90 | ns/mean | stdev | iterations |
|-----|-------|--------|--------|---------|-------|------------|
| 1 | 7.066 | 6.918 | 7.177 | 7.046 | 0.086 | 56777728 |
| | rounds: [6.918, 7.072, 7.177, 7.066, 6.999] | | | | | |
| 2 | 6.870 | 6.833 | 6.992 | 6.885 | 0.059 | 58104832 |
| | rounds: [6.833, 6.896, 6.992, 6.870, 6.834] | | | | | |
| 3 | 6.972 | 6.842 | 7.118 | 6.965 | 0.091 | 57446400 |
| | rounds: [7.118, 6.913, 6.842, 6.980, 6.972] | | | | | |

#### Optimized

| Run | ns/op | ns/min | ns/p90 | ns/mean | stdev | iterations |
|-----|-------|--------|--------|---------|-------|------------|
| 1 | 7.479 | 7.428 | 7.522 | 7.469 | 0.035 | 53554176 |
| | rounds: [7.479, 7.486, 7.432, 7.522, 7.428] | | | | | |
| 2 | 7.316 | 7.217 | 7.411 | 7.308 | 0.069 | 54745088 |
| | rounds: [7.217, 7.316, 7.344, 7.411, 7.250] | | | | | |
| 3 | 7.446 | 7.302 | 7.545 | 7.424 | 0.085 | 53892096 |
| | rounds: [7.467, 7.358, 7.545, 7.446, 7.302] | | | | | |

### GeoIpWalkShift / tree_256_24bit

#### Original

| Run | ns/op | ns/min | ns/p90 | ns/mean | stdev | iterations |
|-----|-------|--------|--------|---------|-------|------------|
| 1 | 3.612 | 3.563 | 3.652 | 3.601 | 0.033 | 111083520 |
| | rounds: [3.568, 3.652, 3.613, 3.563, 3.612] | | | | | |
| 2 | 3.576 | 3.561 | 3.615 | 3.585 | 0.019 | 111590400 |
| | rounds: [3.576, 3.561, 3.615, 3.598, 3.574] | | | | | |
| 3 | 3.619 | 3.601 | 3.653 | 3.622 | 0.018 | 110441472 |
| | rounds: [3.619, 3.609, 3.601, 3.653, 3.629] | | | | | |

#### Optimized

| Run | ns/op | ns/min | ns/p90 | ns/mean | stdev | iterations |
|-----|-------|--------|--------|---------|-------|------------|
| 1 | 5.174 | 5.114 | 5.231 | 5.175 | 0.041 | 77300736 |
| | rounds: [5.174, 5.204, 5.151, 5.114, 5.231] | | | | | |
| 2 | 5.139 | 5.105 | 5.163 | 5.134 | 0.023 | 77913088 |
| | rounds: [5.105, 5.139, 5.111, 5.163, 5.154] | | | | | |
| 3 | 5.173 | 5.128 | 5.203 | 5.165 | 0.027 | 77453312 |
| | rounds: [5.142, 5.128, 5.173, 5.178, 5.203] | | | | | |

### GeoIpWalkShift / tree_65k_24bit

#### Original

| Run | ns/op | ns/min | ns/p90 | ns/mean | stdev | iterations |
|-----|-------|--------|--------|---------|-------|------------|
| 1 | 5.610 | 5.531 | 5.673 | 5.599 | 0.050 | 71447552 |
| | rounds: [5.531, 5.558, 5.625, 5.673, 5.610] | | | | | |
| 2 | 5.684 | 5.548 | 5.747 | 5.658 | 0.071 | 70708224 |
| | rounds: [5.747, 5.702, 5.684, 5.609, 5.548] | | | | | |
| 3 | 5.684 | 5.593 | 5.717 | 5.678 | 0.045 | 70453248 |
| | rounds: [5.714, 5.717, 5.684, 5.682, 5.593] | | | | | |

#### Optimized

| Run | ns/op | ns/min | ns/p90 | ns/mean | stdev | iterations |
|-----|-------|--------|--------|---------|-------|------------|
| 1 | 6.107 | 6.077 | 6.194 | 6.118 | 0.040 | 65388544 |
| | rounds: [6.077, 6.194, 6.095, 6.115, 6.107] | | | | | |
| 2 | 6.173 | 5.999 | 7.140 | 6.331 | 0.422 | 63442944 |
| | rounds: [6.010, 6.334, 6.173, 7.140, 5.999] | | | | | |
| 3 | 6.171 | 6.058 | 6.233 | 6.151 | 0.069 | 65044480 |
| | rounds: [6.083, 6.058, 6.208, 6.171, 6.233] | | | | | |

### GeoIpWalkShift / tree_65k_28bit

#### Original

| Run | ns/op | ns/min | ns/p90 | ns/mean | stdev | iterations |
|-----|-------|--------|--------|---------|-------|------------|
| 1 | 6.371 | 6.185 | 6.463 | 6.364 | 0.100 | 62869504 |
| | rounds: [6.463, 6.371, 6.451, 6.185, 6.351] | | | | | |
| 2 | 6.469 | 6.282 | 6.558 | 6.435 | 0.101 | 62181376 |
| | rounds: [6.558, 6.282, 6.469, 6.509, 6.355] | | | | | |
| 3 | 6.374 | 6.269 | 6.453 | 6.366 | 0.069 | 62842880 |
| | rounds: [6.307, 6.453, 6.427, 6.269, 6.374] | | | | | |

#### Optimized

| Run | ns/op | ns/min | ns/p90 | ns/mean | stdev | iterations |
|-----|-------|--------|--------|---------|-------|------------|
| 1 | 7.171 | 7.153 | 7.266 | 7.199 | 0.046 | 55568384 |
| | rounds: [7.161, 7.243, 7.153, 7.171, 7.266] | | | | | |
| 2 | 7.197 | 7.087 | 7.254 | 7.193 | 0.061 | 55616512 |
| | rounds: [7.087, 7.254, 7.197, 7.251, 7.176] | | | | | |
| 3 | 7.154 | 7.071 | 7.277 | 7.154 | 0.070 | 55921664 |
| | rounds: [7.277, 7.106, 7.071, 7.161, 7.154] | | | | | |

### GeoIpWalkShift / tree_65k_32bit

#### Original

| Run | ns/op | ns/min | ns/p90 | ns/mean | stdev | iterations |
|-----|-------|--------|--------|---------|-------|------------|
| 1 | 5.214 | 5.065 | 5.340 | 5.192 | 0.095 | 77068288 |
| | rounds: [5.115, 5.065, 5.227, 5.214, 5.340] | | | | | |
| 2 | 5.302 | 5.266 | 5.339 | 5.304 | 0.027 | 75427840 |
| | rounds: [5.302, 5.266, 5.282, 5.339, 5.328] | | | | | |
| 3 | 5.218 | 5.195 | 5.341 | 5.236 | 0.054 | 76409856 |
| | rounds: [5.196, 5.218, 5.341, 5.228, 5.195] | | | | | |

#### Optimized

| Run | ns/op | ns/min | ns/p90 | ns/mean | stdev | iterations |
|-----|-------|--------|--------|---------|-------|------------|
| 1 | 6.806 | 6.673 | 7.075 | 6.827 | 0.142 | 58615808 |
| | rounds: [6.871, 6.673, 6.806, 7.075, 6.713] | | | | | |
| 2 | 6.736 | 6.622 | 6.896 | 6.743 | 0.091 | 59335680 |
| | rounds: [6.695, 6.765, 6.622, 6.736, 6.896] | | | | | |
| 3 | 6.807 | 6.740 | 6.899 | 6.813 | 0.052 | 58712064 |
| | rounds: [6.899, 6.740, 6.788, 6.833, 6.807] | | | | | |

### SkipVlan / vlan_plain

#### Original

| Run | ns/op | ns/min | ns/p90 | ns/mean | stdev | iterations |
|-----|-------|--------|--------|---------|-------|------------|
| 1 | 1.406 | 1.391 | 1.422 | 1.405 | 0.011 | 284759040 |
| | rounds: [1.422, 1.395, 1.406, 1.410, 1.391] | | | | | |
| 2 | 1.402 | 1.395 | 1.420 | 1.406 | 0.009 | 284516352 |
| | rounds: [1.402, 1.420, 1.414, 1.399, 1.395] | | | | | |
| 3 | 1.409 | 1.376 | 1.446 | 1.407 | 0.024 | 284289024 |
| | rounds: [1.446, 1.376, 1.409, 1.417, 1.389] | | | | | |

#### Optimized

| Run | ns/op | ns/min | ns/p90 | ns/mean | stdev | iterations |
|-----|-------|--------|--------|---------|-------|------------|
| 1 | 1.225 | 1.212 | 1.239 | 1.225 | 0.011 | 326615040 |
| | rounds: [1.212, 1.235, 1.225, 1.213, 1.239] | | | | | |
| 2 | 1.212 | 1.194 | 1.245 | 1.216 | 0.017 | 328951808 |
| | rounds: [1.245, 1.194, 1.223, 1.212, 1.207] | | | | | |
| 3 | 1.223 | 1.210 | 1.230 | 1.221 | 0.007 | 327745536 |
| | rounds: [1.223, 1.216, 1.223, 1.230, 1.210] | | | | | |

### SkipVlan / vlan_qinq

#### Original

| Run | ns/op | ns/min | ns/p90 | ns/mean | stdev | iterations |
|-----|-------|--------|--------|---------|-------|------------|
| 1 | 2.098 | 2.087 | 2.126 | 2.105 | 0.016 | 190001152 |
| | rounds: [2.126, 2.123, 2.093, 2.087, 2.098] | | | | | |
| 2 | 2.136 | 2.114 | 2.167 | 2.140 | 0.020 | 186953728 |
| | rounds: [2.167, 2.159, 2.123, 2.114, 2.136] | | | | | |
| 3 | 2.115 | 2.095 | 2.155 | 2.121 | 0.020 | 188649472 |
| | rounds: [2.155, 2.125, 2.114, 2.115, 2.095] | | | | | |

#### Optimized

| Run | ns/op | ns/min | ns/p90 | ns/mean | stdev | iterations |
|-----|-------|--------|--------|---------|-------|------------|
| 1 | 1.831 | 1.725 | 1.856 | 1.797 | 0.053 | 222764032 |
| | rounds: [1.856, 1.832, 1.743, 1.725, 1.831] | | | | | |
| 2 | 1.733 | 1.682 | 1.791 | 1.738 | 0.035 | 230259712 |
| | rounds: [1.791, 1.732, 1.733, 1.754, 1.682] | | | | | |
| 3 | 1.743 | 1.678 | 1.794 | 1.745 | 0.040 | 229397504 |
| | rounds: [1.743, 1.733, 1.794, 1.775, 1.678] | | | | | |

### FlowProbe / flowprobe_hit

#### Original

| Run | ns/op | ns/min | ns/p90 | ns/mean | stdev | iterations |
|-----|-------|--------|--------|---------|-------|------------|
| 1 | 1.457 | 1.442 | 1.463 | 1.455 | 0.007 | 274842624 |
| | rounds: [1.455, 1.457, 1.463, 1.442, 1.459] | | | | | |
| 2 | 1.464 | 1.447 | 1.470 | 1.461 | 0.009 | 273792000 |
| | rounds: [1.468, 1.447, 1.470, 1.455, 1.464] | | | | | |
| 3 | 1.478 | 1.472 | 1.491 | 1.479 | 0.006 | 270457856 |
| | rounds: [1.478, 1.472, 1.491, 1.479, 1.475] | | | | | |

#### Optimized

| Run | ns/op | ns/min | ns/p90 | ns/mean | stdev | iterations |
|-----|-------|--------|--------|---------|-------|------------|
| 1 | 1.624 | 1.605 | 1.645 | 1.624 | 0.015 | 246300672 |
| | rounds: [1.637, 1.609, 1.645, 1.624, 1.605] | | | | | |
| 2 | 1.623 | 1.591 | 1.638 | 1.615 | 0.019 | 247699456 |
| | rounds: [1.595, 1.638, 1.623, 1.628, 1.591] | | | | | |
| 3 | 1.615 | 1.590 | 1.621 | 1.611 | 0.011 | 248361984 |
| | rounds: [1.590, 1.615, 1.621, 1.606, 1.620] | | | | | |

### FlowProbe / flowprobe_miss

#### Original

| Run | ns/op | ns/min | ns/p90 | ns/mean | stdev | iterations |
|-----|-------|--------|--------|---------|-------|------------|
| 1 | 4.907 | 4.873 | 4.979 | 4.926 | 0.042 | 81210368 |
| | rounds: [4.900, 4.972, 4.873, 4.907, 4.979] | | | | | |
| 2 | 5.068 | 4.914 | 5.080 | 5.015 | 0.075 | 79774720 |
| | rounds: [5.080, 5.068, 4.914, 4.935, 5.080] | | | | | |
| 3 | 4.997 | 4.855 | 5.037 | 4.967 | 0.066 | 80541696 |
| | rounds: [4.855, 4.997, 5.037, 4.935, 5.013] | | | | | |

#### Optimized

| Run | ns/op | ns/min | ns/p90 | ns/mean | stdev | iterations |
|-----|-------|--------|--------|---------|-------|------------|
| 1 | 3.541 | 3.417 | 3.563 | 3.513 | 0.053 | 113885184 |
| | rounds: [3.546, 3.499, 3.541, 3.563, 3.417] | | | | | |
| 2 | 3.453 | 3.393 | 3.519 | 3.451 | 0.042 | 115920896 |
| | rounds: [3.453, 3.428, 3.463, 3.393, 3.519] | | | | | |
| 3 | 3.436 | 3.401 | 3.749 | 3.539 | 0.149 | 113231872 |
| | rounds: [3.749, 3.418, 3.401, 3.436, 3.691] | | | | | |

### FlowProbe / flowprobe_short

#### Original

| Run | ns/op | ns/min | ns/p90 | ns/mean | stdev | iterations |
|-----|-------|--------|--------|---------|-------|------------|
| 1 | 2.964 | 2.918 | 3.032 | 2.970 | 0.043 | 134697984 |
| | rounds: [2.932, 3.006, 2.964, 2.918, 3.032] | | | | | |
| 2 | 3.011 | 2.993 | 3.145 | 3.037 | 0.057 | 131749888 |
| | rounds: [3.145, 2.995, 2.993, 3.042, 3.011] | | | | | |
| 3 | 3.055 | 2.987 | 3.057 | 3.041 | 0.027 | 131549184 |
| | rounds: [2.987, 3.056, 3.051, 3.055, 3.057] | | | | | |

#### Optimized

| Run | ns/op | ns/min | ns/p90 | ns/mean | stdev | iterations |
|-----|-------|--------|--------|---------|-------|------------|
| 1 | 1.722 | 1.689 | 1.741 | 1.717 | 0.018 | 232935424 |
| | rounds: [1.689, 1.722, 1.741, 1.708, 1.727] | | | | | |
| 2 | 1.689 | 1.684 | 1.698 | 1.689 | 0.005 | 236813312 |
| | rounds: [1.691, 1.684, 1.684, 1.689, 1.698] | | | | | |
| 3 | 1.709 | 1.702 | 1.724 | 1.712 | 0.007 | 233641984 |
| | rounds: [1.724, 1.716, 1.709, 1.709, 1.702] | | | | | |

### ReasmRender / render_64_segs

#### Original

| Run | ns/op | ns/min | ns/p90 | ns/mean | stdev | iterations |
|-----|-------|--------|--------|---------|-------|------------|
| 1 | 970.280 | 962.371 | 973.001 | 968.046 | 4.237 | 416768 |
| | rounds: [963.624, 973.001, 970.280, 970.953, 962.371] | | | | | |
| 2 | 969.217 | 964.405 | 971.364 | 968.722 | 2.604 | 415744 |
| | rounds: [969.217, 971.364, 971.231, 964.405, 967.395] | | | | | |
| 3 | 995.567 | 991.626 | 1002.383 | 996.127 | 3.933 | 403456 |
| | rounds: [998.436, 1002.383, 992.621, 995.567, 991.626] | | | | | |

#### Optimized

| Run | ns/op | ns/min | ns/p90 | ns/mean | stdev | iterations |
|-----|-------|--------|--------|---------|-------|------------|
| 1 | 1046.180 | 1041.973 | 1070.761 | 1050.809 | 10.616 | 381952 |
| | rounds: [1052.301, 1041.973, 1046.180, 1042.831, 1070.761] | | | | | |
| 2 | 1018.378 | 1000.905 | 1024.079 | 1015.187 | 7.913 | 397312 |
| | rounds: [1018.378, 1024.079, 1000.905, 1013.336, 1019.234] | | | | | |
| 3 | 1024.215 | 1020.423 | 1045.432 | 1028.307 | 9.000 | 391168 |
| | rounds: [1028.864, 1045.432, 1020.423, 1024.215, 1022.602] | | | | | |

### ReasmRender / render_96_segs

#### Original

| Run | ns/op | ns/min | ns/p90 | ns/mean | stdev | iterations |
|-----|-------|--------|--------|---------|-------|------------|
| 1 | 1507.512 | 1503.717 | 1536.809 | 1514.932 | 12.648 | 266240 |
| | rounds: [1536.809, 1507.512, 1503.717, 1521.533, 1505.091] | | | | | |
| 2 | 1506.857 | 1470.510 | 1532.929 | 1503.690 | 20.297 | 268288 |
| | rounds: [1506.857, 1532.929, 1497.232, 1510.922, 1470.510] | | | | | |
| 3 | 1538.168 | 1518.970 | 1586.963 | 1546.519 | 23.937 | 262144 |
| | rounds: [1538.168, 1558.215, 1518.970, 1530.281, 1586.963] | | | | | |

#### Optimized

| Run | ns/op | ns/min | ns/p90 | ns/mean | stdev | iterations |
|-----|-------|--------|--------|---------|-------|------------|
| 1 | 1583.373 | 1549.650 | 1597.272 | 1574.186 | 18.332 | 257024 |
| | rounds: [1597.272, 1549.650, 1583.373, 1555.637, 1584.996] | | | | | |
| 2 | 1572.922 | 1542.232 | 1576.982 | 1564.265 | 13.468 | 258048 |
| | rounds: [1554.990, 1576.982, 1574.201, 1542.232, 1572.922] | | | | | |
| 3 | 1584.891 | 1573.393 | 1595.482 | 1584.277 | 7.662 | 254976 |
| | rounds: [1584.891, 1595.482, 1573.393, 1578.875, 1588.744] | | | | | |

### ReasmRender / render_300_segs

#### Original

| Run | ns/op | ns/min | ns/p90 | ns/mean | stdev | iterations |
|-----|-------|--------|--------|---------|-------|------------|
| 1 | 5180.511 | 5068.713 | 11329.967 | 6376.889 | 2476.973 | 72704 |
| | rounds: [11329.967, 5195.807, 5109.448, 5180.511, 5068.713] | | | | | |
| 2 | 4978.302 | 4902.130 | 9168.121 | 5791.748 | 1688.441 | 74752 |
| | rounds: [9168.121, 4902.130, 4978.302, 4930.859, 4979.327] | | | | | |
| 3 | 9889.221 | 9661.719 | 10901.367 | 10040.352 | 448.599 | 43008 |
| | rounds: [10901.367, 9661.719, 9889.221, 10022.693, 9726.758] | | | | | |

#### Optimized

| Run | ns/op | ns/min | ns/p90 | ns/mean | stdev | iterations |
|-----|-------|--------|--------|---------|-------|------------|
| 1 | 5371.009 | 5339.701 | 5654.715 | 5423.202 | 116.914 | 75776 |
| | rounds: [5654.715, 5339.701, 5371.009, 5390.436, 5360.150] | | | | | |
| 2 | 5327.207 | 5217.799 | 5772.070 | 5380.471 | 200.575 | 75776 |
| | rounds: [5772.070, 5331.849, 5217.799, 5327.207, 5253.431] | | | | | |
| 3 | 5385.566 | 5230.469 | 6920.378 | 5638.393 | 644.714 | 73728 |
| | rounds: [6920.378, 5230.469, 5385.566, 5251.921, 5403.633] | | | | | |

### ReasmRender / render_4096_segs

#### Original

| Run | ns/op | ns/min | ns/p90 | ns/mean | stdev | iterations |
|-----|-------|--------|--------|---------|-------|------------|
| 1 | 425111.133 | 422439.160 | 438350.293 | 427670.137 | 5594.583 | 5120 |
| | rounds: [438350.293, 424751.465, 427698.633, 425111.133, 422439.160] | | | | | |
| 2 | 431090.137 | 423068.359 | 435254.297 | 430206.914 | 3969.337 | 5120 |
| | rounds: [431456.445, 423068.359, 435254.297, 431090.137, 430165.332] | | | | | |
| 3 | 433290.625 | 432343.945 | 440660.742 | 434717.695 | 3075.612 | 5120 |
| | rounds: [432643.750, 434649.414, 433290.625, 432343.945, 440660.742] | | | | | |

#### Optimized

| Run | ns/op | ns/min | ns/p90 | ns/mean | stdev | iterations |
|-----|-------|--------|--------|---------|-------|------------|
| 1 | 421349.609 | 418631.738 | 422443.555 | 421172.305 | 1391.738 | 5120 |
| | rounds: [422443.555, 422420.605, 421016.016, 418631.738, 421349.609] | | | | | |
| 2 | 430101.270 | 427013.672 | 432516.699 | 430157.754 | 1855.662 | 5120 |
| | rounds: [427013.672, 432516.699, 431418.262, 429738.867, 430101.270] | | | | | |
| 3 | 433777.734 | 428983.398 | 441064.453 | 433537.734 | 4279.871 | 5120 |
| | rounds: [428983.398, 433777.734, 441064.453, 434050.000, 429813.086] | | | | | |

### ReasmRender / render_1024_contig

#### Original

| Run | ns/op | ns/min | ns/p90 | ns/mean | stdev | iterations |
|-----|-------|--------|--------|---------|-------|------------|
| 1 | 17104.023 | 16912.441 | 17357.227 | 17111.484 | 143.520 | 25600 |
| | rounds: [17124.707, 16912.441, 17059.023, 17104.023, 17357.227] | | | | | |
| 2 | 17468.418 | 17391.152 | 17751.270 | 17529.312 | 124.747 | 25600 |
| | rounds: [17570.234, 17751.270, 17465.488, 17468.418, 17391.152] | | | | | |
| 3 | 30233.691 | 29349.089 | 30948.893 | 30106.374 | 573.310 | 15360 |
| | rounds: [29349.089, 29599.284, 30233.691, 30948.893, 30400.911] | | | | | |

#### Optimized

| Run | ns/op | ns/min | ns/p90 | ns/mean | stdev | iterations |
|-----|-------|--------|--------|---------|-------|------------|
| 1 | 17384.414 | 17276.094 | 18104.316 | 17627.758 | 350.784 | 25600 |
| | rounds: [18001.816, 17384.414, 18104.316, 17372.148, 17276.094] | | | | | |
| 2 | 18409.688 | 18018.652 | 18899.199 | 18380.129 | 301.737 | 25600 |
| | rounds: [18422.090, 18409.688, 18151.016, 18018.652, 18899.199] | | | | | |
| 3 | 17273.867 | 16932.910 | 17626.484 | 17307.504 | 250.763 | 25600 |
| | rounds: [17626.484, 17273.867, 17535.469, 16932.910, 17168.789] | | | | | |

### DistinctPids / distinctpids_2000x12

#### Original

| Run | ns/op | ns/min | ns/p90 | ns/mean | stdev | iterations |
|-----|-------|--------|--------|---------|-------|------------|
| 1 | 5217.878 | 5178.864 | 5318.893 | 5248.614 | 55.687 | 77824 |
| | rounds: [5217.350, 5318.893, 5217.878, 5178.864, 5310.085] | | | | | |
| 2 | 5307.650 | 5248.086 | 5398.542 | 5313.022 | 53.446 | 76800 |
| | rounds: [5307.650, 5269.310, 5248.086, 5341.523, 5398.542] | | | | | |
| 3 | 5250.586 | 5186.304 | 5405.970 | 5267.472 | 73.765 | 77824 |
| | rounds: [5186.304, 5260.163, 5405.970, 5250.586, 5234.336] | | | | | |

#### Optimized

| Run | ns/op | ns/min | ns/p90 | ns/mean | stdev | iterations |
|-----|-------|--------|--------|---------|-------|------------|
| 1 | 1515.377 | 1494.916 | 1541.159 | 1514.032 | 15.720 | 266240 |
| | rounds: [1494.916, 1515.377, 1516.016, 1541.159, 1502.693] | | | | | |
| 2 | 1474.930 | 1466.464 | 1527.701 | 1484.138 | 22.118 | 272384 |
| | rounds: [1473.343, 1527.701, 1474.930, 1466.464, 1478.254] | | | | | |
| 3 | 1502.524 | 1489.853 | 1533.075 | 1505.907 | 14.933 | 268288 |
| | rounds: [1533.075, 1489.853, 1495.801, 1502.524, 1508.280] | | | | | |

### GroupByPid / distinctpids_2000x12

#### Original

| Run | ns/op | ns/min | ns/p90 | ns/mean | stdev | iterations |
|-----|-------|--------|--------|---------|-------|------------|
| 1 | 12800.516 | 12752.665 | 13331.901 | 12900.939 | 217.302 | 34816 |
| | rounds: [12838.895, 13331.901, 12780.720, 12800.516, 12752.665] | | | | | |
| 2 | 13683.138 | 13260.205 | 13850.863 | 13599.023 | 216.822 | 30720 |
| | rounds: [13683.138, 13850.863, 13442.725, 13758.187, 13260.205] | | | | | |
| 3 | 13087.549 | 12965.765 | 14011.100 | 13420.298 | 465.628 | 31744 |
| | rounds: [14011.100, 13087.549, 13072.233, 12965.765, 13964.844] | | | | | |

#### Optimized

| Run | ns/op | ns/min | ns/p90 | ns/mean | stdev | iterations |
|-----|-------|--------|--------|---------|-------|------------|
| 1 | 10929.553 | 10729.919 | 11119.153 | 10937.356 | 124.858 | 40960 |
| | rounds: [10929.553, 10729.919, 10929.102, 10979.053, 11119.153] | | | | | |
| 2 | 11478.097 | 11302.414 | 12241.490 | 11583.256 | 345.335 | 35840 |
| | rounds: [12241.490, 11312.932, 11478.097, 11302.414, 11581.348] | | | | | |
| 3 | 11566.532 | 11329.157 | 11833.189 | 11601.010 | 200.644 | 35840 |
| | rounds: [11833.189, 11451.618, 11566.532, 11824.554, 11329.157] | | | | | |

### DistinctPids / distinctpids_200x200

#### Original

| Run | ns/op | ns/min | ns/p90 | ns/mean | stdev | iterations |
|-----|-------|--------|--------|---------|-------|------------|
| 1 | 4425.656 | 4369.645 | 4474.908 | 4423.052 | 35.600 | 92160 |
| | rounds: [4369.645, 4402.675, 4474.908, 4425.656, 4442.377] | | | | | |
| 2 | 4465.120 | 4439.838 | 4553.651 | 4482.463 | 38.987 | 92160 |
| | rounds: [4553.651, 4439.838, 4465.120, 4490.115, 4463.590] | | | | | |
| 3 | 4201.717 | 4156.404 | 4245.045 | 4204.391 | 30.231 | 97280 |
| | rounds: [4245.045, 4156.404, 4201.717, 4192.712, 4226.079] | | | | | |

#### Optimized

| Run | ns/op | ns/min | ns/p90 | ns/mean | stdev | iterations |
|-----|-------|--------|--------|---------|-------|------------|
| 1 | 314.884 | 305.722 | 317.066 | 312.561 | 4.148 | 1283072 |
| | rounds: [317.066, 305.722, 315.174, 309.962, 314.884] | | | | | |
| 2 | 311.363 | 308.152 | 314.599 | 311.396 | 2.293 | 1287168 |
| | rounds: [308.152, 309.774, 313.094, 314.599, 311.363] | | | | | |
| 3 | 308.323 | 302.330 | 317.263 | 309.501 | 5.214 | 1295360 |
| | rounds: [317.263, 313.163, 306.427, 308.323, 302.330] | | | | | |

### GroupByPid / distinctpids_200x200

#### Original

| Run | ns/op | ns/min | ns/p90 | ns/mean | stdev | iterations |
|-----|-------|--------|--------|---------|-------|------------|
| 1 | 13467.529 | 13125.586 | 13573.112 | 13385.260 | 158.981 | 30720 |
| | rounds: [13471.777, 13467.529, 13288.298, 13573.112, 13125.586] | | | | | |
| 2 | 14434.993 | 14019.743 | 14745.085 | 14455.745 | 268.723 | 30720 |
| | rounds: [14730.241, 14745.085, 14434.993, 14348.665, 14019.743] | | | | | |
| 3 | 13810.400 | 13324.837 | 14098.291 | 13754.049 | 258.185 | 30720 |
| | rounds: [13810.400, 13652.197, 13884.521, 14098.291, 13324.837] | | | | | |

#### Optimized

| Run | ns/op | ns/min | ns/p90 | ns/mean | stdev | iterations |
|-----|-------|--------|--------|---------|-------|------------|
| 1 | 8618.789 | 8503.877 | 8717.632 | 8631.669 | 79.716 | 49152 |
| | rounds: [8618.789, 8602.500, 8715.549, 8503.877, 8717.632] | | | | | |
| 2 | 8986.784 | 8814.312 | 9248.372 | 9039.514 | 152.034 | 46080 |
| | rounds: [9248.372, 8986.784, 8814.312, 8984.418, 9163.683] | | | | | |
| 3 | 9291.439 | 9015.668 | 9568.359 | 9282.072 | 183.139 | 46080 |
| | rounds: [9351.866, 9183.030, 9291.439, 9015.668, 9568.359] | | | | | |

### DistinctPids / distinctpids_20kx400

#### Original

| Run | ns/op | ns/min | ns/p90 | ns/mean | stdev | iterations |
|-----|-------|--------|--------|---------|-------|------------|
| 1 | 68832.666 | 67914.697 | 69518.262 | 68750.107 | 652.006 | 10240 |
| | rounds: [68097.998, 67914.697, 68832.666, 69518.262, 69386.914] | | | | | |
| 2 | 70451.904 | 68868.896 | 71152.734 | 70371.855 | 825.878 | 10240 |
| | rounds: [71098.584, 70287.158, 71152.734, 70451.904, 68868.896] | | | | | |
| 3 | 77129.150 | 76284.326 | 80117.139 | 77829.902 | 1465.437 | 9216 |
| | rounds: [76663.721, 80117.139, 76284.326, 78955.176, 77129.150] | | | | | |

#### Optimized

| Run | ns/op | ns/min | ns/p90 | ns/mean | stdev | iterations |
|-----|-------|--------|--------|---------|-------|------------|
| 1 | 19602.197 | 19422.461 | 19656.758 | 19575.473 | 80.162 | 22528 |
| | rounds: [19422.461, 19611.816, 19656.758, 19602.197, 19584.131] | | | | | |
| 2 | 19372.090 | 19249.141 | 19610.938 | 19381.277 | 132.226 | 24576 |
| | rounds: [19610.938, 19418.438, 19255.781, 19249.141, 19372.090] | | | | | |
| 3 | 19323.223 | 19112.090 | 19605.078 | 19333.832 | 178.047 | 24576 |
| | rounds: [19112.090, 19182.305, 19605.078, 19323.223, 19446.465] | | | | | |

### GroupByPid / distinctpids_20kx400

#### Original

| Run | ns/op | ns/min | ns/p90 | ns/mean | stdev | iterations |
|-----|-------|--------|--------|---------|-------|------------|
| 1 | 358411.035 | 353393.164 | 367386.328 | 360719.727 | 5666.942 | 5120 |
| | rounds: [367337.891, 367386.328, 357070.215, 353393.164, 358411.035] | | | | | |
| 2 | 355971.973 | 345848.340 | 364954.883 | 354742.383 | 7084.298 | 5120 |
| | rounds: [359070.996, 364954.883, 347865.723, 355971.973, 345848.340] | | | | | |
| 3 | 350204.980 | 348445.801 | 380162.891 | 357299.434 | 11881.359 | 5120 |
| | rounds: [357744.727, 380162.891, 349938.770, 348445.801, 350204.980] | | | | | |

#### Optimized

| Run | ns/op | ns/min | ns/p90 | ns/mean | stdev | iterations |
|-----|-------|--------|--------|---------|-------|------------|
| 1 | 335654.395 | 319321.680 | 344431.348 | 331092.773 | 10023.566 | 5120 |
| | rounds: [344431.348, 336580.762, 335654.395, 319475.684, 319321.680] | | | | | |
| 2 | 314655.566 | 312926.172 | 340917.285 | 319784.336 | 10645.413 | 5120 |
| | rounds: [340917.285, 313652.637, 314655.566, 316770.020, 312926.172] | | | | | |
| 3 | 337430.078 | 322187.598 | 347067.969 | 334698.086 | 9071.329 | 5120 |
| | rounds: [347067.969, 326653.418, 337430.078, 340151.367, 322187.598] | | | | | |

### ReasmOverlap / reasm_4

#### Original

| Run | ns/op | ns/min | ns/p90 | ns/mean | stdev | iterations |
|-----|-------|--------|--------|---------|-------|------------|
| 1 | 2.964 | 2.905 | 2.980 | 2.954 | 0.026 | 135424000 |
| | rounds: [2.980, 2.965, 2.964, 2.955, 2.905] | | | | | |
| 2 | 2.971 | 2.943 | 3.016 | 2.982 | 0.027 | 134176768 |
| | rounds: [3.016, 2.943, 3.008, 2.971, 2.971] | | | | | |
| 3 | 2.957 | 2.913 | 2.999 | 2.956 | 0.029 | 135323648 |
| | rounds: [2.999, 2.957, 2.971, 2.940, 2.913] | | | | | |

#### Optimized

| Run | ns/op | ns/min | ns/p90 | ns/mean | stdev | iterations |
|-----|-------|--------|--------|---------|-------|------------|
| 1 | 4.014 | 4.011 | 4.137 | 4.047 | 0.049 | 98864128 |
| | rounds: [4.060, 4.011, 4.137, 4.011, 4.014] | | | | | |
| 2 | 3.978 | 3.950 | 4.035 | 3.985 | 0.028 | 100372480 |
| | rounds: [3.988, 4.035, 3.978, 3.950, 3.976] | | | | | |
| 3 | 4.035 | 4.013 | 4.110 | 4.043 | 0.034 | 98933760 |
| | rounds: [4.038, 4.022, 4.013, 4.035, 4.110] | | | | | |

### ReasmOverlap / reasm_64

#### Original

| Run | ns/op | ns/min | ns/p90 | ns/mean | stdev | iterations |
|-----|-------|--------|--------|---------|-------|------------|
| 1 | 10.435 | 10.375 | 10.572 | 10.458 | 0.067 | 38252544 |
| | rounds: [10.572, 10.483, 10.424, 10.435, 10.375] | | | | | |
| 2 | 10.504 | 10.374 | 10.718 | 10.524 | 0.130 | 38016000 |
| | rounds: [10.374, 10.621, 10.718, 10.504, 10.402] | | | | | |
| 3 | 10.267 | 10.116 | 10.511 | 10.299 | 0.146 | 38848512 |
| | rounds: [10.418, 10.511, 10.267, 10.116, 10.184] | | | | | |

#### Optimized

| Run | ns/op | ns/min | ns/p90 | ns/mean | stdev | iterations |
|-----|-------|--------|--------|---------|-------|------------|
| 1 | 12.384 | 12.365 | 12.642 | 12.447 | 0.104 | 32140288 |
| | rounds: [12.377, 12.469, 12.365, 12.642, 12.384] | | | | | |
| 2 | 12.354 | 12.305 | 12.397 | 12.347 | 0.031 | 32397312 |
| | rounds: [12.354, 12.323, 12.397, 12.305, 12.358] | | | | | |
| 3 | 12.473 | 12.449 | 12.589 | 12.488 | 0.051 | 32032768 |
| | rounds: [12.473, 12.449, 12.457, 12.589, 12.474] | | | | | |

### ReasmOverlap / reasm_1024

#### Original

| Run | ns/op | ns/min | ns/p90 | ns/mean | stdev | iterations |
|-----|-------|--------|--------|---------|-------|------------|
| 1 | 115.120 | 113.579 | 117.560 | 115.293 | 1.317 | 3471360 |
| | rounds: [114.608, 115.599, 113.579, 115.120, 117.560] | | | | | |
| 2 | 115.061 | 111.804 | 118.519 | 114.607 | 2.351 | 3492864 |
| | rounds: [111.804, 112.590, 118.519, 115.063, 115.061] | | | | | |
| 3 | 118.121 | 113.187 | 121.535 | 117.572 | 2.777 | 3406848 |
| | rounds: [113.187, 116.225, 118.794, 121.535, 118.121] | | | | | |

#### Optimized

| Run | ns/op | ns/min | ns/p90 | ns/mean | stdev | iterations |
|-----|-------|--------|--------|---------|-------|------------|
| 1 | 131.139 | 130.292 | 132.018 | 131.076 | 0.586 | 3053568 |
| | rounds: [132.018, 130.662, 130.292, 131.139, 131.268] | | | | | |
| 2 | 129.372 | 127.699 | 139.791 | 131.368 | 4.347 | 3049472 |
| | rounds: [129.372, 128.922, 131.056, 127.699, 139.791] | | | | | |
| 3 | 131.499 | 129.815 | 132.903 | 131.314 | 1.262 | 3050496 |
| | rounds: [129.928, 132.423, 132.903, 129.815, 131.499] | | | | | |

### ReasmOverlap / reasm_8192

#### Original

| Run | ns/op | ns/min | ns/p90 | ns/mean | stdev | iterations |
|-----|-------|--------|--------|---------|-------|------------|
| 1 | 1181.244 | 1168.674 | 1214.968 | 1186.919 | 15.783 | 339968 |
| | rounds: [1214.968, 1178.328, 1168.674, 1181.244, 1191.378] | | | | | |
| 2 | 1183.807 | 1169.964 | 1211.106 | 1190.948 | 15.775 | 337920 |
| | rounds: [1211.106, 1169.964, 1183.807, 1182.384, 1207.480] | | | | | |
| 3 | 1207.021 | 1189.123 | 1228.796 | 1206.931 | 12.793 | 332800 |
| | rounds: [1207.021, 1228.796, 1189.123, 1202.165, 1207.548] | | | | | |

#### Optimized

| Run | ns/op | ns/min | ns/p90 | ns/mean | stdev | iterations |
|-----|-------|--------|--------|---------|-------|------------|
| 1 | 1170.642 | 1154.472 | 1199.876 | 1173.294 | 14.865 | 343040 |
| | rounds: [1170.642, 1167.279, 1154.472, 1199.876, 1174.201] | | | | | |
| 2 | 1155.971 | 1153.381 | 1176.765 | 1161.879 | 9.373 | 346112 |
| | rounds: [1153.381, 1169.071, 1155.971, 1154.206, 1176.765] | | | | | |
| 3 | 1168.521 | 1148.147 | 1190.532 | 1170.176 | 14.745 | 345088 |
| | rounds: [1148.147, 1162.376, 1190.532, 1181.305, 1168.521] | | | | | |

### ReasmOverlap / reasm_64_dup

#### Original

| Run | ns/op | ns/min | ns/p90 | ns/mean | stdev | iterations |
|-----|-------|--------|--------|---------|-------|------------|
| 1 | 2.504 | 2.480 | 2.536 | 2.510 | 0.022 | 159406080 |
| | rounds: [2.480, 2.536, 2.493, 2.504, 2.535] | | | | | |
| 2 | 2.534 | 2.518 | 2.569 | 2.541 | 0.020 | 157454336 |
| | rounds: [2.524, 2.518, 2.534, 2.569, 2.558] | | | | | |
| 3 | 2.710 | 2.654 | 2.780 | 2.711 | 0.042 | 147599360 |
| | rounds: [2.710, 2.654, 2.721, 2.780, 2.688] | | | | | |

#### Optimized

| Run | ns/op | ns/min | ns/p90 | ns/mean | stdev | iterations |
|-----|-------|--------|--------|---------|-------|------------|
| 1 | 3.404 | 3.389 | 3.449 | 3.409 | 0.022 | 117346304 |
| | rounds: [3.389, 3.404, 3.449, 3.390, 3.413] | | | | | |
| 2 | 3.358 | 3.342 | 3.410 | 3.366 | 0.026 | 118828032 |
| | rounds: [3.410, 3.342, 3.342, 3.380, 3.358] | | | | | |
| 3 | 3.378 | 3.350 | 3.445 | 3.385 | 0.033 | 118196224 |
| | rounds: [3.378, 3.350, 3.445, 3.388, 3.362] | | | | | |

### HasLowerSubstring / sub_16_present

#### Original

| Run | ns/op | ns/min | ns/p90 | ns/mean | stdev | iterations |
|-----|-------|--------|--------|---------|-------|------------|
| 1 | 9.889 | 9.828 | 10.059 | 9.909 | 0.079 | 40373248 |
| | rounds: [10.059, 9.828, 9.889, 9.900, 9.867] | | | | | |
| 2 | 9.995 | 9.956 | 10.069 | 10.002 | 0.043 | 39995392 |
| | rounds: [10.032, 9.958, 10.069, 9.956, 9.995] | | | | | |
| 3 | 9.954 | 9.870 | 10.079 | 9.980 | 0.074 | 40083456 |
| | rounds: [10.079, 10.046, 9.954, 9.953, 9.870] | | | | | |

#### Optimized

| Run | ns/op | ns/min | ns/p90 | ns/mean | stdev | iterations |
|-----|-------|--------|--------|---------|-------|------------|
| 1 | 8.752 | 8.479 | 8.995 | 8.738 | 0.189 | 45800448 |
| | rounds: [8.479, 8.582, 8.882, 8.752, 8.995] | | | | | |
| 2 | 8.465 | 8.360 | 8.665 | 8.475 | 0.111 | 47209472 |
| | rounds: [8.512, 8.373, 8.360, 8.465, 8.665] | | | | | |
| 3 | 8.643 | 8.416 | 9.038 | 8.651 | 0.226 | 46269440 |
| | rounds: [8.439, 8.721, 8.416, 9.038, 8.643] | | | | | |

### HasLowerSubstring / sub_16_absent

#### Original

| Run | ns/op | ns/min | ns/p90 | ns/mean | stdev | iterations |
|-----|-------|--------|--------|---------|-------|------------|
| 1 | 6.966 | 6.840 | 7.026 | 6.949 | 0.066 | 57571328 |
| | rounds: [6.914, 7.026, 6.999, 6.966, 6.840] | | | | | |
| 2 | 7.096 | 7.006 | 7.206 | 7.102 | 0.064 | 56326144 |
| | rounds: [7.006, 7.119, 7.085, 7.096, 7.206] | | | | | |
| 3 | 7.150 | 6.952 | 7.190 | 7.086 | 0.101 | 56460288 |
| | rounds: [7.150, 7.190, 6.977, 7.163, 6.952] | | | | | |

#### Optimized

| Run | ns/op | ns/min | ns/p90 | ns/mean | stdev | iterations |
|-----|-------|--------|--------|---------|-------|------------|
| 1 | 2.797 | 2.750 | 3.101 | 2.864 | 0.126 | 139907072 |
| | rounds: [2.886, 2.797, 2.750, 3.101, 2.787] | | | | | |
| 2 | 2.746 | 2.722 | 2.761 | 2.742 | 0.015 | 145893376 |
| | rounds: [2.752, 2.728, 2.761, 2.746, 2.722] | | | | | |
| 3 | 2.770 | 2.739 | 3.037 | 2.817 | 0.112 | 142220288 |
| | rounds: [3.037, 2.770, 2.745, 2.794, 2.739] | | | | | |

### HasLowerSubstring / sub_256_absent

#### Original

| Run | ns/op | ns/min | ns/p90 | ns/mean | stdev | iterations |
|-----|-------|--------|--------|---------|-------|------------|
| 1 | 47.315 | 46.972 | 47.696 | 47.333 | 0.318 | 8454144 |
| | rounds: [47.315, 46.991, 47.696, 47.691, 46.972] | | | | | |
| 2 | 47.777 | 47.323 | 47.887 | 47.677 | 0.198 | 8392704 |
| | rounds: [47.777, 47.790, 47.610, 47.323, 47.887] | | | | | |
| 3 | 47.730 | 46.312 | 49.566 | 47.851 | 1.110 | 8366080 |
| | rounds: [48.469, 47.178, 49.566, 46.312, 47.730] | | | | | |

#### Optimized

| Run | ns/op | ns/min | ns/p90 | ns/mean | stdev | iterations |
|-----|-------|--------|--------|---------|-------|------------|
| 1 | 44.933 | 44.489 | 45.850 | 45.119 | 0.480 | 8868864 |
| | rounds: [44.933, 45.462, 45.850, 44.859, 44.489] | | | | | |
| 2 | 44.747 | 44.249 | 45.179 | 44.745 | 0.320 | 8942592 |
| | rounds: [44.578, 44.971, 44.747, 44.249, 45.179] | | | | | |
| 3 | 44.807 | 44.411 | 45.927 | 44.990 | 0.513 | 8895488 |
| | rounds: [45.069, 44.807, 44.736, 44.411, 45.927] | | | | | |

### HasLowerSubstring / sub_4k_absent

#### Original

| Run | ns/op | ns/min | ns/p90 | ns/mean | stdev | iterations |
|-----|-------|--------|--------|---------|-------|------------|
| 1 | 682.540 | 678.708 | 698.952 | 687.430 | 7.955 | 584704 |
| | rounds: [694.857, 682.540, 678.708, 698.952, 682.093] | | | | | |
| 2 | 697.227 | 685.985 | 699.643 | 693.729 | 6.039 | 578560 |
| | rounds: [686.822, 697.227, 685.985, 699.643, 698.968] | | | | | |
| 3 | 684.475 | 675.761 | 711.259 | 687.811 | 12.565 | 584704 |
| | rounds: [711.259, 684.475, 678.743, 688.819, 675.761] | | | | | |

#### Optimized

| Run | ns/op | ns/min | ns/p90 | ns/mean | stdev | iterations |
|-----|-------|--------|--------|---------|-------|------------|
| 1 | 699.197 | 683.389 | 721.474 | 699.128 | 12.860 | 575488 |
| | rounds: [721.474, 699.197, 683.389, 690.425, 701.154] | | | | | |
| 2 | 685.296 | 672.696 | 700.903 | 685.112 | 9.259 | 586752 |
| | rounds: [680.266, 672.696, 686.399, 685.296, 700.903] | | | | | |
| 3 | 695.525 | 686.922 | 698.335 | 693.165 | 4.360 | 579584 |
| | rounds: [695.525, 689.134, 698.335, 695.909, 686.922] | | | | | |

### HasLowerSubstring / sub_4k_present

#### Original

| Run | ns/op | ns/min | ns/p90 | ns/mean | stdev | iterations |
|-----|-------|--------|--------|---------|-------|------------|
| 1 | 961.026 | 959.699 | 966.363 | 962.106 | 2.541 | 418816 |
| | rounds: [961.026, 966.363, 959.863, 959.699, 963.580] | | | | | |
| 2 | 965.606 | 961.504 | 981.731 | 969.705 | 8.086 | 415744 |
| | rounds: [961.504, 962.836, 965.606, 976.848, 981.731] | | | | | |
| 3 | 966.645 | 936.559 | 985.221 | 964.449 | 15.662 | 416768 |
| | rounds: [967.221, 985.221, 936.559, 966.645, 966.597] | | | | | |

#### Optimized

| Run | ns/op | ns/min | ns/p90 | ns/mean | stdev | iterations |
|-----|-------|--------|--------|---------|-------|------------|
| 1 | 892.027 | 880.540 | 895.930 | 889.665 | 6.089 | 452608 |
| | rounds: [895.930, 884.570, 895.260, 892.027, 880.540] | | | | | |
| 2 | 885.816 | 876.934 | 893.327 | 884.806 | 5.458 | 455680 |
| | rounds: [885.816, 876.934, 881.531, 886.424, 893.327] | | | | | |
| 3 | 904.555 | 890.934 | 914.911 | 904.521 | 7.943 | 445440 |
| | rounds: [909.091, 903.113, 890.934, 914.911, 904.555] | | | | | |

### HasLowerSubstring / sub_64k_absent

#### Original

| Run | ns/op | ns/min | ns/p90 | ns/mean | stdev | iterations |
|-----|-------|--------|--------|---------|-------|------------|
| 1 | 12475.600 | 12326.144 | 12520.564 | 12438.449 | 72.081 | 35840 |
| | rounds: [12486.049, 12475.600, 12383.887, 12326.144, 12520.564] | | | | | |
| 2 | 12495.940 | 12280.971 | 12821.582 | 12525.128 | 191.302 | 35840 |
| | rounds: [12280.971, 12495.940, 12821.582, 12644.657, 12382.492] | | | | | |
| 3 | 12422.614 | 12177.720 | 12700.837 | 12423.619 | 191.341 | 35840 |
| | rounds: [12558.929, 12422.614, 12257.994, 12177.720, 12700.837] | | | | | |

#### Optimized

| Run | ns/op | ns/min | ns/p90 | ns/mean | stdev | iterations |
|-----|-------|--------|--------|---------|-------|------------|
| 1 | 17673.203 | 17100.859 | 17948.496 | 17640.129 | 298.482 | 25600 |
| | rounds: [17673.203, 17100.859, 17948.496, 17878.496, 17599.590] | | | | | |
| 2 | 17606.230 | 16554.199 | 18049.570 | 17463.520 | 550.113 | 25600 |
| | rounds: [18049.570, 17606.230, 16554.199, 17162.559, 17945.039] | | | | | |
| 3 | 18059.258 | 17283.340 | 18616.543 | 17989.211 | 430.078 | 25600 |
| | rounds: [18059.258, 18616.543, 17874.941, 17283.340, 18111.973] | | | | | |

### ParseTcpHeader / tcp_20

#### Original

| Run | ns/op | ns/min | ns/p90 | ns/mean | stdev | iterations |
|-----|-------|--------|--------|---------|-------|------------|
| 1 | 3.380 | 3.378 | 3.412 | 3.386 | 0.013 | 118137856 |
| | rounds: [3.378, 3.412, 3.381, 3.380, 3.378] | | | | | |
| 2 | 3.362 | 3.340 | 3.471 | 3.387 | 0.049 | 118117376 |
| | rounds: [3.414, 3.471, 3.349, 3.340, 3.362] | | | | | |
| 3 | 3.409 | 3.317 | 3.480 | 3.399 | 0.053 | 117706752 |
| | rounds: [3.412, 3.409, 3.317, 3.480, 3.378] | | | | | |

#### Optimized

| Run | ns/op | ns/min | ns/p90 | ns/mean | stdev | iterations |
|-----|-------|--------|--------|---------|-------|------------|
| 1 | 6.565 | 6.486 | 6.704 | 6.580 | 0.076 | 60801024 |
| | rounds: [6.525, 6.486, 6.619, 6.704, 6.565] | | | | | |
| 2 | 6.504 | 6.467 | 6.582 | 6.513 | 0.038 | 61421568 |
| | rounds: [6.493, 6.518, 6.467, 6.582, 6.504] | | | | | |
| 3 | 6.563 | 6.482 | 6.679 | 6.572 | 0.068 | 60876800 |
| | rounds: [6.608, 6.563, 6.526, 6.482, 6.679] | | | | | |

### ParseTcpHeader / tcp_52_payload

#### Original

| Run | ns/op | ns/min | ns/p90 | ns/mean | stdev | iterations |
|-----|-------|--------|--------|---------|-------|------------|
| 1 | 3.398 | 3.373 | 3.432 | 3.399 | 0.023 | 117683200 |
| | rounds: [3.398, 3.377, 3.432, 3.373, 3.417] | | | | | |
| 2 | 3.382 | 3.367 | 3.442 | 3.393 | 0.026 | 117907456 |
| | rounds: [3.382, 3.377, 3.397, 3.367, 3.442] | | | | | |
| 3 | 3.382 | 3.335 | 3.446 | 3.388 | 0.036 | 118089728 |
| | rounds: [3.335, 3.395, 3.446, 3.382, 3.380] | | | | | |

#### Optimized

| Run | ns/op | ns/min | ns/p90 | ns/mean | stdev | iterations |
|-----|-------|--------|--------|---------|-------|------------|
| 1 | 6.554 | 6.515 | 6.578 | 6.554 | 0.021 | 61045760 |
| | rounds: [6.554, 6.515, 6.568, 6.578, 6.553] | | | | | |
| 2 | 6.457 | 6.420 | 6.617 | 6.493 | 0.072 | 61615104 |
| | rounds: [6.457, 6.440, 6.617, 6.420, 6.529] | | | | | |
| 3 | 6.567 | 6.536 | 6.629 | 6.573 | 0.031 | 60855296 |
| | rounds: [6.560, 6.629, 6.574, 6.567, 6.536] | | | | | |

### ParseTcpHeader / tcp_12_invalid

#### Original

| Run | ns/op | ns/min | ns/p90 | ns/mean | stdev | iterations |
|-----|-------|--------|--------|---------|-------|------------|
| 1 | 1.696 | 1.668 | 1.708 | 1.690 | 0.016 | 236761088 |
| | rounds: [1.708, 1.668, 1.674, 1.696, 1.702] | | | | | |
| 2 | 1.710 | 1.692 | 1.728 | 1.710 | 0.013 | 233957376 |
| | rounds: [1.701, 1.692, 1.718, 1.710, 1.728] | | | | | |
| 3 | 1.721 | 1.706 | 1.737 | 1.719 | 0.011 | 232743936 |
| | rounds: [1.722, 1.706, 1.721, 1.737, 1.708] | | | | | |

#### Optimized

| Run | ns/op | ns/min | ns/p90 | ns/mean | stdev | iterations |
|-----|-------|--------|--------|---------|-------|------------|
| 1 | 5.684 | 5.633 | 5.780 | 5.694 | 0.051 | 70257664 |
| | rounds: [5.780, 5.657, 5.633, 5.716, 5.684] | | | | | |
| 2 | 5.657 | 5.621 | 5.692 | 5.659 | 0.024 | 70684672 |
| | rounds: [5.657, 5.649, 5.692, 5.621, 5.677] | | | | | |
| 3 | 5.721 | 5.686 | 5.761 | 5.721 | 0.025 | 69928960 |
| | rounds: [5.732, 5.705, 5.721, 5.761, 5.686] | | | | | |

### FormatBytes / fmt_small

#### Original

| Run | ns/op | ns/min | ns/p90 | ns/mean | stdev | iterations |
|-----|-------|--------|--------|---------|-------|------------|
| 1 | 105.944 | 105.850 | 107.076 | 106.256 | 0.465 | 3767296 |
| | rounds: [105.942, 105.850, 107.076, 105.944, 106.469] | | | | | |
| 2 | 108.780 | 106.372 | 112.627 | 109.126 | 2.124 | 3668992 |
| | rounds: [108.780, 112.627, 106.372, 110.048, 107.805] | | | | | |
| 3 | 105.949 | 104.334 | 106.387 | 105.728 | 0.737 | 3786752 |
| | rounds: [106.267, 105.949, 104.334, 106.387, 105.703] | | | | | |

#### Optimized

| Run | ns/op | ns/min | ns/p90 | ns/mean | stdev | iterations |
|-----|-------|--------|--------|---------|-------|------------|
| 1 | 89.266 | 87.909 | 89.568 | 88.963 | 0.610 | 4499456 |
| | rounds: [89.568, 89.410, 87.909, 88.664, 89.266] | | | | | |
| 2 | 87.686 | 87.108 | 91.040 | 88.205 | 1.435 | 4538368 |
| | rounds: [87.740, 87.108, 91.040, 87.686, 87.450] | | | | | |
| 3 | 88.180 | 87.467 | 90.116 | 88.487 | 0.910 | 4525056 |
| | rounds: [87.467, 88.180, 90.116, 88.726, 87.944] | | | | | |

### FormatBytes / fmt_mb

#### Original

| Run | ns/op | ns/min | ns/p90 | ns/mean | stdev | iterations |
|-----|-------|--------|--------|---------|-------|------------|
| 1 | 181.820 | 178.692 | 183.989 | 181.047 | 2.019 | 2211840 |
| | rounds: [178.692, 181.892, 183.989, 178.842, 181.820] | | | | | |
| 2 | 181.842 | 179.855 | 187.316 | 182.617 | 2.502 | 2194432 |
| | rounds: [181.842, 179.855, 181.638, 182.434, 187.316] | | | | | |
| 3 | 181.728 | 179.194 | 184.880 | 181.665 | 2.026 | 2203648 |
| | rounds: [179.194, 184.880, 181.728, 182.633, 179.891] | | | | | |

#### Optimized

| Run | ns/op | ns/min | ns/p90 | ns/mean | stdev | iterations |
|-----|-------|--------|--------|---------|-------|------------|
| 1 | 183.950 | 182.321 | 190.303 | 186.057 | 3.457 | 2152448 |
| | rounds: [183.950, 190.176, 190.303, 183.537, 182.321] | | | | | |
| 2 | 182.562 | 181.491 | 187.210 | 183.847 | 2.315 | 2178048 |
| | rounds: [181.954, 182.562, 181.491, 187.210, 186.017] | | | | | |
| 3 | 185.329 | 183.222 | 191.754 | 186.117 | 2.974 | 2152448 |
| | rounds: [183.222, 185.329, 186.016, 184.262, 191.754] | | | | | |

### FormatBytes / fmt_gb

#### Original

| Run | ns/op | ns/min | ns/p90 | ns/mean | stdev | iterations |
|-----|-------|--------|--------|---------|-------|------------|
| 1 | 174.447 | 173.348 | 176.936 | 174.888 | 1.251 | 2288640 |
| | rounds: [173.348, 174.126, 174.447, 176.936, 175.580] | | | | | |
| 2 | 177.869 | 173.981 | 178.941 | 176.993 | 1.889 | 2263040 |
| | rounds: [178.941, 173.981, 175.638, 178.534, 177.869] | | | | | |
| 3 | 176.058 | 175.431 | 177.744 | 176.551 | 0.964 | 2268160 |
| | rounds: [175.857, 175.431, 176.058, 177.744, 177.665] | | | | | |

#### Optimized

| Run | ns/op | ns/min | ns/p90 | ns/mean | stdev | iterations |
|-----|-------|--------|--------|---------|-------|------------|
| 1 | 180.171 | 177.709 | 183.102 | 180.234 | 1.857 | 2222080 |
| | rounds: [183.102, 180.171, 178.956, 177.709, 181.231] | | | | | |
| 2 | 179.199 | 177.981 | 181.058 | 179.357 | 1.118 | 2231296 |
| | rounds: [179.199, 181.058, 177.981, 180.116, 178.429] | | | | | |
| 3 | 183.078 | 180.037 | 186.546 | 183.018 | 2.097 | 2188288 |
| | rounds: [186.546, 183.197, 180.037, 183.078, 182.230] | | | | | |

### FormatBytes / fmt_tb

#### Original

| Run | ns/op | ns/min | ns/p90 | ns/mean | stdev | iterations |
|-----|-------|--------|--------|---------|-------|------------|
| 1 | 176.657 | 176.139 | 177.597 | 176.838 | 0.620 | 2265088 |
| | rounds: [176.139, 176.657, 176.259, 177.597, 177.540] | | | | | |
| 2 | 178.591 | 176.438 | 181.386 | 179.021 | 1.926 | 2236416 |
| | rounds: [177.629, 181.386, 181.061, 176.438, 178.591] | | | | | |
| 3 | 176.401 | 174.503 | 180.856 | 177.241 | 2.153 | 2258944 |
| | rounds: [176.248, 174.503, 180.856, 176.401, 178.197] | | | | | |

#### Optimized

| Run | ns/op | ns/min | ns/p90 | ns/mean | stdev | iterations |
|-----|-------|--------|--------|---------|-------|------------|
| 1 | 183.976 | 181.281 | 185.749 | 183.918 | 1.493 | 2178048 |
| | rounds: [181.281, 183.976, 184.820, 185.749, 183.763] | | | | | |
| 2 | 182.768 | 177.674 | 185.025 | 181.607 | 3.193 | 2206720 |
| | rounds: [184.620, 177.948, 177.674, 185.025, 182.768] | | | | | |
| 3 | 180.254 | 180.051 | 184.566 | 181.346 | 1.725 | 2207744 |
| | rounds: [180.254, 184.566, 180.125, 180.051, 181.734] | | | | | |

### KeyOf / key_v4

#### Original

| Run | ns/op | ns/min | ns/p90 | ns/mean | stdev | iterations |
|-----|-------|--------|--------|---------|-------|------------|
| 1 | 2.780 | 2.742 | 2.789 | 2.768 | 0.020 | 144494592 |
| | rounds: [2.789, 2.742, 2.748, 2.780, 2.783] | | | | | |
| 2 | 2.775 | 2.752 | 2.804 | 2.779 | 0.020 | 143942656 |
| | rounds: [2.752, 2.800, 2.775, 2.804, 2.765] | | | | | |
| 3 | 2.938 | 2.895 | 2.945 | 2.928 | 0.019 | 136608768 |
| | rounds: [2.945, 2.944, 2.895, 2.938, 2.920] | | | | | |

#### Optimized

| Run | ns/op | ns/min | ns/p90 | ns/mean | stdev | iterations |
|-----|-------|--------|--------|---------|-------|------------|
| 1 | 3.429 | 3.407 | 3.442 | 3.428 | 0.012 | 116688896 |
| | rounds: [3.425, 3.407, 3.442, 3.437, 3.429] | | | | | |
| 2 | 3.404 | 3.376 | 3.463 | 3.414 | 0.031 | 117178368 |
| | rounds: [3.434, 3.376, 3.392, 3.463, 3.404] | | | | | |
| 3 | 3.438 | 3.425 | 3.461 | 3.441 | 0.014 | 116236288 |
| | rounds: [3.461, 3.453, 3.425, 3.438, 3.430] | | | | | |

### KeyOf / key_v6

#### Original

| Run | ns/op | ns/min | ns/p90 | ns/mean | stdev | iterations |
|-----|-------|--------|--------|---------|-------|------------|
| 1 | 2.768 | 2.739 | 2.789 | 2.763 | 0.018 | 144785408 |
| | rounds: [2.768, 2.739, 2.771, 2.789, 2.748] | | | | | |
| 2 | 2.813 | 2.756 | 2.842 | 2.803 | 0.030 | 142711808 |
| | rounds: [2.842, 2.786, 2.819, 2.756, 2.813] | | | | | |
| 3 | 2.752 | 2.725 | 2.813 | 2.762 | 0.029 | 144865280 |
| | rounds: [2.725, 2.752, 2.813, 2.766, 2.752] | | | | | |

#### Optimized

| Run | ns/op | ns/min | ns/p90 | ns/mean | stdev | iterations |
|-----|-------|--------|--------|---------|-------|------------|
| 1 | 3.621 | 3.599 | 3.729 | 3.641 | 0.046 | 109863936 |
| | rounds: [3.639, 3.620, 3.599, 3.729, 3.621] | | | | | |
| 2 | 3.614 | 3.568 | 3.671 | 3.612 | 0.035 | 110765056 |
| | rounds: [3.620, 3.586, 3.671, 3.614, 3.568] | | | | | |
| 3 | 3.593 | 3.570 | 3.779 | 3.654 | 0.091 | 109533184 |
| | rounds: [3.570, 3.579, 3.750, 3.593, 3.779] | | | | | |

### WideToUtf8 / utf8_ascii

#### Original

| Run | ns/op | ns/min | ns/p90 | ns/mean | stdev | iterations |
|-----|-------|--------|--------|---------|-------|------------|
| 1 | 29.025 | 28.543 | 29.662 | 29.015 | 0.369 | 13791232 |
| | rounds: [28.814, 29.030, 29.662, 29.025, 28.543] | | | | | |
| 2 | 29.258 | 29.019 | 31.178 | 29.631 | 0.788 | 13511680 |
| | rounds: [29.497, 31.178, 29.202, 29.258, 29.019] | | | | | |
| 3 | 28.982 | 28.429 | 29.224 | 28.917 | 0.263 | 13838336 |
| | rounds: [29.005, 28.982, 29.224, 28.945, 28.429] | | | | | |

#### Optimized

| Run | ns/op | ns/min | ns/p90 | ns/mean | stdev | iterations |
|-----|-------|--------|--------|---------|-------|------------|
| 1 | 6.535 | 6.352 | 6.585 | 6.503 | 0.083 | 61518848 |
| | rounds: [6.535, 6.585, 6.561, 6.352, 6.484] | | | | | |
| 2 | 6.398 | 6.352 | 6.526 | 6.414 | 0.059 | 62373888 |
| | rounds: [6.352, 6.383, 6.410, 6.398, 6.526] | | | | | |
| 3 | 6.405 | 6.342 | 6.434 | 6.399 | 0.032 | 62516224 |
| | rounds: [6.405, 6.421, 6.392, 6.342, 6.434] | | | | | |

### WideToUtf8 / utf8_mixed

#### Original

| Run | ns/op | ns/min | ns/p90 | ns/mean | stdev | iterations |
|-----|-------|--------|--------|---------|-------|------------|
| 1 | 245.851 | 241.109 | 247.913 | 245.474 | 2.369 | 1633280 |
| | rounds: [241.109, 247.913, 245.851, 245.319, 247.176] | | | | | |
| 2 | 244.941 | 243.074 | 247.470 | 245.162 | 1.793 | 1636352 |
| | rounds: [244.941, 247.470, 243.382, 246.944, 243.074] | | | | | |
| 3 | 242.703 | 240.612 | 244.793 | 242.742 | 1.353 | 1650688 |
| | rounds: [242.703, 243.247, 242.352, 240.612, 244.793] | | | | | |

#### Optimized

| Run | ns/op | ns/min | ns/p90 | ns/mean | stdev | iterations |
|-----|-------|--------|--------|---------|-------|------------|
| 1 | 118.926 | 116.569 | 126.829 | 119.898 | 3.632 | 3340288 |
| | rounds: [118.926, 126.829, 119.682, 117.484, 116.569] | | | | | |
| 2 | 117.663 | 115.626 | 119.707 | 117.636 | 1.438 | 3402752 |
| | rounds: [115.626, 119.707, 117.663, 116.590, 118.592] | | | | | |
| 3 | 118.278 | 118.111 | 118.902 | 118.441 | 0.309 | 3380224 |
| | rounds: [118.278, 118.204, 118.111, 118.902, 118.712] | | | | | |

### WideToUtf8 / utf8_emoji

#### Original

| Run | ns/op | ns/min | ns/p90 | ns/mean | stdev | iterations |
|-----|-------|--------|--------|---------|-------|------------|
| 1 | 231.698 | 221.352 | 240.878 | 231.812 | 7.058 | 1729536 |
| | rounds: [240.878, 237.871, 231.698, 227.261, 221.352] | | | | | |
| 2 | 238.137 | 232.245 | 240.885 | 236.848 | 3.273 | 1692672 |
| | rounds: [239.149, 233.825, 238.137, 232.245, 240.885] | | | | | |
| 3 | 228.003 | 226.574 | 239.675 | 230.775 | 4.805 | 1735680 |
| | rounds: [239.675, 227.686, 226.574, 231.936, 228.003] | | | | | |

#### Optimized

| Run | ns/op | ns/min | ns/p90 | ns/mean | stdev | iterations |
|-----|-------|--------|--------|---------|-------|------------|
| 1 | 106.161 | 101.558 | 114.733 | 107.027 | 4.824 | 3747840 |
| | rounds: [101.558, 102.780, 106.161, 114.733, 109.905] | | | | | |
| 2 | 105.018 | 102.153 | 106.571 | 104.775 | 1.457 | 3820544 |
| | rounds: [106.571, 105.443, 104.693, 102.153, 105.018] | | | | | |
| 3 | 103.286 | 102.103 | 105.216 | 103.514 | 1.080 | 3867648 |
| | rounds: [104.151, 103.286, 102.103, 105.216, 102.815] | | | | | |

### WideToUtf8 / utf8_4k

#### Original

| Run | ns/op | ns/min | ns/p90 | ns/mean | stdev | iterations |
|-----|-------|--------|--------|---------|-------|------------|
| 1 | 872.693 | 852.980 | 882.921 | 868.879 | 12.058 | 462848 |
| | rounds: [872.693, 856.486, 852.980, 879.313, 882.921] | | | | | |
| 2 | 874.939 | 868.796 | 883.019 | 874.928 | 5.593 | 458752 |
| | rounds: [868.874, 874.939, 883.019, 879.010, 868.796] | | | | | |
| 3 | 862.532 | 857.107 | 865.245 | 861.452 | 2.839 | 466944 |
| | rounds: [862.880, 859.496, 865.245, 857.107, 862.532] | | | | | |

#### Optimized

| Run | ns/op | ns/min | ns/p90 | ns/mean | stdev | iterations |
|-----|-------|--------|--------|---------|-------|------------|
| 1 | 269.348 | 262.241 | 274.004 | 268.505 | 4.104 | 1492992 |
| | rounds: [262.241, 269.348, 265.817, 271.112, 274.004] | | | | | |
| 2 | 272.882 | 265.802 | 276.588 | 271.563 | 3.928 | 1475584 |
| | rounds: [268.372, 265.802, 276.588, 274.168, 272.882] | | | | | |
| 3 | 269.500 | 265.845 | 270.400 | 268.518 | 1.758 | 1490944 |
| | rounds: [270.400, 269.797, 267.047, 269.500, 265.845] | | | | | |

### GeoRead / be_u32_unaligned

#### Original

| Run | ns/op | ns/min | ns/p90 | ns/mean | stdev | iterations |
|-----|-------|--------|--------|---------|-------|------------|
| 1 | 1.273 | 1.255 | 1.288 | 1.272 | 0.011 | 314457088 |
| | rounds: [1.288, 1.279, 1.255, 1.266, 1.273] | | | | | |
| 2 | 1.284 | 1.275 | 1.317 | 1.289 | 0.016 | 310313984 |
| | rounds: [1.284, 1.317, 1.276, 1.275, 1.294] | | | | | |
| 3 | 1.293 | 1.259 | 1.302 | 1.288 | 0.015 | 310720512 |
| | rounds: [1.302, 1.293, 1.259, 1.286, 1.298] | | | | | |

#### Optimized

| Run | ns/op | ns/min | ns/p90 | ns/mean | stdev | iterations |
|-----|-------|--------|--------|---------|-------|------------|
| 1 | 1.340 | 1.304 | 1.378 | 1.339 | 0.027 | 298949632 |
| | rounds: [1.304, 1.340, 1.315, 1.355, 1.378] | | | | | |
| 2 | 1.327 | 1.317 | 1.396 | 1.340 | 0.029 | 298603520 |
| | rounds: [1.339, 1.396, 1.322, 1.327, 1.317] | | | | | |
| 3 | 1.309 | 1.303 | 1.332 | 1.313 | 0.011 | 304765952 |
| | rounds: [1.309, 1.315, 1.332, 1.303, 1.304] | | | | | |

### GeoRead / be_u64_unaligned

#### Original

| Run | ns/op | ns/min | ns/p90 | ns/mean | stdev | iterations |
|-----|-------|--------|--------|---------|-------|------------|
| 1 | 1.459 | 1.455 | 1.495 | 1.468 | 0.016 | 272458752 |
| | rounds: [1.477, 1.495, 1.455, 1.455, 1.459] | | | | | |
| 2 | 1.482 | 1.464 | 1.493 | 1.481 | 0.010 | 270109696 |
| | rounds: [1.487, 1.482, 1.464, 1.493, 1.478] | | | | | |
| 3 | 1.449 | 1.440 | 1.472 | 1.452 | 0.011 | 275507200 |
| | rounds: [1.449, 1.472, 1.454, 1.444, 1.440] | | | | | |

#### Optimized

| Run | ns/op | ns/min | ns/p90 | ns/mean | stdev | iterations |
|-----|-------|--------|--------|---------|-------|------------|
| 1 | 1.388 | 1.352 | 1.400 | 1.378 | 0.019 | 290298880 |
| | rounds: [1.392, 1.400, 1.360, 1.352, 1.388] | | | | | |
| 2 | 1.340 | 1.315 | 1.374 | 1.344 | 0.020 | 297765888 |
| | rounds: [1.315, 1.336, 1.353, 1.374, 1.340] | | | | | |
| 3 | 1.344 | 1.331 | 1.390 | 1.352 | 0.022 | 295892992 |
| | rounds: [1.334, 1.331, 1.390, 1.362, 1.344] | | | | | |

### GeoRead / be_bytes_14

#### Original

| Run | ns/op | ns/min | ns/p90 | ns/mean | stdev | iterations |
|-----|-------|--------|--------|---------|-------|------------|
| 1 | 1.761 | 1.733 | 1.813 | 1.766 | 0.026 | 226518016 |
| | rounds: [1.761, 1.759, 1.813, 1.764, 1.733] | | | | | |
| 2 | 1.768 | 1.749 | 1.794 | 1.769 | 0.016 | 226077696 |
| | rounds: [1.768, 1.779, 1.758, 1.794, 1.749] | | | | | |
| 3 | 1.751 | 1.744 | 1.781 | 1.757 | 0.013 | 227720192 |
| | rounds: [1.751, 1.781, 1.747, 1.760, 1.744] | | | | | |

#### Optimized

| Run | ns/op | ns/min | ns/p90 | ns/mean | stdev | iterations |
|-----|-------|--------|--------|---------|-------|------------|
| 1 | 2.058 | 2.055 | 2.065 | 2.059 | 0.004 | 194234368 |
| | rounds: [2.058, 2.055, 2.063, 2.056, 2.065] | | | | | |
| 2 | 2.052 | 1.945 | 2.056 | 2.023 | 0.043 | 197777408 |
| | rounds: [1.945, 2.056, 2.052, 2.011, 2.053] | | | | | |
| 3 | 2.031 | 1.997 | 2.086 | 2.032 | 0.030 | 196866048 |
| | rounds: [2.031, 2.012, 2.086, 2.036, 1.997] | | | | | |

### GeoRead / be_bytes_58

#### Original

| Run | ns/op | ns/min | ns/p90 | ns/mean | stdev | iterations |
|-----|-------|--------|--------|---------|-------|------------|
| 1 | 2.339 | 2.267 | 2.401 | 2.336 | 0.057 | 171308032 |
| | rounds: [2.401, 2.278, 2.339, 2.397, 2.267] | | | | | |
| 2 | 2.123 | 2.103 | 2.192 | 2.143 | 0.034 | 186682368 |
| | rounds: [2.122, 2.103, 2.123, 2.175, 2.192] | | | | | |
| 3 | 2.130 | 2.099 | 2.169 | 2.128 | 0.025 | 187963392 |
| | rounds: [2.099, 2.141, 2.169, 2.130, 2.104] | | | | | |

#### Optimized

| Run | ns/op | ns/min | ns/p90 | ns/mean | stdev | iterations |
|-----|-------|--------|--------|---------|-------|------------|
| 1 | 1.311 | 1.303 | 1.315 | 1.310 | 0.005 | 305390592 |
| | rounds: [1.306, 1.314, 1.311, 1.303, 1.315] | | | | | |
| 2 | 1.405 | 1.399 | 1.427 | 1.410 | 0.010 | 283769856 |
| | rounds: [1.427, 1.402, 1.415, 1.405, 1.399] | | | | | |
| 3 | 1.496 | 1.481 | 1.517 | 1.496 | 0.012 | 267310080 |
| | rounds: [1.517, 1.490, 1.498, 1.481, 1.496] | | | | | |

### TlsBe / tls_rec_hdr

#### Original

| Run | ns/op | ns/min | ns/p90 | ns/mean | stdev | iterations |
|-----|-------|--------|--------|---------|-------|------------|
| 1 | 1.737 | 1.723 | 1.749 | 1.735 | 0.010 | 230553600 |
| | rounds: [1.749, 1.737, 1.743, 1.723, 1.723] | | | | | |
| 2 | 1.734 | 1.722 | 1.761 | 1.737 | 0.014 | 230288384 |
| | rounds: [1.725, 1.761, 1.745, 1.722, 1.734] | | | | | |
| 3 | 1.737 | 1.721 | 1.768 | 1.740 | 0.016 | 229872640 |
| | rounds: [1.729, 1.768, 1.737, 1.721, 1.746] | | | | | |

#### Optimized

| Run | ns/op | ns/min | ns/p90 | ns/mean | stdev | iterations |
|-----|-------|--------|--------|---------|-------|------------|
| 1 | 1.729 | 1.715 | 1.757 | 1.735 | 0.016 | 230557696 |
| | rounds: [1.729, 1.757, 1.749, 1.715, 1.725] | | | | | |
| 2 | 1.738 | 1.725 | 1.749 | 1.738 | 0.009 | 230127616 |
| | rounds: [1.738, 1.734, 1.725, 1.746, 1.749] | | | | | |
| 3 | 1.732 | 1.727 | 1.753 | 1.738 | 0.011 | 230149120 |
| | rounds: [1.727, 1.753, 1.729, 1.749, 1.732] | | | | | |

### TlsBe / tls_u24

#### Original

| Run | ns/op | ns/min | ns/p90 | ns/mean | stdev | iterations |
|-----|-------|--------|--------|---------|-------|------------|
| 1 | 1.273 | 1.263 | 1.285 | 1.274 | 0.007 | 314088448 |
| | rounds: [1.278, 1.263, 1.269, 1.273, 1.285] | | | | | |
| 2 | 1.285 | 1.283 | 1.325 | 1.297 | 0.017 | 308501504 |
| | rounds: [1.285, 1.325, 1.284, 1.283, 1.307] | | | | | |
| 3 | 1.283 | 1.270 | 1.309 | 1.285 | 0.013 | 311206912 |
| | rounds: [1.270, 1.289, 1.277, 1.283, 1.309] | | | | | |

#### Optimized

| Run | ns/op | ns/min | ns/p90 | ns/mean | stdev | iterations |
|-----|-------|--------|--------|---------|-------|------------|
| 1 | 1.304 | 1.277 | 1.390 | 1.313 | 0.041 | 305038336 |
| | rounds: [1.277, 1.304, 1.310, 1.390, 1.282] | | | | | |
| 2 | 1.303 | 1.269 | 1.339 | 1.305 | 0.023 | 306550784 |
| | rounds: [1.302, 1.303, 1.313, 1.339, 1.269] | | | | | |
| 3 | 1.298 | 1.291 | 1.310 | 1.300 | 0.007 | 307756032 |
| | rounds: [1.310, 1.295, 1.291, 1.298, 1.304] | | | | | |

### FindSubstringLong / findlong_8

#### Original

| Run | ns/op | ns/min | ns/p90 | ns/mean | stdev | iterations |
|-----|-------|--------|--------|---------|-------|------------|
| 1 | 680.329 | 677.803 | 681.344 | 679.955 | 1.263 | 589824 |
| | rounds: [680.329, 680.924, 679.374, 681.344, 677.803] | | | | | |
| 2 | 688.579 | 683.523 | 704.081 | 690.793 | 7.001 | 581632 |
| | rounds: [683.523, 687.569, 690.210, 688.579, 704.081] | | | | | |
| 3 | 692.273 | 671.119 | 699.125 | 686.569 | 10.359 | 584704 |
| | rounds: [692.366, 699.125, 677.961, 692.273, 671.119] | | | | | |

#### Optimized

| Run | ns/op | ns/min | ns/p90 | ns/mean | stdev | iterations |
|-----|-------|--------|--------|---------|-------|------------|
| 1 | 1614.973 | 1600.213 | 1629.848 | 1614.724 | 11.377 | 249856 |
| | rounds: [1624.478, 1604.110, 1600.213, 1614.973, 1629.848] | | | | | |
| 2 | 1601.258 | 1599.727 | 1608.382 | 1602.749 | 3.041 | 250880 |
| | rounds: [1608.382, 1599.727, 1601.068, 1601.258, 1603.308] | | | | | |
| 3 | 1610.585 | 1598.966 | 1635.016 | 1616.363 | 13.515 | 248832 |
| | rounds: [1610.585, 1629.091, 1598.966, 1608.157, 1635.016] | | | | | |

### FindSubstringLong / findlong_16

#### Original

| Run | ns/op | ns/min | ns/p90 | ns/mean | stdev | iterations |
|-----|-------|--------|--------|---------|-------|------------|
| 1 | 764.163 | 755.049 | 774.999 | 766.033 | 7.407 | 524288 |
| | rounds: [755.049, 764.163, 773.555, 762.397, 774.999] | | | | | |
| 2 | 772.867 | 772.207 | 784.987 | 776.162 | 4.978 | 519168 |
| | rounds: [772.207, 778.399, 772.349, 784.987, 772.867] | | | | | |
| 3 | 766.217 | 755.147 | 767.422 | 764.007 | 4.502 | 526336 |
| | rounds: [766.353, 767.422, 755.147, 766.217, 764.898] | | | | | |

#### Optimized

| Run | ns/op | ns/min | ns/p90 | ns/mean | stdev | iterations |
|-----|-------|--------|--------|---------|-------|------------|
| 1 | 912.297 | 909.511 | 923.041 | 915.690 | 5.504 | 438272 |
| | rounds: [912.297, 909.511, 921.557, 923.041, 912.044] | | | | | |
| 2 | 920.484 | 910.563 | 926.055 | 918.508 | 5.680 | 438272 |
| | rounds: [920.484, 926.055, 913.473, 921.965, 910.563] | | | | | |
| 3 | 920.864 | 906.790 | 925.083 | 917.892 | 6.442 | 438272 |
| | rounds: [921.773, 914.949, 925.083, 906.790, 920.864] | | | | | |

### FindSubstringLong / findlong_32_absent

#### Original

| Run | ns/op | ns/min | ns/p90 | ns/mean | stdev | iterations |
|-----|-------|--------|--------|---------|-------|------------|
| 1 | 678.664 | 668.094 | 687.950 | 678.291 | 6.508 | 592896 |
| | rounds: [681.053, 687.950, 668.094, 675.695, 678.664] | | | | | |
| 2 | 670.402 | 665.654 | 694.880 | 675.666 | 10.268 | 594944 |
| | rounds: [665.654, 670.402, 677.034, 694.880, 670.359] | | | | | |
| 3 | 676.073 | 662.474 | 682.588 | 673.574 | 8.448 | 595968 |
| | rounds: [664.821, 676.073, 681.911, 682.588, 662.474] | | | | | |

#### Optimized

| Run | ns/op | ns/min | ns/p90 | ns/mean | stdev | iterations |
|-----|-------|--------|--------|---------|-------|------------|
| 1 | 392.229 | 387.545 | 398.409 | 392.701 | 3.904 | 1021952 |
| | rounds: [398.409, 389.760, 395.561, 392.229, 387.545] | | | | | |
| 2 | 387.275 | 382.960 | 395.712 | 388.597 | 4.452 | 1032192 |
| | rounds: [387.275, 391.252, 395.712, 385.785, 382.960] | | | | | |
| 3 | 391.353 | 389.753 | 395.558 | 392.147 | 1.951 | 1021952 |
| | rounds: [395.558, 392.750, 391.353, 389.753, 391.322] | | | | | |

### FindSubstringLong / findlong_32_present

#### Original

| Run | ns/op | ns/min | ns/p90 | ns/mean | stdev | iterations |
|-----|-------|--------|--------|---------|-------|------------|
| 1 | 744.480 | 735.451 | 749.296 | 743.564 | 4.602 | 540672 |
| | rounds: [745.924, 749.296, 744.480, 742.670, 735.451] | | | | | |
| 2 | 760.253 | 743.255 | 772.932 | 757.629 | 10.290 | 531456 |
| | rounds: [743.255, 749.710, 761.996, 760.253, 772.932] | | | | | |
| 3 | 750.867 | 742.624 | 754.972 | 749.093 | 4.739 | 536576 |
| | rounds: [750.867, 752.502, 742.624, 744.501, 754.972] | | | | | |

#### Optimized

| Run | ns/op | ns/min | ns/p90 | ns/mean | stdev | iterations |
|-----|-------|--------|--------|---------|-------|------------|
| 1 | 471.607 | 464.735 | 473.281 | 469.769 | 3.444 | 855040 |
| | rounds: [464.735, 466.593, 472.628, 471.607, 473.281] | | | | | |
| 2 | 471.138 | 466.518 | 476.773 | 471.248 | 3.317 | 850944 |
| | rounds: [466.518, 471.866, 469.942, 476.773, 471.138] | | | | | |
| 3 | 472.360 | 469.709 | 479.937 | 473.071 | 3.656 | 848896 |
| | rounds: [469.709, 470.265, 472.360, 473.082, 479.937] | | | | | |

### BuildLowerAll / lowerall_10

#### Original

| Run | ns/op | ns/min | ns/p90 | ns/mean | stdev | iterations |
|-----|-------|--------|--------|---------|-------|------------|
| 1 | 274.478 | 271.659 | 276.657 | 274.424 | 1.768 | 1460224 |
| | rounds: [275.863, 271.659, 276.657, 274.478, 273.464] | | | | | |
| 2 | 277.879 | 275.125 | 281.417 | 278.325 | 2.619 | 1439744 |
| | rounds: [281.261, 281.417, 277.879, 275.125, 275.945] | | | | | |
| 3 | 263.017 | 260.729 | 269.842 | 264.261 | 3.381 | 1516544 |
| | rounds: [260.729, 266.272, 261.443, 263.017, 269.842] | | | | | |

#### Optimized

| Run | ns/op | ns/min | ns/p90 | ns/mean | stdev | iterations |
|-----|-------|--------|--------|---------|-------|------------|
| 1 | 60.817 | 60.429 | 61.432 | 60.846 | 0.347 | 6576128 |
| | rounds: [60.817, 61.432, 60.429, 60.583, 60.971] | | | | | |
| 2 | 59.994 | 59.367 | 60.965 | 60.129 | 0.667 | 6656000 |
| | rounds: [60.965, 60.840, 59.479, 59.367, 59.994] | | | | | |
| 3 | 60.158 | 59.312 | 60.681 | 60.116 | 0.461 | 6657024 |
| | rounds: [60.409, 60.681, 60.158, 60.022, 59.312] | | | | | |

### CompareWide / cmpwide_eq_256

#### Original

| Run | ns/op | ns/min | ns/p90 | ns/mean | stdev | iterations |
|-----|-------|--------|--------|---------|-------|------------|
| 1 | 16.040 | 15.884 | 16.251 | 16.074 | 0.124 | 24889344 |
| | rounds: [16.040, 16.037, 16.157, 15.884, 16.251] | | | | | |
| 2 | 16.201 | 15.996 | 16.626 | 16.253 | 0.206 | 24617984 |
| | rounds: [15.996, 16.626, 16.201, 16.254, 16.187] | | | | | |
| 3 | 15.943 | 15.516 | 16.424 | 15.958 | 0.307 | 25077760 |
| | rounds: [16.124, 15.785, 15.943, 15.516, 16.424] | | | | | |

#### Optimized

| Run | ns/op | ns/min | ns/p90 | ns/mean | stdev | iterations |
|-----|-------|--------|--------|---------|-------|------------|
| 1 | 17.079 | 15.752 | 17.628 | 16.942 | 0.686 | 23654400 |
| | rounds: [17.628, 16.689, 17.560, 15.752, 17.079] | | | | | |
| 2 | 16.798 | 16.315 | 17.605 | 16.963 | 0.518 | 23604224 |
| | rounds: [16.566, 16.798, 17.533, 17.605, 16.315] | | | | | |
| 3 | 17.343 | 16.446 | 17.628 | 17.108 | 0.464 | 23400448 |
| | rounds: [17.453, 17.628, 17.343, 16.669, 16.446] | | | | | |

### CompareWide / cmpwide_late_256

#### Original

| Run | ns/op | ns/min | ns/p90 | ns/mean | stdev | iterations |
|-----|-------|--------|--------|---------|-------|------------|
| 1 | 18.247 | 17.857 | 18.630 | 18.226 | 0.251 | 21952512 |
| | rounds: [18.247, 18.278, 18.118, 18.630, 17.857] | | | | | |
| 2 | 19.327 | 18.603 | 19.391 | 19.070 | 0.367 | 20984832 |
| | rounds: [18.641, 19.327, 18.603, 19.391, 19.389] | | | | | |
| 3 | 18.315 | 17.656 | 19.512 | 18.426 | 0.610 | 21734400 |
| | rounds: [17.656, 18.160, 19.512, 18.315, 18.485] | | | | | |

#### Optimized

| Run | ns/op | ns/min | ns/p90 | ns/mean | stdev | iterations |
|-----|-------|--------|--------|---------|-------|------------|
| 1 | 18.468 | 18.305 | 19.065 | 18.626 | 0.283 | 21483520 |
| | rounds: [18.468, 18.845, 18.449, 18.305, 19.065] | | | | | |
| 2 | 18.648 | 17.757 | 18.797 | 18.343 | 0.465 | 21824512 |
| | rounds: [18.797, 18.714, 17.797, 17.757, 18.648] | | | | | |
| 3 | 18.144 | 17.373 | 19.005 | 18.165 | 0.542 | 22042624 |
| | rounds: [19.005, 18.409, 17.893, 18.144, 17.373] | | | | | |

### CompareWide / cmpwide_short

#### Original

| Run | ns/op | ns/min | ns/p90 | ns/mean | stdev | iterations |
|-----|-------|--------|--------|---------|-------|------------|
| 1 | 2.527 | 2.497 | 2.556 | 2.524 | 0.022 | 158488576 |
| | rounds: [2.497, 2.539, 2.527, 2.502, 2.556] | | | | | |
| 2 | 2.549 | 2.521 | 2.581 | 2.549 | 0.020 | 156953600 |
| | rounds: [2.537, 2.557, 2.521, 2.549, 2.581] | | | | | |
| 3 | 2.556 | 2.520 | 2.598 | 2.555 | 0.031 | 156561408 |
| | rounds: [2.598, 2.556, 2.522, 2.520, 2.579] | | | | | |

#### Optimized

| Run | ns/op | ns/min | ns/p90 | ns/mean | stdev | iterations |
|-----|-------|--------|--------|---------|-------|------------|
| 1 | 2.344 | 2.306 | 2.393 | 2.344 | 0.030 | 170871808 |
| | rounds: [2.393, 2.320, 2.344, 2.360, 2.306] | | | | | |
| 2 | 2.338 | 2.334 | 2.608 | 2.391 | 0.108 | 167607296 |
| | rounds: [2.608, 2.335, 2.338, 2.341, 2.334] | | | | | |
| 3 | 2.353 | 2.320 | 2.391 | 2.356 | 0.024 | 169840640 |
| | rounds: [2.353, 2.391, 2.320, 2.368, 2.345] | | | | | |

### ConnKey / keyhash_22

#### Original

| Run | ns/op | ns/min | ns/p90 | ns/mean | stdev | iterations |
|-----|-------|--------|--------|---------|-------|------------|
| 1 | 7.180 | 7.115 | 7.264 | 7.195 | 0.053 | 55601152 |
| | rounds: [7.180, 7.240, 7.115, 7.174, 7.264] | | | | | |
| 2 | 7.309 | 7.245 | 7.345 | 7.299 | 0.039 | 54803456 |
| | rounds: [7.245, 7.264, 7.309, 7.333, 7.345] | | | | | |
| 3 | 7.241 | 7.185 | 7.302 | 7.245 | 0.038 | 55216128 |
| | rounds: [7.238, 7.185, 7.241, 7.258, 7.302] | | | | | |

#### Optimized

| Run | ns/op | ns/min | ns/p90 | ns/mean | stdev | iterations |
|-----|-------|--------|--------|---------|-------|------------|
| 1 | 2.453 | 2.394 | 2.477 | 2.438 | 0.030 | 164073472 |
| | rounds: [2.453, 2.394, 2.477, 2.412, 2.456] | | | | | |
| 2 | 2.438 | 2.418 | 2.540 | 2.470 | 0.053 | 162013184 |
| | rounds: [2.418, 2.438, 2.540, 2.528, 2.426] | | | | | |
| 3 | 2.465 | 2.393 | 2.541 | 2.461 | 0.048 | 162580480 |
| | rounds: [2.469, 2.465, 2.439, 2.541, 2.393] | | | | | |

### ConnKey / keyhash_46

#### Original

| Run | ns/op | ns/min | ns/p90 | ns/mean | stdev | iterations |
|-----|-------|--------|--------|---------|-------|------------|
| 1 | 19.308 | 18.915 | 19.787 | 19.324 | 0.280 | 20707328 |
| | rounds: [19.241, 19.308, 18.915, 19.787, 19.368] | | | | | |
| 2 | 20.214 | 19.706 | 20.358 | 20.098 | 0.247 | 19907584 |
| | rounds: [20.358, 19.706, 20.293, 19.920, 20.214] | | | | | |
| 3 | 19.554 | 18.970 | 20.464 | 19.762 | 0.558 | 20259840 |
| | rounds: [18.970, 19.494, 20.464, 19.554, 20.329] | | | | | |

#### Optimized

| Run | ns/op | ns/min | ns/p90 | ns/mean | stdev | iterations |
|-----|-------|--------|--------|---------|-------|------------|
| 1 | 3.295 | 3.247 | 3.353 | 3.293 | 0.037 | 121467904 |
| | rounds: [3.295, 3.353, 3.309, 3.247, 3.264] | | | | | |
| 2 | 3.264 | 3.194 | 3.330 | 3.260 | 0.044 | 122710016 |
| | rounds: [3.330, 3.241, 3.194, 3.274, 3.264] | | | | | |
| 3 | 3.202 | 3.145 | 3.326 | 3.220 | 0.061 | 124276736 |
| | rounds: [3.190, 3.145, 3.235, 3.326, 3.202] | | | | | |

### ConnKey / keyeq_46

#### Original

| Run | ns/op | ns/min | ns/p90 | ns/mean | stdev | iterations |
|-----|-------|--------|--------|---------|-------|------------|
| 1 | 3.862 | 3.756 | 3.929 | 3.858 | 0.062 | 103717888 |
| | rounds: [3.830, 3.862, 3.756, 3.929, 3.912] | | | | | |
| 2 | 3.824 | 3.773 | 3.887 | 3.830 | 0.038 | 104446976 |
| | rounds: [3.815, 3.824, 3.852, 3.887, 3.773] | | | | | |
| 3 | 3.886 | 3.713 | 3.950 | 3.867 | 0.081 | 103493632 |
| | rounds: [3.886, 3.950, 3.713, 3.881, 3.905] | | | | | |

#### Optimized

| Run | ns/op | ns/min | ns/p90 | ns/mean | stdev | iterations |
|-----|-------|--------|--------|---------|-------|------------|
| 1 | 3.981 | 3.925 | 4.112 | 4.002 | 0.062 | 99975168 |
| | rounds: [3.925, 3.981, 4.112, 3.975, 4.017] | | | | | |
| 2 | 4.007 | 3.934 | 4.044 | 3.997 | 0.036 | 100076544 |
| | rounds: [3.993, 4.044, 3.934, 4.007, 4.009] | | | | | |
| 3 | 3.981 | 3.942 | 4.157 | 4.006 | 0.077 | 99926016 |
| | rounds: [3.967, 3.983, 3.981, 3.942, 4.157] | | | | | |

### ConnKey / keyeq_46_diff

#### Original

| Run | ns/op | ns/min | ns/p90 | ns/mean | stdev | iterations |
|-----|-------|--------|--------|---------|-------|------------|
| 1 | 3.947 | 3.877 | 4.074 | 3.963 | 0.073 | 100977664 |
| | rounds: [4.016, 3.900, 3.877, 3.947, 4.074] | | | | | |
| 2 | 4.033 | 3.908 | 4.096 | 4.004 | 0.079 | 99929088 |
| | rounds: [3.914, 4.072, 3.908, 4.096, 4.033] | | | | | |
| 3 | 3.937 | 3.918 | 4.089 | 3.982 | 0.070 | 100480000 |
| | rounds: [3.937, 4.089, 4.044, 3.918, 3.923] | | | | | |

#### Optimized

| Run | ns/op | ns/min | ns/p90 | ns/mean | stdev | iterations |
|-----|-------|--------|--------|---------|-------|------------|
| 1 | 4.170 | 4.082 | 4.254 | 4.160 | 0.065 | 96180224 |
| | rounds: [4.170, 4.199, 4.095, 4.254, 4.082] | | | | | |
| 2 | 4.103 | 4.048 | 4.266 | 4.141 | 0.083 | 96646144 |
| | rounds: [4.266, 4.048, 4.103, 4.211, 4.075] | | | | | |
| 3 | 4.115 | 4.067 | 4.280 | 4.169 | 0.089 | 96001024 |
| | rounds: [4.109, 4.067, 4.272, 4.280, 4.115] | | | | | |

### ComputeBps / bps_1k

#### Original

| Run | ns/op | ns/min | ns/p90 | ns/mean | stdev | iterations |
|-----|-------|--------|--------|---------|-------|------------|
| 1 | 853.053 | 845.460 | 868.436 | 854.976 | 7.726 | 470016 |
| | rounds: [850.730, 845.460, 857.202, 853.053, 868.436] | | | | | |
| 2 | 848.422 | 847.947 | 852.968 | 849.726 | 1.933 | 474112 |
| | rounds: [850.930, 852.968, 847.947, 848.362, 848.422] | | | | | |
| 3 | 853.746 | 848.567 | 861.003 | 854.492 | 5.365 | 471040 |
| | rounds: [848.567, 853.746, 861.003, 848.835, 860.311] | | | | | |

#### Optimized

| Run | ns/op | ns/min | ns/p90 | ns/mean | stdev | iterations |
|-----|-------|--------|--------|---------|-------|------------|
| 1 | 516.449 | 511.862 | 518.575 | 515.683 | 2.799 | 778240 |
| | rounds: [516.449, 518.558, 518.575, 511.862, 512.969] | | | | | |
| 2 | 515.836 | 507.988 | 555.144 | 524.536 | 17.411 | 765952 |
| | rounds: [515.836, 532.222, 555.144, 511.490, 507.988] | | | | | |
| 3 | 517.631 | 514.081 | 551.801 | 523.627 | 14.255 | 765952 |
| | rounds: [517.631, 514.535, 514.081, 551.801, 520.086] | | | | | |

### SumPidTraffic / pidsum_500_50

#### Original

| Run | ns/op | ns/min | ns/p90 | ns/mean | stdev | iterations |
|-----|-------|--------|--------|---------|-------|------------|
| 1 | 13637.419 | 13422.054 | 13769.010 | 13633.431 | 117.183 | 30720 |
| | rounds: [13422.054, 13708.089, 13769.010, 13630.583, 13637.419] | | | | | |
| 2 | 13456.494 | 13287.598 | 14306.445 | 13631.201 | 374.264 | 30720 |
| | rounds: [14306.445, 13348.389, 13757.080, 13456.494, 13287.598] | | | | | |
| 3 | 13432.536 | 13269.352 | 13588.851 | 13433.099 | 106.914 | 30720 |
| | rounds: [13588.851, 13492.350, 13269.352, 13432.536, 13382.406] | | | | | |

#### Optimized

| Run | ns/op | ns/min | ns/p90 | ns/mean | stdev | iterations |
|-----|-------|--------|--------|---------|-------|------------|
| 1 | 3705.438 | 3653.476 | 3749.628 | 3704.046 | 34.462 | 111616 |
| | rounds: [3680.402, 3731.286, 3653.476, 3705.438, 3749.628] | | | | | |
| 2 | 3744.173 | 3681.734 | 3763.472 | 3727.461 | 36.355 | 109568 |
| | rounds: [3762.151, 3681.734, 3763.472, 3744.173, 3685.773] | | | | | |
| 3 | 3675.977 | 3630.131 | 3761.328 | 3689.224 | 43.599 | 111616 |
| | rounds: [3671.480, 3630.131, 3707.204, 3675.977, 3761.328] | | | | | |

### SumPidTraffic / pidsum_5k_700

#### Original

| Run | ns/op | ns/min | ns/p90 | ns/mean | stdev | iterations |
|-----|-------|--------|--------|---------|-------|------------|
| 1 | 881505.957 | 880517.383 | 889611.914 | 883011.602 | 3390.970 | 5120 |
| | rounds: [880517.383, 880696.484, 882726.270, 881505.957, 889611.914] | | | | | |
| 2 | 886237.012 | 882751.660 | 888918.652 | 886050.488 | 2332.675 | 5120 |
| | rounds: [882751.660, 888176.758, 886237.012, 884168.359, 888918.652] | | | | | |
| 3 | 882065.137 | 879828.027 | 884989.648 | 882117.832 | 1664.221 | 5120 |
| | rounds: [882065.137, 879828.027, 882180.859, 881525.488, 884989.648] | | | | | |

#### Optimized

| Run | ns/op | ns/min | ns/p90 | ns/mean | stdev | iterations |
|-----|-------|--------|--------|---------|-------|------------|
| 1 | 208690.723 | 205048.340 | 209067.188 | 207730.723 | 1499.422 | 5120 |
| | rounds: [209067.188, 207128.711, 205048.340, 208690.723, 208718.652] | | | | | |
| 2 | 205675.000 | 204401.855 | 207241.992 | 205750.039 | 918.063 | 5120 |
| | rounds: [206000.879, 205430.469, 204401.855, 207241.992, 205675.000] | | | | | |
| 3 | 207956.152 | 205176.855 | 211908.301 | 207776.523 | 2411.316 | 5120 |
| | rounds: [208289.062, 205176.855, 205552.246, 207956.152, 211908.301] | | | | | |

### FormatBpsCell / bpscell_hot

#### Original

| Run | ns/op | ns/min | ns/p90 | ns/mean | stdev | iterations |
|-----|-------|--------|--------|---------|-------|------------|
| 1 | 398.085 | 391.544 | 405.726 | 398.860 | 4.900 | 1006592 |
| | rounds: [391.544, 396.471, 398.085, 402.476, 405.726] | | | | | |
| 2 | 401.281 | 398.652 | 410.376 | 402.689 | 4.005 | 996352 |
| | rounds: [398.652, 401.281, 401.967, 401.172, 410.376] | | | | | |
| 3 | 395.275 | 394.144 | 399.289 | 396.068 | 1.925 | 1012736 |
| | rounds: [395.275, 394.144, 397.178, 394.457, 399.289] | | | | | |

#### Optimized

| Run | ns/op | ns/min | ns/p90 | ns/mean | stdev | iterations |
|-----|-------|--------|--------|---------|-------|------------|
| 1 | 332.676 | 330.423 | 335.893 | 333.246 | 1.865 | 1202176 |
| | rounds: [335.893, 332.667, 330.423, 332.676, 334.570] | | | | | |
| 2 | 329.683 | 324.395 | 330.798 | 328.244 | 2.391 | 1220608 |
| | rounds: [329.781, 330.798, 324.395, 326.563, 329.683] | | | | | |
| 3 | 332.427 | 323.189 | 338.055 | 330.584 | 5.840 | 1213440 |
| | rounds: [323.189, 324.392, 332.427, 334.856, 338.055] | | | | | |

### FormatBpsCell / bpscell_idle

#### Original

| Run | ns/op | ns/min | ns/p90 | ns/mean | stdev | iterations |
|-----|-------|--------|--------|---------|-------|------------|
| 1 | 2.110 | 2.091 | 2.125 | 2.106 | 0.013 | 189935616 |
| | rounds: [2.110, 2.125, 2.111, 2.091, 2.093] | | | | | |
| 2 | 2.131 | 2.095 | 2.164 | 2.135 | 0.024 | 187400192 |
| | rounds: [2.154, 2.130, 2.095, 2.131, 2.164] | | | | | |
| 3 | 2.108 | 2.068 | 2.164 | 2.113 | 0.031 | 189379584 |
| | rounds: [2.108, 2.104, 2.068, 2.164, 2.119] | | | | | |

#### Optimized

| Run | ns/op | ns/min | ns/p90 | ns/mean | stdev | iterations |
|-----|-------|--------|--------|---------|-------|------------|
| 1 | 2.540 | 2.512 | 2.594 | 2.541 | 0.028 | 157431808 |
| | rounds: [2.540, 2.594, 2.520, 2.512, 2.540] | | | | | |
| 2 | 2.551 | 2.468 | 2.589 | 2.547 | 0.043 | 157108224 |
| | rounds: [2.551, 2.589, 2.468, 2.582, 2.544] | | | | | |
| 3 | 2.564 | 2.537 | 2.606 | 2.563 | 0.025 | 156073984 |
| | rounds: [2.564, 2.537, 2.606, 2.570, 2.539] | | | | | |

### WidenUtf8 / widen_org_34

#### Original

| Run | ns/op | ns/min | ns/p90 | ns/mean | stdev | iterations |
|-----|-------|--------|--------|---------|-------|------------|
| 1 | 56.742 | 56.091 | 57.070 | 56.628 | 0.336 | 7065600 |
| | rounds: [56.091, 57.070, 56.437, 56.801, 56.742] | | | | | |
| 2 | 54.560 | 54.021 | 55.190 | 54.577 | 0.424 | 7331840 |
| | rounds: [54.021, 54.231, 54.560, 55.190, 54.885] | | | | | |
| 3 | 54.234 | 53.523 | 54.922 | 54.287 | 0.462 | 7371776 |
| | rounds: [53.523, 54.208, 54.547, 54.234, 54.922] | | | | | |

#### Optimized

| Run | ns/op | ns/min | ns/p90 | ns/mean | stdev | iterations |
|-----|-------|--------|--------|---------|-------|------------|
| 1 | 47.321 | 46.692 | 48.141 | 47.329 | 0.479 | 8455168 |
| | rounds: [47.431, 48.141, 47.061, 46.692, 47.321] | | | | | |
| 2 | 47.320 | 47.152 | 48.253 | 47.492 | 0.401 | 8424448 |
| | rounds: [47.320, 47.211, 47.152, 47.522, 48.253] | | | | | |
| 3 | 47.354 | 46.884 | 48.432 | 47.552 | 0.538 | 8415232 |
| | rounds: [46.884, 48.432, 47.354, 47.852, 47.237] | | | | | |

### WidenUtf8 / widen_iso

#### Original

| Run | ns/op | ns/min | ns/p90 | ns/mean | stdev | iterations |
|-----|-------|--------|--------|---------|-------|------------|
| 1 | 4.414 | 4.379 | 4.448 | 4.417 | 0.024 | 90571776 |
| | rounds: [4.414, 4.448, 4.406, 4.379, 4.436] | | | | | |
| 2 | 4.500 | 4.413 | 4.558 | 4.494 | 0.048 | 89020416 |
| | rounds: [4.516, 4.413, 4.558, 4.483, 4.500] | | | | | |
| 3 | 3.992 | 3.980 | 4.099 | 4.025 | 0.048 | 99388416 |
| | rounds: [4.068, 3.980, 4.099, 3.992, 3.988] | | | | | |

#### Optimized

| Run | ns/op | ns/min | ns/p90 | ns/mean | stdev | iterations |
|-----|-------|--------|--------|---------|-------|------------|
| 1 | 4.279 | 4.161 | 4.342 | 4.267 | 0.060 | 93765632 |
| | rounds: [4.342, 4.297, 4.256, 4.279, 4.161] | | | | | |
| 2 | 4.253 | 4.189 | 4.294 | 4.246 | 0.034 | 94217216 |
| | rounds: [4.294, 4.236, 4.257, 4.253, 4.189] | | | | | |
| 3 | 4.295 | 4.226 | 4.354 | 4.288 | 0.046 | 93301760 |
| | rounds: [4.354, 4.316, 4.295, 4.248, 4.226] | | | | | |

### WidenUtf8 / widen_mixed_15

#### Original

| Run | ns/op | ns/min | ns/p90 | ns/mean | stdev | iterations |
|-----|-------|--------|--------|---------|-------|------------|
| 1 | 36.027 | 35.994 | 37.276 | 36.351 | 0.495 | 11011072 |
| | rounds: [36.000, 36.027, 35.994, 37.276, 36.459] | | | | | |
| 2 | 36.425 | 35.912 | 37.021 | 36.480 | 0.420 | 10969088 |
| | rounds: [36.425, 36.883, 37.021, 36.161, 35.912] | | | | | |
| 3 | 36.740 | 35.746 | 36.807 | 36.498 | 0.404 | 10963968 |
| | rounds: [35.746, 36.404, 36.807, 36.740, 36.793] | | | | | |

#### Optimized

| Run | ns/op | ns/min | ns/p90 | ns/mean | stdev | iterations |
|-----|-------|--------|--------|---------|-------|------------|
| 1 | 35.903 | 35.647 | 36.021 | 35.862 | 0.126 | 11155456 |
| | rounds: [35.814, 36.021, 35.647, 35.925, 35.903] | | | | | |
| 2 | 35.698 | 35.489 | 36.385 | 35.779 | 0.320 | 11184128 |
| | rounds: [35.489, 35.698, 35.770, 35.550, 36.385] | | | | | |
| 3 | 35.988 | 35.371 | 36.550 | 35.960 | 0.468 | 11127808 |
| | rounds: [36.390, 35.498, 35.371, 35.988, 36.550] | | | | | |

### IsGlobalV4 / unicast_v4_mixed

#### Original

| Run | ns/op | ns/min | ns/p90 | ns/mean | stdev | iterations |
|-----|-------|--------|--------|---------|-------|------------|
| 1 | 1.953 | 1.931 | 2.006 | 1.959 | 0.025 | 204214272 |
| | rounds: [1.931, 1.953, 2.006, 1.961, 1.945] | | | | | |
| 2 | 1.981 | 1.939 | 1.991 | 1.971 | 0.019 | 202927104 |
| | rounds: [1.939, 1.991, 1.981, 1.962, 1.984] | | | | | |
| 3 | 1.976 | 1.945 | 1.983 | 1.972 | 0.014 | 202881024 |
| | rounds: [1.983, 1.976, 1.981, 1.945, 1.974] | | | | | |

#### Optimized

| Run | ns/op | ns/min | ns/p90 | ns/mean | stdev | iterations |
|-----|-------|--------|--------|---------|-------|------------|
| 1 | 1.593 | 1.577 | 1.613 | 1.594 | 0.012 | 250911744 |
| | rounds: [1.577, 1.599, 1.613, 1.590, 1.593] | | | | | |
| 2 | 1.594 | 1.561 | 1.608 | 1.586 | 0.017 | 252174336 |
| | rounds: [1.594, 1.574, 1.608, 1.561, 1.595] | | | | | |
| 3 | 1.583 | 1.581 | 1.603 | 1.588 | 0.009 | 251907072 |
| | rounds: [1.603, 1.581, 1.581, 1.583, 1.593] | | | | | |

### IsGlobalV6 / unicast_v6_mixed

#### Original

| Run | ns/op | ns/min | ns/p90 | ns/mean | stdev | iterations |
|-----|-------|--------|--------|---------|-------|------------|
| 1 | 2.489 | 2.416 | 2.537 | 2.481 | 0.044 | 161283072 |
| | rounds: [2.416, 2.489, 2.447, 2.537, 2.516] | | | | | |
| 2 | 2.522 | 2.417 | 2.555 | 2.491 | 0.054 | 160642048 |
| | rounds: [2.417, 2.526, 2.522, 2.555, 2.436] | | | | | |
| 3 | 2.532 | 2.466 | 2.611 | 2.535 | 0.050 | 157839360 |
| | rounds: [2.565, 2.502, 2.611, 2.466, 2.532] | | | | | |

#### Optimized

| Run | ns/op | ns/min | ns/p90 | ns/mean | stdev | iterations |
|-----|-------|--------|--------|---------|-------|------------|
| 1 | 2.215 | 2.188 | 2.274 | 2.222 | 0.030 | 180027392 |
| | rounds: [2.274, 2.215, 2.200, 2.235, 2.188] | | | | | |
| 2 | 2.133 | 2.114 | 2.190 | 2.144 | 0.030 | 186613760 |
| | rounds: [2.114, 2.133, 2.116, 2.190, 2.166] | | | | | |
| 3 | 2.110 | 2.086 | 2.158 | 2.116 | 0.027 | 189057024 |
| | rounds: [2.134, 2.086, 2.110, 2.158, 2.093] | | | | | |

### PairSnapshot / pair_500

#### Original

| Run | ns/op | ns/min | ns/p90 | ns/mean | stdev | iterations |
|-----|-------|--------|--------|---------|-------|------------|
| 1 | 35458.073 | 35262.923 | 36053.613 | 35604.486 | 292.953 | 15360 |
| | rounds: [35262.923, 35458.073, 35413.509, 36053.613, 35834.310] | | | | | |
| 2 | 35699.837 | 35184.733 | 36652.148 | 35863.171 | 510.523 | 15360 |
| | rounds: [35579.460, 35184.733, 36199.674, 36652.148, 35699.837] | | | | | |
| 3 | 36458.724 | 35730.762 | 36683.496 | 36282.018 | 365.392 | 15360 |
| | rounds: [36683.496, 36560.872, 36458.724, 35730.762, 35976.237] | | | | | |

#### Optimized

| Run | ns/op | ns/min | ns/p90 | ns/mean | stdev | iterations |
|-----|-------|--------|--------|---------|-------|------------|
| 1 | 34295.475 | 33894.466 | 34921.745 | 34366.992 | 363.680 | 15360 |
| | rounds: [34611.751, 33894.466, 34295.475, 34111.523, 34921.745] | | | | | |
| 2 | 35066.960 | 33767.936 | 35398.730 | 34700.130 | 686.396 | 15360 |
| | rounds: [35398.730, 35066.960, 35287.435, 33767.936, 33979.590] | | | | | |
| 3 | 34534.245 | 34039.062 | 35106.022 | 34586.413 | 398.132 | 15360 |
| | rounds: [34534.245, 34039.062, 35106.022, 34296.940, 34955.794] | | | | | |

### PairSnapshot / pair_5k

#### Original

| Run | ns/op | ns/min | ns/p90 | ns/mean | stdev | iterations |
|-----|-------|--------|--------|---------|-------|------------|
| 1 | 503027.344 | 499047.852 | 504727.930 | 502804.258 | 2026.337 | 5120 |
| | rounds: [502780.273, 499047.852, 504727.930, 504437.891, 503027.344] | | | | | |
| 2 | 515478.809 | 508875.684 | 518964.062 | 514411.895 | 3774.677 | 5120 |
| | rounds: [518964.062, 508875.684, 517430.469, 511310.449, 515478.809] | | | | | |
| 3 | 507999.219 | 506423.633 | 514526.172 | 509182.441 | 2827.340 | 5120 |
| | rounds: [514526.172, 507999.219, 507638.867, 509324.316, 506423.633] | | | | | |

#### Optimized

| Run | ns/op | ns/min | ns/p90 | ns/mean | stdev | iterations |
|-----|-------|--------|--------|---------|-------|------------|
| 1 | 579771.582 | 576372.852 | 588064.844 | 580541.914 | 4101.375 | 5120 |
| | rounds: [588064.844, 576372.852, 579771.582, 577489.453, 581010.840] | | | | | |
| 2 | 574380.176 | 571377.246 | 576438.672 | 573965.293 | 1740.452 | 5120 |
| | rounds: [574380.176, 574843.066, 572787.305, 571377.246, 576438.672] | | | | | |
| 3 | 576830.469 | 575488.379 | 588744.629 | 579267.324 | 4936.792 | 5120 |
| | rounds: [579439.746, 575488.379, 576830.469, 575833.398, 588744.629] | | | | | |

### PairSnapshot / pair_mdns

#### Original

| Run | ns/op | ns/min | ns/p90 | ns/mean | stdev | iterations |
|-----|-------|--------|--------|---------|-------|------------|
| 1 | 5340.977 | 5316.641 | 5494.870 | 5377.033 | 63.793 | 76800 |
| | rounds: [5316.641, 5391.699, 5494.870, 5340.977, 5340.977] | | | | | |
| 2 | 5424.720 | 5342.005 | 5591.629 | 5435.183 | 83.904 | 75776 |
| | rounds: [5424.720, 5392.376, 5425.182, 5342.005, 5591.629] | | | | | |
| 3 | 5342.181 | 5331.289 | 5481.003 | 5372.279 | 55.474 | 76800 |
| | rounds: [5331.289, 5341.810, 5365.111, 5481.003, 5342.181] | | | | | |

#### Optimized

| Run | ns/op | ns/min | ns/p90 | ns/mean | stdev | iterations |
|-----|-------|--------|--------|---------|-------|------------|
| 1 | 4419.683 | 4358.377 | 4446.224 | 4412.996 | 32.152 | 92160 |
| | rounds: [4446.224, 4358.377, 4419.683, 4398.801, 4441.895] | | | | | |
| 2 | 4458.209 | 4406.863 | 4566.086 | 4469.708 | 52.223 | 92160 |
| | rounds: [4462.088, 4458.209, 4406.863, 4566.086, 4455.295] | | | | | |
| 3 | 4445.296 | 4409.842 | 4533.811 | 4462.513 | 44.118 | 92160 |
| | rounds: [4433.729, 4409.842, 4445.296, 4533.811, 4489.887] | | | | | |

### FindCountedKey / findkey_short_hit

#### Original

| Run | ns/op | ns/min | ns/p90 | ns/mean | stdev | iterations |
|-----|-------|--------|--------|---------|-------|------------|
| 1 | 3.163 | 3.131 | 3.262 | 3.176 | 0.047 | 125987840 |
| | rounds: [3.163, 3.181, 3.131, 3.142, 3.262] | | | | | |
| 2 | 3.164 | 3.133 | 3.207 | 3.169 | 0.024 | 126256128 |
| | rounds: [3.178, 3.164, 3.133, 3.161, 3.207] | | | | | |
| 3 | 3.174 | 3.102 | 3.184 | 3.158 | 0.031 | 126680064 |
| | rounds: [3.184, 3.174, 3.102, 3.146, 3.184] | | | | | |

#### Optimized

| Run | ns/op | ns/min | ns/p90 | ns/mean | stdev | iterations |
|-----|-------|--------|--------|---------|-------|------------|
| 1 | 3.397 | 3.342 | 3.509 | 3.410 | 0.058 | 117342208 |
| | rounds: [3.370, 3.431, 3.342, 3.509, 3.397] | | | | | |
| 2 | 3.414 | 3.389 | 3.471 | 3.417 | 0.030 | 117066752 |
| | rounds: [3.389, 3.422, 3.390, 3.414, 3.471] | | | | | |
| 3 | 3.389 | 3.355 | 3.568 | 3.415 | 0.078 | 117199872 |
| | rounds: [3.400, 3.568, 3.389, 3.362, 3.355] | | | | | |

### FindCountedKey / findkey_short_miss

#### Original

| Run | ns/op | ns/min | ns/p90 | ns/mean | stdev | iterations |
|-----|-------|--------|--------|---------|-------|------------|
| 1 | 2.532 | 2.521 | 2.563 | 2.538 | 0.014 | 157641728 |
| | rounds: [2.542, 2.532, 2.530, 2.563, 2.521] | | | | | |
| 2 | 2.543 | 2.521 | 2.602 | 2.552 | 0.031 | 156783616 |
| | rounds: [2.521, 2.543, 2.522, 2.570, 2.602] | | | | | |
| 3 | 2.535 | 2.520 | 2.551 | 2.535 | 0.010 | 157804544 |
| | rounds: [2.520, 2.529, 2.551, 2.540, 2.535] | | | | | |

#### Optimized

| Run | ns/op | ns/min | ns/p90 | ns/mean | stdev | iterations |
|-----|-------|--------|--------|---------|-------|------------|
| 1 | 2.751 | 2.695 | 2.823 | 2.767 | 0.049 | 144623616 |
| | rounds: [2.822, 2.751, 2.695, 2.823, 2.741] | | | | | |
| 2 | 2.786 | 2.741 | 2.795 | 2.773 | 0.022 | 144241664 |
| | rounds: [2.793, 2.795, 2.752, 2.786, 2.741] | | | | | |
| 3 | 2.762 | 2.735 | 2.912 | 2.802 | 0.072 | 142826496 |
| | rounds: [2.864, 2.735, 2.912, 2.762, 2.738] | | | | | |

### FindCountedKey / findkey_long_hit

#### Original

| Run | ns/op | ns/min | ns/p90 | ns/mean | stdev | iterations |
|-----|-------|--------|--------|---------|-------|------------|
| 1 | 2.774 | 2.764 | 2.795 | 2.779 | 0.012 | 143960064 |
| | rounds: [2.764, 2.769, 2.774, 2.790, 2.795] | | | | | |
| 2 | 2.776 | 2.766 | 2.822 | 2.788 | 0.023 | 143462400 |
| | rounds: [2.776, 2.766, 2.822, 2.769, 2.808] | | | | | |
| 3 | 2.754 | 2.717 | 2.808 | 2.761 | 0.030 | 144894976 |
| | rounds: [2.752, 2.717, 2.808, 2.774, 2.754] | | | | | |

#### Optimized

| Run | ns/op | ns/min | ns/p90 | ns/mean | stdev | iterations |
|-----|-------|--------|--------|---------|-------|------------|
| 1 | 2.974 | 2.934 | 2.992 | 2.970 | 0.019 | 134708224 |
| | rounds: [2.992, 2.934, 2.977, 2.971, 2.974] | | | | | |
| 2 | 3.019 | 2.998 | 3.076 | 3.035 | 0.031 | 131802112 |
| | rounds: [3.015, 3.019, 3.076, 2.998, 3.069] | | | | | |
| 3 | 2.960 | 2.949 | 3.004 | 2.970 | 0.019 | 134680576 |
| | rounds: [2.949, 2.960, 3.004, 2.959, 2.978] | | | | | |

### FindCountedKey / findkey_64_hit

#### Original

| Run | ns/op | ns/min | ns/p90 | ns/mean | stdev | iterations |
|-----|-------|--------|--------|---------|-------|------------|
| 1 | 141.932 | 141.494 | 143.496 | 142.196 | 0.685 | 2816000 |
| | rounds: [142.167, 141.494, 141.932, 141.891, 143.496] | | | | | |
| 2 | 142.310 | 140.992 | 144.719 | 142.615 | 1.371 | 2808832 |
| | rounds: [144.719, 141.472, 143.582, 140.992, 142.310] | | | | | |
| 3 | 141.585 | 140.688 | 144.274 | 142.091 | 1.275 | 2818048 |
| | rounds: [141.214, 140.688, 144.274, 142.694, 141.585] | | | | | |

#### Optimized

| Run | ns/op | ns/min | ns/p90 | ns/mean | stdev | iterations |
|-----|-------|--------|--------|---------|-------|------------|
| 1 | 148.998 | 146.563 | 150.550 | 148.802 | 1.407 | 2691072 |
| | rounds: [148.015, 149.884, 148.998, 150.550, 146.563] | | | | | |
| 2 | 149.639 | 148.926 | 150.470 | 149.582 | 0.541 | 2676736 |
| | rounds: [148.926, 149.639, 149.753, 149.123, 150.470] | | | | | |
| 3 | 150.396 | 149.038 | 153.114 | 150.747 | 1.525 | 2656256 |
| | rounds: [150.396, 149.381, 149.038, 153.114, 151.807] | | | | | |

### FindCountedKey / findkey_64_miss

#### Original

| Run | ns/op | ns/min | ns/p90 | ns/mean | stdev | iterations |
|-----|-------|--------|--------|---------|-------|------------|
| 1 | 141.931 | 140.906 | 142.763 | 141.832 | 0.603 | 2823168 |
| | rounds: [141.931, 141.976, 142.763, 141.586, 140.906] | | | | | |
| 2 | 141.883 | 140.447 | 144.659 | 142.080 | 1.465 | 2819072 |
| | rounds: [140.964, 142.446, 141.883, 144.659, 140.447] | | | | | |
| 3 | 141.428 | 140.758 | 144.044 | 142.167 | 1.461 | 2817024 |
| | rounds: [143.815, 141.428, 144.044, 140.758, 140.789] | | | | | |

#### Optimized

| Run | ns/op | ns/min | ns/p90 | ns/mean | stdev | iterations |
|-----|-------|--------|--------|---------|-------|------------|
| 1 | 148.175 | 147.095 | 150.663 | 148.295 | 1.268 | 2701312 |
| | rounds: [147.095, 148.175, 147.307, 150.663, 148.234] | | | | | |
| 2 | 149.238 | 148.166 | 151.243 | 149.325 | 1.078 | 2681856 |
| | rounds: [151.243, 149.238, 149.518, 148.166, 148.461] | | | | | |
| 3 | 148.560 | 147.551 | 150.600 | 148.902 | 1.040 | 2688000 |
| | rounds: [150.600, 148.560, 148.360, 149.438, 147.551] | | | | | |

### FlowKeyEqual / flowkey_hit

#### Original

| Run | ns/op | ns/min | ns/p90 | ns/mean | stdev | iterations |
|-----|-------|--------|--------|---------|-------|------------|
| 1 | 1.388 | 1.383 | 1.396 | 1.388 | 0.004 | 288158720 |
| | rounds: [1.383, 1.396, 1.388, 1.389, 1.386] | | | | | |
| 2 | 1.386 | 1.376 | 1.407 | 1.388 | 0.011 | 288118784 |
| | rounds: [1.379, 1.407, 1.376, 1.395, 1.386] | | | | | |
| 3 | 1.381 | 1.357 | 1.425 | 1.388 | 0.024 | 288228352 |
| | rounds: [1.381, 1.406, 1.372, 1.357, 1.425] | | | | | |

#### Optimized

| Run | ns/op | ns/min | ns/p90 | ns/mean | stdev | iterations |
|-----|-------|--------|--------|---------|-------|------------|
| 1 | 1.282 | 1.273 | 1.301 | 1.286 | 0.010 | 311179264 |
| | rounds: [1.282, 1.293, 1.278, 1.273, 1.301] | | | | | |
| 2 | 1.291 | 1.268 | 1.315 | 1.292 | 0.017 | 309616640 |
| | rounds: [1.268, 1.315, 1.291, 1.282, 1.305] | | | | | |
| 3 | 1.284 | 1.274 | 1.302 | 1.285 | 0.011 | 311217152 |
| | rounds: [1.274, 1.294, 1.284, 1.302, 1.274] | | | | | |

### FlowKeyEqual / flowkey_miss

#### Original

| Run | ns/op | ns/min | ns/p90 | ns/mean | stdev | iterations |
|-----|-------|--------|--------|---------|-------|------------|
| 1 | 1.566 | 1.528 | 1.602 | 1.564 | 0.023 | 255738880 |
| | rounds: [1.560, 1.566, 1.602, 1.566, 1.528] | | | | | |
| 2 | 1.553 | 1.545 | 1.556 | 1.552 | 0.004 | 257782784 |
| | rounds: [1.545, 1.550, 1.555, 1.556, 1.553] | | | | | |
| 3 | 1.553 | 1.549 | 1.585 | 1.562 | 0.014 | 256110592 |
| | rounds: [1.549, 1.570, 1.553, 1.585, 1.552] | | | | | |

#### Optimized

| Run | ns/op | ns/min | ns/p90 | ns/mean | stdev | iterations |
|-----|-------|--------|--------|---------|-------|------------|
| 1 | 1.348 | 1.337 | 1.349 | 1.345 | 0.005 | 297396224 |
| | rounds: [1.349, 1.348, 1.337, 1.349, 1.342] | | | | | |
| 2 | 1.354 | 1.343 | 1.363 | 1.354 | 0.007 | 295377920 |
| | rounds: [1.363, 1.360, 1.343, 1.351, 1.354] | | | | | |
| 3 | 1.358 | 1.341 | 1.373 | 1.356 | 0.011 | 295075840 |
| | rounds: [1.341, 1.361, 1.358, 1.373, 1.346] | | | | | |

### CmpFlowAddr / cmpaddr_late

#### Original

| Run | ns/op | ns/min | ns/p90 | ns/mean | stdev | iterations |
|-----|-------|--------|--------|---------|-------|------------|
| 1 | 1.801 | 1.794 | 1.819 | 1.803 | 0.009 | 221914112 |
| | rounds: [1.795, 1.804, 1.794, 1.819, 1.801] | | | | | |
| 2 | 1.817 | 1.800 | 1.834 | 1.817 | 0.013 | 220100608 |
| | rounds: [1.834, 1.830, 1.800, 1.806, 1.817] | | | | | |
| 3 | 1.811 | 1.766 | 1.822 | 1.801 | 0.021 | 222159872 |
| | rounds: [1.822, 1.789, 1.816, 1.811, 1.766] | | | | | |

#### Optimized

| Run | ns/op | ns/min | ns/p90 | ns/mean | stdev | iterations |
|-----|-------|--------|--------|---------|-------|------------|
| 1 | 1.373 | 1.365 | 1.394 | 1.375 | 0.010 | 290829312 |
| | rounds: [1.394, 1.365, 1.369, 1.373, 1.377] | | | | | |
| 2 | 1.383 | 1.362 | 1.423 | 1.385 | 0.021 | 288950272 |
| | rounds: [1.383, 1.362, 1.371, 1.384, 1.423] | | | | | |
| 3 | 1.404 | 1.385 | 1.411 | 1.400 | 0.010 | 285684736 |
| | rounds: [1.404, 1.385, 1.408, 1.393, 1.411] | | | | | |

### FilterHandles / handles_100k

#### Original

| Run | ns/op | ns/min | ns/p90 | ns/mean | stdev | iterations |
|-----|-------|--------|--------|---------|-------|------------|
| 1 | 405619.824 | 404010.059 | 405858.594 | 405192.461 | 725.313 | 5120 |
| | rounds: [405785.547, 404010.059, 405619.824, 405858.594, 404688.281] | | | | | |
| 2 | 410704.688 | 407890.918 | 416000.781 | 411259.805 | 2845.770 | 5120 |
| | rounds: [407890.918, 416000.781, 409115.820, 410704.688, 412586.816] | | | | | |
| 3 | 406293.164 | 402972.168 | 407406.152 | 405720.352 | 1501.190 | 5120 |
| | rounds: [407406.152, 405503.906, 406426.367, 406293.164, 402972.168] | | | | | |

#### Optimized

| Run | ns/op | ns/min | ns/p90 | ns/mean | stdev | iterations |
|-----|-------|--------|--------|---------|-------|------------|
| 1 | 752621.191 | 749866.797 | 753557.031 | 751991.602 | 1337.424 | 5120 |
| | rounds: [751067.871, 752621.191, 752845.117, 749866.797, 753557.031] | | | | | |
| 2 | 760045.410 | 756183.496 | 761175.879 | 759552.617 | 1797.165 | 5120 |
| | rounds: [760045.410, 756183.496, 759425.781, 761175.879, 760932.520] | | | | | |
| 3 | 762843.066 | 756308.496 | 767700.195 | 762396.445 | 4065.452 | 5120 |
| | rounds: [756308.496, 759633.496, 765496.973, 767700.195, 762843.066] | | | | | |

### ClassifyEvent / etw_mixed

#### Original

| Run | ns/op | ns/min | ns/p90 | ns/mean | stdev | iterations |
|-----|-------|--------|--------|---------|-------|------------|
| 1 | 1.626 | 1.622 | 1.691 | 1.639 | 0.026 | 244119552 |
| | rounds: [1.634, 1.622, 1.622, 1.626, 1.691] | | | | | |
| 2 | 1.647 | 1.623 | 1.885 | 1.691 | 0.098 | 237254656 |
| | rounds: [1.660, 1.623, 1.885, 1.647, 1.642] | | | | | |
| 3 | 1.635 | 1.603 | 1.648 | 1.633 | 0.016 | 245009408 |
| | rounds: [1.632, 1.603, 1.648, 1.645, 1.635] | | | | | |

#### Optimized

| Run | ns/op | ns/min | ns/p90 | ns/mean | stdev | iterations |
|-----|-------|--------|--------|---------|-------|------------|
| 1 | 1.792 | 1.771 | 1.909 | 1.812 | 0.050 | 220916736 |
| | rounds: [1.771, 1.806, 1.782, 1.909, 1.792] | | | | | |
| 2 | 1.816 | 1.805 | 1.835 | 1.818 | 0.010 | 220059648 |
| | rounds: [1.821, 1.805, 1.811, 1.835, 1.816] | | | | | |
| 3 | 1.821 | 1.808 | 1.865 | 1.832 | 0.022 | 218415104 |
| | rounds: [1.808, 1.865, 1.821, 1.813, 1.851] | | | | | |

### ParseEventPayload / etw_payload

#### Original

| Run | ns/op | ns/min | ns/p90 | ns/mean | stdev | iterations |
|-----|-------|--------|--------|---------|-------|------------|
| 1 | 1.515 | 1.487 | 1.531 | 1.513 | 0.016 | 264344576 |
| | rounds: [1.530, 1.504, 1.487, 1.531, 1.515] | | | | | |
| 2 | 1.512 | 1.495 | 1.531 | 1.512 | 0.013 | 264485888 |
| | rounds: [1.495, 1.522, 1.512, 1.502, 1.531] | | | | | |
| 3 | 1.509 | 1.490 | 1.539 | 1.510 | 0.017 | 264928256 |
| | rounds: [1.516, 1.497, 1.539, 1.490, 1.509] | | | | | |

#### Optimized

| Run | ns/op | ns/min | ns/p90 | ns/mean | stdev | iterations |
|-----|-------|--------|--------|---------|-------|------------|
| 1 | 1.260 | 1.249 | 1.275 | 1.261 | 0.010 | 317168640 |
| | rounds: [1.260, 1.275, 1.252, 1.249, 1.270] | | | | | |
| 2 | 1.268 | 1.244 | 1.285 | 1.265 | 0.014 | 316185600 |
| | rounds: [1.244, 1.268, 1.259, 1.285, 1.270] | | | | | |
| 3 | 1.283 | 1.270 | 1.292 | 1.281 | 0.009 | 312174592 |
| | rounds: [1.271, 1.292, 1.270, 1.291, 1.283] | | | | | |

### JsonEscape / json_proc

#### Original

| Run | ns/op | ns/min | ns/p90 | ns/mean | stdev | iterations |
|-----|-------|--------|--------|---------|-------|------------|
| 1 | 60.633 | 60.531 | 61.416 | 60.837 | 0.335 | 6578176 |
| | rounds: [61.416, 61.014, 60.633, 60.594, 60.531] | | | | | |
| 2 | 60.555 | 60.486 | 62.041 | 60.899 | 0.590 | 6572032 |
| | rounds: [60.486, 60.513, 62.041, 60.900, 60.555] | | | | | |
| 3 | 60.935 | 59.772 | 63.110 | 60.975 | 1.213 | 6565888 |
| | rounds: [63.110, 59.772, 59.840, 60.935, 61.216] | | | | | |

#### Optimized

| Run | ns/op | ns/min | ns/p90 | ns/mean | stdev | iterations |
|-----|-------|--------|--------|---------|-------|------------|
| 1 | 62.384 | 62.151 | 64.304 | 62.904 | 0.815 | 6363136 |
| | rounds: [63.349, 62.384, 62.151, 62.331, 64.304] | | | | | |
| 2 | 62.604 | 61.837 | 63.112 | 62.493 | 0.437 | 6405120 |
| | rounds: [63.112, 61.837, 62.604, 62.203, 62.707] | | | | | |
| 3 | 63.545 | 62.941 | 64.638 | 63.689 | 0.685 | 6284288 |
| | rounds: [64.638, 63.004, 62.941, 63.545, 64.317] | | | | | |

### JsonEscape / json_row

#### Original

| Run | ns/op | ns/min | ns/p90 | ns/mean | stdev | iterations |
|-----|-------|--------|--------|---------|-------|------------|
| 1 | 191.710 | 190.512 | 193.328 | 191.862 | 0.908 | 2087936 |
| | rounds: [190.512, 192.134, 191.710, 193.328, 191.629] | | | | | |
| 2 | 193.479 | 192.642 | 194.808 | 193.613 | 0.708 | 2069504 |
| | rounds: [193.326, 194.808, 193.808, 193.479, 192.642] | | | | | |
| 3 | 193.742 | 189.558 | 199.624 | 194.564 | 3.387 | 2060288 |
| | rounds: [196.642, 193.255, 199.624, 189.558, 193.742] | | | | | |

#### Optimized

| Run | ns/op | ns/min | ns/p90 | ns/mean | stdev | iterations |
|-----|-------|--------|--------|---------|-------|------------|
| 1 | 100.952 | 100.343 | 102.439 | 101.160 | 0.694 | 3956736 |
| | rounds: [100.952, 100.343, 102.439, 101.159, 100.907] | | | | | |
| 2 | 102.978 | 101.607 | 103.190 | 102.645 | 0.578 | 3900416 |
| | rounds: [103.190, 102.978, 101.607, 102.432, 103.017] | | | | | |
| 3 | 102.901 | 102.130 | 105.657 | 103.567 | 1.376 | 3866624 |
| | rounds: [102.901, 102.130, 104.713, 105.657, 102.435] | | | | | |

### JsonEscape / json_64_quotes

#### Original

| Run | ns/op | ns/min | ns/p90 | ns/mean | stdev | iterations |
|-----|-------|--------|--------|---------|-------|------------|
| 1 | 171.627 | 168.798 | 173.783 | 171.480 | 1.799 | 2334720 |
| | rounds: [172.947, 170.246, 171.627, 168.798, 173.783] | | | | | |
| 2 | 170.835 | 167.669 | 173.889 | 170.830 | 1.969 | 2343936 |
| | rounds: [171.003, 173.889, 167.669, 170.835, 170.756] | | | | | |
| 3 | 167.572 | 167.540 | 169.699 | 168.107 | 0.829 | 2382848 |
| | rounds: [169.699, 168.158, 167.565, 167.572, 167.540] | | | | | |

#### Optimized

| Run | ns/op | ns/min | ns/p90 | ns/mean | stdev | iterations |
|-----|-------|--------|--------|---------|-------|------------|
| 1 | 90.906 | 90.336 | 91.463 | 90.970 | 0.389 | 4399104 |
| | rounds: [90.336, 90.865, 91.281, 91.463, 90.906] | | | | | |
| 2 | 96.939 | 94.460 | 98.380 | 96.645 | 1.344 | 4143104 |
| | rounds: [95.968, 98.380, 94.460, 96.939, 97.480] | | | | | |
| 3 | 89.672 | 88.603 | 91.448 | 89.703 | 1.065 | 4462592 |
| | rounds: [91.448, 89.672, 90.181, 88.612, 88.603] | | | | | |

### CsvEscape / csv_plain_64

#### Original

| Run | ns/op | ns/min | ns/p90 | ns/mean | stdev | iterations |
|-----|-------|--------|--------|---------|-------|------------|
| 1 | 37.908 | 37.115 | 38.098 | 37.677 | 0.412 | 10631168 |
| | rounds: [37.247, 37.115, 38.018, 37.908, 38.098] | | | | | |
| 2 | 35.707 | 35.609 | 37.232 | 36.087 | 0.614 | 11089920 |
| | rounds: [35.609, 36.225, 35.663, 35.707, 37.232] | | | | | |
| 3 | 35.617 | 35.168 | 35.957 | 35.556 | 0.273 | 11252736 |
| | rounds: [35.617, 35.168, 35.957, 35.684, 35.355] | | | | | |

#### Optimized

| Run | ns/op | ns/min | ns/p90 | ns/mean | stdev | iterations |
|-----|-------|--------|--------|---------|-------|------------|
| 1 | 35.661 | 35.493 | 35.727 | 35.645 | 0.086 | 11224064 |
| | rounds: [35.493, 35.661, 35.620, 35.725, 35.727] | | | | | |
| 2 | 36.369 | 35.934 | 37.746 | 36.562 | 0.619 | 10946560 |
| | rounds: [36.285, 37.746, 36.369, 36.475, 35.934] | | | | | |
| 3 | 35.987 | 35.791 | 36.094 | 35.980 | 0.106 | 11119616 |
| | rounds: [35.964, 36.094, 35.791, 36.063, 35.987] | | | | | |

### CsvEscape / csv_quoted

#### Original

| Run | ns/op | ns/min | ns/p90 | ns/mean | stdev | iterations |
|-----|-------|--------|--------|---------|-------|------------|
| 1 | 69.555 | 67.753 | 70.629 | 69.367 | 0.991 | 5771264 |
| | rounds: [69.555, 70.027, 67.753, 70.629, 68.872] | | | | | |
| 2 | 70.024 | 69.713 | 70.203 | 69.967 | 0.176 | 5719040 |
| | rounds: [70.024, 69.713, 69.822, 70.203, 70.072] | | | | | |
| 3 | 68.964 | 68.914 | 70.991 | 69.571 | 0.825 | 5752832 |
| | rounds: [68.958, 70.991, 70.029, 68.914, 68.964] | | | | | |

#### Optimized

| Run | ns/op | ns/min | ns/p90 | ns/mean | stdev | iterations |
|-----|-------|--------|--------|---------|-------|------------|
| 1 | 45.122 | 44.615 | 45.725 | 45.165 | 0.421 | 8860672 |
| | rounds: [44.615, 45.549, 45.122, 44.815, 45.725] | | | | | |
| 2 | 45.588 | 45.026 | 46.281 | 45.679 | 0.487 | 8761344 |
| | rounds: [45.588, 45.314, 46.281, 45.026, 46.186] | | | | | |
| 3 | 45.094 | 44.509 | 45.710 | 45.118 | 0.425 | 8869888 |
| | rounds: [45.438, 45.710, 44.837, 45.094, 44.509] | | | | | |

### FormatPort / port_443

#### Original

| Run | ns/op | ns/min | ns/p90 | ns/mean | stdev | iterations |
|-----|-------|--------|--------|---------|-------|------------|
| 1 | 32.984 | 32.562 | 33.800 | 33.132 | 0.461 | 12078080 |
| | rounds: [33.800, 32.788, 32.984, 32.562, 33.524] | | | | | |
| 2 | 34.729 | 33.820 | 35.005 | 34.618 | 0.412 | 11558912 |
| | rounds: [34.813, 34.725, 35.005, 33.820, 34.729] | | | | | |
| 3 | 33.210 | 32.988 | 33.679 | 33.261 | 0.232 | 12029952 |
| | rounds: [33.210, 33.679, 33.298, 32.988, 33.129] | | | | | |

#### Optimized

| Run | ns/op | ns/min | ns/p90 | ns/mean | stdev | iterations |
|-----|-------|--------|--------|---------|-------|------------|
| 1 | 5.144 | 5.020 | 5.241 | 5.153 | 0.078 | 77649920 |
| | rounds: [5.020, 5.136, 5.144, 5.241, 5.222] | | | | | |
| 2 | 5.283 | 5.126 | 5.329 | 5.259 | 0.070 | 76083200 |
| | rounds: [5.126, 5.283, 5.265, 5.291, 5.329] | | | | | |
| 3 | 5.269 | 5.112 | 5.444 | 5.277 | 0.131 | 75849728 |
| | rounds: [5.269, 5.112, 5.403, 5.444, 5.157] | | | | | |

### FormatPort / port_65535

#### Original

| Run | ns/op | ns/min | ns/p90 | ns/mean | stdev | iterations |
|-----|-------|--------|--------|---------|-------|------------|
| 1 | 34.412 | 33.911 | 35.057 | 34.416 | 0.374 | 11625472 |
| | rounds: [33.911, 34.238, 35.057, 34.412, 34.464] | | | | | |
| 2 | 34.765 | 34.208 | 35.598 | 34.839 | 0.463 | 11486208 |
| | rounds: [34.208, 34.765, 34.598, 35.028, 35.598] | | | | | |
| 3 | 34.966 | 34.438 | 35.085 | 34.840 | 0.246 | 11484160 |
| | rounds: [34.438, 34.966, 35.085, 34.673, 35.035] | | | | | |

#### Optimized

| Run | ns/op | ns/min | ns/p90 | ns/mean | stdev | iterations |
|-----|-------|--------|--------|---------|-------|------------|
| 1 | 8.488 | 8.364 | 8.557 | 8.466 | 0.068 | 47253504 |
| | rounds: [8.488, 8.504, 8.364, 8.417, 8.557] | | | | | |
| 2 | 8.562 | 8.454 | 8.876 | 8.646 | 0.185 | 46288896 |
| | rounds: [8.454, 8.859, 8.876, 8.562, 8.478] | | | | | |
| 3 | 8.496 | 8.415 | 9.575 | 8.688 | 0.445 | 46153728 |
| | rounds: [8.496, 8.512, 9.575, 8.444, 8.415] | | | | | |

### FormatU64Dec / u64_20digit

#### Original

| Run | ns/op | ns/min | ns/p90 | ns/mean | stdev | iterations |
|-----|-------|--------|--------|---------|-------|------------|
| 1 | 70.032 | 67.252 | 70.195 | 69.175 | 1.205 | 5785600 |
| | rounds: [68.253, 70.195, 70.032, 70.144, 67.252] | | | | | |
| 2 | 70.589 | 69.339 | 71.060 | 70.473 | 0.611 | 5679104 |
| | rounds: [70.436, 70.589, 70.942, 71.060, 69.339] | | | | | |
| 3 | 69.301 | 69.190 | 77.492 | 70.995 | 3.254 | 5648384 |
| | rounds: [69.723, 69.270, 69.190, 69.301, 77.492] | | | | | |

#### Optimized

| Run | ns/op | ns/min | ns/p90 | ns/mean | stdev | iterations |
|-----|-------|--------|--------|---------|-------|------------|
| 1 | 13.097 | 12.820 | 13.669 | 13.164 | 0.280 | 30402560 |
| | rounds: [13.669, 12.820, 13.182, 13.097, 13.050] | | | | | |
| 2 | 12.976 | 12.901 | 13.273 | 13.056 | 0.149 | 30643200 |
| | rounds: [12.935, 12.901, 13.194, 12.976, 13.273] | | | | | |
| 3 | 13.332 | 13.049 | 13.556 | 13.285 | 0.178 | 30120960 |
| | rounds: [13.134, 13.556, 13.049, 13.351, 13.332] | | | | | |

### FormatDuration / dur_hms

#### Original

| Run | ns/op | ns/min | ns/p90 | ns/mean | stdev | iterations |
|-----|-------|--------|--------|---------|-------|------------|
| 1 | 64.623 | 64.093 | 65.768 | 64.927 | 0.673 | 6162432 |
| | rounds: [65.678, 64.471, 64.623, 64.093, 65.768] | | | | | |
| 2 | 67.224 | 65.474 | 67.843 | 66.841 | 0.947 | 5989376 |
| | rounds: [65.978, 67.224, 67.843, 65.474, 67.687] | | | | | |
| 3 | 65.971 | 64.852 | 66.554 | 65.796 | 0.562 | 6085632 |
| | rounds: [64.852, 65.971, 65.598, 66.006, 66.554] | | | | | |

#### Optimized

| Run | ns/op | ns/min | ns/p90 | ns/mean | stdev | iterations |
|-----|-------|--------|--------|---------|-------|------------|
| 1 | 8.076 | 7.825 | 8.479 | 8.097 | 0.237 | 49443840 |
| | rounds: [8.479, 7.825, 7.883, 8.223, 8.076] | | | | | |
| 2 | 8.516 | 8.305 | 8.584 | 8.463 | 0.108 | 47276032 |
| | rounds: [8.584, 8.545, 8.516, 8.305, 8.364] | | | | | |
| 3 | 8.545 | 8.263 | 8.775 | 8.551 | 0.173 | 46800896 |
| | rounds: [8.775, 8.263, 8.670, 8.545, 8.501] | | | | | |

### FormatIpv4 / ipv4

#### Original

| Run | ns/op | ns/min | ns/p90 | ns/mean | stdev | iterations |
|-----|-------|--------|--------|---------|-------|------------|
| 1 | 227.579 | 226.191 | 229.404 | 227.637 | 1.035 | 1760256 |
| | rounds: [227.731, 226.191, 227.280, 227.579, 229.404] | | | | | |
| 2 | 229.886 | 228.569 | 242.879 | 232.221 | 5.375 | 1724416 |
| | rounds: [230.641, 229.886, 242.879, 229.130, 228.569] | | | | | |
| 3 | 227.381 | 226.206 | 251.377 | 232.034 | 9.690 | 1728512 |
| | rounds: [227.381, 228.062, 227.143, 226.206, 251.377] | | | | | |

#### Optimized

| Run | ns/op | ns/min | ns/p90 | ns/mean | stdev | iterations |
|-----|-------|--------|--------|---------|-------|------------|
| 1 | 28.742 | 28.434 | 28.780 | 28.680 | 0.128 | 13949952 |
| | rounds: [28.780, 28.682, 28.742, 28.762, 28.434] | | | | | |
| 2 | 29.216 | 29.088 | 29.805 | 29.362 | 0.273 | 13626368 |
| | rounds: [29.152, 29.549, 29.216, 29.088, 29.805] | | | | | |
| 3 | 28.832 | 28.349 | 29.588 | 28.945 | 0.436 | 13824000 |
| | rounds: [28.832, 29.267, 28.349, 29.588, 28.691] | | | | | |

### FormatIpv6 / ipv6_full

#### Original

| Run | ns/op | ns/min | ns/p90 | ns/mean | stdev | iterations |
|-----|-------|--------|--------|---------|-------|------------|
| 1 | 928.071 | 923.169 | 937.622 | 929.650 | 5.158 | 433152 |
| | rounds: [928.071, 933.229, 923.169, 937.622, 926.159] | | | | | |
| 2 | 934.245 | 921.233 | 943.851 | 933.639 | 7.578 | 430080 |
| | rounds: [938.130, 930.738, 943.851, 921.233, 934.245] | | | | | |
| 3 | 903.504 | 902.316 | 914.559 | 906.761 | 4.821 | 443392 |
| | rounds: [903.504, 903.173, 910.250, 914.559, 902.316] | | | | | |

#### Optimized

| Run | ns/op | ns/min | ns/p90 | ns/mean | stdev | iterations |
|-----|-------|--------|--------|---------|-------|------------|
| 1 | 42.139 | 41.447 | 42.456 | 41.951 | 0.389 | 9537536 |
| | rounds: [42.167, 42.456, 41.447, 42.139, 41.545] | | | | | |
| 2 | 44.271 | 43.857 | 46.178 | 44.787 | 0.857 | 8937472 |
| | rounds: [44.271, 43.857, 45.363, 46.178, 44.267] | | | | | |
| 3 | 42.858 | 41.768 | 43.110 | 42.673 | 0.478 | 9376768 |
| | rounds: [42.858, 42.984, 41.768, 42.647, 43.110] | | | | | |

### FormatIpv6 / ipv6_zip

#### Original

| Run | ns/op | ns/min | ns/p90 | ns/mean | stdev | iterations |
|-----|-------|--------|--------|---------|-------|------------|
| 1 | 162.728 | 161.684 | 166.161 | 163.367 | 1.533 | 2452480 |
| | rounds: [166.161, 161.684, 162.585, 163.679, 162.728] | | | | | |
| 2 | 163.224 | 161.780 | 167.663 | 164.130 | 2.008 | 2439168 |
| | rounds: [167.663, 163.224, 161.780, 164.800, 163.185] | | | | | |
| 3 | 162.030 | 161.449 | 167.546 | 163.349 | 2.271 | 2452480 |
| | rounds: [161.775, 161.449, 167.546, 162.030, 163.947] | | | | | |

#### Optimized

| Run | ns/op | ns/min | ns/p90 | ns/mean | stdev | iterations |
|-----|-------|--------|--------|---------|-------|------------|
| 1 | 11.968 | 11.930 | 12.107 | 11.993 | 0.061 | 33355776 |
| | rounds: [11.968, 12.107, 12.002, 11.958, 11.930] | | | | | |
| 2 | 12.139 | 12.026 | 12.372 | 12.175 | 0.119 | 32859136 |
| | rounds: [12.103, 12.139, 12.236, 12.372, 12.026] | | | | | |
| 3 | 12.208 | 12.065 | 12.575 | 12.287 | 0.186 | 32564224 |
| | rounds: [12.164, 12.575, 12.208, 12.424, 12.065] | | | | | |

### CpWidth / cp_boundaries

#### Original

| Run | ns/op | ns/min | ns/p90 | ns/mean | stdev | iterations |
|-----|-------|--------|--------|---------|-------|------------|
| 1 | 2.439 | 2.414 | 2.475 | 2.438 | 0.022 | 164109312 |
| | rounds: [2.475, 2.418, 2.439, 2.414, 2.442] | | | | | |
| 2 | 2.470 | 2.431 | 2.479 | 2.464 | 0.017 | 162360320 |
| | rounds: [2.462, 2.431, 2.476, 2.479, 2.470] | | | | | |
| 3 | 2.141 | 2.105 | 2.283 | 2.156 | 0.066 | 185703424 |
| | rounds: [2.283, 2.107, 2.144, 2.105, 2.141] | | | | | |

#### Optimized

| Run | ns/op | ns/min | ns/p90 | ns/mean | stdev | iterations |
|-----|-------|--------|--------|---------|-------|------------|
| 1 | 2.007 | 1.993 | 2.036 | 2.007 | 0.015 | 199276544 |
| | rounds: [1.995, 2.007, 2.007, 1.993, 2.036] | | | | | |
| 2 | 2.005 | 2.001 | 2.027 | 2.009 | 0.009 | 199096320 |
| | rounds: [2.027, 2.005, 2.001, 2.008, 2.005] | | | | | |
| 3 | 1.997 | 1.963 | 2.057 | 2.001 | 0.031 | 199944192 |
| | rounds: [1.997, 1.963, 1.997, 1.991, 2.057] | | | | | |

### CpWidth / cp_random_sweep

#### Original

| Run | ns/op | ns/min | ns/p90 | ns/mean | stdev | iterations |
|-----|-------|--------|--------|---------|-------|------------|
| 1 | 6.248 | 6.193 | 6.321 | 6.245 | 0.046 | 64053248 |
| | rounds: [6.248, 6.193, 6.202, 6.263, 6.321] | | | | | |
| 2 | 6.255 | 6.142 | 6.360 | 6.259 | 0.083 | 63924224 |
| | rounds: [6.255, 6.360, 6.343, 6.197, 6.142] | | | | | |
| 3 | 6.179 | 6.126 | 6.209 | 6.173 | 0.027 | 64804864 |
| | rounds: [6.179, 6.166, 6.209, 6.126, 6.184] | | | | | |

#### Optimized

| Run | ns/op | ns/min | ns/p90 | ns/mean | stdev | iterations |
|-----|-------|--------|--------|---------|-------|------------|
| 1 | 6.672 | 6.656 | 6.964 | 6.750 | 0.118 | 59283456 |
| | rounds: [6.964, 6.672, 6.656, 6.790, 6.667] | | | | | |
| 2 | 6.811 | 6.767 | 6.866 | 6.810 | 0.039 | 58739712 |
| | rounds: [6.769, 6.811, 6.866, 6.840, 6.767] | | | | | |
| 3 | 6.781 | 6.625 | 6.904 | 6.760 | 0.096 | 59188224 |
| | rounds: [6.797, 6.904, 6.692, 6.625, 6.781] | | | | | |

### DisplayWidth / width_ascii_47

#### Original

| Run | ns/op | ns/min | ns/p90 | ns/mean | stdev | iterations |
|-----|-------|--------|--------|---------|-------|------------|
| 1 | 42.510 | 42.092 | 43.046 | 42.626 | 0.364 | 9387008 |
| | rounds: [42.092, 43.024, 42.510, 43.046, 42.457] | | | | | |
| 2 | 42.603 | 42.419 | 42.920 | 42.671 | 0.195 | 9376768 |
| | rounds: [42.419, 42.534, 42.920, 42.603, 42.876] | | | | | |
| 3 | 42.352 | 41.795 | 42.694 | 42.334 | 0.340 | 9451520 |
| | rounds: [42.146, 42.694, 42.352, 41.795, 42.682] | | | | | |

#### Optimized

| Run | ns/op | ns/min | ns/p90 | ns/mean | stdev | iterations |
|-----|-------|--------|--------|---------|-------|------------|
| 1 | 4.881 | 4.805 | 5.001 | 4.889 | 0.066 | 81840128 |
| | rounds: [4.805, 4.846, 4.881, 4.911, 5.001] | | | | | |
| 2 | 4.866 | 4.807 | 4.979 | 4.878 | 0.060 | 82015232 |
| | rounds: [4.906, 4.979, 4.866, 4.832, 4.807] | | | | | |
| 3 | 4.863 | 4.843 | 4.915 | 4.875 | 0.030 | 82060288 |
| | rounds: [4.863, 4.905, 4.849, 4.915, 4.843] | | | | | |

### DisplayWidth / width_mixed_30

#### Original

| Run | ns/op | ns/min | ns/p90 | ns/mean | stdev | iterations |
|-----|-------|--------|--------|---------|-------|------------|
| 1 | 60.968 | 60.537 | 61.389 | 61.003 | 0.280 | 6559744 |
| | rounds: [61.389, 61.155, 60.537, 60.967, 60.968] | | | | | |
| 2 | 61.794 | 61.461 | 62.558 | 61.858 | 0.382 | 6469632 |
| | rounds: [61.895, 61.583, 61.461, 61.794, 62.558] | | | | | |
| 3 | 61.003 | 60.573 | 61.366 | 60.950 | 0.317 | 6565888 |
| | rounds: [60.573, 61.204, 61.366, 60.605, 61.003] | | | | | |

#### Optimized

| Run | ns/op | ns/min | ns/p90 | ns/mean | stdev | iterations |
|-----|-------|--------|--------|---------|-------|------------|
| 1 | 27.827 | 27.569 | 28.493 | 27.916 | 0.309 | 14331904 |
| | rounds: [27.894, 27.827, 27.569, 28.493, 27.795] | | | | | |
| 2 | 28.459 | 27.568 | 28.678 | 28.287 | 0.418 | 14146560 |
| | rounds: [28.678, 28.650, 27.568, 28.081, 28.459] | | | | | |
| 3 | 28.412 | 27.917 | 28.733 | 28.377 | 0.284 | 14099456 |
| | rounds: [27.917, 28.583, 28.238, 28.412, 28.733] | | | | | |

### DisplayWidth / width_ascii_256

#### Original

| Run | ns/op | ns/min | ns/p90 | ns/mean | stdev | iterations |
|-----|-------|--------|--------|---------|-------|------------|
| 1 | 175.723 | 173.884 | 179.401 | 176.187 | 1.806 | 2273280 |
| | rounds: [176.387, 179.401, 175.723, 175.537, 173.884] | | | | | |
| 2 | 175.223 | 174.402 | 181.564 | 176.341 | 2.646 | 2270208 |
| | rounds: [181.564, 175.223, 174.402, 175.690, 174.825] | | | | | |
| 3 | 174.244 | 172.947 | 175.966 | 174.510 | 1.047 | 2293760 |
| | rounds: [175.322, 175.966, 174.244, 174.070, 172.947] | | | | | |

#### Optimized

| Run | ns/op | ns/min | ns/p90 | ns/mean | stdev | iterations |
|-----|-------|--------|--------|---------|-------|------------|
| 1 | 6.248 | 6.046 | 6.649 | 6.308 | 0.207 | 63485952 |
| | rounds: [6.248, 6.413, 6.649, 6.046, 6.183] | | | | | |
| 2 | 6.233 | 6.118 | 6.360 | 6.251 | 0.089 | 64005120 |
| | rounds: [6.337, 6.118, 6.360, 6.208, 6.233] | | | | | |
| 3 | 6.263 | 5.966 | 6.488 | 6.270 | 0.177 | 63850496 |
| | rounds: [5.966, 6.488, 6.241, 6.391, 6.263] | | | | | |

### DisplayWidth / width_mixed_320

#### Original

| Run | ns/op | ns/min | ns/p90 | ns/mean | stdev | iterations |
|-----|-------|--------|--------|---------|-------|------------|
| 1 | 463.520 | 455.265 | 494.518 | 473.271 | 15.855 | 847872 |
| | rounds: [490.035, 494.518, 463.018, 455.265, 463.520] | | | | | |
| 2 | 478.378 | 454.453 | 504.224 | 478.615 | 16.642 | 840704 |
| | rounds: [486.508, 469.512, 504.224, 478.378, 454.453] | | | | | |
| 3 | 488.876 | 482.306 | 524.857 | 495.416 | 15.715 | 811008 |
| | rounds: [524.857, 482.306, 497.782, 488.876, 483.258] | | | | | |

#### Optimized

| Run | ns/op | ns/min | ns/p90 | ns/mean | stdev | iterations |
|-----|-------|--------|--------|---------|-------|------------|
| 1 | 270.276 | 269.079 | 280.453 | 273.565 | 4.937 | 1466368 |
| | rounds: [270.276, 280.453, 278.652, 269.079, 269.367] | | | | | |
| 2 | 270.276 | 265.468 | 277.705 | 271.358 | 4.695 | 1477632 |
| | rounds: [270.276, 267.557, 277.705, 275.785, 265.468] | | | | | |
| 3 | 271.328 | 269.606 | 277.140 | 272.158 | 2.595 | 1471488 |
| | rounds: [269.606, 270.927, 277.140, 271.328, 271.790] | | | | | |

### TruncateToWidth / trunc_fits

#### Original

| Run | ns/op | ns/min | ns/p90 | ns/mean | stdev | iterations |
|-----|-------|--------|--------|---------|-------|------------|
| 1 | 65.371 | 64.844 | 65.875 | 65.406 | 0.337 | 6118400 |
| | rounds: [65.575, 64.844, 65.366, 65.875, 65.371] | | | | | |
| 2 | 65.328 | 64.967 | 65.602 | 65.298 | 0.207 | 6127616 |
| | rounds: [64.967, 65.370, 65.328, 65.223, 65.602] | | | | | |
| 3 | 65.516 | 64.762 | 66.075 | 65.390 | 0.453 | 6120448 |
| | rounds: [65.553, 66.075, 65.516, 65.045, 64.762] | | | | | |

#### Optimized

| Run | ns/op | ns/min | ns/p90 | ns/mean | stdev | iterations |
|-----|-------|--------|--------|---------|-------|------------|
| 1 | 32.335 | 31.807 | 33.047 | 32.350 | 0.408 | 12369920 |
| | rounds: [32.335, 32.140, 31.807, 32.419, 33.047] | | | | | |
| 2 | 30.532 | 29.549 | 30.922 | 30.269 | 0.521 | 13221888 |
| | rounds: [29.770, 30.922, 30.572, 29.549, 30.532] | | | | | |
| 3 | 33.547 | 33.302 | 34.655 | 33.743 | 0.479 | 11858944 |
| | rounds: [33.457, 34.655, 33.302, 33.547, 33.755] | | | | | |

### TruncateToWidth / trunc_cut

#### Original

| Run | ns/op | ns/min | ns/p90 | ns/mean | stdev | iterations |
|-----|-------|--------|--------|---------|-------|------------|
| 1 | 94.017 | 93.118 | 94.705 | 93.867 | 0.553 | 4262912 |
| | rounds: [94.705, 93.420, 94.074, 94.017, 93.118] | | | | | |
| 2 | 95.512 | 95.140 | 96.070 | 95.536 | 0.315 | 4189184 |
| | rounds: [95.140, 95.634, 95.512, 96.070, 95.325] | | | | | |
| 3 | 94.712 | 93.975 | 96.270 | 94.794 | 0.828 | 4221952 |
| | rounds: [94.712, 96.270, 94.058, 94.954, 93.975] | | | | | |

#### Optimized

| Run | ns/op | ns/min | ns/p90 | ns/mean | stdev | iterations |
|-----|-------|--------|--------|---------|-------|------------|
| 1 | 33.691 | 33.151 | 34.038 | 33.591 | 0.302 | 11911168 |
| | rounds: [33.691, 33.693, 33.382, 34.038, 33.151] | | | | | |
| 2 | 33.647 | 32.990 | 34.648 | 33.745 | 0.531 | 11858944 |
| | rounds: [33.643, 33.647, 33.798, 32.990, 34.648] | | | | | |
| 3 | 33.351 | 33.236 | 33.815 | 33.469 | 0.220 | 11954176 |
| | rounds: [33.636, 33.236, 33.815, 33.351, 33.306] | | | | | |

### TruncateToWidth / trunc_wide_cut

#### Original

| Run | ns/op | ns/min | ns/p90 | ns/mean | stdev | iterations |
|-----|-------|--------|--------|---------|-------|------------|
| 1 | 129.780 | 127.105 | 130.533 | 128.985 | 1.366 | 3103744 |
| | rounds: [129.906, 129.780, 127.599, 130.533, 127.105] | | | | | |
| 2 | 129.821 | 128.044 | 133.449 | 130.312 | 1.833 | 3073024 |
| | rounds: [129.821, 130.990, 128.044, 129.257, 133.449] | | | | | |
| 3 | 129.061 | 127.375 | 129.446 | 128.597 | 0.846 | 3113984 |
| | rounds: [129.061, 129.446, 129.309, 127.375, 127.795] | | | | | |

#### Optimized

| Run | ns/op | ns/min | ns/p90 | ns/mean | stdev | iterations |
|-----|-------|--------|--------|---------|-------|------------|
| 1 | 53.340 | 52.703 | 53.971 | 53.426 | 0.449 | 7490560 |
| | rounds: [53.288, 53.827, 53.971, 53.340, 52.703] | | | | | |
| 2 | 53.844 | 53.260 | 55.142 | 53.959 | 0.659 | 7416832 |
| | rounds: [53.456, 53.844, 55.142, 54.092, 53.260] | | | | | |
| 3 | 53.780 | 53.104 | 54.339 | 53.645 | 0.464 | 7458816 |
| | rounds: [53.104, 53.153, 53.780, 53.849, 54.339] | | | | | |

### PayloadSize / size_code_mix

#### Original

| Run | ns/op | ns/min | ns/p90 | ns/mean | stdev | iterations |
|-----|-------|--------|--------|---------|-------|------------|
| 1 | 2.141 | 2.120 | 2.184 | 2.145 | 0.021 | 186517504 |
| | rounds: [2.141, 2.144, 2.184, 2.120, 2.136] | | | | | |
| 2 | 2.151 | 2.128 | 2.199 | 2.156 | 0.023 | 185575424 |
| | rounds: [2.199, 2.151, 2.145, 2.157, 2.128] | | | | | |
| 3 | 2.141 | 2.138 | 2.203 | 2.155 | 0.025 | 185663488 |
| | rounds: [2.138, 2.141, 2.150, 2.141, 2.203] | | | | | |

#### Optimized

| Run | ns/op | ns/min | ns/p90 | ns/mean | stdev | iterations |
|-----|-------|--------|--------|---------|-------|------------|
| 1 | 2.201 | 2.164 | 2.227 | 2.200 | 0.021 | 181804032 |
| | rounds: [2.201, 2.164, 2.215, 2.227, 2.196] | | | | | |
| 2 | 2.249 | 2.199 | 2.264 | 2.242 | 0.024 | 178448384 |
| | rounds: [2.237, 2.261, 2.199, 2.264, 2.249] | | | | | |
| 3 | 2.241 | 2.187 | 2.288 | 2.238 | 0.033 | 178788352 |
| | rounds: [2.241, 2.252, 2.187, 2.222, 2.288] | | | | | |

### PayloadSize / size_inline_only

#### Original

| Run | ns/op | ns/min | ns/p90 | ns/mean | stdev | iterations |
|-----|-------|--------|--------|---------|-------|------------|
| 1 | 1.688 | 1.677 | 1.693 | 1.687 | 0.005 | 237154304 |
| | rounds: [1.685, 1.690, 1.688, 1.677, 1.693] | | | | | |
| 2 | 1.694 | 1.675 | 1.715 | 1.698 | 0.015 | 235580416 |
| | rounds: [1.694, 1.694, 1.675, 1.715, 1.713] | | | | | |
| 3 | 1.669 | 1.667 | 1.691 | 1.677 | 0.011 | 238546944 |
| | rounds: [1.667, 1.691, 1.691, 1.669, 1.667] | | | | | |

#### Optimized

| Run | ns/op | ns/min | ns/p90 | ns/mean | stdev | iterations |
|-----|-------|--------|--------|---------|-------|------------|
| 1 | 1.589 | 1.567 | 1.692 | 1.604 | 0.045 | 249567232 |
| | rounds: [1.599, 1.692, 1.573, 1.567, 1.589] | | | | | |
| 2 | 1.606 | 1.600 | 1.639 | 1.614 | 0.015 | 247901184 |
| | rounds: [1.606, 1.600, 1.600, 1.624, 1.639] | | | | | |
| 3 | 1.604 | 1.580 | 1.652 | 1.606 | 0.025 | 249137152 |
| | rounds: [1.587, 1.580, 1.652, 1.604, 1.607] | | | | | |

### ReadPointer / ptr_size_mix

#### Original

| Run | ns/op | ns/min | ns/p90 | ns/mean | stdev | iterations |
|-----|-------|--------|--------|---------|-------|------------|
| 1 | 2.299 | 2.288 | 2.307 | 2.298 | 0.008 | 174086144 |
| | rounds: [2.307, 2.299, 2.306, 2.288, 2.288] | | | | | |
| 2 | 2.303 | 2.279 | 2.327 | 2.300 | 0.017 | 173916160 |
| | rounds: [2.288, 2.303, 2.305, 2.279, 2.327] | | | | | |
| 3 | 2.271 | 2.256 | 2.313 | 2.284 | 0.024 | 175167488 |
| | rounds: [2.266, 2.256, 2.271, 2.313, 2.313] | | | | | |

#### Optimized

| Run | ns/op | ns/min | ns/p90 | ns/mean | stdev | iterations |
|-----|-------|--------|--------|---------|-------|------------|
| 1 | 2.216 | 2.170 | 2.264 | 2.218 | 0.031 | 180420608 |
| | rounds: [2.264, 2.205, 2.233, 2.170, 2.216] | | | | | |
| 2 | 2.317 | 2.275 | 2.358 | 2.320 | 0.027 | 172465152 |
| | rounds: [2.312, 2.317, 2.358, 2.275, 2.336] | | | | | |
| 3 | 2.323 | 2.213 | 2.490 | 2.335 | 0.089 | 171521024 |
| | rounds: [2.345, 2.490, 2.323, 2.306, 2.213] | | | | | |

### TlsExt / ext_classify

#### Original

| Run | ns/op | ns/min | ns/p90 | ns/mean | stdev | iterations |
|-----|-------|--------|--------|---------|-------|------------|
| 1 | 1.750 | 1.721 | 1.817 | 1.756 | 0.033 | 227902464 |
| | rounds: [1.750, 1.750, 1.721, 1.817, 1.740] | | | | | |
| 2 | 1.825 | 1.791 | 1.890 | 1.836 | 0.034 | 217933824 |
| | rounds: [1.825, 1.791, 1.818, 1.890, 1.857] | | | | | |
| 3 | 1.833 | 1.777 | 1.842 | 1.820 | 0.025 | 219868160 |
| | rounds: [1.805, 1.777, 1.842, 1.842, 1.833] | | | | | |

#### Optimized

| Run | ns/op | ns/min | ns/p90 | ns/mean | stdev | iterations |
|-----|-------|--------|--------|---------|-------|------------|
| 1 | 1.459 | 1.429 | 1.482 | 1.455 | 0.020 | 274881536 |
| | rounds: [1.470, 1.482, 1.429, 1.459, 1.438] | | | | | |
| 2 | 1.465 | 1.451 | 1.467 | 1.462 | 0.006 | 273529856 |
| | rounds: [1.461, 1.465, 1.467, 1.451, 1.467] | | | | | |
| 3 | 1.474 | 1.461 | 1.531 | 1.486 | 0.025 | 269327360 |
| | rounds: [1.492, 1.531, 1.470, 1.461, 1.474] | | | | | |

### TlsExt / sni_strip_all

#### Original

| Run | ns/op | ns/min | ns/p90 | ns/mean | stdev | iterations |
|-----|-------|--------|--------|---------|-------|------------|
| 1 | 36.276 | 35.300 | 36.474 | 36.044 | 0.423 | 11101184 |
| | rounds: [36.276, 35.862, 36.307, 36.474, 35.300] | | | | | |
| 2 | 36.431 | 35.833 | 36.998 | 36.497 | 0.437 | 10964992 |
| | rounds: [35.833, 36.431, 36.951, 36.271, 36.998] | | | | | |
| 3 | 35.978 | 35.306 | 36.649 | 35.967 | 0.434 | 11124736 |
| | rounds: [36.090, 35.306, 35.813, 35.978, 36.649] | | | | | |

#### Optimized

| Run | ns/op | ns/min | ns/p90 | ns/mean | stdev | iterations |
|-----|-------|--------|--------|---------|-------|------------|
| 1 | 2.088 | 2.084 | 2.199 | 2.111 | 0.045 | 189593600 |
| | rounds: [2.084, 2.199, 2.088, 2.097, 2.085] | | | | | |
| 2 | 2.116 | 2.096 | 2.138 | 2.118 | 0.014 | 188886016 |
| | rounds: [2.096, 2.113, 2.116, 2.126, 2.138] | | | | | |
| 3 | 2.121 | 2.109 | 2.174 | 2.128 | 0.024 | 188011520 |
| | rounds: [2.110, 2.174, 2.109, 2.125, 2.121] | | | | | |

### TlsExt / sni_strip_tail

#### Original

| Run | ns/op | ns/min | ns/p90 | ns/mean | stdev | iterations |
|-----|-------|--------|--------|---------|-------|------------|
| 1 | 3.435 | 3.427 | 3.478 | 3.444 | 0.018 | 116140032 |
| | rounds: [3.435, 3.478, 3.451, 3.427, 3.431] | | | | | |
| 2 | 3.463 | 3.438 | 3.501 | 3.469 | 0.022 | 115310592 |
| | rounds: [3.484, 3.501, 3.463, 3.438, 3.460] | | | | | |
| 3 | 3.542 | 3.533 | 3.752 | 3.594 | 0.083 | 111363072 |
| | rounds: [3.603, 3.539, 3.752, 3.533, 3.542] | | | | | |

#### Optimized

| Run | ns/op | ns/min | ns/p90 | ns/mean | stdev | iterations |
|-----|-------|--------|--------|---------|-------|------------|
| 1 | 4.234 | 4.015 | 4.285 | 4.192 | 0.099 | 95508480 |
| | rounds: [4.268, 4.015, 4.234, 4.155, 4.285] | | | | | |
| 2 | 4.334 | 4.192 | 4.531 | 4.353 | 0.112 | 91950080 |
| | rounds: [4.404, 4.531, 4.334, 4.192, 4.306] | | | | | |
| 3 | 4.181 | 4.154 | 4.426 | 4.230 | 0.100 | 94611456 |
| | rounds: [4.154, 4.171, 4.426, 4.219, 4.181] | | | | | |

### Rd / rd16_mixed

#### Original

| Run | ns/op | ns/min | ns/p90 | ns/mean | stdev | iterations |
|-----|-------|--------|--------|---------|-------|------------|
| 1 | 1.264 | 1.252 | 1.303 | 1.272 | 0.018 | 314644480 |
| | rounds: [1.279, 1.259, 1.303, 1.252, 1.264] | | | | | |
| 2 | 1.268 | 1.255 | 1.308 | 1.277 | 0.020 | 313396224 |
| | rounds: [1.293, 1.260, 1.268, 1.308, 1.255] | | | | | |
| 3 | 1.265 | 1.246 | 1.284 | 1.263 | 0.013 | 316744704 |
| | rounds: [1.255, 1.246, 1.265, 1.265, 1.284] | | | | | |

#### Optimized

| Run | ns/op | ns/min | ns/p90 | ns/mean | stdev | iterations |
|-----|-------|--------|--------|---------|-------|------------|
| 1 | 1.326 | 1.317 | 1.341 | 1.328 | 0.008 | 301320192 |
| | rounds: [1.322, 1.332, 1.341, 1.317, 1.326] | | | | | |
| 2 | 1.287 | 1.269 | 1.295 | 1.286 | 0.010 | 311063552 |
| | rounds: [1.287, 1.295, 1.269, 1.284, 1.295] | | | | | |
| 3 | 1.346 | 1.328 | 1.355 | 1.341 | 0.011 | 298253312 |
| | rounds: [1.330, 1.355, 1.346, 1.347, 1.328] | | | | | |

### Rd / rd32_mixed

#### Original

| Run | ns/op | ns/min | ns/p90 | ns/mean | stdev | iterations |
|-----|-------|--------|--------|---------|-------|------------|
| 1 | 1.326 | 1.315 | 1.377 | 1.337 | 0.022 | 299313152 |
| | rounds: [1.315, 1.324, 1.342, 1.377, 1.326] | | | | | |
| 2 | 1.331 | 1.311 | 1.458 | 1.354 | 0.053 | 295867392 |
| | rounds: [1.311, 1.458, 1.331, 1.322, 1.348] | | | | | |
| 3 | 1.323 | 1.311 | 1.334 | 1.323 | 0.008 | 302386176 |
| | rounds: [1.311, 1.317, 1.334, 1.323, 1.330] | | | | | |

#### Optimized

| Run | ns/op | ns/min | ns/p90 | ns/mean | stdev | iterations |
|-----|-------|--------|--------|---------|-------|------------|
| 1 | 1.275 | 1.266 | 1.290 | 1.278 | 0.010 | 313076736 |
| | rounds: [1.266, 1.290, 1.275, 1.269, 1.288] | | | | | |
| 2 | 1.287 | 1.279 | 1.309 | 1.292 | 0.011 | 309521408 |
| | rounds: [1.279, 1.309, 1.287, 1.286, 1.302] | | | | | |
| 3 | 1.351 | 1.299 | 1.354 | 1.338 | 0.021 | 298930176 |
| | rounds: [1.299, 1.351, 1.352, 1.337, 1.354] | | | | | |

### JoinSamples / join_small

#### Original

| Run | ns/op | ns/min | ns/p90 | ns/mean | stdev | iterations |
|-----|-------|--------|--------|---------|-------|------------|
| 1 | 120.220 | 117.066 | 123.302 | 120.247 | 2.196 | 3330048 |
| | rounds: [120.220, 118.799, 117.066, 123.302, 121.848] | | | | | |
| 2 | 125.271 | 123.583 | 128.233 | 125.458 | 1.526 | 3191808 |
| | rounds: [123.583, 125.370, 125.271, 124.834, 128.233] | | | | | |
| 3 | 123.158 | 122.249 | 126.426 | 123.895 | 1.455 | 3231744 |
| | rounds: [122.249, 126.426, 124.497, 123.147, 123.158] | | | | | |

#### Optimized

| Run | ns/op | ns/min | ns/p90 | ns/mean | stdev | iterations |
|-----|-------|--------|--------|---------|-------|------------|
| 1 | 1721.945 | 1697.879 | 1793.555 | 1731.834 | 32.371 | 234496 |
| | rounds: [1793.555, 1697.879, 1719.771, 1726.019, 1721.945] | | | | | |
| 2 | 1731.084 | 1686.411 | 1756.788 | 1725.324 | 28.106 | 234496 |
| | rounds: [1699.607, 1686.411, 1731.084, 1756.788, 1752.730] | | | | | |
| 3 | 1731.726 | 1660.004 | 1751.191 | 1709.512 | 37.891 | 236544 |
| | rounds: [1751.191, 1731.726, 1667.726, 1660.004, 1736.916] | | | | | |

### JoinSamples / join_500_300

#### Original

| Run | ns/op | ns/min | ns/p90 | ns/mean | stdev | iterations |
|-----|-------|--------|--------|---------|-------|------------|
| 1 | 80117.383 | 78446.875 | 80677.930 | 79844.941 | 750.762 | 5120 |
| | rounds: [80153.613, 80117.383, 78446.875, 79828.906, 80677.930] | | | | | |
| 2 | 80673.633 | 79700.391 | 80978.906 | 80527.910 | 467.406 | 5120 |
| | rounds: [80922.852, 80673.633, 80363.770, 80978.906, 79700.391] | | | | | |
| 3 | 79772.754 | 79188.867 | 81559.180 | 80194.648 | 840.702 | 5120 |
| | rounds: [79736.133, 80716.309, 79188.867, 81559.180, 79772.754] | | | | | |

#### Optimized

| Run | ns/op | ns/min | ns/p90 | ns/mean | stdev | iterations |
|-----|-------|--------|--------|---------|-------|------------|
| 1 | 55993.359 | 54689.355 | 56820.605 | 55830.059 | 706.812 | 10240 |
| | rounds: [55993.359, 54689.355, 56130.713, 56820.605, 55516.260] | | | | | |
| 2 | 57071.875 | 56418.604 | 58236.279 | 57335.479 | 705.514 | 10240 |
| | rounds: [56871.973, 56418.604, 58078.662, 57071.875, 58236.279] | | | | | |
| 3 | 57222.168 | 57063.672 | 57532.861 | 57249.697 | 179.907 | 10240 |
| | rounds: [57066.797, 57532.861, 57362.988, 57222.168, 57063.672] | | | | | |

### JoinSamples / join_2k_2k

#### Original

| Run | ns/op | ns/min | ns/p90 | ns/mean | stdev | iterations |
|-----|-------|--------|--------|---------|-------|------------|
| 1 | 2187062.891 | 2174286.035 | 2189425.879 | 2185073.047 | 5602.040 | 5120 |
| | rounds: [2174286.035, 2189266.504, 2189425.879, 2185323.926, 2187062.891] | | | | | |
| 2 | 2203220.215 | 2201915.430 | 2218953.711 | 2207325.117 | 6442.355 | 5120 |
| | rounds: [2218953.711, 2203220.215, 2202807.910, 2209728.320, 2201915.430] | | | | | |
| 3 | 2191956.445 | 2185951.758 | 2194566.699 | 2191301.074 | 3010.070 | 5120 |
| | rounds: [2185951.758, 2191956.445, 2194566.699, 2190503.027, 2193527.441] | | | | | |

#### Optimized

| Run | ns/op | ns/min | ns/p90 | ns/mean | stdev | iterations |
|-----|-------|--------|--------|---------|-------|------------|
| 1 | 265342.578 | 260799.219 | 266079.102 | 264411.699 | 1890.442 | 5120 |
| | rounds: [265492.285, 260799.219, 264345.312, 266079.102, 265342.578] | | | | | |
| 2 | 269846.680 | 266281.738 | 276066.113 | 269853.887 | 3507.981 | 5120 |
| | rounds: [269846.680, 266705.566, 266281.738, 276066.113, 270369.336] | | | | | |
| 3 | 266275.684 | 263665.039 | 270287.305 | 266187.207 | 2295.669 | 5120 |
| | rounds: [266275.684, 264410.059, 270287.305, 266297.949, 263665.039] | | | | | |

### JoinSamples / join_mdns

#### Original

| Run | ns/op | ns/min | ns/p90 | ns/mean | stdev | iterations |
|-----|-------|--------|--------|---------|-------|------------|
| 1 | 75775.977 | 74651.318 | 76217.871 | 75484.385 | 586.863 | 10240 |
| | rounds: [74651.318, 75775.977, 74948.730, 75828.027, 76217.871] | | | | | |
| 2 | 75750.684 | 75097.510 | 76991.895 | 75890.723 | 619.287 | 10240 |
| | rounds: [75097.510, 75750.684, 75661.670, 75951.855, 76991.895] | | | | | |
| 3 | 74932.910 | 74751.367 | 78385.254 | 75569.688 | 1410.559 | 9216 |
| | rounds: [74932.910, 74990.430, 74751.367, 74788.477, 78385.254] | | | | | |

#### Optimized

| Run | ns/op | ns/min | ns/p90 | ns/mean | stdev | iterations |
|-----|-------|--------|--------|---------|-------|------------|
| 1 | 28774.674 | 28524.967 | 29093.034 | 28765.677 | 194.202 | 15360 |
| | rounds: [29093.034, 28524.967, 28620.866, 28774.674, 28814.844] | | | | | |
| 2 | 29334.342 | 28362.988 | 29751.921 | 29244.297 | 480.896 | 15360 |
| | rounds: [29334.342, 28362.988, 29751.921, 29194.303, 29577.930] | | | | | |
| 3 | 29490.072 | 29021.061 | 30352.767 | 29588.236 | 458.842 | 15360 |
| | rounds: [29021.061, 29490.072, 30352.767, 29795.475, 29281.803] | | | | | |

### FormatStreamHex / hex_1460_16

#### Original

| Run | ns/op | ns/min | ns/p90 | ns/mean | stdev | iterations |
|-----|-------|--------|--------|---------|-------|------------|
| 1 | 105308.984 | 103537.305 | 105798.340 | 104989.336 | 785.764 | 5120 |
| | rounds: [105442.383, 103537.305, 105308.984, 105798.340, 104859.668] | | | | | |
| 2 | 105526.172 | 105385.840 | 106689.941 | 105770.488 | 477.183 | 5120 |
| | rounds: [106689.941, 105385.840, 105772.363, 105478.125, 105526.172] | | | | | |
| 3 | 105254.297 | 104524.902 | 105642.285 | 105171.582 | 369.384 | 5120 |
| | rounds: [105254.297, 105093.164, 104524.902, 105642.285, 105343.262] | | | | | |

#### Optimized

| Run | ns/op | ns/min | ns/p90 | ns/mean | stdev | iterations |
|-----|-------|--------|--------|---------|-------|------------|
| 1 | 6989.331 | 6943.587 | 7079.476 | 6991.732 | 47.796 | 61440 |
| | rounds: [6943.587, 6989.331, 7079.476, 6954.321, 6991.943] | | | | | |
| 2 | 7137.971 | 7112.663 | 7203.711 | 7149.938 | 31.064 | 57344 |
| | rounds: [7133.700, 7161.648, 7112.663, 7137.971, 7203.711] | | | | | |
| 3 | 7066.675 | 6899.447 | 7121.517 | 7037.324 | 76.251 | 61440 |
| | rounds: [7066.675, 6899.447, 7079.614, 7019.368, 7121.517] | | | | | |

### FormatStreamHex / hex_64k_64

#### Original

| Run | ns/op | ns/min | ns/p90 | ns/mean | stdev | iterations |
|-----|-------|--------|--------|---------|-------|------------|
| 1 | 4347238.574 | 4305942.090 | 4352438.770 | 4338773.770 | 16952.049 | 5120 |
| | rounds: [4305942.090, 4348777.148, 4347238.574, 4339472.266, 4352438.770] | | | | | |
| 2 | 4348931.934 | 4315612.695 | 4360776.758 | 4342258.652 | 15757.755 | 5120 |
| | rounds: [4360776.758, 4348931.934, 4351442.188, 4334529.688, 4315612.695] | | | | | |
| 3 | 4331566.211 | 4312876.367 | 4335139.160 | 4328233.984 | 7955.400 | 5120 |
| | rounds: [4332878.809, 4312876.367, 4328709.375, 4331566.211, 4335139.160] | | | | | |

#### Optimized

| Run | ns/op | ns/min | ns/p90 | ns/mean | stdev | iterations |
|-----|-------|--------|--------|---------|-------|------------|
| 1 | 158260.645 | 157179.492 | 159788.672 | 158414.824 | 888.417 | 5120 |
| | rounds: [159788.672, 158927.148, 157918.164, 158260.645, 157179.492] | | | | | |
| 2 | 161329.102 | 156301.758 | 164063.281 | 160311.289 | 2700.199 | 5120 |
| | rounds: [156301.758, 161508.594, 158353.711, 161329.102, 164063.281] | | | | | |
| 3 | 157415.137 | 155940.625 | 160544.141 | 158167.754 | 1812.232 | 5120 |
| | rounds: [156878.027, 155940.625, 157415.137, 160060.840, 160544.141] | | | | | |

### FormatStreamHex / hex_40_16

#### Original

| Run | ns/op | ns/min | ns/p90 | ns/mean | stdev | iterations |
|-----|-------|--------|--------|---------|-------|------------|
| 1 | 3134.230 | 3081.727 | 3184.254 | 3129.624 | 36.332 | 130048 |
| | rounds: [3099.095, 3184.254, 3148.812, 3134.230, 3081.727] | | | | | |
| 2 | 3094.002 | 3066.564 | 3139.586 | 3103.279 | 25.700 | 132096 |
| | rounds: [3094.002, 3139.586, 3092.259, 3123.986, 3066.564] | | | | | |
| 3 | 3130.023 | 3093.224 | 3220.074 | 3138.348 | 43.772 | 130048 |
| | rounds: [3220.074, 3138.270, 3130.023, 3093.224, 3110.149] | | | | | |

#### Optimized

| Run | ns/op | ns/min | ns/p90 | ns/mean | stdev | iterations |
|-----|-------|--------|--------|---------|-------|------------|
| 1 | 288.518 | 286.797 | 290.618 | 288.661 | 1.247 | 1388544 |
| | rounds: [289.161, 288.209, 288.518, 290.618, 286.797] | | | | | |
| 2 | 292.737 | 289.042 | 316.563 | 298.632 | 10.630 | 1343488 |
| | rounds: [316.563, 304.950, 289.865, 292.737, 289.042] | | | | | |
| 3 | 286.649 | 284.194 | 295.549 | 287.668 | 4.089 | 1393664 |
| | rounds: [286.649, 287.120, 295.549, 284.194, 284.826] | | | | | |

### ToLowerW / lower_proc

#### Original

| Run | ns/op | ns/min | ns/p90 | ns/mean | stdev | iterations |
|-----|-------|--------|--------|---------|-------|------------|
| 1 | 59.813 | 59.040 | 60.032 | 59.678 | 0.357 | 6707200 |
| | rounds: [59.948, 59.558, 60.032, 59.040, 59.813] | | | | | |
| 2 | 59.499 | 58.915 | 60.296 | 59.491 | 0.508 | 6727680 |
| | rounds: [59.499, 58.915, 60.296, 58.996, 59.748] | | | | | |
| 3 | 58.675 | 58.047 | 60.643 | 58.930 | 0.913 | 6791168 |
| | rounds: [58.675, 60.643, 58.047, 58.973, 58.312] | | | | | |

#### Optimized

| Run | ns/op | ns/min | ns/p90 | ns/mean | stdev | iterations |
|-----|-------|--------|--------|---------|-------|------------|
| 1 | 33.469 | 33.338 | 33.836 | 33.555 | 0.203 | 11923456 |
| | rounds: [33.756, 33.375, 33.338, 33.469, 33.836] | | | | | |
| 2 | 33.648 | 33.286 | 33.939 | 33.634 | 0.216 | 11895808 |
| | rounds: [33.286, 33.743, 33.551, 33.648, 33.939] | | | | | |
| 3 | 34.116 | 33.964 | 35.665 | 34.498 | 0.632 | 11600896 |
| | rounds: [35.665, 33.964, 34.116, 34.076, 34.667] | | | | | |

### ToLowerW / lower_path

#### Original

| Run | ns/op | ns/min | ns/p90 | ns/mean | stdev | iterations |
|-----|-------|--------|--------|---------|-------|------------|
| 1 | 856.495 | 846.108 | 893.100 | 862.103 | 16.175 | 466944 |
| | rounds: [860.224, 846.108, 856.495, 893.100, 854.587] | | | | | |
| 2 | 857.695 | 848.017 | 865.308 | 857.406 | 5.844 | 470016 |
| | rounds: [865.308, 848.017, 861.119, 857.695, 854.890] | | | | | |
| 3 | 861.046 | 843.648 | 881.082 | 862.626 | 12.072 | 465920 |
| | rounds: [843.648, 867.083, 881.082, 861.046, 860.270] | | | | | |

#### Optimized

| Run | ns/op | ns/min | ns/p90 | ns/mean | stdev | iterations |
|-----|-------|--------|--------|---------|-------|------------|
| 1 | 63.409 | 62.989 | 64.686 | 63.566 | 0.602 | 6295552 |
| | rounds: [62.989, 63.120, 63.626, 63.409, 64.686] | | | | | |
| 2 | 63.844 | 62.806 | 66.784 | 64.393 | 1.420 | 6217728 |
| | rounds: [63.844, 66.784, 62.806, 65.134, 63.395] | | | | | |
| 3 | 64.470 | 62.894 | 66.479 | 64.525 | 1.275 | 6204416 |
| | rounds: [64.470, 62.894, 66.479, 63.492, 65.291] | | | | | |

### ToLowerW / lower_4k

#### Original

| Run | ns/op | ns/min | ns/p90 | ns/mean | stdev | iterations |
|-----|-------|--------|--------|---------|-------|------------|
| 1 | 9550.412 | 9522.689 | 9667.155 | 9585.707 | 58.646 | 46080 |
| | rounds: [9550.412, 9522.689, 9543.132, 9645.150, 9667.155] | | | | | |
| 2 | 9536.806 | 9447.146 | 9719.347 | 9558.841 | 104.175 | 46080 |
| | rounds: [9447.146, 9458.333, 9719.347, 9536.806, 9632.574] | | | | | |
| 3 | 9520.909 | 9445.041 | 9584.147 | 9516.356 | 45.598 | 46080 |
| | rounds: [9520.909, 9584.147, 9445.041, 9534.462, 9497.222] | | | | | |

#### Optimized

| Run | ns/op | ns/min | ns/p90 | ns/mean | stdev | iterations |
|-----|-------|--------|--------|---------|-------|------------|
| 1 | 505.821 | 501.293 | 506.107 | 504.222 | 2.097 | 795648 |
| | rounds: [501.293, 505.821, 506.107, 505.835, 502.055] | | | | | |
| 2 | 505.537 | 504.763 | 516.565 | 508.473 | 4.525 | 789504 |
| | rounds: [505.156, 504.763, 516.565, 510.346, 505.537] | | | | | |
| 3 | 494.977 | 490.739 | 504.106 | 496.442 | 5.076 | 807936 |
| | rounds: [491.991, 500.396, 490.739, 494.977, 504.106] | | | | | |

### ToLowerW / lower_mixed

#### Original

| Run | ns/op | ns/min | ns/p90 | ns/mean | stdev | iterations |
|-----|-------|--------|--------|---------|-------|------------|
| 1 | 1379.225 | 1371.015 | 1464.978 | 1399.813 | 34.532 | 288768 |
| | rounds: [1379.225, 1378.918, 1371.015, 1404.930, 1464.978] | | | | | |
| 2 | 1391.521 | 1377.419 | 1397.618 | 1389.578 | 7.446 | 289792 |
| | rounds: [1377.419, 1391.521, 1396.078, 1397.618, 1385.254] | | | | | |
| 3 | 1378.188 | 1370.833 | 1383.395 | 1377.708 | 4.004 | 291840 |
| | rounds: [1370.833, 1377.849, 1383.395, 1378.274, 1378.188] | | | | | |

#### Optimized

| Run | ns/op | ns/min | ns/p90 | ns/mean | stdev | iterations |
|-----|-------|--------|--------|---------|-------|------------|
| 1 | 84.515 | 84.223 | 87.376 | 85.261 | 1.235 | 4695040 |
| | rounds: [84.515, 85.949, 84.240, 87.376, 84.223] | | | | | |
| 2 | 85.174 | 84.841 | 87.200 | 85.578 | 0.875 | 4676608 |
| | rounds: [84.841, 85.774, 84.904, 87.200, 85.174] | | | | | |
| 3 | 86.247 | 83.375 | 87.355 | 85.826 | 1.392 | 4665344 |
| | rounds: [86.247, 83.375, 86.799, 85.355, 87.355] | | | | | |

### Pipeline / packet_hotpath

#### Original

| Run | ns/op | ns/min | ns/p90 | ns/mean | stdev | iterations |
|-----|-------|--------|--------|---------|-------|------------|
| 1 | 79.796 | 79.336 | 81.084 | 79.978 | 0.587 | 5004288 |
| | rounds: [81.084, 79.756, 79.919, 79.336, 79.796] | | | | | |
| 2 | 79.010 | 78.694 | 82.452 | 79.757 | 1.404 | 5018624 |
| | rounds: [78.811, 79.815, 78.694, 79.010, 82.452] | | | | | |
| 3 | 80.673 | 79.380 | 81.204 | 80.344 | 0.777 | 4982784 |
| | rounds: [80.673, 81.204, 81.010, 79.451, 79.380] | | | | | |

#### Optimized

| Run | ns/op | ns/min | ns/p90 | ns/mean | stdev | iterations |
|-----|-------|--------|--------|---------|-------|------------|
| 1 | 45.229 | 44.830 | 46.631 | 45.419 | 0.652 | 8811520 |
| | rounds: [45.501, 44.901, 45.229, 46.631, 44.830] | | | | | |
| 2 | 45.537 | 45.165 | 45.648 | 45.423 | 0.194 | 8809472 |
| | rounds: [45.547, 45.165, 45.648, 45.537, 45.217] | | | | | |
| 3 | 45.610 | 45.268 | 45.761 | 45.563 | 0.162 | 8780800 |
| | rounds: [45.613, 45.610, 45.761, 45.564, 45.268] | | | | | |

---

## Recommendations

Of 133 benchmarked workloads:
- 71 faster (speedup > 5%)
- 35 slower (regression > 5%)
- 27 statistically tied (within 5%)

**Overall geometric mean speedup: 1.59x**

### Recommendation: Integrate with Monitoring

Most functions (71/133) are faster, but 35 show regressions.
Consider integrating the faster functions individually, starting with the highest speedups.

### Integration Checklist
1. Follow `wintcp/tests/asm/HOWTO-INTEGRATION.md` for step-by-step instructions.
2. Add `WINTCP_ENABLE_ASM_OPTIMIZATIONS` guard for CPU feature detection.
3. Run `wintcp\tests\cli.bat` and `wintcp-tests.exe unit` to verify.
4. Monitor production performance after deployment.
