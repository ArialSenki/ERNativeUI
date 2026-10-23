#pragma once

#include <ernativeui/erui.h>

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace erui::detail {

inline constexpr std::size_t scaleform_text_max_visible_units =
    ERUI_STYLED_TEXT_MAX_CODE_UNITS;

struct ScaleformTextStyleSpan {
    std::uint32_t flags{};
    std::uint32_t offset{};
    std::uint32_t length{};
    ERUI_Color color{};
};

struct ScaleformStyledText {
    std::wstring text{};
    std::vector<ScaleformTextStyleSpan> spans{};
};

// The largest possible representation of one accepted StyledText value:
// every visible UTF-16 unit expands from '&' to "&amp;" and every style span
// carries all four allowlisted wrappers. Keeping this bound explicit lets the
// native presentation store expose fixed-address buffers without truncation.
inline constexpr std::size_t scaleform_style_wrapper_max_units = 50;
inline constexpr std::size_t scaleform_html_max_units =
    scaleform_text_max_visible_units * 5u +
    static_cast<std::size_t>(ERUI_STYLED_TEXT_MAX_SPANS) *
        scaleform_style_wrapper_max_units;

// Copies and validates an ABI styled-text view. No caller-owned pointer is
// retained. Empty text is accepted only when allow_empty is true.
[[nodiscard]] ERUI_Result parse_scaleform_styled_text(
    const ERUI_StyledTextView& input,
    bool allow_empty,
    ScaleformStyledText& output) noexcept;

// Encodes literal text plus validated allowlisted spans as Scaleform HTML.
// The plain overload is used for initial row registration and deliberately
// treats all input as text rather than markup.
[[nodiscard]] ERUI_Result encode_scaleform_html(
    const ScaleformStyledText& input,
    std::wstring& output) noexcept;

[[nodiscard]] ERUI_Result encode_scaleform_html(
    std::wstring_view plain_text,
    std::wstring& output) noexcept;

} // namespace erui::detail
