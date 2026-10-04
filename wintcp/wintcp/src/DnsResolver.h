// DnsResolver.h
// Asynchronous reverse-DNS for remote endpoints: a dedicated
// worker thread resolves PTR records with getnameinfo(NI_NAMEREQD) while
// the UI thread keeps painting. Results (positive and negative) are cached
// per address; each result is delivered to a sink running on the worker
// thread, which normally posts it to the window.
//
// Known trade-off: getnameinfo has no timeout parameter, so one slow DNS
// query delays only this thread (the refresh worker and UI are unaffected),
// and application exit may wait for the in-flight lookup to return.
//
// B4 verdict: single worker, unbounded per-query wait, positive+negative
// cache, no --dns-timeout/--dns-workers knobs. A timeout cannot be built on
// an API that has none (cancelling getnameinfo mid-call is not supported),
// and a worker pool multiplies concurrent DNS load for no latency win on the
// refresh path, which never waits for names. Documented shape, stays.

#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0601
#endif

#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>

#include <atomic>
#include <condition_variable>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>

namespace wintcp {

class DnsResolver {
public:
    struct Result {
        std::wstring address;    // as given to Offer()
        std::wstring hostname;   // empty = no PTR record / lookup failed
    };
    // Invoked on the worker thread; must not throw.
    using Sink = std::function<void(std::unique_ptr<Result>)>;

    DnsResolver() = default;
    ~DnsResolver();

    DnsResolver(const DnsResolver&) = delete;
    DnsResolver& operator=(const DnsResolver&) = delete;

    // Spawn the (idle until offers arrive) worker. False if already running.
    bool Start(Sink sink);

    // Stop and join the worker. Safe to call multiple times.
    void Stop();

    // Gate: when disabled, queued lookups wait instead of running.
    void SetEnabled(bool on);
    bool Enabled() const { return enabled_.load(); }

    // Queue 'addr' for resolution unless it is empty, a wildcard ("*"),
    // already resolved (cached), or already queued.
    void Offer(const std::wstring& addr);

    void ClearCache();

    // One blocking reverse-DNS lookup. Returns "" when there is no PTR record
    // or the query fails.
    //
    // Public because the command line needs it: a one-shot pass has no worker
    // to hand results to, and asking for `--resolve` should mean "wait for the
    // answers" rather than "print a frame of em-dashes and exit". The GUI does
    // NOT use this - it uses the worker above, so a slow query never stalls
    // painting. Both paths share the cache-free core (getnameinfo) so they
    // cannot disagree about what a given address resolves to.
    //
    // The trade-off documented at the top of this file applies: getnameinfo has
    // no timeout. A caller on the command line blocks here for as long as the
    // resolver takes, which is why --resolve is opt-in.
    static std::wstring Lookup(const std::wstring& addr);

private:
    void Run();

    std::thread thread_;
    std::mutex m_;
    std::condition_variable cv_;
    bool stop_ = false;
    std::atomic<bool> enabled_{false};
    std::deque<std::wstring> queue_;
    std::unordered_set<std::wstring> queued_;
    std::unordered_map<std::wstring, std::wstring> cache_;  // addr -> host ("" = none)
    Sink sink_;
};

}  // namespace wintcp
