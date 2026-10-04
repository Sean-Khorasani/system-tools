// DnsResolver.cpp
// Worker thread: pop an address, getnameinfo(NI_NAMEREQD), cache, deliver.

#include "DnsResolver.h"
#include "Utils.h"   // RunGuarded (R2 worker containment)

namespace wintcp {
namespace {

// getnameinfo needs a sockaddr; parse the literal with InetPton. Link-local
// addresses may carry a "%scope" suffix that InetPton rejects - strip it
// (the scope is meaningless for a reverse lookup anyway).
std::wstring StripScope(const std::wstring& addr) {
    const size_t pct = addr.find(L'%');
    return (pct == std::wstring::npos) ? addr : addr.substr(0, pct);
}

}  // namespace

DnsResolver::~DnsResolver() {
    Stop();
}

bool DnsResolver::Start(Sink sink) {
    if (thread_.joinable()) return false;
    {
        std::lock_guard<std::mutex> lk(m_);
        stop_ = false;
        sink_ = std::move(sink);
    }
    thread_ = std::thread([this] { Run(); });
    return true;
}

void DnsResolver::Stop() {
    {
        std::lock_guard<std::mutex> lk(m_);
        stop_ = true;
    }
    cv_.notify_all();
    if (thread_.joinable()) thread_.join();
}

void DnsResolver::SetEnabled(bool on) {
    enabled_.store(on);
    cv_.notify_all();
}

void DnsResolver::Offer(const std::wstring& addr) {
    if (addr.empty() || addr == L"*") return;
    const std::wstring bare = StripScope(addr);
    if (bare.empty()) return;
    bool notify = false;
    {
        std::lock_guard<std::mutex> lk(m_);
        if (cache_.find(bare) != cache_.end()) return;
        if (queued_.find(bare) != queued_.end()) return;
        queue_.push_back(bare);
        queued_.insert(bare);
        notify = true;
    }
    if (notify) cv_.notify_all();
}

void DnsResolver::ClearCache() {
    std::lock_guard<std::mutex> lk(m_);
    cache_.clear();
}

std::wstring DnsResolver::Lookup(const std::wstring& addr) {
    const std::wstring bare = StripScope(addr);
    const bool v6 = bare.find(L':') != std::wstring::npos;

    sockaddr_storage ss = {};
    int sockLen = 0;
    if (v6) {
        auto* v4 = reinterpret_cast<sockaddr_in6*>(&ss);
        v4->sin6_family = AF_INET6;
        if (::InetPtonW(AF_INET6, bare.c_str(), &v4->sin6_addr) != 1)
            return std::wstring();
        sockLen = sizeof(sockaddr_in6);
    } else {
        auto* v4 = reinterpret_cast<sockaddr_in*>(&ss);
        v4->sin_family = AF_INET;
        if (::InetPtonW(AF_INET, bare.c_str(), &v4->sin_addr) != 1)
            return std::wstring();
        sockLen = sizeof(sockaddr_in);
    }

    // One constant for the whole GetNameInfoW buffer trio: the array, the
    // count argument, and the terminator index. 1025 wide chars is
    // NI_MAXHOST (1024) plus the NUL; the three spellings below used to be
    // three separate literals, so changing the size meant finding all of them.
    // The count expression is preserved exactly as it was
    // (1025/sizeof(wchar_t)-1 = 511): it reads oddly next to a 1025-char
    // buffer, but a DNS name is at most 253 chars, so 511 already truncates
    // nothing real. Widening it would be a behaviour change, not a rename.
    constexpr size_t kHostBufChars = 1025;
    wchar_t host[kHostBufChars] = {0};
    // NI_NAMEREQD: fail (-> negative cache) instead of echoing the numeric
    // address back when no PTR record exists. (SDK spells the wide entry
    // point GetNameInfoW.)
    if (::GetNameInfoW(reinterpret_cast<const sockaddr*>(&ss), sockLen, host,
                       static_cast<DWORD>(kHostBufChars / sizeof(wchar_t)) - 1,
                       nullptr, 0, NI_NAMEREQD) != 0)
        return std::wstring();
    host[kHostBufChars - 1] = L'\0';
    return std::wstring(host);
}

void DnsResolver::Run() {
    for (;;) {
        std::wstring addr;
        Sink sink;
        {
            std::unique_lock<std::mutex> lk(m_);
            cv_.wait(lk, [this] {
                return stop_ || (enabled_.load() && !queue_.empty());
            });
            if (stop_) return;
            addr = queue_.front();
            queue_.pop_front();
            queued_.erase(addr);
            sink = sink_;
        }

        // R2: a throwing lookup resolves to empty — the same answer a failed
        // lookup gives, and the cache records it either way. A name we could
        // not compute is not worth a dead resolver thread.
        std::wstring host;
        if (!RunGuarded([&] { host = Lookup(addr); }).empty()) host.clear();
        auto result = std::make_unique<Result>();
        result->address = addr;
        result->hostname = host;

        {
            std::lock_guard<std::mutex> lk(m_);
            cache_[addr] = host;                   // negative results too
        }
        // A throwing sink drops one answer; the address stays cached as
        // unknown and the next refresh re-requests it if still visible.
        if (sink) (void)RunGuarded([&] { sink(std::move(result)); });
        // Loop: predicate re-checks stop_/queue_/enabled_.
    }
}

}  // namespace wintcp
