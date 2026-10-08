// BlockConn.h
// SPDX-License-Identifier: Apache-2.0
// Two-layer "block this connection":
//   Layer 1 - tear the live TCP connection down immediately.
//   Layer 2 - install Windows Firewall outbound rules so it cannot come back.
//
// Layer 1 is IPv4-only. Windows ships no supported public API for tearing down
// an individual IPv6 connection; see the long note on SetIpv6Teardown below.
// Layer 2 works for both families, so an IPv6 block still stops reconnection -
// it just cannot drop the connection that exists right now.

#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0601
#endif

// Firewall access uses COM (CLSID_NetFwPolicy2 -> INetFwPolicy2), not the old
// INetFwMgr. INetFwPolicy2 exists from Vista onward, so _WIN32_WINNT=0x0601 is
// sufficient and no symbol here needs a higher NTDDI.
//
// LINKING: no library is required. netfw.h declares its GUIDs as bare
// "EXTERN_C const CLSID CLSID_NetFwPolicy2", implying the historical
// hnetfw.lib - but no current Windows SDK ships it (10.0.19041.0 and
// 10.0.26100.0 contain only NetFW.TLB, a type library for #import), and
// linking it fails with LNK1104. BlockConn.cpp therefore takes the GUIDs from
// netfw.h's own DECLSPEC_UUID declarations via __uuidof, so the module links
// with nothing added to the .vcxproj and no hnetfw.lib on the link line.
// winsock2.h must come before windows.h so the winsock 2 declarations win over
// the older winsock.h pulled in by windows.h. The firewall COM headers are only
// needed by the .cpp, so they are not included here.
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>

#include <cstdint>
#include <string>
#include <vector>   // R3: ParseLedgerBytes hands back the parsed pairs

namespace wintcp {

// Every rule this module creates is named "<kRuleNamePrefix><suffix>". The
// prefix is the ownership tag: unblocking and cleanup only ever touch rules
// whose name starts with it, so a user-authored rule that happens to look
// similar is never deleted.
extern const wchar_t* const kRuleNamePrefix;   // L"WinTCP block: "

// The rule *name* (not the description) is what the Windows Firewall policy
// store actually caps. NET_FW_MAX_RULE_NAME_LEN is not defined by netfw.h;
// the effective limit is 255 characters, and a name that exceeds it is
// rejected by the service with 0x8007007A (ERROR_INVALID_NAME). We therefore
// truncate the label to keep prefix + address + ports + label under 255, and
// assert that the identity part of the name can never be truncated - the
// address and both ports are what make the name unique, so clipping them
// would make two different connections collide.
constexpr size_t kMaxRuleNameChars = 255;

enum class BlockOutcome {
    kBlocked,        // connection killed and rules in place
    kRulesOnly,      // rules in place, but the live connection could not be killed
    kKilledOnly,     // connection killed, but rule creation failed
    kFailed,
    kAlreadyBlocked, // both rules were already present; nothing was created
};

const wchar_t* BlockOutcomeToString(BlockOutcome o);

struct BlockRequest {
    bool ipv6 = false;
    unsigned char remoteAddr[16] = {0};   // raw network-order bytes
    uint16_t localPort = 0;               // host order
    uint16_t remotePort = 0;              // host order
    std::wstring label;                   // for the rule names
};

// Block the connection described by 'req': kill it if the family allows,
// then install the outbound firewall rules that stop it returning.
//
// 'error' is optional; when supplied it always receives a non-empty human
// readable string for every outcome except kBlocked and kAlreadyBlocked, and
// it is cleared first so a stale message from a previous call cannot be
// mistaken for this one's. Never throws.
BlockOutcome BlockConnection(const BlockRequest& req, std::wstring* error);

// Remove exactly the rules BlockConnection created for 'req'. Returns true
// when the connection is not blocked afterwards - including the case where it
// was never blocked in the first place. 'error' behaves as above.
bool UnblockConnection(const BlockRequest& req, std::wstring* error);

// True when both rules for 'req' are present and enabled. This reports what
// the firewall says, so a user who deleted or disabled our rule by hand is
// correctly reported as not blocked. Returns false (never throws) if the
// firewall cannot be queried at all.
bool IsConnectionBlocked(const BlockRequest& req);

// How many rules currently carry the WinTCP tag, in any direction or family.
//
// IMPORTANT - how this is counted, and why. It does NOT use
// INetFwRules::get__NewEnum. Measured on this machine: a rule created through
// INetFwRules::Add, verified present via INetFwRules::Item and via
// `netsh advfirewall firewall show rule name=...`, is **absent from the
// IEnumVARIANT enumeration** in the same process. netsh reports it correctly
// (RemoteIP 203.0.113.7/32, Protocol TCP, Action Block) and the rule does
// enforce; it is only the enumerator that is incomplete. So an
// enumeration-based count reported 0 for a rule that was really there, and
// RemoveAllWinTcpRules would have left every such rule behind - a firewall
// rule the user cannot remove is exactly the "block" that must never exist.
//
// So the count is done by asking the firewall for each name we could have
// created, via Item(). That is O(number of possible peers) rather than
// O(system rules), which is the right trade: correctness over speed for a
// count that runs on demand.
// A count that cannot be made is not a count of zero: R3 added an error
// channel because `blocks` reporting "0" for a ledger it refused to read is
// the silent-wrong-answer shape this tool does not otherwise ship. *error is
// optional and, when supplied, is set ONLY on that failure - a genuinely
// empty ledger is not an error.
int CountWinTcpRules(std::wstring* error = nullptr);

// Delete every rule carrying the WinTCP tag. Intended for "remove all the
// blocks this app created" on shutdown. Returns true when no tagged rule
// survives; the failing names are appended to 'error'.
bool RemoveAllWinTcpRules(std::wstring* error);

// R3: parse the ledger's bytes into rule-name pairs. PURE - no file, no
// registry, no environment - so the selftest can drive it with hostile input
// without touching the real ledger under %APPDATA%, which is the one thing it
// must never do. The file cap lives in ReadLedger (it needs the file size);
// this covers the per-line cap and UTF-8 validation.
//
// FALSE + error on bytes that cannot be trusted - a line over
// kLedgerMaxLineBytes, a NUL byte, or invalid UTF-8 - and *out is then left
// UNTOUCHED, so a caller can never half-apply a ledger it has already
// rejected. Refusing the whole file is deliberate: a skipped line is a rule
// name nobody recorded, and the whole point of the ledger is that
// RemoveAllWinTcpRules can still find the rule it created.
bool ParseLedgerBytes(const char* data, size_t len,
                      std::vector<std::wstring>* out, std::wstring* error);

// R3: the per-line cap, in the header so the selftest that pins the boundary
// uses the same number the parser does rather than a copy of it. A name is at
// most kMaxRuleNameChars (255) wchar_t and a wchar_t is at most 4 bytes in
// UTF-8, so a line this code can legitimately write cannot exceed
// 2*255*4 + 1 = 2041 bytes; 4096 is 2x that headroom, and the limit is
// inclusive.
constexpr size_t kLedgerMaxLineBytes = 4096;

// --- Layer 1 detail ---------------------------------------------------------
//
// TRUTH ABOUT IPv6 TEARDOWN, as verified against the Windows 10/11 SDK
// headers (um\iphlpapi.h) rather than assumed:
//
//   * SetTcpEntry(_In_ PMIB_TCPROW) is the *only* connection-state mutator in
//     iphlpapi.h. Its comment block states outright: "The only state that it
//     can be set to is MIB_TCP_STATE_DELETE_TCB."
//   * There is no SetTcpEntry2, no SetTcp6Entry, and no IPv6 variant of the
//     MIB_TCPROW setter anywhere in the SDK. A recursive search of every
//     header in Include\10.0.26100.0 for SetTcpEntry2 / SetTcp6Entry returns
//     nothing. MIB_TCP6ROW is enumerated (GetTcp6Table) and owns-module lookup
//     works (GetOwnerModuleFromTcp6Entry) but it is never accepted as
//     teardown input.
//   * SetPerTcp6ConnectionEStats does take a PMIB_TCP6ROW, but it only sets
//     per-connection EStats (the TCP_ESTATS_TYPE set in shared\tcpestats.h -
//     data, send congestion, path, buffers, observed RTT, bandwidth, fine RTT).
//     It has no state/delete semantics; there is nothing to smuggle a
//     DELETE_TCB through it.
//
// So there is no supported public per-connection IPv6 teardown. The options
// that do exist (closing a duplicated socket handle, or a WFP callout that
// injects RST) are privileged, race with the connection already being gone,
// and are not a supported API contract. This module does not pretend
// otherwise: SetIpv6Teardown returns false with an explicit reason and
// BlockConnection then reports kRulesOnly rather than claiming a kill.

// Kill the live IPv4 TCP connection identified by the 4-tuple (localPort,
// remoteAddr, remotePort). 'remoteAddr' is 4 raw network-order bytes.
// 'localAddr' may be null to mean "any local address", which is what
// BlockConnection does: BlockRequest deliberately does not carry a local
// address, and a machine may hold the same local port bound to several
// addresses, so every matching row is torn down rather than leaving a sibling
// socket alive after the user asked for the connection to be gone.
// Returns false for a zero port or unspecified remote address, and for a
// connection that is no longer present.
bool SetIpv4Teardown(const unsigned char* remoteAddr, uint16_t localPort,
                     uint16_t remotePort, std::wstring* error);

}  // namespace wintcp
