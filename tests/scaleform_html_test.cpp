#include "scaleform_html.hpp"

#include <array>
#ifdef NDEBUG
// This target is exercised from the Release preset; keep its regression
// assertions active in the same configuration used for production binaries.
#undef NDEBUG
#endif
#include <cassert>
#include <cstdint>
#include <string>
#include <string_view>

namespace {

ERUI_Utf16View utf16(std::wstring_view value) {
    static_assert(sizeof(wchar_t) == sizeof(std::uint16_t));
    return {
        value.empty()
            ? nullptr
            : reinterpret_cast<const std::uint16_t*>(value.data()),
        static_cast<std::uint32_t>(value.size()),
        0,
    };
}

ERUI_TextStyleSpan span(
    std::uint32_t offset,
    std::uint32_t length,
    ERUI_TextStyleFlags flags,
    ERUI_Color color = {}) {
    ERUI_TextStyleSpan result{};
    result.size = sizeof(result);
    result.flags = flags;
    result.offset = offset;
    result.length = length;
    result.color = color;
    return result;
}

ERUI_StyledTextView styled(
    std::wstring_view text,
    const ERUI_TextStyleSpan* spans = nullptr,
    std::uint32_t count = 0) {
    ERUI_StyledTextView result{};
    result.size = sizeof(result);
    result.text = utf16(text);
    result.spans = spans;
    result.span_count = count;
    result.span_stride = sizeof(ERUI_TextStyleSpan);
    return result;
}

void verify_plain_text_is_literal() {
    std::wstring encoded{};
    assert(erui::detail::encode_scaleform_html(
        L"A < B & C > D \"quoted\"", encoded) == ERUI_OK);
    assert(encoded == L"A &lt; B &amp; C &gt; D \"quoted\"");

    ERUI_StyledTextView view = styled(L"<b>not markup</b> & text");
    erui::detail::ScaleformStyledText parsed{};
    assert(erui::detail::parse_scaleform_styled_text(
        view, false, parsed) == ERUI_OK);
    assert(erui::detail::encode_scaleform_html(parsed, encoded) == ERUI_OK);
    assert(encoded ==
        L"&lt;b&gt;not markup&lt;/b&gt; &amp; text");
}

void verify_allowlisted_markup_is_deterministic() {
    constexpr ERUI_Color kGold{0x6B, 0x64, 0x50, 0};
    const std::array spans{
        span(0, 6,
            ERUI_TEXT_STYLE_CUSTOM_COLOR |
            ERUI_TEXT_STYLE_BOLD |
            ERUI_TEXT_STYLE_ITALIC |
            ERUI_TEXT_STYLE_UNDERLINE,
            kGold),
        span(9, 3, ERUI_TEXT_STYLE_UNDERLINE),
    };
    ERUI_StyledTextView view = styled(
        L"A&B<C> / end", spans.data(),
        static_cast<std::uint32_t>(spans.size()));
    erui::detail::ScaleformStyledText parsed{};
    assert(erui::detail::parse_scaleform_styled_text(
        view, false, parsed) == ERUI_OK);

    std::wstring encoded{};
    assert(erui::detail::encode_scaleform_html(parsed, encoded) == ERUI_OK);
    assert(encoded ==
        L"<font color=\"#6B6450\"><b><i><u>"
        L"A&amp;B&lt;C&gt;"
        L"</u></i></b></font> / <u>end</u>");
}

void verify_unicode_boundaries() {
    const std::wstring text = L"A\U0001F600Z";
    assert(text.size() == 4);
    const ERUI_TextStyleSpan whole_emoji = span(
        1, 2, ERUI_TEXT_STYLE_UNDERLINE);
    ERUI_StyledTextView view = styled(text, &whole_emoji, 1);
    erui::detail::ScaleformStyledText parsed{};
    assert(erui::detail::parse_scaleform_styled_text(
        view, false, parsed) == ERUI_OK);
    std::wstring encoded{};
    assert(erui::detail::encode_scaleform_html(parsed, encoded) == ERUI_OK);
    assert(encoded == L"A<u>\U0001F600</u>Z");

    const ERUI_TextStyleSpan split_start = span(
        2, 1, ERUI_TEXT_STYLE_UNDERLINE);
    view = styled(text, &split_start, 1);
    assert(erui::detail::parse_scaleform_styled_text(
        view, false, parsed) == ERUI_INVALID_ARGUMENT);

    const ERUI_TextStyleSpan split_end = span(
        1, 1, ERUI_TEXT_STYLE_UNDERLINE);
    view = styled(text, &split_end, 1);
    assert(erui::detail::parse_scaleform_styled_text(
        view, false, parsed) == ERUI_INVALID_ARGUMENT);
}

void verify_invalid_views_are_rejected_atomically() {
    erui::detail::ScaleformStyledText output{};
    output.text = L"sentinel";

    ERUI_TextStyleSpan candidate = span(
        0, 1, ERUI_TEXT_STYLE_UNDERLINE);
    ERUI_StyledTextView view = styled(L"abc", &candidate, 1);

    candidate.flags |= 1u << 31;
    assert(erui::detail::parse_scaleform_styled_text(
        view, false, output) == ERUI_INVALID_ARGUMENT);
    assert(output.text == L"sentinel");

    candidate = span(0, 1, ERUI_TEXT_STYLE_BOLD);
    candidate.color.red = 1;
    assert(erui::detail::parse_scaleform_styled_text(
        view, false, output) == ERUI_INVALID_ARGUMENT);

    candidate = span(0, 1, ERUI_TEXT_STYLE_CUSTOM_COLOR, {1, 2, 3, 1});
    assert(erui::detail::parse_scaleform_styled_text(
        view, false, output) == ERUI_INVALID_ARGUMENT);

    candidate = span(2, 2, ERUI_TEXT_STYLE_UNDERLINE);
    assert(erui::detail::parse_scaleform_styled_text(
        view, false, output) == ERUI_INVALID_ARGUMENT);

    candidate = span(0, 0, ERUI_TEXT_STYLE_UNDERLINE);
    assert(erui::detail::parse_scaleform_styled_text(
        view, false, output) == ERUI_INVALID_ARGUMENT);

    candidate = span(0, 1, 0);
    assert(erui::detail::parse_scaleform_styled_text(
        view, false, output) == ERUI_INVALID_ARGUMENT);

    const std::array overlapping{
        span(0, 2, ERUI_TEXT_STYLE_BOLD),
        span(1, 1, ERUI_TEXT_STYLE_ITALIC),
    };
    view = styled(L"abc", overlapping.data(), 2);
    assert(erui::detail::parse_scaleform_styled_text(
        view, false, output) == ERUI_INVALID_ARGUMENT);

    view = styled(L"abc");
    view.spans = &candidate;
    assert(erui::detail::parse_scaleform_styled_text(
        view, false, output) == ERUI_INVALID_ARGUMENT);

    view = styled(L"abc");
    view.span_stride = 0;
    assert(erui::detail::parse_scaleform_styled_text(
        view, false, output) == ERUI_INVALID_ARGUMENT);

    view = styled(L"abc", &candidate, 1);
    view.span_stride = sizeof(candidate) + 1;
    assert(erui::detail::parse_scaleform_styled_text(
        view, false, output) == ERUI_INVALID_ARGUMENT);

    view = styled(L"abc", &candidate, 1);
    view.reserved[0] = 1;
    assert(erui::detail::parse_scaleform_styled_text(
        view, false, output) == ERUI_INVALID_ARGUMENT);

    view = styled(L"");
    assert(erui::detail::parse_scaleform_styled_text(
        view, false, output) == ERUI_INVALID_ARGUMENT);
    assert(erui::detail::parse_scaleform_styled_text(
        view, true, output) == ERUI_OK);
    assert(output.text.empty());
}

} // namespace

int main() {
    verify_plain_text_is_literal();
    verify_allowlisted_markup_is_deterministic();
    verify_unicode_boundaries();
    verify_invalid_views_are_rejected_atomically();
    return 0;
}
