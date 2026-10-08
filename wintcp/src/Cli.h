// Cli.h
// Unified command line: wintcp.exe <command> [switches]. The only CLI mode.

#pragma once

namespace wintcp {

// True when the process was invoked with arguments (argc >= 2, not counting
// a lone elevation marker): those form a CLI invocation, never GUI options.
// Invoked bare (double-click, or with no arguments - or only the elevation
// marker from a UAC relaunch) it stays the GUI. This contract must never
// change - widening it would make a bare launch run the CLI instead of
// opening the window.
bool IsCliRequested(int argc, wchar_t** argv);

// Dispatch the unified grammar. Returns the process exit code:
//   0 = success (with --quiet: at least one row matched)
//   1 = failure, or an empty result (with --quiet: nothing matched)
//   2 = bad arguments / unknown command
//   3 = refused: a mutating command without --yes
int RunCli(int argc, wchar_t** argv);

}  // namespace wintcp
