#pragma once

#include "menu.hpp"

#include <cstddef>
#include <deque>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace erui::detail {

class TextRegistry {
public:
    static constexpr TextId base_id = 0x0F2000;

    TextId add(std::wstring_view text);
    void freeze();

    // Fork: swaps the text of an existing ID, also after freeze(). The old
    // string stays allocated because the game may still hold its pointer.
    // Callers serialize replace(); lookup() may run concurrently.
    bool replace(TextId id, std::wstring_view text);

    [[nodiscard]] const wchar_t* lookup(int message_id) const noexcept;
    // Fork: true when text equals any text ever registered (current or
    // replaced). Safe to call concurrently with replace().
    [[nodiscard]] bool contains(std::wstring_view text) const noexcept;
    [[nodiscard]] std::size_t size() const noexcept { return entries_.size(); }
    [[nodiscard]] bool frozen() const noexcept { return frozen_; }

private:
    const wchar_t* intern(std::wstring_view text);

    std::deque<std::wstring> storage_{};      // stable addresses, never shrinks
    std::vector<const wchar_t*> entries_{};   // current text of each ID
    // Fork: every stored text by content, so switching languages back and
    // forth reuses the same strings instead of growing storage_ each time.
    std::unordered_map<std::wstring_view, const wchar_t*> pool_{};
    bool frozen_{false};
};

} // namespace erui::detail
