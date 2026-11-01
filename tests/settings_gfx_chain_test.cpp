// Fork: the host runs two GFX stages inside one OpenFile hook. The upstream
// completion (rows, TextInput, ColorPicker, tab slots) goes first; the fork's
// settings patch (Mod Settings tab, wide slider values, backdrop) goes on the
// bytes it produced. This checks that the fork stage still recognizes and
// patches the upstream output, that the chain is stable, and that upstream
// still understands the final movie.

#include "gfx/gfx_document.hpp"
#include "gfx/settings_gfx_runtime.hpp"
#include "gfx/settings_gfx_transformer.hpp"
#include "settings_movie.hpp"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace {

int g_failures = 0;

void check(bool condition, const std::string& what) {
    std::printf("%s %s\n", condition ? "ok  " : "FAIL", what.c_str());
    if (!condition) ++g_failures;
}

std::vector<std::uint8_t> read_file(const std::filesystem::path& path) {
    std::ifstream stream(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>()};
}

std::optional<erui::gfx::SettingsGfxInspection> inspect(
    std::span<const std::uint8_t> bytes) {
    erui::gfx::GfxDocumentParseResult parsed = erui::gfx::GfxDocument::parse(bytes);
    if (!parsed.success()) return std::nullopt;
    const erui::gfx::SettingsGfxInspectionResult result =
        erui::gfx::inspect_settings_widgets_gfx(parsed.document);
    if (!result.success()) return std::nullopt;
    return result.inspection;
}

// The options runtime_gfx.cpp uses once the upstream stage is active.
erui::gfx::SettingsMovieOptions chained_options() {
    erui::gfx::SettingsMovieOptions options{};
    options.advanced_text_input = erui::gfx::TextInputPresentation::native;
    options.advanced_color_picker = erui::gfx::ColorPickerPresentation::native;
    return options;
}

void test_chain(
    const std::filesystem::path& vanilla_dir,
    const char* name,
    erui::gfx::SettingsMovie movie,
    erui::gfx::SettingsGfxRuntimeTarget target) {
    const std::vector<std::uint8_t> vanilla = read_file(vanilla_dir / name);
    if (vanilla.empty()) {
        std::printf("skip %s (fixture missing)\n", name);
        return;
    }
    const std::string label(name);

    erui::gfx::SettingsGfxRuntimeCache cache{8};
    check(cache.ready(), label + ": upstream cache ready");
    const erui::gfx::SettingsGfxRuntimeResult upstream = cache.prepare(
        target, std::span<const std::uint8_t>(vanilla.data(), vanilla.size()));
    check(upstream.has_replacement(), label + ": upstream completes the vanilla movie");
    if (!upstream.has_replacement()) return;
    const std::vector<std::uint8_t> completed(
        upstream.replacement_bytes,
        upstream.replacement_bytes + upstream.replacement_size);

    const erui::gfx::SettingsMovieOptions options = chained_options();
    const erui::gfx::PatchResult fork = erui::gfx::patch_settings_movie(
        movie, completed, options);
    if (!fork.success()) std::printf("     fork stage: %s\n", fork.message.c_str());
    check(fork.success(), label + ": fork stage accepts the upstream output");
    if (!fork.success()) return;
    check(!fork.report.already_satisfied && !fork.output.empty(),
        label + ": fork stage adds its own changes");
    const std::vector<std::uint8_t>& chained =
        fork.output.empty() ? completed : fork.output;

    const erui::gfx::PatchResult again = erui::gfx::patch_settings_movie(
        movie, chained, options);
    check(again.success() && again.report.already_satisfied,
        label + ": fork stage is idempotent on the chained movie");

    const auto before = inspect(completed);
    const auto after = inspect(chained);
    check(before && after, label + ": upstream inspection still parses the chained movie");
    if (before && after) {
        check(after->character_name_text_input == before->character_name_text_input &&
                after->character_creation_color_picker ==
                    before->character_creation_color_picker,
            label + ": upstream TextInput/ColorPicker presentation is kept");
        check(after->color_picker_host_count == before->color_picker_host_count,
            label + ": no duplicate ColorPicker hosts");
    }

    if (movie == erui::gfx::SettingsMovie::game_options) {
        const auto full = [](std::span<const std::uint8_t> bytes)
            -> std::optional<erui::gfx::SettingsGfxInspection> {
            erui::gfx::GfxDocumentParseResult parsed =
                erui::gfx::GfxDocument::parse(bytes);
            if (!parsed.success()) return std::nullopt;
            const erui::gfx::SettingsGfxInspectionResult result =
                erui::gfx::inspect_settings_gfx(parsed.document);
            if (!result.success()) return std::nullopt;
            return result.inspection;
        };
        const auto rows_before = full(completed);
        const auto rows_after = full(chained);
        check(rows_before && rows_after, label + ": full settings inspection parses both");
        if (rows_before && rows_after) {
            std::printf("     tab cells %u -> %u, game options rows %u -> %u\n",
                static_cast<unsigned>(rows_before->tab_list_cells),
                static_cast<unsigned>(rows_after->tab_list_cells),
                static_cast<unsigned>(rows_before->game_options_rows),
                static_cast<unsigned>(rows_after->game_options_rows));
            check(rows_after->tab_list_cells == rows_before->tab_list_cells + 1,
                label + ": the Mod Settings tab adds one tab cell");
            check(rows_after->game_options_rows == rows_before->game_options_rows &&
                    rows_after->camera_options_rows == rows_before->camera_options_rows,
                label + ": upstream row capacity is kept");
        }
    }

    // Upstream on a movie that already went through the chain (a loose file
    // built from it, or a second open) must not undo the fork's changes.
    erui::gfx::SettingsGfxRuntimeCache second{8};
    const erui::gfx::SettingsGfxRuntimeResult reopened = second.prepare(
        target, std::span<const std::uint8_t>(chained.data(), chained.size()));
    check(reopened.success(), label + ": upstream accepts the chained movie");
    if (reopened.has_replacement()) {
        const std::vector<std::uint8_t> recompleted(
            reopened.replacement_bytes,
            reopened.replacement_bytes + reopened.replacement_size);
        const erui::gfx::PatchResult kept = erui::gfx::patch_settings_movie(
            movie, recompleted, options);
        check(kept.success() && kept.report.already_satisfied,
            label + ": fork changes survive a second upstream pass");
    }
}

// r150: small SHA-256 so the test can pin the exact transformed bytes without
// linking the host's internal sha256.cpp into this target.
std::string sha256_hex(std::span<const std::uint8_t> data) {
    static constexpr std::uint32_t k[64] = {
        0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1,
        0x923f82a4, 0xab1c5ed5, 0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3,
        0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174, 0xe49b69c1, 0xefbe4786,
        0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
        0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147,
        0x06ca6351, 0x14292967, 0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13,
        0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85, 0xa2bfe8a1, 0xa81a664b,
        0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
        0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a,
        0x5b9cca4f, 0x682e6ff3, 0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208,
        0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};
    std::uint32_t h[8] = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
        0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
    std::vector<std::uint8_t> m(data.begin(), data.end());
    const std::uint64_t bits = static_cast<std::uint64_t>(data.size()) * 8U;
    m.push_back(0x80);
    while (m.size() % 64 != 56) m.push_back(0);
    for (int i = 7; i >= 0; --i) m.push_back(static_cast<std::uint8_t>(bits >> (i * 8)));
    const auto rotr = [](std::uint32_t x, int n) { return (x >> n) | (x << (32 - n)); };
    for (std::size_t block = 0; block < m.size(); block += 64) {
        std::uint32_t w[64];
        for (int i = 0; i < 16; ++i) {
            w[i] = (std::uint32_t{m[block + i * 4]} << 24) |
                (std::uint32_t{m[block + i * 4 + 1]} << 16) |
                (std::uint32_t{m[block + i * 4 + 2]} << 8) |
                std::uint32_t{m[block + i * 4 + 3]};
        }
        for (int i = 16; i < 64; ++i) {
            const std::uint32_t s0 = rotr(w[i - 15], 7) ^ rotr(w[i - 15], 18) ^ (w[i - 15] >> 3);
            const std::uint32_t s1 = rotr(w[i - 2], 17) ^ rotr(w[i - 2], 19) ^ (w[i - 2] >> 10);
            w[i] = w[i - 16] + s0 + w[i - 7] + s1;
        }
        std::uint32_t a = h[0], b = h[1], c = h[2], d = h[3];
        std::uint32_t e = h[4], f = h[5], g = h[6], hh = h[7];
        for (int i = 0; i < 64; ++i) {
            const std::uint32_t t1 = hh + (rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25)) +
                ((e & f) ^ (~e & g)) + k[i] + w[i];
            const std::uint32_t t2 = (rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22)) +
                ((a & b) ^ (a & c) ^ (b & c));
            hh = g; g = f; f = e; e = d + t1; d = c; c = b; b = a; a = t1 + t2;
        }
        h[0] += a; h[1] += b; h[2] += c; h[3] += d;
        h[4] += e; h[5] += f; h[6] += g; h[7] += hh;
    }
    std::string hex;
    char buffer[9];
    for (const std::uint32_t word : h) {
        std::snprintf(buffer, sizeof buffer, "%08x", static_cast<unsigned>(word));
        hex += buffer;
    }
    return hex;
}

// r150: grow_panel now inserts every missing row of a panel in one
// GfxDocument::apply instead of one apply (full copy, serialize, reparse,
// reindex) per row. The digests below were taken from the per-row path on
// the vanilla 2.7.1 02_040 before that change, so the batched path must
// reproduce those bytes exactly. Also prints the transform time.
void test_transform_identity(const std::filesystem::path& vanilla_dir) {
    const char* name = "02_040_optionsetting.gfx";
    const std::vector<std::uint8_t> vanilla = read_file(vanilla_dir / name);
    if (vanilla.empty()) {
        std::printf("skip %s identity (fixture missing)\n", name);
        return;
    }
    struct Case {
        std::uint16_t tab_slots;
        const char* expected;
    };
    static constexpr Case kCases[] = {
        {8, "246b622a05f9e6af2c6bf12ac63112c42764f4f1f351ad1eb072a9048901c4ca"},
        {10, "ebff402ed096759a6ad197594990f27864b8c2af7c5aa002fbc10493a2b6840b"},
    };
    for (const Case& item : kCases) {
        const erui::gfx::SettingsGfxRequirements requirements =
            erui::gfx::settings_gfx_runtime_requirements(
                erui::gfx::SettingsGfxRuntimeTarget::game_options,
                item.tab_slots);
        constexpr int kRuns = 20;
        double total_ms{};
        std::string digest;
        bool ok = true;
        for (int run = 0; run < kRuns; ++run) {
            erui::gfx::GfxDocumentParseResult parsed = erui::gfx::GfxDocument::parse(
                std::span<const std::uint8_t>(vanilla.data(), vanilla.size()));
            if (!parsed.success()) { ok = false; break; }
            const auto start = std::chrono::steady_clock::now();
            erui::gfx::SettingsGfxTransformResult result =
                erui::gfx::transform_settings_gfx(
                    std::move(parsed.document), requirements);
            const auto stop = std::chrono::steady_clock::now();
            total_ms += std::chrono::duration<double, std::milli>(stop - start).count();
            if (!result.success()) { ok = false; break; }
            const erui::gfx::GfxDocumentBytesResult bytes =
                result.document.serialize_and_validate();
            if (!bytes.success()) { ok = false; break; }
            const std::string current = sha256_hex(bytes.bytes);
            if (!digest.empty() && current != digest) { ok = false; break; }
            digest = current;
        }
        const std::string label = std::string(name) + " tab slots " +
            std::to_string(item.tab_slots);
        check(ok, label + ": transform succeeds and is deterministic");
        std::printf("     %s: sha256 %s, transform %.3f ms (mean of %d)\n",
            label.c_str(), digest.c_str(), total_ms / kRuns, kRuns);
        if (*item.expected) {
            check(digest == item.expected,
                label + ": output bytes match the per-row path");
        }
    }
}

} // namespace

int main() {
    std::filesystem::path vanilla_dir;
    if (const char* env = std::getenv("NT_GFX_VANILLA"); env && *env) {
        vanilla_dir = env;
    }
#if defined(ERNATIVEUI_TEST_VANILLA_GFX_DIR)
    if (vanilla_dir.empty()) vanilla_dir = ERNATIVEUI_TEST_VANILLA_GFX_DIR;
#endif
    if (vanilla_dir.empty()) {
        std::printf("skip: no vanilla GFX folder (NT_GFX_VANILLA)\n");
        return 0;
    }
    test_transform_identity(vanilla_dir);
    test_chain(vanilla_dir, "02_040_optionsetting.gfx",
        erui::gfx::SettingsMovie::game_options,
        erui::gfx::SettingsGfxRuntimeTarget::game_options);
    test_chain(vanilla_dir, "02_042_pc_graphicsetting.gfx",
        erui::gfx::SettingsMovie::advanced_settings,
        erui::gfx::SettingsGfxRuntimeTarget::advanced_settings);
    std::printf("%d failure(s)\n", g_failures);
    return g_failures == 0 ? 0 : 1;
}
