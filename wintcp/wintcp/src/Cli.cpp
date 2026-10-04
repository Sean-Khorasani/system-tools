// Cli.cpp
// Unified command-line entry: wintcp.exe <command> [switches].
//
// There is exactly ONE CLI mode. An earlier revision had a second,
// flag-style mode (-c, --csv, --json, ...); it was removed because two
// grammars meant two parsers, two help texts and twice the confusion, for
// zero extra capability - every legacy switch has a verb spelling:
//
//   -c / --csv / --json      ->  list --format csv|json | export --format ...
//   --format <set>           ->  --columns <set>  (applies to json too)
//   --watch [sec]            ->  --watch [sec] on list/ps/top/stat
//   -q / --quiet             ->  list|ps --quiet (rc answers, no output)
//   --selftest / --bench     ->  selftest | bench
//   --version / -h / --help  ->  version | help
//   -a / -n / --once         ->  dropped (accepted-and-ignored parity flags;
//                                the verbs always list all numeric endpoints
//                                in one pass)
//
// Bare launch (no arguments) stays the GUI; that contract must never change.
// A lone elevation marker (--__wintcp-elevated) also means "no user args":
// that is the elevated GUI relaunch, which used to die here with
// "Unknown option" instead of opening the window.

#include "Cli.h"

#include <string>
#include <vector>

#include "CliCommands.h"
#include "Utils.h"

namespace wintcp {
namespace {

// The elevation marker Reelevate() appends. A process-level flag, never a
// command or a switch: it is stripped before parsing, and IsElevatedInstance
// keeps reading it from the raw command line, so nothing else changes.
bool IsMarker(const wchar_t* a) {
    return a != nullptr &&
           ::wcscmp(a, L"--__wintcp-elevated") == 0;
}

bool IsGlobalHelp(const std::wstring& a) {
    return a == L"--help" || a == L"-h" || a == L"-?" || a == L"/?" ||
           a == L"/help";
}

}  // namespace

bool IsCliRequested(int argc, wchar_t** argv) {
    // Unchanged on purpose: invoked bare (double-click, no arguments) this
    // has to stay the GUI. Any real argument at all means CLI. The elevation
    // marker does not count: on its own it is the elevated GUI relaunch.
    if (argc < 2 || argv == nullptr) return false;
    for (int i = 1; i < argc; ++i) {
        if (!IsMarker(argv[i])) return true;
    }
    return false;
}

int RunCli(int argc, wchar_t** argv) {
    // Strip the elevation marker: it must be invisible to the parser
    // wherever it appears (front, back, or beside a real command, as the
    // elevated relaunch preserves the original arguments).
    std::vector<wchar_t*> args;
    args.reserve(static_cast<size_t>(argc));
    args.push_back(argv[0]);
    for (int i = 1; i < argc; ++i) {
        if (!IsMarker(argv[i])) args.push_back(argv[i]);
    }
    const int n = static_cast<int>(args.size());
    if (n < 2) return 2;   // unreachable via main(); defensive only

    // Global help outranks everything, wherever it appears first.
    const std::wstring first = args[1] != nullptr ? args[1] : L"";
    if (IsGlobalHelp(first)) {
        // `wintcp --help [cmd]` behaves like `wintcp help [cmd]`.
        std::vector<wchar_t*> helpArgs;
        helpArgs.reserve(static_cast<size_t>(n + 1));
        helpArgs.push_back(args[0]);
        static wchar_t kHelp[] = L"help";
        helpArgs.push_back(kHelp);
        for (int i = 2; i < n; ++i) helpArgs.push_back(args[i]);
        return RunCliCommand(static_cast<int>(helpArgs.size()),
                             helpArgs.data());
    }
    return RunCliCommand(n, args.data());
}

}  // namespace wintcp
