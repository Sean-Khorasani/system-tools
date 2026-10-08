// Alerts.cpp
// SPDX-License-Identifier: Apache-2.0
// See Alerts.h.

#include "Alerts.h"
#include "Utils.h"   // kBytesPerKB/MB/GB shared unit ladder (A6)

#include <iphlpapi.h>   // MIB_TCP_STATE_LISTEN
#include <tcpmib.h>

#include <cstdio>

namespace wintcp {
namespace {

// Latch helper: returns true only on the rising edge of a condition.
inline bool Rising(bool nowActive, bool* latched) {
    const bool rising = (nowActive && !*latched);
    *latched = nowActive;
    return rising;
}

}  // namespace

std::wstring FormatBps(double bps) {
    if (!(bps > 0.0)) return L"0 B/s";
    wchar_t buf[64] = {0};
    if (bps >= kBytesPerGB) {
        ::swprintf_s(buf, L"%.1f GB/s", bps / kBytesPerGB);
    } else if (bps >= kBytesPerMB) {
        ::swprintf_s(buf, L"%.1f MB/s", bps / kBytesPerMB);
    } else if (bps >= kBytesPerKB) {
        ::swprintf_s(buf, L"%.0f KB/s", bps / kBytesPerKB);
    } else {
        ::swprintf_s(buf, L"%.0f B/s", bps);
    }
    return buf;
}

void AlertEngine::Reset() {
    latched_ = 0;
    sawListener_ = false;
    sawConnection_ = false;
    lastCount_ = 0;
    suppressed_ = 0;
}

std::vector<Alert> AlertEngine::Evaluate(const std::vector<Connection>& rows,
                                         const AlertSettings& s) {
    std::vector<Alert> out;
    suppressed_ = 0;
    // Everything below is inert when alerting is off, but the latches are
    // still updated so that turning alerts on does not immediately fire on
    // conditions that were already true. Firing on a pre-existing condition
    // the first time the user enables alerts would be a lie about timing.
    if (!s.enabled) {
        lastCount_ = rows.size();
        return out;
    }

    // Per-connection throughput. Only rows whose rate is actually known are
    // considered: an unknown rate is not "below the threshold", it is "no
    // data yet", and alerting on the difference would be meaningless.
    // rx and tx are summed - a transfer saturating a link in either
    // direction is the event the user cares about, not a directional split.
    double peakBps = 0.0;
    size_t peakCount = 0;
    for (const Connection& c : rows) {
        if (!c.bpsKnown) continue;
        const double total = c.rxBps + c.txBps;
        if (total > peakBps) { peakBps = total; peakCount = 1; }
        else if (peakBps > 0.0 && total == peakBps) { ++peakCount; }
    }

    const bool overWarn = (s.bpsWarn > 0.0 && peakBps >= s.bpsWarn);
    const bool overCrit = (s.bpsCritical > 0.0 && peakBps >= s.bpsCritical);
    bool latchWarn = (latched_ & (1u << 0)) != 0;
    bool latchCrit = (latched_ & (1u << 1)) != 0;

    // Count every condition that is active but did not notify, so the UI can
    // distinguish "nothing happened" from "it is all already on fire and you
    // have been told once". Counted for BOTH thresholds, not just the warning
    // branch: a latched critical alert is just as suppressed as a latched
    // warning, and leaving it uncounted would make the number under-report.
    if (Rising(overCrit, &latchCrit)) {
        Alert a;
        a.kind = AlertKind::kBpsCritical;
        a.title = L"Throughput critical";
        a.text = FormatBps(peakBps) +
                 (peakCount > 1 ? L" on " + std::to_wstring(peakCount) +
                                      L" connections."
                                : std::wstring(L" on a connection."));
        a.rising = true;
        out.push_back(a);
    } else if (overCrit) {
        ++suppressed_;
    }
    // The warning is only interesting if the critical threshold has not
    // already taken over; reporting both for the same event is noise.
    if (overWarn && !overCrit) {
        if (Rising(overWarn, &latchWarn)) {
            Alert a;
            a.kind = AlertKind::kBpsWarn;
            a.title = L"High throughput";
            a.text = FormatBps(peakBps) + L" observed.";
            a.rising = true;
            out.push_back(a);
        } else {
            ++suppressed_;
        }
    } else {
        latchWarn = overWarn;   // re-sync the latch
    }
    latched_ = (latched_ & ~((1u << 0) | (1u << 1))) |
               (latchWarn ? (1u << 0) : 0) |
               (latchCrit ? (1u << 1) : 0);

    // Connection count.
    bool latchCount = (latched_ & (1u << 2)) != 0;
    const bool overCount = (s.connectionWarn > 0 && rows.size() >= s.connectionWarn);
    if (Rising(overCount, &latchCount)) {
        Alert a;
        a.kind = AlertKind::kConnectionCount;
        a.title = L"Many connections";
        a.text = std::to_wstring(rows.size()) + L" connections open.";
        a.rising = true;
        out.push_back(a);
    } else if (overCount) {
        ++suppressed_;
    }
    latched_ = (latched_ & ~(1u << 2)) | (latchCount ? (1u << 2) : 0);

    // Listening sockets and brand-new connections.
    size_t listeners = 0;
    for (const Connection& c : rows) {
        if (c.protocol == IPPROTO_TCP &&
            c.state == MIB_TCP_STATE_LISTEN)
            ++listeners;
    }
    const bool anyListener = listeners > 0;
    const bool anyConnection = rows.size() > listeners;
    bool latchL = (latched_ & (1u << 3)) != 0;
    bool latchC = (latched_ & (1u << 4)) != 0;
    const bool newListener = Rising(anyListener, &latchL);
    const bool newConn = Rising(anyConnection, &latchC);
    latched_ = (latched_ & ~((1u << 3) | (1u << 4))) |
               (latchL ? (1u << 3) : 0) | (latchC ? (1u << 4) : 0);

    if (s.alertOnNewListener && newListener) {
        Alert a;
        a.kind = AlertKind::kNewListener;
        a.title = L"Listening socket";
        a.text = std::to_wstring(listeners) +
                 (listeners == 1 ? L" socket is listening."
                                 : L" sockets are listening.");
        a.rising = true;
        out.push_back(a);
    }
    if (s.alertOnNewConnection && newConn) {
        Alert a;
        a.kind = AlertKind::kNewConnection;
        a.title = L"Connection established";
        a.text = std::to_wstring(rows.size() - listeners) + L" active connection(s).";
        a.rising = true;
        out.push_back(a);
    }

    sawListener_ = anyListener;
    sawConnection_ = anyConnection;
    lastCount_ = rows.size();
    return out;
}

// No owner parameter: NOTIFYICONDATAW already carries hWnd, and passing a
// second one invites the two to disagree. The caller owns the nid and its
// lifetime.
bool ShowTrayBalloon(NOTIFYICONDATAW* nid, const std::wstring& title,
                     const std::wstring& text, DWORD iconFlags) {
    if (nid == nullptr) return false;
    // NIF_INFO is NOT in the icon's original uFlags; it has to be added here.
    // A balloon sent without it is silently dropped by the shell, which looks
    // exactly like "the alert feature does not work".
    nid->uFlags |= NIF_INFO;
    nid->hIcon = nullptr;   // required when NIF_INFO is set
    nid->dwInfoFlags = iconFlags;
    nid->uVersion = NOTIFYICON_VERSION_4;
    nid->szInfoTitle[0] = L'\0';
    nid->szInfo[0] = L'\0';
    ::wcsncpy_s(nid->szInfoTitle, title.c_str(), _TRUNCATE);
    ::wcsncpy_s(nid->szInfo, text.c_str(), _TRUNCATE);
    return ::Shell_NotifyIconW(NIM_MODIFY, nid) != FALSE;
}

}  // namespace wintcp
