#include "text_registry.hpp"

#include <Windows.h>

#include <limits>
#include <stdexcept>

namespace erui::detail {

TextId TextRegistry::add(std::wstring_view text) {
    if (frozen_) {
        throw std::logic_error("custom text registry is already frozen");
    }
    if (entries_.size() >=
        static_cast<std::size_t>((std::numeric_limits<TextId>::max)() - base_id)) {
        throw std::overflow_error("custom text ID range exhausted");
    }

    entries_.push_back(intern(text));
    return base_id + static_cast<TextId>(entries_.size() - 1);
}

namespace {
// Guards every registry's pool_ and storage_ (kept out of the class layout).
SRWLOCK g_pool_lock = SRWLOCK_INIT;
}

const wchar_t* TextRegistry::intern(std::wstring_view text) {
    auto* const lock = &g_pool_lock;
    AcquireSRWLockExclusive(lock);
    try {
        const wchar_t* result{};
        if (const auto found = pool_.find(text); found != pool_.end()) {
            result = found->second;
        } else {
            storage_.emplace_back(text);
            const std::wstring& stored = storage_.back();
            pool_.emplace(std::wstring_view(stored), stored.c_str());
            result = stored.c_str();
        }
        ReleaseSRWLockExclusive(lock);
        return result;
    } catch (...) {
        ReleaseSRWLockExclusive(lock);
        throw;
    }
}

bool TextRegistry::contains(std::wstring_view text) const noexcept {
    auto* const lock = &g_pool_lock;
    AcquireSRWLockShared(lock);
    bool found = false;
    try {
        found = pool_.find(text) != pool_.end();
    } catch (...) {
    }
    ReleaseSRWLockShared(lock);
    return found;
}

void TextRegistry::freeze() {
    if (frozen_) {
        return;
    }
    entries_.shrink_to_fit();
    frozen_ = true;
}

bool TextRegistry::replace(TextId id, std::wstring_view text) {
    if (id < base_id) return false;
    const auto index = static_cast<std::size_t>(id - base_id);
    if (index >= entries_.size()) return false;
    if (text == std::wstring_view(entries_[index])) return true;
    const wchar_t* const stored = intern(text);
    // A single aligned pointer store: concurrent lookups see old or new text.
    void* const slot = static_cast<void*>(&entries_[index]);
    InterlockedExchangePointer(static_cast<PVOID volatile*>(slot), const_cast<wchar_t*>(stored));
    return true;
}

const wchar_t* TextRegistry::lookup(int message_id) const noexcept {
    if (message_id < 0) {
        return nullptr;
    }
    const auto id = static_cast<TextId>(message_id);
    if (id < base_id) {
        return nullptr;
    }

    const auto index = static_cast<std::size_t>(id - base_id);
    if (index >= entries_.size()) {
        return nullptr;
    }
    return *static_cast<const wchar_t* const volatile*>(&entries_[index]);
}

} // namespace erui::detail
