// Utils.cpp
// Implementations for Utils.h.

#include "Utils.h"

#include <cstdio>
#include <cwchar>
#include <cwctype>
#include <fstream>

namespace wintcp {

namespace {

// fe80::/10 — the only IPv6 range where a numeric scope id carries meaning
// ("which interface"), and so the only range that prints one. Encoded as a
// first-byte equality plus a top-two-bits mask, matching the test TcpTable's
// PrintIpv6 already used; see Ipv6ScopeSuffix (Utils.h) for why this is one
// function rather than a per-file check.
constexpr unsigned char kLinkLocalFirstByte = 0xFE;
constexpr unsigned char kLinkLocalSecondMask = 0xC0;    // top two bits
constexpr unsigned char kLinkLocalSecondValue = 0x80;   // must be 10xxxxxx

}  // namespace

std::wstring Ipv6ScopeSuffix(const unsigned char raw[16], unsigned scopeId) {
    if (raw == nullptr || scopeId == 0) return std::wstring();
    if (raw[0] != kLinkLocalFirstByte ||
        (raw[1] & kLinkLocalSecondMask) != kLinkLocalSecondValue)
        return std::wstring();
    wchar_t scope[16] = {0};
    ::swprintf_s(scope, L"%%%u", scopeId);
    return std::wstring(scope);
}

std::wstring FormatSystemError(DWORD errorCode) {
    LPWSTR buffer = nullptr;
    DWORD flags = FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM |
                  FORMAT_MESSAGE_IGNORE_INSERTS;
    DWORD chars = ::FormatMessageW(flags, nullptr, errorCode,
                                   MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
                                   reinterpret_cast<LPWSTR>(&buffer), 0, nullptr);
    std::wstring msg;
    if (chars != 0 && buffer != nullptr) {
        msg.assign(buffer, chars);
        // Trim trailing CR/LF/space added by FormatMessage.
        while (!msg.empty() && (msg.back() == L'\r' || msg.back() == L'\n' ||
                                msg.back() == L' ' || msg.back() == L'\t' || msg.back() == L'.')) {
            // Keep one trailing period semantics simple: just trim CR/LF/whitespace.
            if (msg.back() == L'.') break;
            msg.pop_back();
        }
    } else {
        wchar_t fallback[64] = {0};
        ::swprintf_s(fallback, L"Unknown error %lu (0x%08lX)",
                     static_cast<unsigned long>(errorCode),
                     static_cast<unsigned long>(errorCode));
        msg = fallback;
    }
    if (buffer != nullptr) {
        ::LocalFree(buffer);
    }
    return msg;
}

std::wstring FormatCurrentTime() {
    SYSTEMTIME st = {};
    ::GetLocalTime(&st);
    wchar_t buf[64] = {0};
    ::swprintf_s(buf, L"%04u-%02u-%02u %02u:%02u:%02u",
                 static_cast<unsigned>(st.wYear), static_cast<unsigned>(st.wMonth),
                 static_cast<unsigned>(st.wDay), static_cast<unsigned>(st.wHour),
                 static_cast<unsigned>(st.wMinute), static_cast<unsigned>(st.wSecond));
    return std::wstring(buf);
}

std::wstring ToLowerW(const std::wstring& s) {
    std::wstring out(s);
    for (wchar_t& ch : out) {
        ch = static_cast<wchar_t>(::towlower(static_cast<wint_t>(ch)));
    }
    return out;
}

bool ContainsCaseInsensitive(const std::wstring& haystack, const std::wstring& needle) {
    if (needle.empty()) return true;
    if (needle.size() > haystack.size()) return false;
    const std::wstring h = ToLowerW(haystack);
    const std::wstring n = ToLowerW(needle);
    return h.find(n) != std::wstring::npos;
}

std::string WideToUtf8(const std::wstring& s) {
    if (s.empty()) return std::string();
    int needed = ::WideCharToMultiByte(CP_UTF8, 0, s.c_str(), -1, nullptr, 0, nullptr, nullptr);
    if (needed <= 0) return std::string();
    // 'needed' includes the terminating NUL.
    std::string out(static_cast<size_t>(needed) - 1, '\0');
    int written = ::WideCharToMultiByte(CP_UTF8, 0, s.c_str(), -1, out.data(), needed,
                                        nullptr, nullptr);
    if (written <= 0) return std::string();
    // written includes NUL; out already excludes it.
    if (static_cast<size_t>(written - 1) != out.size()) {
        out.resize(static_cast<size_t>(written - 1));
    }
    return out;
}

std::wstring Utf8ToWide(const char* s) {
    if (s == nullptr || *s == '\0') return std::wstring();
    const int needed = ::MultiByteToWideChar(CP_UTF8, 0, s, -1, nullptr, 0);
    if (needed <= 1) return std::wstring();
    std::wstring out(static_cast<size_t>(needed - 1), L'\0');
    if (::MultiByteToWideChar(CP_UTF8, 0, s, -1, out.data(), needed) <= 0)
        return std::wstring();
    return out;
}

std::wstring WriteUtf8FileWithBom(const std::wstring& path,
                                  const std::string& utf8Content,
                                  bool withBom) {
    HANDLE h = ::CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                             FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        return FormatSystemError(::GetLastError());
    }
    const unsigned char bom[3] = {0xEF, 0xBB, 0xBF};
    DWORD written = 0;
    BOOL ok = TRUE;
    if (withBom) {
        ok = ::WriteFile(h, bom, sizeof(bom), &written, nullptr);
        if (!ok) {
            std::wstring err = FormatSystemError(::GetLastError());
            ::CloseHandle(h);
            return err;
        }
    }
    if (!utf8Content.empty()) {
        size_t offset = 0;
        while (offset < utf8Content.size()) {
            DWORD chunk = static_cast<DWORD>(utf8Content.size() - offset);
            DWORD w = 0;
            ok = ::WriteFile(h, utf8Content.data() + offset, chunk, &w, nullptr);
            if (!ok) {
                std::wstring err = FormatSystemError(::GetLastError());
                ::CloseHandle(h);
                return err;
            }
            if (w == 0) break;
            offset += w;
        }
    }
    ::CloseHandle(h);
    return std::wstring();  // success
}

std::string CsvEscapeUtf8(const std::string& field) {
    bool needQuote = field.find_first_of(",\"\r\n") != std::string::npos;
    if (!needQuote) return field;
    std::string out;
    out.reserve(field.size() + 2);
    out.push_back('"');
    for (char c : field) {
        if (c == '"') out += "\"\"";
        else out.push_back(c);
    }
    out.push_back('"');
    return out;
}

std::wstring FormatPort(UINT port) {
    wchar_t buf[16] = {0};
    ::swprintf_s(buf, L"%u", port);
    return std::wstring(buf);
}

std::wstring JoinEndpoint(const std::wstring& addressLiteral, UINT port, bool isIpv6) {
    std::wstring out;
    if (isIpv6) {
        out.push_back(L'[');
        out += addressLiteral;
        out.push_back(L']');
    } else {
        out += addressLiteral;
    }
    out.push_back(L':');
    out += FormatPort(port);
    return out;
}

std::wstring FormatBytes(ULONGLONG bytes) {
    const double b = static_cast<double>(bytes);
    wchar_t buf[32] = {0};
    if (b < kBytesPerKB) {
        ::swprintf_s(buf, L"%llu B", static_cast<unsigned long long>(bytes));
    } else if (b < kBytesPerMB) {
        ::swprintf_s(buf, L"%.1f KB", b / kBytesPerKB);
    } else if (b < kBytesPerGB) {
        ::swprintf_s(buf, L"%.1f MB", b / kBytesPerMB);
    } else if (b < kBytesPerTB) {
        ::swprintf_s(buf, L"%.2f GB", b / kBytesPerGB);
    } else {
        ::swprintf_s(buf, L"%.2f TB", b / kBytesPerTB);
    }
    return std::wstring(buf);
}

bool HasLowerSubstring(const std::wstring& haystackLower,
                       const std::wstring& needleLower) {
    if (needleLower.empty()) return true;
    return haystackLower.find(needleLower) != std::wstring::npos;
}

UINT QueryDpiForWindow(HWND hwnd) {
    if (hwnd != nullptr) {
        // GetDpiForWindow: Win10 1607+; resolve dynamically to keep the
        // Win7 target (_WIN32_WINNT 0x0601) link-compatible.
        using GetDpiForWindow_t = UINT(WINAPI*)(HWND);
        static const GetDpiForWindow_t getDpiForWindow =
            reinterpret_cast<GetDpiForWindow_t>(::GetProcAddress(
                ::GetModuleHandleW(L"user32.dll"), "GetDpiForWindow"));
        if (getDpiForWindow != nullptr) {
            const UINT dpi = getDpiForWindow(hwnd);
            if (dpi != 0) return dpi;
        }
    }
    HDC hdc = ::GetDC(nullptr);
    UINT dpi = 96;
    if (hdc != nullptr) {
        const int caps = ::GetDeviceCaps(hdc, LOGPIXELSX);
        if (caps > 0) dpi = static_cast<UINT>(caps);
        ::ReleaseDC(nullptr, hdc);
    }
    return (dpi == 0) ? 96 : dpi;
}

}  // namespace wintcp
