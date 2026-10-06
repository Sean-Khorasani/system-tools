@echo off
REM cli.bat - the CLI behaviour gate: run the real binary, assert stdout
REM markers and exit codes. Fast, no desktop, CI-safe. Row counts are NEVER
REM asserted (live machine); only shapes, headers, markers and exit codes.
REM
REM Exit codes of the product: 0 ok, 1 failure/empty, 2 bad args,
REM 3 refused (--yes missing).
REM This script exits 0 when all checks pass, 1 otherwise.
REM
REM HEADLESS BY CONSTRUCTION, with no opt-out flag. The trailing `--uiharness`
REM smoke used to live here and was skipped by passing a magic `quick`
REM argument - which meant every "headless" run still popped a window unless
REM the caller knew to opt out. That flag needs a real window, so it now has
REM its own file: gui.bat. This one no longer launches the GUI at all,
REM which is what the contract at the top always claimed.
REM
REM Safety rule: this gate never passes --yes to a mutating verb (kill/close/
REM block/unblock/capture). Destructive paths are verified by hand, not CI.
REM
REM NOT RUN BY THIS SUITE: d2probe.bat, which produced the measurements behind
REM D2's design ("of 391 sockets walked, 4 answered SIO_TCP_INFO, one call took
REM 28,281 ms, the next was still running after 90 s"). It walks the machine's
REM sockets with a thread each, which is the opposite of what a fast regression
REM suite wants, and the numbers in SocketTraffic.h are machine-specific - a
REM different host will have different ones and may need a different pool size.
REM It is kept, and buildable, precisely so that re-measuring is a command
REM rather than an archaeology exercise.
REM
REM Usage: cli.bat [path-to-exe]   (default: ..\..\build\wintcp.exe, resolved
REM        from this script's own directory, so it runs from anywhere)

setlocal EnableExtensions
set BIN=%~1
if "%BIN%"=="" set BIN=%~dp0..\..\build\wintcp.exe
if not exist "%BIN%" (
    echo CLI: missing %BIN% - run build.bat first.
    exit /b 1
)
set OUT=%TEMP%\wngolden_out_%RANDOM%.txt
set FAILS=0
set CHECKS=0

call :t "help overview" "help" 0 "Usage:"
call :t "help global flag" "--help" 0 "Usage:"
call :t "help short flag" "-h" 0 "Usage:"
call :t "help list" "help list" 0 "--filter F"
call :t "list --help" "list --help" 0 "Examples:"
call :t "list help positional" "list help" 0 "Examples:"
call :t "help bogus" "help bogus" 2 "unknown command"
call :t "unknown command" "nonsense" 2 "Try 'wintcp.exe help'"
call :t "marker ignored" "version --__wintcp-elevated" 0 "WinTCP"
call :t "version banner" "version" 0 "WinTCP"
call :t "list header" "list --limit 1" 0 "Proto"
call :t "list json array" "list --format json --limit 1" 0 "["
call :t "list json columns" "list --format json --columns proto,pid --limit 1" 0 "proto"
call :t "list full columns json" "list --format json --columns full --limit 1" 0 "country"
REM F5.12: --format jsonl is `json` with the array wrapper removed - identical
REM objects, one per line. Three properties are asserted that the array form
REM violates and this form must not: no line may be a lone opening bracket, no
REM line may be a lone closing bracket, and some line must open with { . A
REM --watch consumer reading line at a time would otherwise block until a
REM closing bracket that only the array form ever emits.
"%BIN%" list --format jsonl --limit 3 > "%OUT%" 2>&1
set JLRC=%ERRORLEVEL%
set JLRAW=0
if not "%JLRC%"=="0" set JLRAW=1
findstr /x /c:"[" "%OUT%" >nul 2>&1
if not errorlevel 1 set JLRAW=1
findstr /x /c:"]" "%OUT%" >nul 2>&1
if not errorlevel 1 set JLRAW=1
findstr /b /c:"{" "%OUT%" >nul 2>&1
if errorlevel 1 set JLRAW=1
set /a CHECKS+=1
if "%JLRAW%"=="1" (
    echo FAIL list jsonl one object per line [rc=%JLRC%, or a lone bracket line, or no opener]
    set /a FAILS+=1
) else (
    echo ok - list jsonl one object per line
)
call :t "list jsonl columns" "list --format jsonl --columns proto,pid --limit 1" 0 "proto"
call :t "help list documents jsonl" "help list" 0 "jsonl"
call :t "list unknown format" "list --format ndjson --limit 1" 2 "unknown --format"
call :t "group jsonl refuses per-connection column" "list --group --format jsonl --columns remote" 2 "JSON key would promise"
call :t "changes accepts jsonl" "list --changes --format jsonl --count 1" 0 "baseline:"
call :t "list filter" "list --filter port:443 --limit 2" 0 "Proto"
call :t "list state hyphen" "list --filter state:time-wait --limit 1" 0 "Proto"
REM README study shape: two clauses in one --filter (state + exclude) must list
REM non-loopback listeners and honour --columns. Shape only, no row counts.
"%BIN%" list --filter "state:listen exclude:127." --columns pid,process,lport > "%OUT%" 2>&1
set RCL=%ERRORLEVEL%
set /a CHECKS+=1
if not "%RCL%"=="0" (
    echo FAIL list listen non-loopback [rc=%RCL%, want 0]
    set /a FAILS+=1
) else (
    findstr /c:"Local port" "%OUT%" >nul 2>&1
    if errorlevel 1 (
        echo FAIL list listen non-loopback [marker missing: Local port]
        set /a FAILS+=1
    ) else (
        echo ok - list listen non-loopback
    )
)
call :t "list group" "list --group --limit 3" 0 "Proto"
call :t "list traffic" "list --traffic --limit 1" 0 "Proto"
call :t "list changes" "list --watch 1 --changes --count 2" 0 "baseline:"
call :t "list watch bounded" "list --watch 1 --count 2 --columns process" 0 "Process"
call :t "filter mem threshold" "list --filter mem:1TB --columns process" 0 "Process"
call :t "filter mem range" "list --filter mem:100-300 --columns process,mem" 0 "Process"
call :t "filter cpu threshold" "list --filter cpu:99 --columns process,cpu" 0 "Process"
call :t "filter traffic threshold no join" "list --traffic --filter tx:100GB --columns process" 0 "Process"
call :t "filter joined clause parses" "list --traffic --filter tx:1GB --columns process" 0 "Process"
REM D1 regression: `duration:` on a --traffic run depends on the kernel-age
REM join, so the pre-join candidate set must not be filtered by it. Assert the
REM COMMAND SHAPE only - the header is what proves the clause parsed and the
REM pipeline ran; row counts are never asserted (live machine). Before the fix
REM this filter could only ever answer "no rows", so rc 0 with a header is the
REM part that was silently wrong; the row-level proof is by hand.
call :t "filter duration threshold" "list --traffic --filter duration:1h --columns process,duration" 0 "Duration"
call :t "filter duration range" "list --traffic --filter duration:1h-2d --columns process,duration" 0 "Duration"
call :t "filter age alias" "list --traffic --filter age:1h --columns process,duration" 0 "Duration"
REM Missing-switch advice (G7): an enrichment clause whose switch is off can
REM only ever match nothing, so the run must SAY which switch to add - on
REM stderr (rc 0, marker found in merged output), never on the stdout data
REM stream (:tout proves that half). Both halves are deterministic: the hint
REM depends only on the switches given, not on the live machine.
call :t "host filter advice" "list --filter host:cdn --limit 1" 0 "add --dns"
call :tout "host advice off stdout" "list --filter host:cdn --limit 1" "add --dns"
call :t "country filter advice" "list --filter country:us --limit 1" 0 "add --db"
call :tout "country advice off stdout" "list --filter country:us --limit 1" "add --db"
call :t "traffic filter advice" "list --filter tx:1KB --limit 1" 0 "add --traffic"
call :tout "traffic advice off stdout" "list --filter tx:1KB --limit 1" "add --traffic"
call :t "cwnd filter advice" "list --filter cwnd:1 --limit 1" 0 "add --traffic"
REM SPEED's advice names BOTH facts a reader needs: the switch, and that a rate
REM cannot be computed from one snapshot. Gating only "add --traffic" would
REM have passed while `--filter speed:1KB` still matched nothing after the
REM switch was added, so the marker pins the --count requirement too.
call :t "speed filter advice" "list --filter speed:1KB --limit 1" 0 "--count 2"
call :tout "speed advice off stdout" "list --filter speed:1KB --limit 1" "--count 2"
REM D13, columns half: the same silent-empty failure for COLUMNS - an
REM enrichment column with its switch off must name the switch on stderr,
REM never on the stdout table, never fatal.
call :t "column host advice" "list --columns host --limit 1" 0 "column:"
call :tout "column advice off stdout" "list --columns host --limit 1" "column:"
call :t "list quiet hit" "list --filter tcp: --quiet" 0 "."
call :t "list quiet miss" "list --filter pid:99999999 --quiet" 1 "."
REM D24: a switch the verb does not act on is now an ARGUMENT error (exit 2)
REM naming the switch and the verb. Before this the parser was global and every
REM verb silently swallowed every switch, all rc 0 and all wrong - a script that
REM misspelled one got a successful run and the wrong answer. These pin the
REM refusal for both shapes: a switch that belongs to ANOTHER verb, and one
REM that belongs to this verb but is refused on principle (export --limit).
REM No parentheses in a check name: :t echoes the name from inside an
REM `if (...)` block, and a stray `)` closes the block early (cmd then reports
REM "[rc was unexpected at this time" and aborts the whole run).
call :t "switch not on export - changes" "export --out %OUT% --changes" 2 "honours:"
call :t "switch not on export - pid" "export --out %OUT% --pid 1" 2 "honours:"
call :t "switch not on list - pid" "list --pid 1" 2 "not a switch of this command"
call :t "switch not on list - name" "list --name x" 2 "not a switch of this command"
call :t "switch not on details - format" "details --select pid:1 --format json" 2 "not a switch of this command"
call :t "switch not on ps - group" "ps --group" 2 "not a switch of this command"
call :t "switch not on stat - traffic" "stat --traffic" 2 "not a switch of this command"
call :t "switch not on kill - format" "kill --pid 1 --dry-run --format json" 2 "not a switch of this command"
call :t "switch not on blocks - group" "blocks --group" 2 "not a switch of this command"
REM (a `selftest --traffic` switch-rejection check used to live here; it moved
REM out with the verb)
REM ...and the LAST entry of each verb's list must be accepted, which a naive
REM substring match got wrong (it appended a trailing space to the needle and
REM so failed on the final entry - the shape that passes in testing and fails
REM in production). These are the regression pins for that.
call :t "last switch on list - event" "list --watch 1 --changes --event appear --count 2" 0 "baseline:"
REM The quiet check needs a .csv name: --out alone infers the format from the
REM extension, and %OUT% is a .txt, so this one is about the switch list and
REM not about format inference (which has its own check further down).
REM %RANDOM% in the name, because R4 now REFUSES an existing --out. A fixed name
REM made this case pass only because the overwrite was silent, and left a file
REM in %TEMP% that made the NEXT run of this gate fail - a self-poisoning test.
set QF=%TEMP%\wngolden_q_%RANDOM%.csv
call :t "last switch on export - quiet" "export --out %QF% --quiet" 0 "."
if exist "%QF%" del "%QF%" >nul 2>&1
REM --limit is refused with export's OWN explanation, not the generic
REM "not a switch of this command": export has a considered opinion about it
REM (an export always writes the whole view, so a file cannot hold a subset and
REM misreport its row count), and the generic message would hide that.
call :t "export limit refused on principle" "export --out %OUT% --limit 5" 2 "not accepted"
REM R4: --out replaces the whole file, so an existing path is refused unless
REM --force authorises it. The write underneath is CREATE_ALWAYS, and before
REM this the refusal did not exist: a 6-byte file was replaced by a 844-byte
REM export, exit 0, nothing in the output about what it had destroyed.
REM Three assertions, because the interesting failure is a silent one: the
REM refusal must fire, the file must survive it, and --force must still work.
set R4F=%TEMP%\wngolden_r4_%RANDOM%.csv
echo PRE-EXISTING-CONTENT> "%R4F%"
call :t "export refuses an existing --out" "export --out %R4F%" 2 "already exists"
call :t "the refusal names the switch" "export --out %R4F%" 2 "--force"
REM The file must be byte-identical afterwards. Asserting only the exit code
REM would pass even if the refusal came after the write.
set /a CHECKS+=1
findstr /b /c:"PRE-EXISTING-CONTENT" "%R4F%" >nul 2>&1
if errorlevel 1 (
    echo FAIL export refused but still overwrote [file was destroyed]
    set /a FAILURES+=1
) else (
    echo ok - export refused without touching the file
)
call :t "export --force replaces it" "export --out %R4F% --force" 0 "exported"
set /a CHECKS+=1
findstr /b /c:"PRE-EXISTING-CONTENT" "%R4F%" >nul 2>&1
if not errorlevel 1 (
    echo FAIL export --force did not replace the file
    set /a FAILURES+=1
) else (
    echo ok - export --force replaced it
)
if exist "%R4F%" del "%R4F%" >nul 2>&1
REM capture --out is the same CREATE_ALWAYS and the same rule, but the REFUSAL
REM cannot be asserted from here: capture resolves --select before it reaches the
REM guard, and no fixed selector matches exactly one row on an arbitrary machine
REM - port:443 matches dozens, so the case exits 1 on the selector instead and
REM would assert nothing. The guard itself is covered by the r4.* selftest checks
REM in Bench.cpp against a real file. What IS worth asserting here is that
REM capture ACCEPTS --force, since an unlisted switch would exit 2 and quietly
REM make the whole feature unreachable.
call :t "capture accepts --force" "capture --select port:443 --secs 1 --dry-run --force" 1 "refine to one"
call :t "export accepts --force" "export --out %TEMP%\wngolden_r4f_%RANDOM%.csv --force" 0 "exported"
REM D24, --quiet contract: it must now be a zero-byte contract on export too.
REM It used to print "exported N rows" anyway, so a script using --quiet to
REM keep a log clean got a line in it. Both streams must be empty.
set XQOUTF=%TEMP%\wngolden_xq_%RANDOM%.txt
set XQERRF=%TEMP%\wngolden_xqe_%RANDOM%.txt
REM The exported file itself gets %RANDOM% too, for the R4 reason above: a
REM fixed name here made this check fail on the SECOND run of the gate.
set XQCSV=%TEMP%\wngolden_xq_%RANDOM%.csv
if exist "%XQOUTF%" del "%XQOUTF%"
if exist "%XQERRF%" del "%XQERRF%"
if exist "%XQCSV%" del "%XQCSV%"
"%BIN%" export --out "%XQCSV%" --quiet > "%XQOUTF%" 2> "%XQERRF%"
set /a CHECKS+=1
set XQBAD=0
for %%F in ("%XQOUTF%") do if not "%%~zF"=="0" set XQBAD=1
for %%F in ("%XQERRF%") do if not "%%~zF"=="0" set XQBAD=1
if "%XQBAD%"=="1" (
    echo FAIL export quiet zero bytes [stdout or stderr not empty]
    set /a FAILS+=1
) else (
    echo ok - export quiet zero bytes
)
if exist "%XQOUTF%" del "%XQOUTF%"
if exist "%XQERRF%" del "%XQERRF%"
if exist "%XQCSV%" del "%XQCSV%"
REM D25: --event selects which change kinds print. Before it there was NO way
REM to ask "what OPENED", which is the question an event feed exists for, and
REM `--filter event:appear` did not work either (no such field name, so it
REM became a literal substring search that matched nothing).
call :t "event appear only" "list --watch 1 --changes --event appear --count 3 --filter proto:tcp" 0 "baseline:"
call :t "event bad name" "list --watch 1 --changes --event bogus --count 2" 2 "bad --event"
call :tout "event appear emits no DISAPPEAR" "list --watch 1 --changes --event appear --count 3" "DISAPPEAR"
REM D26: the note reached the row, so `note:` is a real filter field. A bare
REM `note:` asks for annotated rows; a word asks for that word. Both are
REM asserted through the exit code only, because which rows carry a note
REM depends on the machine's bookmark store.
call :t "note bare parses" "list --filter note: --columns process" 0 "Process"
call :t "note word parses" "list --filter note:vendor --columns process" 0 "Process"
call :t "note quoted parses" "list --filter note:""vendor api"" --columns process" 0 "Process"
call :t "path quoted parses" "list --filter path:""program files"" --columns process" 0 "Process"
call :t "preset apply limit" "preset apply --name wngolden-missing --limit 1" 1 "not found"
REM --quiet is a zero-byte contract on BOTH streams: no advisory hint may leak
REM (the D3 head hint used to print on stderr under --quiet when a switch was
REM on and the head was unenrichable). rc may be 0 or 1 depending on the live
REM view - only the silence is asserted.
set QERRF=%TEMP%\wngolden_qerr_%RANDOM%.txt
if exist "%QERRF%" del "%QERRF%"
"%BIN%" list --traffic --limit 1 --quiet > "%OUT%" 2> "%QERRF%"
set /a CHECKS+=1
set QBAD=0
for %%F in ("%OUT%") do if not "%%~zF"=="0" set QBAD=1
for %%F in ("%QERRF%") do if not "%%~zF"=="0" set QBAD=1
if "%QBAD%"=="1" (
    echo FAIL quiet zero bytes [stdout or stderr not empty]
    set /a FAILS+=1
) else (
    echo ok - quiet zero bytes
)
if exist "%QERRF%" del "%QERRF%"
call :t "list bad format" "list --format bogus" 2 "unknown --format"
call :t "list bad sort" "list --sort bogus" 2 "unknown --sort"
call :t "list bad columns" "list --columns bogus" 2 "no known column"
call :t "ps header" "ps --limit 2" 0 "PID"
call :t "ps json" "ps --format json --limit 1" 0 "pid"
call :t "ps sort mem" "ps --sort mem --limit 1" 0 "PID"
call :t "ps bad sort" "ps --sort bogus" 2 "unknown --sort"
call :t "ps quiet hit" "ps --quiet" 0 "."
call :t "top single" "top --limit 2" 0 "PID"
call :t "top bounded" "top --count 1 --limit 1" 0 "PID"
call :t "stat line" "stat" 0 "CPU"
call :t "stat json" "stat --format json" 0 "cpuKnown"
call :t "stat bad format" "stat --format csv" 2 "unknown --format"
call :t "details needs select" "details" 2 "is required"
call :t "details no match" "details --select pid:99999999" 1 "no live row"
call :t "kill needs target" "kill" 2 "is required"
REM --- C5: ONE refusal wording ------------------------------------------------
REM Exit 3 is one message shape for every "not without being told twice" answer,
REM so a script can match it once. Asserted on the TEXT and not merely the exit
REM code (the D29 lesson: a wrong message with the right rc still breaks users).
REM The flag in the message is part of the shape - --yes for the mutating verbs,
REM --force for overwriting an existing preset - so both are pinned.
REM Markers deliberately avoid parentheses: call re-parses its arguments, and an
REM unescaped ")" inside a marker truncates the line.
call :t "kill refused" "kill --pid 999999" 3 "refused: pass --yes to proceed"
REM `close` is deliberately NOT asserted here: its refusal happens only after
REM the --select resolves to exactly one live row, and no fixed --select string
REM can promise that on an arbitrary machine (the row set is whatever the host
REM happens to be running). Asserting it would make the suite machine-dependent,
REM which is worse than the gap. `kill` and `unblock` are two different call
REM sites through the same helper, which is what the shape claim rests on.
call :t "unblock refused" "unblock --address 1.2.3.4 --port 443" 3 "refused: pass --yes to proceed"
REM The preset route names --force instead. Same exit code, same meaning,
REM different remedy - and a refusal that suggested --yes here would be a false
REM promise, since no amount of --yes overwrites a saved view.
call :t "preset created" "preset save --name zz_refusal_probe" 0 "preset created: zz_refusal_probe"
call :t "preset exists refused" "preset save --name zz_refusal_probe" 3 "zz_refusal_probe"
call :t "preset exists names the remedy" "preset save --name zz_refusal_probe" 3 "refused: pass --force to proceed"
REM Leave the machine as found: a probe preset persisted in HKCU would outlive
REM this run and make the next run's "created" check fail for no real reason.
"%BIN%" preset delete --name zz_refusal_probe >nul 2>&1
REM Exit 2 is a DIFFERENT kind of answer and must not borrow the exit-3 wording:
REM no flag makes PID 4 killable, so promising one would be false. This is the
REM C5 boundary, asserted so a future "simplification" that merges them fails.
call :t "kill pseudo pid" "kill --pid 4" 2 "refusing"
call :t "kill pseudo pid is not an exit-3 refusal" "kill --pid 4 --yes" 2 "PID 0 and the System pseudo-process"
call :t "kill dry run" "kill --pid 999999 --dry-run" 0 "kill PID 999999"
call :t "kill select dry run" "kill --select pid:99999999 --dry-run" 1 "no live row"
call :t "kill select ambiguous" "kill --select tcp: --dry-run" 1 "refine to one"
call :t "kill pid plus select" "kill --pid 1 --select tcp:" 2 "exclude each other"
call :t "close needs select" "close" 2 "is required"
call :t "close dry run missing" "close --select pid:99999999 --dry-run" 1 "no live row"
call :t "close refused" "close --select tcp: --dry-run" 1 "refine to one"
call :t "block dry run missing" "block --select pid:99999999 --dry-run" 1 "no live row"
call :t "block missing" "block --select pid:99999999 --yes" 1 "no live row"
REM (the older one-line "unblock refused" check is superseded by the C5 block
REM above, which asserts the full wording instead of the word "refused")
call :t "blocks count" "blocks" 0 "wintcp-firewall-rules"
call :t "bookmark list" "bookmark list" 0 "Address"
call :t "bookmark colour needs tag" "bookmark colour --address 1.2.3.4 --port 443" 2 "tag"
call :t "bookmark bad address" "bookmark add --address notanip --port 443" 1 "failed"
call :t "preset missing" "preset show --name wngolden-missing" 1 "not found"
call :t "export needs out" "export --filter tcp:" 2 "is required"
call :t "export bad format" "export --out x --format bogus" 2 "unknown --format"
call :t "geoip needs db" "geoip info" 2 "is required"
call :t "geoip bad db" "geoip info --db C:\nope.mmdb" 1 "could not be opened"
call :t "capture needs select" "capture" 2 "is required"
call :t "capture no match" "capture --select pid:99999999" 1 "no live row"
call :t "capture dry run missing" "capture --select pid:99999999 --dry-run" 1 "no live row"
call :t "capture bad secs" "capture --select pid:1 --secs 0" 2 "bad --secs"

REM `capture --text` / `--dir` / `--out` (2026-10-05). The strict per-verb
REM switch allow-list is what these mostly exercise: all three used to be
REM rejected as "unknown switch" the moment they were added to the parser and
REM forgotten in kVerbSwitches, which is the whole reason --dir is checked here
REM rather than only in the unit tests.
REM
REM The --dir refusal is asserted for the VALUE, not merely the exit code,
REM because a typo silently falling back to "both" would exit 0 and print two
REM directions to a reader who asked for one - a failure no exit-code test sees.
REM
REM The marker is the PARSER's message, "bad --dir", not the verb's "is not a
REM direction". Both paths exist and both exit 2, but the parser's runs first, so
REM the verb's copy is unreachable from the command line; it is a belt-and-braces
REM check for a future caller that builds CaptureOptions directly.
call :t "capture bad dir refused" "capture --select pid:1 --dir sideways" 2 "bad --dir"
call :t "capture dir needs a value" "capture --select pid:1 --dir" 2 "missing value"
call :t "text is capture-only" "list --text" 2 "not a switch of this command"
call :t "dir is capture-only" "list --dir first" 2 "not a switch of this command"
REM --out IS shared with list/export on purpose (same meaning: a file to write),
REM so it is accepted here rather than refused. Dry run only: a real run starts
REM pktmon, which the harness must not do unattended.
call :t "capture accepts out" "capture --select pid:99999999 --out C:\nope.pcapng --dry-run" 1 "no live row"
call :t "list accepts interval" "list --filter tcp: --interval 1 --count 1 --quiet" 0 "."
call :t "ps accepts interval" "ps --interval 1 --count 1 --quiet" 0 "."
call :t "stat accepts interval" "stat --interval 1 --count 1" 0 "."
call :t "stat interval refused on export" "export --out %OUT% --interval 1" 2 "not a switch"

REM The test suite moved OUT of the product (2026-10-02): `selftest`, `bench`
REM and `--uiharness` used to be compiled into wintcp.exe. wintcp-tests.exe now
REM carries them, and these six checks assert the new product contract - the
REM commands are gone rather than silently broken. A test that only checked the
REM happy path would not notice the verbs had been left behind advertising a
REM capability the product no longer has.
call :t "moved selftest verb gone" "selftest" 2 "unknown command"
call :t "moved bench verb gone" "bench" 2 "unknown command"
call :t "moved uiharness switch gone" "--uiharness" 2 "unknown command"
REM And the help surface must not still advertise them. A stale kCommandHelps
REM entry is worse than a stale verb: `help` would promise a command that does
REM not exist.
REM No `|` in this marker: the batch parser treats it as a pipe even inside
REM quotes, which truncated the line and made this check report the wrong
REM marker entirely. Match a fragment instead.
call :t "help does not advertise selftest" "help" 0 "Other: version"
call :t "help selftest page is gone" "help selftest" 2 "unknown command"

REM Single-mode contract (audit 2026-09-29): the removed legacy flag forms
REM must be rejected with exit 2, and the help surface must stay complete.
call :t "legacy -c rejected" "-c" 2 "unknown command"
call :t "legacy --json rejected" "--json" 2 "unknown command"
call :t "legacy --selftest rejected" "--selftest" 2 "unknown command"
call :t "legacy --bench rejected" "--bench" 2 "unknown command"
call :t "legacy --version rejected" "--version" 2 "unknown command"
call :t "help slash form" "/?" 0 "Usage:"
call :t "help kill examples" "help kill" 0 "Examples:"
call :t "unknown switch" "list --bogus" 2 "unknown switch"
call :t "list group traffic" "list --group --traffic --limit 2" 0 "Process"

REM GeoIP error contract. No database can ship with WinTCP (MaxMind licence)
REM and none is assumed on a dev box, so golden asserts the REJECTION path with
REM a file that exists but is not a database - fully deterministic. The happy
REM path (a real tree, both record layouts, real metadata) is covered by
REM wintcp-tests.exe unit's geoip.* checks, which build a valid .mmdb in memory.
set JUNKDB=%TEMP%\wngolden_junk.mmdb
> "%JUNKDB%" echo this is not a geoip database
call :t "geoip info junk db" "geoip info --db %JUNKDB%" 1 "cannot load database"
call :t "geoip lookup no ip" "geoip lookup --db %JUNKDB%" 2 "is required"
call :t "geoip lookup junk db" "geoip lookup --db %JUNKDB% 1.2.3.4" 1 "cannot load database"
call :t "list junk db" "list --db %JUNKDB% --limit 1" 1 "GeoIP:"
call :t "list junk db + dns" "list --db %JUNKDB% --dns --limit 1" 1 "GeoIP:"

REM D3: the unenrichable-window hint is ADVISORY and goes to stderr only, so
REM stdout must stay a clean table that no marker pollutes. Whether the hint
REM fires depends on the live socket landscape (TIME_WAIT/listener rows come
REM and go), so it is NOT asserted here - only the invariant it must never
REM break. :tout below captures stdout alone and fails if the hint leaked.
call :tout "enrich hint not on stdout" "list --dns --limit 4 --columns remote,host,country"

REM preset round-trip with a unique name (HKCU, own profile only).
set PNAME=wngolden-%RANDOM%
call :t "preset save" "preset save --name %PNAME% --filter port:443" 0 "created"
call :t "preset show" "preset show --name %PNAME%" 0 "port:443"
call :t "preset apply" "preset apply --name %PNAME%" 0 "Proto"
call :t "preset delete" "preset delete --name %PNAME%" 0 "deleted"

REM bookmark round-trip (HKCU, own profile only; cleaned up).
call :t "bookmark add" "bookmark add --address 127.0.0.9 --port 9999 --tag 2 --note golden" 0 "added"
call :t "bookmark list has it" "bookmark list" 0 "127.0.0.9"
call :t "bookmark remove" "bookmark remove --address 127.0.0.9 --port 9999" 0 "removed"

REM export to a temp file, then check the file itself.
set CSV=%TEMP%\wngolden_%RANDOM%.csv
"%BIN%" export --out "%CSV%" --filter "tcp:" --format csv > "%OUT%" 2>&1
set /a CHECKS+=1
findstr /c:"Proto,Local address" "%CSV%" >nul 2>&1
if errorlevel 1 (
    echo FAIL export file header
    set /a FAILS+=1
) else (
    echo ok - export file header
)
if exist "%CSV%" del "%CSV%"

REM D27: --group plus a PER-CONNECTION column must be refused BY NAME, because a
REM delimited file's header row is a schema claim. It used to be accepted, and
REM the cell fell through to one arbitrary member of the group while the
REM header still said "Remote address" - measured: a 4-connection process wrote
REM the single value "0.0.0.0" under it, and a spreadsheet loads that silently.
REM Asserted both halves: the refusal names the column, and the group-answerable
REM columns still export (so the fix is a refusal, not a broken --group).
call :t "export group refuses remote" "export --out %TEMP%\wng_d27.csv --group --columns pid,process,remote" 2 "cannot report Remote address"
call :t "export group refuses host" "export --out %TEMP%\wng_d27b.csv --group --columns pid,host" 2 "cannot report Hostname"
call :t "export group safe columns ok" "export --out %TEMP%\wng_d27c.csv --group --columns pid,process,state,cpu,mem" 0 "."
REM The rule keys on the OUTPUT SHAPE, not the verb: a piped stream must not
REM carry a lying header either. json is refused the same way, and says "JSON
REM key" rather than "file header" so the reason is specific to the shape.
call :t "list group json refuses host" "list --group --format json --columns pid,host" 2 "JSON key would promise"
call :t "list group json refuses remote" "list --group --format json --columns remote" 2 "cannot report Remote address"
REM ...but the aligned table is deliberately EXEMPT: its State cell already
REM shows the connection count, so a representative value is readable there.
REM This is the half of D27 that must NOT regress into a refusal.
call :t "list group table keeps remote" "list --group --columns pid,process,remote --filter ""process:svchost.exe""" 0 "."

REM --- 5.5: the bookmark note, as a column and a filter ------------------------
REM A note is written against ONE remote endpoint - that is what a bookmark is
REM keyed on - so it is per-connection and inherits D27's refusal in a grouped
REM delimited stream. Asserting it here is what stops a NEW column from
REM reintroducing the bug D27 fixed: the first version of any column defaults to
REM being treated as group-answerable, and the group renderer then falls through
REM to one arbitrary member while the header still promises the group's value.
call :t "list group csv refuses note" "list --group --format csv --columns pid,note" 2 "cannot report Note"
REM The refusal must also NAME the remedy: one that only says "no" leaves the
REM user guessing between dropping --group and dropping the column, which are
REM opposite actions.
call :t "list group csv refusal lists options" "list --group --format csv --columns pid,note" 2 "Drop the column, or drop --group"
REM The column is accepted on a flat list. Assert the HEADER, not a row: the
REM whole point of the column is that a delimited export carries the name.
call :t "list note column in csv header" "list --format csv --columns remote,note --limit 1" 0 "Remote address,Note"
REM The bare field name means "has a note", as for every other measured field.
call :t "note filter bare accepted" "list --columns remote,note --filter ""note:"" --limit 1" 0 "Note"
REM Unknown-not-everything is the rule for measured fields; a bare note: that
REM matched all 300 rows would be a filter that cannot be wrong.
call :t "note filter bare does not match all" "list --columns remote --filter ""note:"" --limit 1" 0 "."
REM A note that does not exist must match nothing.
call :t "note filter miss matches nothing" "list --columns remote --filter ""note:zzz-no-such-note""" 0 "."

REM --- D29: the kill self-guard, and the pseudo-PID refusal -------------------
REM WinTCP holds its own connections, so it has its own rows, and "End process"
REM on one of them used to end WinTCP. KillPid refuses it by name in the SHARED
REM layer, so the CLI cannot reach it either. The self-PID is not knowable from
REM a batch file, but the pseudo-PID refusal is the same code path and is what
REM every TIME_WAIT and wildcard row produces.
REM
REM --pid and not --select: `--select pid:0` matches every TIME_WAIT and
REM wildcard row on the machine, so it stops at "refine to one" (rc 1) and
REM never reaches the guard being tested. --pid goes straight there.
REM
REM PID 0 fixed 2026-10-01: `--pid 0` used to be swallowed by the argument
REM check, which treated 0 as "no --pid given" and reported a MISSING argument.
REM CliCommands now tracks --pid presence (Args::hasPid), so PID 0 reaches the
REM same pseudo-PID guard as PID 4. Both are asserted because they are
REM different inputs to the same refusal, and a regression in either the parser
REM or the guard must fail loudly.
call :t "kill refuses pid 0" "kill --pid 0 --yes" 2 "refusing PID 0"
call :t "kill pid 0 refusal is explained" "kill --pid 0 --yes" 2 "no process that can be ended"
call :t "kill dry-run refuses pid 0" "kill --pid 0 --dry-run" 2 "refusing PID 0"
call :t "kill yes does not bypass guard" "kill --pid 4 --yes" 2 "refusing PID 4"
REM The refusal must explain itself, not just name the pid - a bare "refusing
REM PID 4" in a log tells a reader nothing about what to do instead.
call :t "kill refusal is explained" "kill --pid 4 --yes" 2 "no process that can be ended"
REM Refused BEFORE --dry-run, so no plan line is ever printed for a target that
REM cannot be killed. A plan reading "kill PID 4" is a bad thing to print even
REM when nothing happens, because the next thing a user does with a plan is
REM paste it somewhere.
call :t "kill dry-run does not plan a pseudo pid" "kill --pid 4 --dry-run" 2 "refusing PID 4"

REM --- R1: crash diagnostics (hidden `crashtest` verb) ------------------------
REM A crash must leave a minidump and a breadcrumb, not silence. This runs a
REM real access violation in a child process and asserts three things: the
REM breadcrumb names a dump, the exit code is the AV status, and a .dmp file
REM actually lands in %LOCALAPPDATA%\WinTCP\crashes. Dumps are removed
REM afterwards - golden leaves the machine as found, and an ever-growing
REM crashes dir would itself be a defect.
REM
REM The rc is the NTSTATUS as a signed 32-bit int (0xC0000005 = -1073741819),
REM which is what cmd reports for an unhandled AV. Pinned exactly: any other
REM value means the filter did not run (WER dialog, silent exit, etc.).
if exist "%LOCALAPPDATA%\WinTCP\crashes\wintcp-*.dmp" del "%LOCALAPPDATA%\WinTCP\crashes\wintcp-*.dmp" >nul 2>&1
set /a CHECKS+=1
"%BIN%" crashtest > "%OUT%" 2>&1
set GOTRC=%ERRORLEVEL%
findstr /c:"fatal: unhandled exception 0xC0000005; minidump: " "%OUT%" >nul 2>&1
if errorlevel 1 (
    echo FAIL crash breadcrumb names a dump [crashtest]
    set /a FAILS+=1
) else if not "%GOTRC%"=="-1073741819" (
    echo FAIL crash exit code is the AV status [rc=%GOTRC%, want -1073741819]
    set /a FAILS+=1
) else (
    echo ok - crash breadcrumb names a dump
)
set /a CHECKS+=1
dir /b "%LOCALAPPDATA%\WinTCP\crashes\wintcp-*.dmp" >nul 2>&1
if errorlevel 1 (
    echo FAIL crash minidump lands on disk [crashtest]
    set /a FAILS+=1
) else (
    echo ok - crash minidump lands on disk
)
del "%LOCALAPPDATA%\WinTCP\crashes\wintcp-*.dmp" >nul 2>&1
REM V7: the breadcrumb on stderr is not always visible - a detached or GUI
REM launch has no stderr for anyone to be reading - so the same record is also
REM written to disk, in the folder the dumps land in, before the dump attempt.
REM findstr fails outright when the file is absent, so the first check below is
REM also the existence check.
set /a CHECKS+=1
findstr /c:"exception 0xC0000005" "%LOCALAPPDATA%\WinTCP\crashes\wintcp-last-crash.txt" >nul 2>&1
if errorlevel 1 (
    echo FAIL crash note on disk records the exception [crashtest]
    set /a FAILS+=1
) else (
    echo ok - crash note on disk records the exception
)
set /a CHECKS+=1
findstr /c:"handler ran; minidump: " "%LOCALAPPDATA%\WinTCP\crashes\wintcp-last-crash.txt" >nul 2>&1
if errorlevel 1 (
    echo FAIL crash note on disk names the dump it wrote [crashtest]
    set /a FAILS+=1
) else (
    echo ok - crash note on disk names the dump
)
REM And the case the note exists for: the handler runs but cannot produce a
REM dump. Every such failure used to report "dump directory unavailable", so a
REM missing dbghelp.dll or a refused write sent the reader to a folder that
REM was perfectly healthy. Put a FILE where the crashes directory belongs -
REM CreateDirectory then fails and the dump cannot be created - and the
REM breadcrumb has to name THAT reason instead of claiming a dump.
REM
REM The real directory is moved aside rather than deleted: it may hold a user's
REM dumps, and golden leaves the machine as found. The backup lives in %TEMP%
REM under a name only this script uses, so a stale one is safe to clear.
set CRASHDIR=%LOCALAPPDATA%\WinTCP\crashes
set CRASHBAK=%TEMP%\wintcp_crashes_goldenbak
set /a CHECKS+=1
rmdir /s /q "%CRASHBAK%" >nul 2>&1
if exist "%CRASHDIR%\" move "%CRASHDIR%" "%CRASHBAK%" >nul 2>&1
type nul > "%CRASHDIR%"
"%BIN%" crashtest > "%OUT%" 2>&1
findstr /c:"no minidump (cannot create the dump file)" "%OUT%" >nul 2>&1
set NO_DUMP_OK=%ERRORLEVEL%
del "%CRASHDIR%" >nul 2>&1
if exist "%CRASHBAK%\" move "%CRASHBAK%" "%CRASHDIR%" >nul 2>&1
if not "%NO_DUMP_OK%"=="0" (
    echo FAIL crash names the real reason when no dump can be written [crashtest]
    set /a FAILS+=1
) else (
    echo ok - crash names the real reason with no dump
)
del "%LOCALAPPDATA%\WinTCP\crashes\wintcp-last-crash.txt" >nul 2>&1

REM --- V2: delay-load degradation holds when DLLs are missing ----------------
REM     DLLs can be shadowed (comdlg32/ole32/oleaut32/shell32 are KnownDLLs and
REM     ignore an application-directory copy). crypt32 gets loaded into the
REM     process early by something else (shell32 / winhttp), so stubbing it does
REM     NOT exercise the missing-DLL branch - tested, confirmed: DllPresent still
REM     returns true because LoadLibraryA returns the already-loaded module.
REM     Only pdh actually gets exercised this way, and it does.
REM
REM Copy the product into a temp dir with a zero-byte stub for pdh, and the run
REM must stay up AND name the missing capability AND degrade its feature. That
REM is the property V2 is about: a missing delay-loaded DLL is a reported
REM degradation, not a crash at load time.
set DL_DIR=%TEMP%\wngolden_dl
rmdir /s /q "%DL_DIR%" >nul 2>&1
md "%DL_DIR%" >nul 2>&1
copy /y "%BIN%" "%DL_DIR%\wintcp.exe" >nul
type nul > "%DL_DIR%\pdh.dll"
set /a CHECKS+=1
"%DL_DIR%\wintcp.exe" version > "%OUT%" 2>&1
set DLRC=%ERRORLEVEL%
findstr /c:"Optional features" "%OUT%" >nul 2>&1
if errorlevel 1 (
    echo FAIL delayload version prints Optional features [dl]
    set /a FAILS+=1
) else (
    findstr /c:"pdh.dll is not present" "%OUT%" >nul 2>&1
    if errorlevel 1 (
        echo FAIL delayload names pdh.dll as missing [dl]
        set /a FAILS+=1
    ) else (
        echo ok - delayload names missing pdh.dll
    )
)
set /a CHECKS+=1
"%DL_DIR%\wintcp.exe" stat --count 1 > "%OUT%" 2>&1
findstr /c:"DISK n/a" "%OUT%" >nul 2>&1
if errorlevel 1 (
    echo FAIL delayload stat degrades disk to n/a without pdh.dll [dl]
    set /a FAILS+=1
) else (
    echo ok - delayload stat degrades disk to n/a
)
set /a CHECKS+=1
if "%DLRC%"=="0" (
    echo ok - delayload version exits 0 with stubs
) else (
    echo FAIL delayload version exits non-zero with stubs [rc=%DLRC%]
    set /a FAILS+=1
)
rmdir /s /q "%DL_DIR%" >nul 2>&1
REM With NO explicit --columns a grouped stream gets a group-answerable
REM default instead of the flat ten (four of which are per-connection). Assert
REM the FILE's header, since the header is the schema claim being fixed.
REM
REM NOTE THE %% IN "CPU %%". The group header contains a literal per-cent sign,
REM and a percent is a variable-expansion character in a batch file: writing
REM one unescaped makes cmd swallow the rest of the line looking for a closing
REM %, which silently aborted this suite the first time round (no FAIL line,
REM no summary - it just stopped). `%%` is the batch escape for a literal %.
REM Only the CPU column has one, which is why the flat header below does not.
"%BIN%" export --out "%TEMP%\wng_d27d.csv" --group > "%OUT%" 2>&1
set /a CHECKS+=1
findstr /c:"PID,Process,Proto,State,Traffic (rx/tx),Net total,CPU %%,Memory (WS),Disk I/O" "%TEMP%\wng_d27d.csv" >nul 2>&1
if errorlevel 1 (
    echo FAIL grouped stream uses the group-answerable default header
    set /a FAILS+=1
) else (
    echo ok - grouped stream uses the group-answerable default header
)
REM ...and the flat default is untouched: a non-grouped export still gets the
REM historical ten columns, or the fix would silently rewrite existing files.
"%BIN%" export --out "%TEMP%\wng_d27e.csv" --filter "tcp: state:listen" > "%OUT%" 2>&1
set /a CHECKS+=1
findstr /c:"Proto,Local address,Local port,Remote address,Remote port,State,PID,Process,Service,Path" "%TEMP%\wng_d27e.csv" >nul 2>&1
if errorlevel 1 (
    echo FAIL flat export keeps the historical ten-column header
    set /a FAILS+=1
) else (
    echo ok - flat export keeps the historical ten-column header
)
if exist "%TEMP%\wng_d27.csv" del "%TEMP%\wng_d27.csv"
if exist "%TEMP%\wng_d27b.csv" del "%TEMP%\wng_d27b.csv"
if exist "%TEMP%\wng_d27c.csv" del "%TEMP%\wng_d27c.csv"
if exist "%TEMP%\wng_d27d.csv" del "%TEMP%\wng_d27d.csv"
if exist "%TEMP%\wng_d27e.csv" del "%TEMP%\wng_d27e.csv"

REM G7 export half: the missing-switch advice must reach stderr on the export
REM path too - a 0-row file with no explanation is the exact failure this
REM exists for. :t merges both streams (rc + marker); :tout proves the advice
REM stayed off the stdout data stream. Both are deterministic (switches only).
call :t "export host advice" "export --out %CSV% --filter host:cdn --format csv" 0 "add --dns"
REM Two cases, one path, and R4 now refuses the second because the first created
REM it. They only ever worked together because the overwrite was silent.
if exist "%CSV%" del "%CSV%"
call :tout "export advice off stdout" "export --out %CSV% --filter host:cdn --format csv" "add --dns"
if exist "%CSV%" del "%CSV%"

REM --format is OPTIONAL for export: the --out extension names the format
REM (the help's own first example, `export --out conns.csv`, relies on it and
REM used to die with "unknown --format 'table'"). Bespoke because the
REM assertion is on the FILE's bytes, not on stdout.
"%BIN%" export --out "%CSV%" --filter "tcp:" > "%OUT%" 2>&1
set RCEX=%ERRORLEVEL%
set /a CHECKS+=1
if not "%RCEX%"=="0" (
    echo FAIL export format from extension [rc=%RCEX%, want 0]
    set /a FAILS+=1
) else (
    findstr /c:"Proto,Local address" "%CSV%" >nul 2>&1
    if errorlevel 1 (
        echo FAIL export format from extension [file not csv-shaped]
        set /a FAILS+=1
    ) else (
        echo ok - export format from extension
    )
)
if exist "%CSV%" del "%CSV%"
REM A name with no known extension is REJECTED, never guessed at.
call :t "export format not inferred" "export --out %TEMP%\wngolden_noext --filter tcp:" 2 "cannot infer"
if exist "%TEMP%\wngolden_noext" del "%TEMP%\wngolden_noext"
REM 9.2.2: --quiet on an enrichment FILTER without its source switch is an
REM argument error (exit 2) naming the switch, not a silent "no match" (1).
REM A quiet filter WITHOUT enrichment must keep working - covered by the
REM many quiet gates above (e.g. "quiet filter matches").
call :t "quiet enrichment refusal" "list --quiet --filter country:x" 2 "refusing"
call :t "quiet enrichment names switch" "list --quiet --filter country:x" 2 "add --db"
call :t "quiet traffic refusal" "list --quiet --filter rtt:100" 2 "add --traffic"

REM 9.3.3/D33: a near-miss command and a near-miss switch are corrected, not
REM merely refused. A far miss stays bare - "nonsense" must not gain a
REM suggestion (its check above pins the bare message).
call :t "did you mean switch" "list --trafffic" 2 "Did you mean --traffic?"
call :t "did you mean command" "lst" 2 "Did you mean 'list'?"
call :t "did you mean help topic" "help lst" 2 "Did you mean 'list'?"

REM 9.3.6: the plan names the kill mode, and the two modes conflict.
REM --dry-run only: this gate never ends a process (header safety rule).
call :t "kill close plan" "kill --pid 1 --dry-run --close" 0 "WM_CLOSE only"
call :t "kill force plan" "kill --pid 1 --dry-run --force" 0 "terminate immediately"
call :t "kill mode conflict" "kill --close --force --pid 1" 2 "exclude each other"

REM 9.3.4: list --out - each contradiction is exit 2 naming its conflict;
REM the happy path and the existing-file refusal need the FILE, so they are
REM bespoke below (same pattern as export's).
call :t "list out refuses watch" "list --out %TEMP%\wng_out_x.csv --watch 1" 2 "one snapshot"
call :t "list out refuses quiet" "list --out %TEMP%\wng_out_x.csv --quiet" 2 "empty file"
set OUTCSV=%TEMP%\wng_out_%RANDOM%.csv
if exist "%OUTCSV%" del "%OUTCSV%"
"%BIN%" list --limit 2 --out "%OUTCSV%" > "%OUT%" 2>&1
set ORC=%ERRORLEVEL%
set /a CHECKS+=1
if not "%ORC%"=="0" (
    echo FAIL list --out happy path [rc=%ORC%, want 0]
    set /a FAILS+=1
) else (
    findstr /c:"wrote" "%OUT%" >nul 2>&1
    if errorlevel 1 (
        echo FAIL list --out happy path [no confirmation line]
        set /a FAILS+=1
    ) else if not exist "%OUTCSV%" (
        echo FAIL list --out happy path [file missing]
        set /a FAILS+=1
    ) else (
        echo ok - list --out happy path
    )
)
"%BIN%" list --limit 2 --out "%OUTCSV%" > "%OUT%" 2>&1
set ORC2=%ERRORLEVEL%
set /a CHECKS+=1
if not "%ORC2%"=="2" (
    echo FAIL list --out exists without force [rc=%ORC2%, want 2]
    set /a FAILS+=1
) else (
    echo ok - list --out exists without force
)
if exist "%OUTCSV%" del "%OUTCSV%"

echo.
echo CLI: %CHECKS% checks, %FAILS% failures.
if not "%FAILS%"=="0" exit /b 1
if exist "%OUT%" del "%OUT%"
echo CLI: PASS
exit /b 0

REM :t <name> <args> <rc> <marker>
REM A marker of "." skips the marker check (rc only).
:t
set TNAME=%~1
set TARGS=%~2
set TRC=%~3
set TMARK=%~4
set /a CHECKS+=1
"%BIN%" %TARGS% > "%OUT%" 2>&1
set GOTRC=%ERRORLEVEL%
if not "%GOTRC%"=="%TRC%" (
    echo FAIL %TNAME% [rc=%GOTRC%, want %TRC%: %TARGS%]
    set /a FAILS+=1
    goto :eof
)
if "%TMARK%"=="." (
    echo ok - %TNAME%
    goto :eof
)
findstr /c:"%TMARK%" "%OUT%" >nul 2>&1
if errorlevel 1 (
    echo FAIL %TNAME% [marker missing: %TMARK%]
    set /a FAILS+=1
    goto :eof
)
echo ok - %TNAME%
goto :eof

REM :tout <name> <args>
REM Like :t, but asserts a NEGATIVE on STDOUT ALONE: the run must succeed and
REM the forbidden text must not appear there. stderr is discarded, so this
REM proves the message was routed away from a data stream (the D3 hint, the
REM `baseline:` line) rather than merely "appears somewhere in the output".
:tout
set TNAME=%~1
set TARGS=%~2
if "%~3"=="" (set TFORB=can be enriched) else (set TFORB=%~3)
set /a CHECKS+=1
set TERRF=%TEMP%\wngolden_err_%RANDOM%.txt
"%BIN%" %TARGS% > "%OUT%" 2> "%TERRF%"
set GOTRC=%ERRORLEVEL%
if not "%GOTRC%"=="0" (
    echo FAIL %TNAME% [rc=%GOTRC%, want 0: %TARGS%]
    set /a FAILS+=1
    del "%TERRF%" >nul 2>&1
    goto :eof
)
findstr /c:"%TFORB%" "%OUT%" >nul 2>&1
if not errorlevel 1 (
    echo FAIL %TNAME% [advisory hint leaked onto stdout: %TARGS%]
    set /a FAILS+=1
    del "%TERRF%" >nul 2>&1
    goto :eof
)
del "%TERRF%" >nul 2>&1
echo ok - %TNAME%
goto :eof
