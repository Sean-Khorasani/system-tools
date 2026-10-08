# Development

How to build, test, benchmark and document WinTCP. User-facing behaviour is in the [GUI guide](gui.md) and the [CLI reference](cli.md); how the code fits together is in [Architecture](architecture.md).

## Contents

- [Building](#building)
- [Testing](#testing)
- [Benchmarks](#benchmarks)
- [Integer-boundary assumptions](#integer-boundary-assumptions)
- [Documentation conventions](#documentation-conventions)

## Building

### Requirements

Visual Studio with the C++ workload (MSVC + a Windows SDK). No administrator rights, no package manager, no third-party dependencies - nothing to install beyond the compiler itself.

### The three build paths

```bat
:: primary build (Developer Command Prompt, or it self-locates vcvars):
build.bat          :: -> build\wintcp.exe
build.bat clean    :: remove build\ first

:: alternatives:
cmake -S . -B build-cmake && cmake --build build-cmake --config Release
:: or open wintcp\wintcp.vcxproj in Visual Studio
```

All three produce the same two binaries: `wintcp.exe` and `build\tests\wintcp-tests.exe`.

### Development build (incremental, not a gate)

`build.bat` recompiles everything on every run because the project is small enough that a clean
rebuild is fast *once*; what it is not fast at is the edit-compile-test loop when you are
hunting a defect and want to recompile only the one file you touched.

```bat
fast-build.bat            :: build-fast\wintcp.exe   (only stale files, 8 in parallel)
fast-build.bat -Test      :: build + smoke + unit    (the whole loop in one shot)
fast-build.bat -TestOnly  :: run the gate against the existing build-fast output
fast-build.bat -Force     :: ignore timestamps / state hash; rebuild everything
fast-build.bat -Clean     :: delete build-fast\ and exit
```

`-Test` is the normal dev call: it compiles only stale translation units (a `.cpp` newer than its
`.obj`, plus anything reachable through the `#include "..."` closure), relinks what changed, then
runs the **same** 13 CLI smoke checks, the harness self-check, and `wintcp-tests.exe unit` that
`cli.bat` does. Exit codes: `0` ok, `1` build/test failed, `2` checks failed, `3` setup error
(missing toolchain, lock held, source-set problem). The `fast-build\` directory is the only output
location; nothing in `wintcp\`, `build\`, or the source tree is written. `temp/` is not touched
either.

There is also `fast-build.ps1` (what `fast-build.bat` invokes); invoke it directly if you want to
run inside an already-configured Native Tools Prompt without the wrapper:
`powershell -NoProfile -ExecutionPolicy Bypass -File fast-build.ps1 -Test`.

**Intended difference, not drift. `fast-build` is a developer aid, `build.bat` is the gate.**
`fast-build` deliberately diverges from `build.bat` so a dev loop can be cheap and debuggable, and
the divergence is *named* in `fast-build.ps1`'s header:

- **`/FS`** is added on the `cl` line because `fast-build` runs up to 8 `cl.exe` instances at once
  and `/Zi` makes them all write `vc140.pdb`; without `/FS` they race and fail with `C1041`. A
  single `cl` (`build.bat`) never needed it.
- **`/Zi` + `/DEBUG` + `/PDB`** produce `build-fast\wintcp.pdb` so a crash in a dev build has
  symbols and a stack trace. `build.bat` links `/DEBUG:NONE` on purpose; treating this difference
  as an "improvement" to copy back would be wrong.
- **`/Gy /GL /LTCG` are off.** Whole-program optimization is a shipping concern (it is why
  `build.bat` carries `/LTCG`), not a dev-loop concern; turning it on would make every incremental
  build a full link.
- **`/I wintcp\src`** is given to the three test translation units so `#include "Connection.h"`
  resolves from `wintcp\tests\` the same way `build.bat`'s test line does.

The smoke gate inside `fast-build` mirrors `wintcp\tests\cli.bat`; if `cli.bat` and `fast-build` ever
disagree on a result, `cli.bat` wins - this is a *smoke* gate, not the gate. `build.bat` (and a `cmake`
or Visual Studio build) remain the things that prove a change ships.

Incremental correctness: touching one `.cpp` recompiles that one file and relinks the affected
binary in ~1s; touching a header recompiles only the translation units whose include closure
contains it (the closure is computed from `wintcp\src` for the test files, the same `/I` `build.bat`
uses). Timestamps are a heuristic; if the inputs or flags change, a SHA-256 of the flag set and
source list is stored in `build-fast\state.txt` and forces a full rebuild on mismatch. A `.lock`
directory survives a crashed run until removed; stale locks are detected by mtime, and if a dead
lock blocks you, delete `build-fast\.lock` (the script prints the PID it thinks holds the lock and
suggests this).

### The flags, and what must not change

`build.bat` compiles both translation-unit sets with:

```text
/std:c++17 /EHsc /W4 /WX /permissive- /Zc:__cplusplus /utf-8
/DUNICODE /D_UNICODE /D_WIN32_WINNT=0x0601 /DWIN32_LEAN_AND_MEAN
/O2 /MT /Gy /GL /GF /Os
```

- **`/W4 /WX`** - warnings are errors. A warning or a non-conforming construct fails the build rather than accumulating.
- **`/permissive- /Zc:__cplusplus`** - conformance mode. Together they make a non-conforming construct a compile error instead of an MSVC extension that later breaks elsewhere.
- **`/utf-8`** - the sources are UTF-8; without this the em-dash and the ellipsis in the output turn into mojibake.
- **`/MT`** - static CRT, so the executable has no runtime dependency to go missing.

Do not weaken any of these to make a build pass.

`/guard:cf` (Control Flow Guard) is now enabled on **all four** build paths:
`build.bat` (`/guard:cf` on both its `cl` lines - it drives compile and link in
one command, so one occurrence covers both), **CMake** (`/guard:cf` on both the
`wintcp` and `wintcp-tests` targets), **Visual Studio** for the product
(`<EnableControlFlowGuard>true</EnableControlFlowGuard>` in all four
configurations) and **Visual Studio** for the tests (the same property in that
project's unconditioned `ItemDefinitionGroup`, which covers every configuration).

**This was an inversion, and the direction of it was the bug.** `build.bat` is the
primary path - it is what gates every change - and it was the one path with no CFG,
while the two secondary paths had it. The same inversion existed twice more: the
CMake `wintcp-tests` target and `wintcp-tests.vcxproj` both lacked it while their
product counterparts had it. So all three gaps had the same shape: the thing that
gates every change was the thing not instrumented. A gap is worth closing by name;
an inversion is worth closing by shape, because it will recur.

Verified rather than assumed: `dumpbin /headers` reports **Control Flow Guard** in
the `DllCharacteristics` of both `build\wintcp.exe` and `build\tests\wintcp-tests.exe`,
and all four gates pass with it enabled. That second part matters more than it looks:
CFG is a *runtime* change as well as a link-time one, and this codebase resolves
function pointers through `GetProcAddress` and delay-loads six DLLs, which is
exactly the shape that CFG can fault on. It does not, here.

`/analyze` is deliberately *not* part of any default build - it is far too slow to gate every compile. Run it ad hoc on the translation unit you are changing.

### The manifest, and why `/MANIFEST:NO` is load-bearing

The application manifest (Common Controls v6 + PerMonitorV2 DPI) is embedded by the resource compiler through `wintcp\res\wintcp.manifest` (`RT_MANIFEST` id 1) - it is *not* the link step's job. If the linker also generated one you get `CVT1100` then `LNK1123`, so all three paths disable it: `/MANIFEST:NO` in `CMakeLists.txt`, `<GenerateManifest>false</GenerateManifest>` in both `.vcxproj` files. Keep them in step - this has already drifted once.

### `/SUBSYSTEM:CONSOLE`

Also a correctness requirement, not a preference. `main.cpp` returns an exit code, and an interactive `cmd.exe` does **not** wait for a GUI-subsystem process to finish - so a batch gate driving a `WINDOWS`-subsystem `wintcp.exe` would read the file before it was written. All three build paths therefore use `Console`; `CMakeLists.txt` and `wintcp.vcxproj` both carry a comment recording the earlier drift.

## Testing

For day-to-day work, `fast-build.bat -Test` (see [Development build (incremental, not a gate)](#development-build-incremental-not-a-gate)) builds the stale files, relinks, and runs this same gate for you in one shot; `-TestOnly` re-runs just the gate against the existing `build-fast\` output without recompiling.

### Test and benchmark binary

`wintcp-tests.exe` is the development test driver. It is **not distributed** - `wintcp.exe` is the tool. It links the *same* product sources as the product, minus `main.cpp`, so every check runs the exact functions the GUI and the CLI run. A test against a copy of the logic would prove nothing.

```bat
build\tests\wintcp-tests.exe unit              :: internal checks; exit 0 only if all pass
build\tests\wintcp-tests.exe ui                :: GUI checks (needs a desktop session)
build\tests\wintcp-tests.exe bench 100000 10   :: time the view pipeline
```

| Mode | What it does | Why it is separate |
|---|---|---|
| `unit` | Runs the **production** parser, diff, sort, grouping, filter, formatting, join, GeoIP, TLS-decode, reassembly and capability code against fixed inputs, printing `PASS`/`FAIL` per check and ending with `selftest: all checks passed`. | No window, no network, no registry writes, so it can gate a change. Exit 0 only when every check passes. |
| `ui` | Constructs the real windows and dialogs and drives the real `WndProc`. | Needs an interactive desktop session, which is why it is not part of `unit`. |
| `bench` | Times the view pipeline over synthetic rows. | Measures the *tool*, not the machine. See [Benchmarks](#benchmarks). |

`selftest`, `bench` and `--uiharness` were compiled into `wintcp.exe` until 2026-10-02. They moved out on that date, and all three are now *unknown command* to the product - verified, not assumed. The one exception is the hidden `crashtest` verb, kept so the gate below can drive the crash path against a real process.

The checks are not padding. Each pins a decision that is easy to get wrong and impossible to eyeball in a list: that 12 identical mDNS sockets produce **zero** change events on a repeat snapshot; that a per-PID byte total is refused as a per-connection rate while a per-socket one is accepted; that a bookmark on port 9999 does not mark a live port 443; that `--event appear` prints no DISAPPEAR; that a count nobody measured is not printed. Those were all real defects.

**A check count is not a coverage figure.** There is no coverage instrumentation in this project, so a count like `PASS 514 / FAIL 0` says how much was *written*, not how much of the code was *exercised*. The number moves whenever a defect becomes a regression test; the gates report it, nothing quotes it.

### Gate scripts

The gates live in `wintcp\tests\` beside the code they exercise, not at the repository root. Every script resolves its own default binary and data paths from `%~dp0..\..\`, so each runs correctly from **any** working directory.

| Gate | What it asserts |
|---|---|
| `wintcp\tests\cli.bat` | CLI behaviour: every command, exit code and refusal message against the real `wintcp.exe`. |
| `wintcp\tests\gui.bat` | The GUI harness; fails loudly if the harness cannot start rather than reporting a vacuous pass. |
| `wintcp\tests\examples.bat` | Every example command printed in this documentation is accepted by the binary. |
| `wintcp-tests.exe unit` | The internal checks above. |

```bat
:: from anywhere - each script anchors itself:
wintcp\tests\cli.bat
wintcp\tests\examples.bat
wintcp\tests\gui.bat

:: override the target binary; examples.bat also takes the command list as arg 2:
wintcp\tests\cli.bat build\wintcp.exe
wintcp\tests\examples.bat build\wintcp.exe my-commands.txt
```

**`cli.bat` and `gui.bat` both begin with a self-test**: each deliberately provokes a known rejection and confirms the harness notices, so a green run means the gate can still fail. **Never run two instances of one script at once** - stdout interleaves and the verdicts stop being attributable.

The console verdicts are prefixed by gate: `CLI:`, `GUI:`, `EXAMPLES:`.

#### `wintcp\tests\examples.txt`

The command list `examples.bat` executes. One command per line, in the order the documentation presents them. `#` and blank lines are skipped.

Its contract is deliberately narrow: it asserts **exit codes only**, and only **rc 2** (bad arguments) counts as a failure. rc 0 and rc 1 are both acceptable - rc 1 means "nothing matched", which is a legitimate answer for a filter on a machine that has no such row. Output content is *not* asserted, because sample output is captured on one host and is explicitly documented as machine-specific.

**`;expect=N` annotates a command whose contract IS a non-zero exit.** Appended to the end of a line, and stripped before the binary ever sees it:

```text
kill --pid 4 ;expect=2
```

Without it, such a command cannot be gated at all - and that is not hypothetical. Two documented commands are in exactly this position: `kill --pid 4`, which the CLI reference says is *always* refused because PID 4 owns no real image, and `export --changes`, whose documented answer is exit 2. Both were absent from `examples.txt` for as long as this rule stood, because including them failed the gate for behaving correctly. `;` appears in no documented command, so the separator cannot collide with a command's own text.

Both directions are asserted, and the harness's self-test proves it: a command annotated `;expect=2` that returns 2 **passes**, and one that returns 0 **fails**. An annotation feature that only checked the passing direction would look correct while verifying nothing. The run also prints `ANNOTATED: N of M`, because the characteristic failure of an annotation is a typo that never matches - which leaves the line running unannotated, so the count is how you notice.

A nested double quote is why it runs through PowerShell rather than a batch `for /f` loop: the documented way to keep a space inside a filter value is `--filter "note:""corporate dns"""`, which a batch loop truncates to `--filter "note:""corporate`.

**If you add or change a documented command, add its line here.**

#### `wintcp\tests\d2probe.bat`

Not a gate. It builds `d2probe.cpp` and walks the system handle table, issuing one `SIO_TCP_INFO` per socket to measure how a machine behaves. Its numbers are what set `kProbeWorkers` (16) and `kNoProgressMs` (250) in `SocketTraffic.h` - see [Architecture](architecture.md#the-probe-pool). It is slow, machine-dependent and deliberately outside CI.

### Running the gates

Run them one at a time, in a Developer Command Prompt:

```bat
build.bat                       :: 1. it must compile clean first
build\tests\wintcp-tests.exe unit
wintcp\tests\cli.bat
wintcp\tests\examples.bat
wintcp\tests\gui.bat
```

## Benchmarks

`bench` builds synthetic rows and times the pipeline. It performs no enumeration, opens no window and touches no network, so it is the one measurement here that reports the *tool* rather than the machine - it answers "would 50k sockets make the list unusable".

```bat
build\tests\wintcp-tests.exe bench 100000 10
```

```text
WinTCP benchmark: rows=100000 iters=10

  [A] ReplaceSnapshot + SetView (no filter)    267.512 ms/op      0.37 Mrow/s

  [B] SetView (filter tcp port:1000-60000)       24.674 ms/op      4.05 Mrow/s

  [C] SetSort + SetView (toggle direction)       54.630 ms/op      1.83 Mrow/s

  [D] GetColumnText (all columns x rows)         79.082 ms/op      1.26 Mrow/s

  [E] ResolveBatch (rows share pids)             15.098 ms/op      6.62 Mrow/s

  [F] ReplaceSnapshot (62-way dup keys)         192.534 ms/op      0.52 Mrow/s

  [G] EtwTraffic::OnEvent (ETW event path)      24.797 ns/event  over 4000000 events

  [G]  same stream, Snapshot() every 1024        26.139 ns/event  (contended)

  total timed: 6335.3 ms

  (QueryPerformanceCounter; [D] iterates the current view, [E] resolves pid->name, [F] pairs duplicate keys, [G] is per event not per row, other stages scale with 'rows')
```

| Argument | Default | What it does |
|---|---|---|
| `bench` | - | Times all eight stages. |
| `[rows]` | `50000`, clamped | Synthetic row count. 100000 is comfortably above a busy machine, so the numbers are not flattering by accident. |
| `[iters]` | `20` | Iterations per stage. Fewer for a quick check; the report is ms/op and Mrow/s, so the two arguments are independent knobs. |

Reading the stages: **[A]** is the whole cost of a refresh (install a new snapshot and rebuild the view) and is the one that has to fit inside a refresh interval; **[B]** shows filtering is cheap relative to it, which is why filter-heavy use is fine; **[C]** is a header click; **[D]** is rendering every column of every visible row - the cost the GUI pays on paint; **[E]** resolves `pid -> name`; **[F]** is the duplicate-key pairing path that mDNS stresses; **[G]** is the ETW event path, reported per event rather than per row, with its contended variant beside it.

`Mrow/s` figures are throughput, not latency. The two together are what tell you whether a machine with 100k endpoints is usable.

Timings vary by machine and by what else is running - they are the one number in this document set that should **never** be copied between runs as though it were stable.

## Integer-boundary assumptions

Four places where a number crosses a type boundary. Each says what is actually true, so the next reader does not re-derive it — and does not have to trust a comment that turns out to be describing a risk that does not exist.

**`unsigned long` is 32 bits, even in this x64 build.** Confirmed with `dumpbin /headers`: `machine (x64)`, `PE32+`. MSVC keeps `long` at 32 bits on Windows regardless of the target, so any accumulator declared `unsigned long` wraps modulo 2^32. This is the one that caused a real bug and is worth knowing before writing the next parser — see `ParseUint` below.

**`ParseUint` bounds before it multiplies.** Its ceiling is 4,000,000,000, the largest value any numeric switch can hold. It used to apply that cap *after* `acc * 10 + digit`, which on a 32-bit `unsigned long` means the cap saw a wrapped value. Measured on the binary built that way:

| Typed | Reported |
|---|---|
| `--limit 4294967296` (2^32, wraps to 0) | accepted |
| `--limit 99999999999999999999` | accepted |
| `kill --pid 4294967296 --dry-run` | `refusing PID 0` |

The last row is why it mattered: the user typed one PID and was told about a different one. Harmless in effect, since PID 0 and PID 4 are refused regardless, but a number silently becoming a *different* number is not something a tool that refuses dangerous work should do. The check is now `acc > (kMaxUintArg - digit) / 10` before the multiply, which cannot itself overflow. Ten `cli.bat` checks pin the boundary from both sides, including leading zeros (`0000000001` is 1; `00000040000000001` is refused) and a negative assertion that no oversized `--pid` ever prints `PID 0`.

**`GetTickCount64` does not wrap, so the elapsed-time subtractions cannot go backwards.** It is 64-bit: 2^64 milliseconds is about 584 million years. The familiar 49.7-day wrap belongs to the 32-bit `GetTickCount`, which this codebase never calls. The subtraction forms — `FrozenAgeMs(now, frozenAt)` in `Freeze.cpp`, `DurationSeconds()` in `ConnectionStore.cpp`, and the `now - lastProgress >= N` loop in `SocketTraffic.cpp` — all guard their operands anyway (`if (now <= frozenAt) return 0;`). Those guards are defensive, not load-bearing: they exist so a caller passing arguments in the wrong order gets 0 rather than a huge number, which is worth keeping for a different reason than overflow.

**`int` narrowing is bounded everywhere it happens.** About a dozen `static_cast<int>(x.size())` calls exist. None can overflow: the ledger's line lengths are capped at 4096 bytes (`kLedgerMaxLineBytes`), the change log has its own documented cap, `argc` is bounded by the OS command line, `DrawText*` takes an `int` in the Win32 API itself, and `MainWindow`'s casts are against `COL_COUNT` (32). The one that *would* be dangerous needs more than 2^31 rows, which IP Helper cannot return. Port arguments are range-checked to 1–65535 before any narrowing to `USHORT`.

Gates for these live in `cli.bat` rather than the selftest, because `ParseUint` is file-local to `CliCommands.cpp` and the boundary is a property of the built binary.

## Documentation conventions

- **The README is a set.** "The README" means the root `README.md` plus every sub-document in `docs\`. Neither is complete alone: the root is the entry point, `docs\` is the reference.
- **Every documented command is executable.** Anything shown as a command must exist in `wintcp\tests\examples.txt`, or the `examples` gate will not check it and it can rot silently. No gate parses Markdown, so documentation can be wrong with all gates green. A command whose documented exit code is 2 carries `;expect=2`.
- **Sample output carries the machine disclaimer.** Output was captured on one Windows 11 host; PIDs, process names, ports and byte totals are machine-specific. Do not invent output to fill a block - if you cannot run it, do not print it.
- **Do not copy measured numbers between runs.** Check counts, gate counts and benchmark timings all move. Update them from the binary, never from the previous paragraph.
- **Case studies have a fixed shape**: a lead sentence, the command block, its output, then a table explaining each switch and why it is in *that* command.
- **Line endings are per-file, not per-repository.** There is no `.gitattributes`, so the tree is mixed on purpose: all four `.bat` files, the `.ps1` and the `.txt` are CRLF, `docs\*.md` are LF, and the sources are a mix of both. Match the file you are editing rather than imposing a style. Every `.bat` must be **uniformly** CRLF - a batch file with mixed endings can behave unpredictably.
- **Anchors are GitHub-slugged from the heading text**, lowercased, punctuation stripped, spaces to hyphens - and repeated punctuation is *not* collapsed, so a heading like "CPU - and memory" slugs to `cpu---and`. Read the heading back before you link to it.
