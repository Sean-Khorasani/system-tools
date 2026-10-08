// TcpTable.cpp
// SPDX-License-Identifier: Apache-2.0
// TCP + UDP enumeration via GetExtendedTcpTable() / GetExtendedUdpTable().
// Fills the identity/display halves of Connection: family, protocol,
// binary addresses, scope ids, ports, state, pid, address+endpoint strings.

#include "TcpTable.h"
#include "Utils.h"

#include <iphlpapi.h>
#include <tcpmib.h>
#include <udpmib.h>

#include <cstring>

namespace wintcp {
namespace {

// Buffer-growth policy for the GetExtendedTcp/UdpTable queries below, shared
// by the TCP and UDP paths (they were two copies of the same three literals).
// 8 attempts is far beyond the 1-2 a growing table needs — reaching it means
// the table is growing faster than we can read it, and persisting would only
// lengthen a refresh that is already pathological. 64 bytes is the smallest
// allocation worth making (below any real table); 1 GB caps a lying `size`
// before it becomes an allocation failure.
constexpr int kTableQueryAttempts = 8;
constexpr ULONG kTableMinProbeBytes = 64;
constexpr ULONG kTableMaxBytes = 1024u * 1024u * 1024u;

// Printable rendering of an IPv4 address stored in network byte order.
std::wstring PrintIpv4(DWORD addrNetworkOrder) {
    IN_ADDR inAddr = {};
    inAddr.S_un.S_addr = addrNetworkOrder;
    wchar_t buf[INET_ADDRSTRLEN] = {0};
    if (::InetNtopW(AF_INET, &inAddr, buf, static_cast<size_t>(INET_ADDRSTRLEN)) == nullptr)
        return std::wstring(L"?.?.?.?");
    return std::wstring(buf);
}

// Printable rendering of a 16-byte IPv6 address, with the numeric scope id
// appended for link-local addresses: "fe80::1%12". The append RULE lives in
// Ipv6ScopeSuffix (Utils.h) because the socket scan must spell it identically:
// this string is a join key, not just a label.
std::wstring PrintIpv6(const UCHAR addr[16], DWORD scopeId) {
    IN6_ADDR in6 = {};
    std::memcpy(in6.s6_addr, addr, 16);
    wchar_t buf[INET6_ADDRSTRLEN] = {0};
    if (::InetNtopW(AF_INET6, &in6, buf, static_cast<size_t>(INET6_ADDRSTRLEN)) == nullptr)
        return std::wstring(L"::");
    std::wstring out(buf);
    out += Ipv6ScopeSuffix(addr, static_cast<unsigned>(scopeId));
    return out;
}

// dwLocalPort/dwRemotePort store the port in network byte order, low 16 bits.
UINT PortNetworkToHost(DWORD portNetworkOrder) {
    return static_cast<UINT>(::ntohs(static_cast<u_short>(portNetworkOrder & 0xFFFF)));
}

DWORD PortHostToNetwork(UINT portHostOrder) {
    return static_cast<DWORD>(::htons(static_cast<u_short>(portHostOrder)));
}

// Generic helpers: query an IP Helper table for one address family, growing
// the buffer until it fits (up to a sane retry limit). Each caller keeps
// its own output vector.
DWORD QueryTcp(ULONG family, std::vector<BYTE>& buffer) {
    ULONG size = 0;
    DWORD rc = ::GetExtendedTcpTable(nullptr, &size, FALSE, family,
                                     TCP_TABLE_OWNER_PID_ALL, 0);
    if (rc != NO_ERROR && rc != ERROR_INSUFFICIENT_BUFFER) return rc;
    for (int attempt = 0; attempt < kTableQueryAttempts; ++attempt) {
        if (size > kTableMaxBytes) return ERROR_INSUFFICIENT_BUFFER;
        if (size < kTableMinProbeBytes) size = kTableMinProbeBytes;
        buffer.resize(size);
        rc = ::GetExtendedTcpTable(buffer.data(), &size, FALSE, family,
                                   TCP_TABLE_OWNER_PID_ALL, 0);
        if (rc == NO_ERROR) return NO_ERROR;
        if (rc != ERROR_INSUFFICIENT_BUFFER) return rc;
    }
    return ERROR_INSUFFICIENT_BUFFER;
}

DWORD QueryUdp(ULONG family, std::vector<BYTE>& buffer) {
    ULONG size = 0;
    DWORD rc = ::GetExtendedUdpTable(nullptr, &size, FALSE, family,
                                     UDP_TABLE_OWNER_PID, 0);
    if (rc != NO_ERROR && rc != ERROR_INSUFFICIENT_BUFFER) return rc;
    for (int attempt = 0; attempt < kTableQueryAttempts; ++attempt) {
        if (size > kTableMaxBytes) return ERROR_INSUFFICIENT_BUFFER;
        if (size < kTableMinProbeBytes) size = kTableMinProbeBytes;
        buffer.resize(size);
        rc = ::GetExtendedUdpTable(buffer.data(), &size, FALSE, family,
                                   UDP_TABLE_OWNER_PID, 0);
        if (rc == NO_ERROR) return NO_ERROR;
        if (rc != ERROR_INSUFFICIENT_BUFFER) return rc;
    }
    return ERROR_INSUFFICIENT_BUFFER;
}

// Error families that are legitimately unavailable (disabled IPv6 stack etc.)
bool IsBenignFamilyError(DWORD rc) {
    return rc == ERROR_NOT_SUPPORTED ||
           rc == ERROR_ADDRESS_NOT_ASSOCIATED ||
           rc == ERROR_PROTOCOL_UNREACHABLE;
}

void FinishRow(Connection& info) {
    info.localEndpoint  = JoinEndpoint(info.localAddress, info.localPort,
                                       info.family == AF_INET6);
    info.remoteEndpoint = JoinEndpoint(info.remoteAddress, info.remotePort,
                                       info.family == AF_INET6);
}

}  // namespace

std::wstring TcpStateToString(DWORD state) {
    switch (state) {
        case MIB_TCP_STATE_CLOSED:     return L"CLOSED";
        case MIB_TCP_STATE_LISTEN:     return L"LISTENING";
        case MIB_TCP_STATE_SYN_SENT:   return L"SYN_SENT";
        case MIB_TCP_STATE_SYN_RCVD:   return L"SYN_RECEIVED";
        case MIB_TCP_STATE_ESTAB:      return L"ESTABLISHED";
        case MIB_TCP_STATE_FIN_WAIT1:  return L"FIN_WAIT_1";
        case MIB_TCP_STATE_FIN_WAIT2:  return L"FIN_WAIT_2";
        case MIB_TCP_STATE_CLOSE_WAIT: return L"CLOSE_WAIT";
        case MIB_TCP_STATE_CLOSING:    return L"CLOSING";
        case MIB_TCP_STATE_LAST_ACK:   return L"LAST_ACK";
        case MIB_TCP_STATE_TIME_WAIT:  return L"TIME_WAIT";
        case MIB_TCP_STATE_DELETE_TCB: return L"DELETE_TCB";
        default: break;
    }
    wchar_t buf[48] = {0};
    ::swprintf_s(buf, L"UNKNOWN(%lu)", static_cast<unsigned long>(state));
    return std::wstring(buf);
}

bool EnumerateEndpoints(std::vector<Connection>& out, std::wstring& errorMessage) {
    out.clear();
    std::vector<BYTE> buffer;

    // ---- TCP / IPv4 -------------------------------------------------------
    DWORD rc = QueryTcp(AF_INET, buffer);
    if (rc != NO_ERROR) {
        errorMessage = L"GetExtendedTcpTable(AF_INET) failed: " + FormatSystemError(rc);
        return false;
    }
    if (buffer.size() >= sizeof(MIB_TCPTABLE_OWNER_PID)) {
        const auto* table = reinterpret_cast<const MIB_TCPTABLE_OWNER_PID*>(buffer.data());
        for (DWORD i = 0; i < table->dwNumEntries; ++i) {
            const MIB_TCPROW_OWNER_PID& row = table->table[i];
            Connection info;
            info.family = AF_INET;
            info.protocol = IPPROTO_TCP;
            info.local4.S_un.S_addr = row.dwLocalAddr;
            info.remote4.S_un.S_addr = row.dwRemoteAddr;
            info.localPort  = PortNetworkToHost(row.dwLocalPort);
            info.remotePort = PortNetworkToHost(row.dwRemotePort);
            info.state = row.dwState;
            info.pid = row.dwOwningPid;
            info.localAddress  = PrintIpv4(row.dwLocalAddr);
            info.remoteAddress = PrintIpv4(row.dwRemoteAddr);
            FinishRow(info);
            out.push_back(std::move(info));
        }
    }

    // ---- TCP / IPv6 -------------------------------------------------------
    buffer.clear();
    rc = QueryTcp(AF_INET6, buffer);
    if (rc != NO_ERROR) {
        if (!IsBenignFamilyError(rc)) {
            errorMessage = L"GetExtendedTcpTable(AF_INET6) failed: " + FormatSystemError(rc);
            return false;
        }
        // IPv6 stack unavailable: keep IPv4 results.
    } else if (buffer.size() >= sizeof(MIB_TCP6TABLE_OWNER_PID)) {
        const auto* table6 = reinterpret_cast<const MIB_TCP6TABLE_OWNER_PID*>(buffer.data());
        for (DWORD i = 0; i < table6->dwNumEntries; ++i) {
            const MIB_TCP6ROW_OWNER_PID& row = table6->table[i];
            Connection info;
            info.family = AF_INET6;
            info.protocol = IPPROTO_TCP;
            std::memcpy(info.local6.s6_addr, row.ucLocalAddr, 16);
            std::memcpy(info.remote6.s6_addr, row.ucRemoteAddr, 16);
            info.localScope = row.dwLocalScopeId;
            info.remoteScope = row.dwRemoteScopeId;
            info.localPort  = PortNetworkToHost(row.dwLocalPort);
            info.remotePort = PortNetworkToHost(row.dwRemotePort);
            info.state = row.dwState;
            info.pid = row.dwOwningPid;
            info.localAddress  = PrintIpv6(row.ucLocalAddr, row.dwLocalScopeId);
            info.remoteAddress = PrintIpv6(row.ucRemoteAddr, row.dwRemoteScopeId);
            FinishRow(info);
            out.push_back(std::move(info));
        }
    }

    // ---- UDP / IPv4 -------------------------------------------------------
    buffer.clear();
    rc = QueryUdp(AF_INET, buffer);
    if (rc != NO_ERROR) {
        if (!IsBenignFamilyError(rc)) {
            errorMessage = L"GetExtendedUdpTable(AF_INET) failed: " + FormatSystemError(rc);
            return false;
        }
    } else if (buffer.size() >= sizeof(MIB_UDPTABLE_OWNER_PID)) {
        const auto* table = reinterpret_cast<const MIB_UDPTABLE_OWNER_PID*>(buffer.data());
        for (DWORD i = 0; i < table->dwNumEntries; ++i) {
            const MIB_UDPROW_OWNER_PID& row = table->table[i];
            Connection info;
            info.family = AF_INET;
            info.protocol = IPPROTO_UDP;
            info.local4.S_un.S_addr = row.dwLocalAddr;
            info.localPort  = PortNetworkToHost(row.dwLocalPort);
            info.state = 0;                       // UDP has no state
            info.pid = row.dwOwningPid;
            info.localAddress  = PrintIpv4(row.dwLocalAddr);
            info.remoteAddress = L"*";            // UDP rows carry no remote endpoint
            FinishRow(info);
            info.remoteEndpoint = L"*:*";
            out.push_back(std::move(info));
        }
    }

    // ---- UDP / IPv6 -------------------------------------------------------
    buffer.clear();
    rc = QueryUdp(AF_INET6, buffer);
    if (rc != NO_ERROR) {
        if (!IsBenignFamilyError(rc)) {
            errorMessage = L"GetExtendedUdpTable(AF_INET6) failed: " + FormatSystemError(rc);
            return false;
        }
    } else if (buffer.size() >= sizeof(MIB_UDP6TABLE_OWNER_PID)) {
        const auto* table6 = reinterpret_cast<const MIB_UDP6TABLE_OWNER_PID*>(buffer.data());
        for (DWORD i = 0; i < table6->dwNumEntries; ++i) {
            const MIB_UDP6ROW_OWNER_PID& row = table6->table[i];
            Connection info;
            info.family = AF_INET6;
            info.protocol = IPPROTO_UDP;
            std::memcpy(info.local6.s6_addr, row.ucLocalAddr, 16);
            info.localScope = row.dwLocalScopeId;
            info.localPort  = PortNetworkToHost(row.dwLocalPort);
            info.state = 0;
            info.pid = row.dwOwningPid;
            info.localAddress  = PrintIpv6(row.ucLocalAddr, row.dwLocalScopeId);
            info.remoteAddress = L"*";            // no remote for UDP (netstat-style)
            FinishRow(info);
            info.remoteEndpoint = L"*:*";
            out.push_back(std::move(info));
        }
    }

    errorMessage.clear();
    return true;
}

bool CloseTcpConnection(const Connection& c, std::wstring& errorMessage) {
    if (c.protocol != IPPROTO_TCP) {
        errorMessage = L"Only TCP connections can be closed (UDP has no sessions).";
        return false;
    }
    if (c.family != AF_INET) {
        errorMessage = L"Windows' SetTcpEntry API only supports IPv4 connections; "
                       L"this row is IPv6.";
        return false;
    }
    MIB_TCPROW row = {};
    row.dwState      = MIB_TCP_STATE_DELETE_TCB;
    row.dwLocalAddr  = c.local4.S_un.S_addr;
    row.dwLocalPort  = PortHostToNetwork(c.localPort);
    row.dwRemoteAddr = c.remote4.S_un.S_addr;
    row.dwRemotePort = PortHostToNetwork(c.remotePort);
    const DWORD rc = ::SetTcpEntry(&row);
    if (rc != NO_ERROR) {
        errorMessage = L"SetTcpEntry failed: " + FormatSystemError(rc);
        if (rc == ERROR_ACCESS_DENIED)
            errorMessage += L" (closing connections requires running as "
                             L"administrator)";
        else if (rc == ERROR_NOT_FOUND || rc == 1168 /* ELEMENT_NOT_FOUND */ ||
                 rc == 317 /* ERROR_MR_MID_NOT_FOUND: the row moved */)
            errorMessage += L" (the row no longer matches the live table - "
                             L"it closed or changed state; list again and "
                             L"retry)";
        return false;
    }
    errorMessage.clear();
    return true;
}

}  // namespace wintcp
