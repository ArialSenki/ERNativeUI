#pragma once

// Fork: process-lifetime cache of the movies the runtime GFX patch serves.
// Scaleform::MemoryFile does not own its buffer and the game may keep a
// MovieDef pointing into it, so entries are never removed and there is no
// size limit: the game opens each movie once (about 120 of them), plus one
// entry per HUD/menu layout variant. A full cache must never make a movie
// fall back to its unpatched bytes.

#include <Windows.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

namespace erui::host {

class MovieCache {
public:
    struct Lookup {
        bool found{};
        // Bytes to serve; nullptr when the movie is served unchanged.
        const std::vector<std::uint8_t>* output{};
    };

    [[nodiscard]] Lookup find(std::uint64_t hash, std::size_t size, std::uint64_t variant) const noexcept {
        AcquireSRWLockShared(&lock_);
        const Lookup result = find_locked(hash, size, variant);
        ReleaseSRWLockShared(&lock_);
        return result;
    }

    // Stores a result (patched == false: serve the original) and returns the
    // stable buffer to serve. If another thread stored the same key first,
    // its entry wins so a key never has two buffers.
    const std::vector<std::uint8_t>* insert(
        std::uint64_t hash,
        std::size_t size,
        std::uint64_t variant,
        std::vector<std::uint8_t> output,
        bool patched) {
        auto entry = std::make_unique<Entry>();
        entry->hash = hash;
        entry->size = size;
        entry->variant = variant;
        entry->patched = patched;
        if (patched) entry->output = std::move(output);
        AcquireSRWLockExclusive(&lock_);
        const Lookup existing = find_locked(hash, size, variant);
        const std::vector<std::uint8_t>* result = existing.output;
        if (!existing.found) {
            try {
                entries_.push_back(std::move(entry));
                result = entries_.back()->patched ? &entries_.back()->output : nullptr;
            } catch (...) {
                ReleaseSRWLockExclusive(&lock_);
                throw;
            }
        }
        ReleaseSRWLockExclusive(&lock_);
        return result;
    }

    // Keeps bytes alive for the process (original bytes read from a
    // non-MemoryFile that are served through the wrapper file). A movie
    // opened again with the same bytes reuses the kept copy.
    const std::vector<std::uint8_t>* keep_alive(std::uint64_t hash, std::vector<std::uint8_t> bytes) {
        auto entry = std::make_unique<Entry>();
        entry->hash = hash;
        entry->size = bytes.size();
        entry->output = std::move(bytes);
        const std::vector<std::uint8_t>* result = &entry->output;
        AcquireSRWLockExclusive(&lock_);
        try {
            const auto existing = std::find_if(kept_.begin(), kept_.end(), [&](const auto& kept) {
                return kept->hash == hash && kept->output == entry->output;
            });
            if (existing != kept_.end()) {
                result = &(*existing)->output;
            } else {
                kept_.push_back(std::move(entry));
            }
        } catch (...) {
            ReleaseSRWLockExclusive(&lock_);
            throw;
        }
        ReleaseSRWLockExclusive(&lock_);
        return result;
    }

    [[nodiscard]] std::size_t size() const noexcept {
        AcquireSRWLockShared(&lock_);
        const std::size_t count = entries_.size();
        ReleaseSRWLockShared(&lock_);
        return count;
    }

private:
    struct Entry {
        std::uint64_t hash{};
        std::size_t size{};
        std::uint64_t variant{};  // HUD/menu layout key; 0 for the settings patch
        std::vector<std::uint8_t> output{};
        bool patched{};
    };

    Lookup find_locked(std::uint64_t hash, std::size_t size, std::uint64_t variant) const noexcept {
        for (const auto& entry : entries_) {
            if (entry->hash == hash && entry->size == size && entry->variant == variant) {
                return {true, entry->patched ? &entry->output : nullptr};
            }
        }
        return {};
    }

    mutable SRWLOCK lock_ = SRWLOCK_INIT;
    std::vector<std::unique_ptr<Entry>> entries_{};
    std::vector<std::unique_ptr<Entry>> kept_{};
};

} // namespace erui::host
