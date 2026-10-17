#include "settings_movie.hpp"

#include <algorithm>
#include <cctype>
#include <string_view>
#include <vector>

namespace erui::gfx {
namespace {

bool equals_ignore_case(std::string_view a, std::string_view b) noexcept {
    if (a.size() != b.size()) return false;
    for (std::size_t index = 0; index < a.size(); ++index) {
        if (std::tolower(static_cast<unsigned char>(a[index])) !=
            std::tolower(static_cast<unsigned char>(b[index]))) {
            return false;
        }
    }
    return true;
}

// --- Backdrop alpha ---------------------------------------------------------

constexpr std::uint16_t kTagEnd = 0;
constexpr std::uint16_t kTagShowFrame = 1;
constexpr std::uint16_t kTagPlaceObject2 = 26;
constexpr std::uint16_t kTagDefineSprite = 39;
constexpr std::uint16_t kTagPlaceObject3 = 70;
constexpr std::uint16_t kTagDefineExternalImage2 = 1009;
constexpr std::string_view kBackdropImage = "MENU_BaseMain_01";

struct Tag {
    std::uint16_t code{};
    std::size_t start{};
    std::size_t body{};
    std::size_t end{};
    bool long_header{};
};

std::uint16_t read_u16(std::span<const std::uint8_t> data, std::size_t at) noexcept {
    return static_cast<std::uint16_t>(data[at] | (data[at + 1] << 8));
}

std::uint32_t read_u32(std::span<const std::uint8_t> data, std::size_t at) noexcept {
    return read_u16(data, at) | (static_cast<std::uint32_t>(read_u16(data, at + 2)) << 16);
}

bool read_tag(std::span<const std::uint8_t> data, std::size_t at, std::size_t limit,
    Tag& tag) noexcept {
    if (at + 2 > limit) return false;
    const std::uint16_t header = read_u16(data, at);
    std::size_t length = header & 0x3F;
    std::size_t body = at + 2;
    if (length == 0x3F) {
        if (body + 4 > limit) return false;
        length = read_u32(data, body);
        body += 4;
    }
    if (length > limit - body) return false;
    tag = {static_cast<std::uint16_t>(header >> 6), at, body, body + length, body - at == 6};
    return true;
}

// Keeps the source tag's header form: the exporter writes long headers even
// for short tags, and the unchanged case must reproduce the input bytes.
void append_tag(std::vector<std::uint8_t>& out, std::uint16_t code, bool long_header,
    const std::vector<std::uint8_t>& body) {
    if (!long_header && body.size() < 0x3F) {
        const auto header = static_cast<std::uint16_t>((code << 6) | body.size());
        out.push_back(static_cast<std::uint8_t>(header));
        out.push_back(static_cast<std::uint8_t>(header >> 8));
    } else {
        const auto header = static_cast<std::uint16_t>((code << 6) | 0x3F);
        out.push_back(static_cast<std::uint8_t>(header));
        out.push_back(static_cast<std::uint8_t>(header >> 8));
        const auto length = static_cast<std::uint32_t>(body.size());
        for (int shift = 0; shift < 32; shift += 8) {
            out.push_back(static_cast<std::uint8_t>(length >> shift));
        }
    }
    out.insert(out.end(), body.begin(), body.end());
}

struct BitReader {
    std::span<const std::uint8_t> data;
    std::size_t bit{};
    std::size_t limit_bits{};
    bool ok{true};

    std::uint32_t take(unsigned count) noexcept {
        std::uint32_t value = 0;
        for (unsigned index = 0; index < count; ++index) {
            if (bit >= limit_bits) {
                ok = false;
                return 0;
            }
            value = (value << 1) | ((data[bit / 8] >> (7 - bit % 8)) & 1u);
            ++bit;
        }
        return value;
    }
    std::size_t byte_end() const noexcept { return (bit + 7) / 8; }
};

// Returns the byte length of a MATRIX record, or 0 if it overruns.
std::size_t matrix_bytes(std::span<const std::uint8_t> data, std::size_t at,
    std::size_t limit) noexcept {
    BitReader bits{data, at * 8, limit * 8};
    if (bits.take(1)) bits.take(bits.take(5) * 2);
    if (bits.take(1)) bits.take(bits.take(5) * 2);
    bits.take(bits.take(5) * 2);
    return bits.ok ? bits.byte_end() - at : 0;
}

// Returns the byte length of a CXFORMWITHALPHA record, or 0 if it overruns.
std::size_t cxform_bytes(std::span<const std::uint8_t> data, std::size_t at,
    std::size_t limit) noexcept {
    BitReader bits{data, at * 8, limit * 8};
    const bool has_add = bits.take(1) != 0;
    const bool has_mult = bits.take(1) != 0;
    const unsigned count = bits.take(4);
    if (has_mult) bits.take(count * 4);
    if (has_add) bits.take(count * 4);
    return bits.ok ? bits.byte_end() - at : 0;
}

// CXFORMWITHALPHA with only multiply terms: RGB x1.0, alpha x(alpha/256).
void append_alpha_cxform(std::vector<std::uint8_t>& out, unsigned alpha) {
    constexpr unsigned kBits = 10;
    std::uint64_t value = 0;
    unsigned used = 0;
    const auto put = [&](std::uint32_t field, unsigned count) {
        value = (value << count) | (field & ((1u << count) - 1));
        used += count;
    };
    put(0, 1);  // HasAddTerms
    put(1, 1);  // HasMultTerms
    put(kBits, 4);
    put(256, kBits);
    put(256, kBits);
    put(256, kBits);
    put(alpha, kBits);
    const unsigned padded = (used + 7) / 8 * 8;
    value <<= padded - used;
    for (int shift = static_cast<int>(padded) - 8; shift >= 0; shift -= 8) {
        out.push_back(static_cast<std::uint8_t>(value >> shift));
    }
}

PatchResult backdrop_failure(const char* message) {
    PatchResult failed{};
    failed.error = ErrorCode::unsupported_gfx;
    failed.message = message;
    return failed;
}

} // namespace

SettingsMovie classify_settings_movie(std::string_view url) noexcept {
    const std::size_t slash = url.find_last_of("/\\:");
    const std::string_view name =
        slash == std::string_view::npos ? url : url.substr(slash + 1);
    if (equals_ignore_case(name, "02_040_optionsetting.gfx")) {
        return SettingsMovie::game_options;
    }
    if (equals_ignore_case(name, "02_042_pc_graphicsetting.gfx")) {
        return SettingsMovie::advanced_settings;
    }
    return SettingsMovie::none;
}

const char* settings_movie_name(SettingsMovie movie) noexcept {
    switch (movie) {
    case SettingsMovie::game_options: return "02_040_optionsetting.gfx";
    case SettingsMovie::advanced_settings: return "02_042_pc_graphicsetting.gfx";
    default: return "(not a settings movie)";
    }
}

PatchResult patch_settings_movie_widgets(
    SettingsMovie movie,
    std::span<const std::uint8_t> input,
    const SettingsMovieOptions& options) noexcept;

PatchResult set_menu_background_alpha(
    std::span<const std::uint8_t> input,
    std::uint8_t alpha_percent) noexcept {
    try {
        if (input.size() < 21 || input[1] != 'F' ||
            !((input[0] == 'G' && input[2] == 'X') || (input[0] == 'F' && input[2] == 'S'))) {
            return backdrop_failure("not an uncompressed GFX movie");
        }
        // Shipped movies carry zero padding after the declared length; it is
        // kept as is and the declared length moves with the edit.
        const std::uint32_t declared = read_u32(input, 4);
        if (declared < 21 || declared > input.size() ||
            std::any_of(input.begin() + declared, input.end(),
                [](std::uint8_t byte) { return byte != 0; })) {
            return backdrop_failure("declared GFX length is invalid");
        }
        const std::span<const std::uint8_t> movie_bytes = input.first(declared);
        const std::size_t rect_bytes = (5 + (input[8] >> 3) * 4 + 7) / 8;
        std::size_t position = 8 + rect_bytes + 4;

        // Root definitions and the first character placed at root depth 1.
        std::uint16_t backdrop_image{};
        bool have_image = false;
        std::vector<Tag> sprites{};
        std::uint16_t wrapper_id{};
        bool have_wrapper = false;
        Tag tag{};
        while (read_tag(movie_bytes, position, movie_bytes.size(), tag) && tag.code != kTagEnd) {
            if (tag.code == kTagDefineExternalImage2 && tag.end - tag.body >= 11) {
                const std::size_t name_length = input[tag.body + 10];
                if (tag.body + 11 + name_length <= tag.end &&
                    equals_ignore_case(std::string_view(
                        reinterpret_cast<const char*>(input.data() + tag.body + 11),
                        name_length), kBackdropImage)) {
                    backdrop_image = static_cast<std::uint16_t>(read_u32(input, tag.body));
                    have_image = true;
                }
            } else if (tag.code == kTagDefineSprite && tag.end - tag.body >= 4) {
                sprites.push_back(tag);
            } else if (!have_wrapper && tag.code == kTagPlaceObject2 &&
                tag.end - tag.body >= 5 && (input[tag.body] & 0x03) == 0x02 &&
                read_u16(input, tag.body + 1) == 1) {
                wrapper_id = read_u16(input, tag.body + 3);
                have_wrapper = true;
            }
            position = tag.end;
        }
        if (tag.code != kTagEnd) return backdrop_failure("truncated tag list");
        if (!have_image) return backdrop_failure("no MENU_BaseMain_01 image");
        if (!have_wrapper) return backdrop_failure("nothing placed at root depth 1");

        const Tag* wrapper = nullptr;
        for (const Tag& sprite : sprites) {
            if (read_u16(input, sprite.body) == wrapper_id) wrapper = &sprite;
        }
        if (!wrapper || read_u16(input, wrapper->body + 2) != 1) {
            return backdrop_failure("root depth 1 is not a one-frame sprite");
        }

        // The wrapper must hold exactly: PlaceObject3(image), ShowFrame, End.
        Tag place{}, show{}, end{};
        if (!read_tag(input, wrapper->body + 4, wrapper->end, place) ||
            !read_tag(input, place.end, wrapper->end, show) ||
            !read_tag(input, show.end, wrapper->end, end) ||
            place.code != kTagPlaceObject3 || show.code != kTagShowFrame ||
            end.code != kTagEnd || end.end != wrapper->end ||
            place.end - place.body < 6) {
            return backdrop_failure("backdrop sprite has an unexpected layout");
        }
        const std::uint8_t flags = input[place.body];
        const std::uint8_t flags2 = input[place.body + 1];
        if ((flags & ~0x08) != 0x06 || flags2 != 0x10 ||
            read_u16(input, place.body + 2) != 1 ||
            read_u16(input, place.body + 4) != backdrop_image) {
            return backdrop_failure("backdrop sprite does not place MENU_BaseMain_01");
        }
        const std::size_t matrix_at = place.body + 6;
        const std::size_t matrix_length = matrix_bytes(input, matrix_at, place.end);
        if (!matrix_length) return backdrop_failure("invalid backdrop matrix");
        std::size_t fields_end = matrix_at + matrix_length;
        if (flags & 0x08) {
            const std::size_t length = cxform_bytes(input, fields_end, place.end);
            if (!length) return backdrop_failure("invalid backdrop color transform");
            fields_end += length;
        }
        if (fields_end != place.end) {
            return backdrop_failure("backdrop placement has extra fields");
        }

        const unsigned percent = alpha_percent > 100 ? 100u : alpha_percent;
        std::vector<std::uint8_t> place_body{};
        place_body.push_back(static_cast<std::uint8_t>(
            percent < 100 ? (flags | 0x08) : (flags & ~0x08)));
        place_body.insert(place_body.end(),
            input.begin() + place.body + 1, input.begin() + matrix_at + matrix_length);
        if (percent < 100) append_alpha_cxform(place_body, (percent * 256 + 50) / 100);

        std::vector<std::uint8_t> sprite_body(
            input.begin() + wrapper->body, input.begin() + wrapper->body + 4);
        append_tag(sprite_body, kTagPlaceObject3, place.long_header, place_body);
        sprite_body.insert(sprite_body.end(),
            input.begin() + show.start, input.begin() + wrapper->end);

        PatchResult result{};
        result.output.assign(input.begin(), input.begin() + wrapper->start);
        append_tag(result.output, kTagDefineSprite, wrapper->long_header, sprite_body);
        result.output.insert(result.output.end(), input.begin() + wrapper->end, input.end());
        const auto length = static_cast<std::uint32_t>(
            declared + result.output.size() - input.size());
        for (int index = 0; index < 4; ++index) {
            result.output[4 + index] = static_cast<std::uint8_t>(length >> (8 * index));
        }
        result.report.already_satisfied =
            std::equal(result.output.begin(), result.output.end(), input.begin(), input.end());
        result.report.bytes_added = result.output.size() > input.size()
            ? result.output.size() - input.size() : 0;
        return result;
    } catch (...) {
        PatchResult failed{};
        failed.error = ErrorCode::invalid_argument;
        failed.message = "out of memory";
        return failed;
    }
}

PatchResult patch_settings_movie(
    SettingsMovie movie,
    std::span<const std::uint8_t> input,
    const SettingsMovieOptions& options) noexcept {
    PatchResult result = patch_settings_movie_widgets(movie, input, options);
    if (!result.success() || options.menu_background_alpha_percent >= 100) {
        return result;
    }
    if (result.output.empty()) result.output.assign(input.begin(), input.end());
    PatchResult backdrop =
        set_menu_background_alpha(result.output, options.menu_background_alpha_percent);
    if (!backdrop.success()) {
        result.report.menu_background_skipped = true;
    } else if (!backdrop.report.already_satisfied) {
        result.report.bytes_added += backdrop.report.bytes_added;
        result.report.already_satisfied = false;
        result.output = std::move(backdrop.output);
    }
    return result;
}

PatchResult patch_settings_movie_widgets(
    SettingsMovie movie,
    std::span<const std::uint8_t> input,
    const SettingsMovieOptions& options) noexcept {
    if (movie == SettingsMovie::game_options) {
        const Inspection inspection = inspect_game_options_panel(input);
        if (!inspection.success()) {
            PatchResult failed{};
            failed.error = inspection.error;
            failed.message = inspection.message;
            failed.report.before = inspection;
            return failed;
        }
        PatchOptions patch_options{};
        patch_options.game_options_rows = options.game_options_rows
            ? options.game_options_rows
            : inspection.game_options_rows;
        patch_options.camera_options_rows = options.camera_options_rows
            ? options.camera_options_rows
            : inspection.camera_options_rows;
        patch_options.text_input_presentation = options.game_options_text_input;
        patch_options.color_picker_presentation = options.game_options_color_picker;
        patch_options.mod_settings_top_level_tab = options.mod_settings_top_level_tab;
        patch_options.wide_slider_values = options.wide_slider_values;
        return patch_game_options_panel(input, patch_options);
    }
    if (movie == SettingsMovie::advanced_settings) {
        return patch_widget_presentations(
            input,
            options.advanced_text_input,
            options.advanced_color_picker,
            options.wide_slider_values);
    }
    PatchResult invalid{};
    invalid.error = ErrorCode::invalid_argument;
    invalid.message = "not a supported settings movie";
    return invalid;
}

} // namespace erui::gfx
