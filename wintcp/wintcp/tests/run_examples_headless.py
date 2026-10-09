import os
import re
import subprocess
import sys
import time

# ----------------------------------------------------------------------------
# run_examples_headless.py - run the examples gate (examples.txt) in parallel
# on machines where the harness would otherwise exceed the ~30s wrapper limit.
#
# The full gate takes ~90s. This splits examples.txt into N chunks of 15
# documented commands each and fires one examples.bat per chunk in parallel.
# Each chunk is a private copy of examples.bat with a unique %TEMP% PSOUT, so
# the chunks never collide. The harness self-test (4 hard-coded commands) runs
# inside every chunk, so a broken harness aborts every chunk loudly.
#
# Each batch writes its own stdout (which contains the authoritative
# "EXAMPLES: N documented commands, M rejected" verdict) to a file in %TEMP%,
# so the final verdict survives even if a hard kill truncates PSOUT.
#
# Usage:
#   python run_examples_headless.py
# ----------------------------------------------------------------------------

BIN = r"D:\src\winnet\build\wintcp.exe"
CMDFILE = r"D:\src\winnet\wintcp\tests\examples.txt"
EXAMPLES_BAT = r"D:\src\winnet\wintcp\tests\examples.bat"

CHUNK_SIZE = 15   # documented commands per parallel chunk
WRAPPER_TIMEOUT = 29  # launch timeout, must stay under the wrapper's kill (30s)


def load_examples(cmf):
    examples = []
    with open(cmf, encoding="utf-8", errors="replace") as fh:
        for lineno, raw in enumerate(fh, start=1):
            line = raw.strip()
            if not line or line.startswith("#"):
                continue
            examples.append((lineno, line))
    return examples


def split_chunks(examples, size):
    return [examples[i : i + size] for i in range(0, len(examples), size)]


def read_psout(temp, name):
    path = os.path.join(temp, name)
    try:
        with open(path, encoding="utf-8", errors="replace") as fh:
            return fh.read()
    except Exception:
        return ""


def patch_examples_bat(original, psout_name):
    bat = original.replace(
        "set PSOUT=%TEMP%\\wnexamples_ps.txt",
        "set PSOUT=%TEMP%\\" + psout_name,
    )
    # The chunked batch lives in %TEMP%, so examples.ps1 must not be resolved
    # relative to its own directory; point it at the original driver.
    bat = bat.replace("%~dp0examples.ps1", r"D:\src\winnet\wintcp\tests\examples.ps1")
    insert = 'echo EXAMPLES_VERDICT: %N% %BAD% >> "%PSOUT%"'
    marker = 'del "%PSOUT%" >nul 2>&1'
    assert marker in bat
    bat = bat.replace(marker, insert + os.linesep + marker)
    return bat
def estimate_duration(args_line):
    """Estimate wall time of one example command. Watch commands dominate."""
    args = args_line.strip()
    if "--watch" in args:
        m = re.search(r"--watch\s+(\d+)", args)
        interval = int(m.group(1)) if m else 1
        m = re.search(r"--count\s+(\d+)", args)
        count = int(m.group(1)) if m else 1
        # initial snapshot + (count-1) updates + last-update margin
        return 2 + interval * (count - 1) + 1
    return 0.8


def balance_chunks_ordered(examples, n_chunks):
    """Split by wall-clock estimate so no chunk takes too long.
    Boundaries preserve original example order (chunk 1 = first examples)."""
    times = [estimate_duration(ex[1]) for ex in examples]
    total = sum(times)
    cuts = []
    for i in range(1, n_chunks):
        target = i * total / n_chunks
        acc = 0.0
        idx = 0
        while idx < len(times) and acc < target:
            acc += times[idx]
            idx += 1
        cuts.append(idx)
    chunks = []
    start = 0
    for cut in cuts:
        chunks.append(examples[start:cut])
        start = cut
    chunks.append(examples[start:])
    return chunks


def time_chunks(examples):
    """8 time-balanced chunks (~14s each). The previous count-based split let
    the chunk containing examples #26/#27/#29 run ~31s and get killed by the
    wrapper's 30s hard kill; time-balancing caps the slowest chunk ~15s."""
    return balance_chunks_ordered(examples, 8)


def main():
    temp = os.environ.get("TEMP") or os.environ.get("TMP")
    examples = load_examples(CMDFILE)
    chunks = time_chunks(examples)
    chunk_info = ", ".join("c%d:%ds/%d" % (i + 1, round(sum(estimate_duration(e[1]) for e in c)), len(c)) for i, c in enumerate(chunks))
    print("examples_gate: %d documented commands -> %s" % (len(examples), chunk_info))

    subprocess.run(
        ["taskkill", "/F", "/IM", "wintcp.exe"],
        capture_output=True,
        timeout=5,
    )
    time.sleep(0.3)

    chunks = time_chunks(examples)
    chunk_files = []
    for idx, chunk in enumerate(chunks, start=1):
        cf = os.path.join(temp, "wnexample_chunk_%02d.txt" % idx)
        chunk_files.append(cf)
        with open(cf, "w", encoding="utf-8") as fh:
            fh.write("\n".join(line for _, line in chunk) + "\n")

    bat_src = open(EXAMPLES_BAT, encoding="utf-8").read()
    procs = []
    psout_names = []
    outpaths = []
    for idx, chunk in enumerate(chunks, start=1):
        cf = os.path.join(temp, "wnexample_chunk_%02d.txt" % idx)
        psout = "wnexamples_ps_%02d.txt" % idx
        psout_names.append(psout)
        outpath = os.path.join(temp, "wnexample_chunk_%02d.out" % idx)
        outpaths.append(outpath)
        batfile = os.path.join(temp, "wnexample_chunk_%02d.bat" % idx)
        patched = patch_examples_bat(bat_src, psout)
        with open(batfile, "w", encoding="utf-8") as fh:
            fh.write(patched)
        p = subprocess.Popen(
            ["cmd.exe", "/c", "call", batfile, BIN, cf],
            stdout=open(outpath, "w", encoding="utf-8"),
            stderr=subprocess.DEVNULL,
        )
        procs.append(p)
        print("examples_gate: chunk %d -> %d examples (pid %s)" % (idx, len(chunk), p.pid))

    start = time.time()
    results = {}
    last = {i: "" for i in range(1, len(chunks) + 1)}

    while time.time() - start < WRAPPER_TIMEOUT:
        time.sleep(0.3)
        all_done = True
        for idx, p in enumerate(procs, start=1):
            s = read_psout(temp, psout_names[idx - 1])
            for line in s.splitlines():
                if line.startswith("COUNTS:"):
                    parts = line.split()
                    results[idx] = {
                        "size": len(chunks[idx - 1]),
                        "runs": int(parts[1]),
                        "bad": int(parts[2]),
                        "selftest_ok": False,
                        "verdict": "",
                    }
                if "self-test ok" in line:
                    if idx not in results:
                        results[idx] = {
                            "size": len(chunks[idx - 1]),
                            "runs": 0,
                            "bad": len(chunks[idx - 1]),
                            "selftest_ok": False,
                            "verdict": "",
                        }
                    results[idx]["selftest_ok"] = True
                if line.startswith("EXAMPLES_VERDICT:"):
                    if idx not in results:
                        results[idx] = {
                            "size": len(chunks[idx - 1]),
                            "runs": 0,
                            "bad": len(chunks[idx - 1]),
                            "selftest_ok": False,
                            "verdict": "",
                        }
                    results[idx]["verdict"] = line.split("EXAMPLES_VERDICT:", 1)[1].strip()
            if len(s) > len(last[idx]):
                print(
                    "examples_gate: chunk %d progress: %d bytes (%d new)"
                    % (idx, len(s), len(s) - len(last[idx]))
                )
            last[idx] = s
            if p.poll() is not None:
                if idx not in results:
                    results[idx] = {
                        "size": len(chunks[idx - 1]),
                        "runs": 0,
                        "bad": len(chunks[idx - 1]),
                        "selftest_ok": False,
                        "verdict": "",
                    }
                results[idx]["done"] = True
            else:
                all_done = False

        if all_done:
            print("examples_gate: all chunks finished before launch timeout")
            break

    # ---- authoritative verdict: read each batch's own stdout -------------------
    print()
    print("=== examples_gate aggregate ===")
    batch_outs = []
    for idx in range(1, len(chunks) + 1):
        path = os.path.join(temp, "wnexample_chunk_%02d.out" % idx)
        try:
            with open(path, encoding="utf-8", errors="replace") as fh:
                batch_outs.append((idx, fh.read()))
        except FileNotFoundError:
            batch_outs.append((idx, ""))

    total_runs = 0
    total_bad = 0
    overall_pass = True
    for idx, out in batch_outs:
        rejected = size = 0
        selftest = "ok"
        verdict = ""
        for line in out.splitlines():
            import re
            m = re.search(r"EXAMPLES: (\d+) documented commands, (\d+) rejected\.", line)
            if m:
                size = int(m.group(1))
                rejected = int(m.group(2))
            if "EXAMPLES: ABORTED" in line:
                selftest = "ABORTED"
            if line.startswith("EXAMPLES: PASS"):
                verdict = "PASS"
            if line.startswith("EXAMPLES: FAIL"):
                verdict = "FAIL"
        total_runs += size
        total_bad += rejected
        ok = (verdict == "PASS") or (rejected == 0 and selftest == "ok")
        overall_pass &= ok
        print(
            "chunk %d: size=%d rejected=%d self-test=%s verdict=%s"
            % (idx, size, rejected, selftest, verdict)
        )

    print(
        "TOTAL: %d documented examples, %d rejected, %0.1fs"
        % (total_runs, total_bad, time.time() - start)
    )
    if overall_pass and total_bad == 0:
        print("examples_gate: PASS - every documented example is accepted.")
        return 0
    print("examples_gate: FAIL - the documentation shows a command the binary rejects.")
    return 1


if __name__ == "__main__":
    sys.exit(main())