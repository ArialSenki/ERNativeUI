#pragma once

// Memory reads that survive a bad game pointer: an access violation inside the
// copy is caught with SEH and reported as failure instead of crashing.

#include <Windows.h>

#include <cstddef>
#include <cstring>

namespace erui::detail {

inline bool guarded_copy(
    void* destination,
    const void* source,
    std::size_t size) noexcept {
    if (!destination || !source || size == 0) return false;
#if defined(_MSC_VER)
    __try {
#endif
        std::memcpy(destination, source, size);
        return true;
#if defined(_MSC_VER)
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
#endif
}

template <typename T>
bool guarded_read(const void* source, T& value) noexcept {
    return guarded_copy(&value, source, sizeof(value));
}

} // namespace erui::detail
