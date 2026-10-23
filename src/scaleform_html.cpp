#include "scaleform_html.hpp"

#include <algorithm>
#include <array>
#include <cstring>
#include <limits>
#include <new>

namespace erui::detail {
namespace {

constexpr std::uint32_t kKnownStyleFlags =
    ERUI_TEXT_STYLE_CUSTOM_COLOR |
    ERUI_TEXT_STYLE_BOLD |
    ERUI_TEXT_STYLE_ITALIC |
    ERUI_TEXT_STYLE_UNDERLINE;

[[nodiscard]] bool is_high_surrogate(std::uint16_t unit) noexcept {
    return unit >= 0xD800u && unit <= 0xDBFFu;
}

[[nodiscard]] bool is_low_surrogate(std::uint16_t unit) noexcept {
    return unit >= 0xDC00u && unit <= 0xDFFFu;
}

[[nodiscard]] bool valid_utf16(std::wstring_view text) noexcept {
    static_assert(sizeof(wchar_t) == sizeof(std::uint16_t));
    for (std::size_t index = 0; index < text.size(); ++index) {
        const auto unit = static_cast<std::uint16_t>(text[index]);
        if (unit == 0) return false;
        if (is_high_surrogate(unit)) {
            if (++index >= text.size() ||
                !is_low_surrogate(
                    static_cast<std::uint16_t>(text[index]))) {
                return false;
            }
        } else if (is_low_surrogate(unit)) {
            return false;
        }
    }
    return true;
}

[[nodiscard]] bool splits_surrogate_pair(
    std::wstring_view text,
    std::size_t boundary) noexcept {
    return boundary != 0 && boundary < text.size() &&
        is_high_surrogate(static_cast<std::uint16_t>(text[boundary - 1])) &&
        is_low_surrogate(static_cast<std::uint16_t>(text[boundary]));
}

void append_escaped_text(std::wstring_view text, std::wstring& output) {
    for (const wchar_t unit : text) {
        switch (unit) {
        case L'&': output.append(L"&amp;"); break;
        case L'<': output.append(L"&lt;"); break;
        case L'>': output.append(L"&gt;"); break;
        default: output.push_back(unit); break;
        }
    }
}

[[nodiscard]] wchar_t hex_digit(std::uint8_t value) noexcept {
    return value < 10 ? static_cast<wchar_t>(L'0' + value)
                      : static_cast<wchar_t>(L'A' + (value - 10));
}

void append_color_open(ERUI_Color color, std::wstring& output) {
    output.append(L"<font color=\"#");
    const std::array channels{color.red, color.green, color.blue};
    for (const std::uint8_t channel : channels) {
        output.push_back(hex_digit(static_cast<std::uint8_t>(channel >> 4u)));
        output.push_back(hex_digit(static_cast<std::uint8_t>(channel & 0x0Fu)));
    }
    output.append(L"\">");
}

[[nodiscard]] bool valid_internal_span(
    const ScaleformTextStyleSpan& span,
    std::wstring_view text,
    std::size_t previous_end) noexcept {
    if (span.flags == 0 || (span.flags & ~kKnownStyleFlags) != 0 ||
        span.length == 0 || span.offset < previous_end ||
        span.offset > text.size() || span.length > text.size() - span.offset) {
        return false;
    }
    const std::size_t end =
        static_cast<std::size_t>(span.offset) + span.length;
    if (splits_surrogate_pair(text, span.offset) ||
        splits_surrogate_pair(text, end) || span.color.reserved != 0) {
        return false;
    }
    if ((span.flags & ERUI_TEXT_STYLE_CUSTOM_COLOR) == 0 &&
        (span.color.red != 0 || span.color.green != 0 ||
         span.color.blue != 0)) {
        return false;
    }
    return true;
}

} // namespace

ERUI_Result parse_scaleform_styled_text(
    const ERUI_StyledTextView& input,
    bool allow_empty,
    ScaleformStyledText& output) noexcept {
    if (input.size != sizeof(input) || input.flags != 0 ||
        input.reserved[0] != 0 || input.reserved[1] != 0 ||
        input.text.reserved != 0 ||
        input.text.length > scaleform_text_max_visible_units ||
        input.span_count > ERUI_STYLED_TEXT_MAX_SPANS) {
        return ERUI_INVALID_ARGUMENT;
    }
    if (input.text.length == 0) {
        if (!allow_empty || input.text.data != nullptr) {
            return ERUI_INVALID_ARGUMENT;
        }
    } else if (!input.text.data) {
        return ERUI_INVALID_ARGUMENT;
    }
    if (input.span_count == 0) {
        if (input.spans != nullptr ||
            input.span_stride != sizeof(ERUI_TextStyleSpan)) {
            return ERUI_INVALID_ARGUMENT;
        }
    } else if (!input.spans ||
               input.span_stride != sizeof(ERUI_TextStyleSpan)) {
        return ERUI_INVALID_ARGUMENT;
    }

    try {
        ScaleformStyledText parsed{};
        parsed.text.resize(input.text.length);
        if (input.text.length != 0) {
            std::memcpy(
                parsed.text.data(),
                input.text.data,
                static_cast<std::size_t>(input.text.length) *
                    sizeof(std::uint16_t));
        }
        if (!valid_utf16(parsed.text)) return ERUI_INVALID_ARGUMENT;

        parsed.spans.reserve(input.span_count);
        const auto* const span_bytes =
            reinterpret_cast<const std::byte*>(input.spans);
        std::size_t previous_end = 0;
        for (std::uint32_t index = 0; index < input.span_count; ++index) {
            ERUI_TextStyleSpan wire{};
            std::memcpy(
                &wire,
                span_bytes + static_cast<std::size_t>(index) *
                    input.span_stride,
                sizeof(wire));
            if (wire.size != sizeof(wire) ||
                wire.reserved[0] != 0 || wire.reserved[1] != 0 ||
                wire.reserved[2] != 0) {
                return ERUI_INVALID_ARGUMENT;
            }
            ScaleformTextStyleSpan span{
                .flags = wire.flags,
                .offset = wire.offset,
                .length = wire.length,
                .color = wire.color,
            };
            if (!valid_internal_span(span, parsed.text, previous_end)) {
                return ERUI_INVALID_ARGUMENT;
            }
            previous_end =
                static_cast<std::size_t>(span.offset) + span.length;
            parsed.spans.push_back(span);
        }
        output = std::move(parsed);
        return ERUI_OK;
    } catch (const std::bad_alloc&) {
        return ERUI_OUT_OF_MEMORY;
    } catch (...) {
        return ERUI_INVALID_ARGUMENT;
    }
}

ERUI_Result encode_scaleform_html(
    const ScaleformStyledText& input,
    std::wstring& output) noexcept {
    if (input.text.size() > scaleform_text_max_visible_units ||
        input.spans.size() > ERUI_STYLED_TEXT_MAX_SPANS ||
        !valid_utf16(input.text)) {
        return ERUI_INVALID_ARGUMENT;
    }
    std::size_t previous_end = 0;
    for (const ScaleformTextStyleSpan& span : input.spans) {
        if (!valid_internal_span(span, input.text, previous_end)) {
            return ERUI_INVALID_ARGUMENT;
        }
        previous_end =
            static_cast<std::size_t>(span.offset) + span.length;
    }

    try {
        std::wstring encoded{};
        const std::size_t reserve_hint = input.text.size() >
                (std::numeric_limits<std::size_t>::max)() -
                    input.spans.size() * scaleform_style_wrapper_max_units
            ? scaleform_html_max_units
            : input.text.size() +
                input.spans.size() * scaleform_style_wrapper_max_units;
        encoded.reserve((std::min)(reserve_hint, scaleform_html_max_units));

        std::size_t cursor = 0;
        for (const ScaleformTextStyleSpan& span : input.spans) {
            append_escaped_text(
                std::wstring_view(input.text).substr(
                    cursor, static_cast<std::size_t>(span.offset) - cursor),
                encoded);
            if ((span.flags & ERUI_TEXT_STYLE_CUSTOM_COLOR) != 0) {
                append_color_open(span.color, encoded);
            }
            if ((span.flags & ERUI_TEXT_STYLE_BOLD) != 0) {
                encoded.append(L"<b>");
            }
            if ((span.flags & ERUI_TEXT_STYLE_ITALIC) != 0) {
                encoded.append(L"<i>");
            }
            if ((span.flags & ERUI_TEXT_STYLE_UNDERLINE) != 0) {
                encoded.append(L"<u>");
            }

            append_escaped_text(
                std::wstring_view(input.text).substr(
                    span.offset, span.length),
                encoded);

            if ((span.flags & ERUI_TEXT_STYLE_UNDERLINE) != 0) {
                encoded.append(L"</u>");
            }
            if ((span.flags & ERUI_TEXT_STYLE_ITALIC) != 0) {
                encoded.append(L"</i>");
            }
            if ((span.flags & ERUI_TEXT_STYLE_BOLD) != 0) {
                encoded.append(L"</b>");
            }
            if ((span.flags & ERUI_TEXT_STYLE_CUSTOM_COLOR) != 0) {
                encoded.append(L"</font>");
            }
            cursor = static_cast<std::size_t>(span.offset) + span.length;
        }
        append_escaped_text(
            std::wstring_view(input.text).substr(cursor), encoded);
        if (encoded.size() > scaleform_html_max_units) {
            return ERUI_INVALID_ARGUMENT;
        }
        output = std::move(encoded);
        return ERUI_OK;
    } catch (const std::bad_alloc&) {
        return ERUI_OUT_OF_MEMORY;
    } catch (...) {
        return ERUI_INVALID_ARGUMENT;
    }
}

ERUI_Result encode_scaleform_html(
    std::wstring_view plain_text,
    std::wstring& output) noexcept {
    try {
        ScaleformStyledText plain{};
        plain.text.assign(plain_text);
        return encode_scaleform_html(plain, output);
    } catch (const std::bad_alloc&) {
        return ERUI_OUT_OF_MEMORY;
    } catch (...) {
        return ERUI_INVALID_ARGUMENT;
    }
}

} // namespace erui::detail
