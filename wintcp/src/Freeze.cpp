// Freeze.cpp
// See Freeze.h.

#include "Freeze.h"

namespace wintcp {

std::uint64_t FrozenAgeMs(ULONGLONG frozenAt, ULONGLONG now) {
    if (now <= frozenAt) return 0;
    return static_cast<std::uint64_t>(now - frozenAt);
}

}  // namespace wintcp
