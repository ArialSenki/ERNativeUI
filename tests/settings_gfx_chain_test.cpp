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
    test_chain(vanilla_dir, "02_040_optionsetting.gfx",
        erui::gfx::SettingsMovie::game_options,
        erui::gfx::SettingsGfxRuntimeTarget::game_options);
    test_chain(vanilla_dir, "02_042_pc_graphicsetting.gfx",
        erui::gfx::SettingsMovie::advanced_settings,
        erui::gfx::SettingsGfxRuntimeTarget::advanced_settings);
    std::printf("%d failure(s)\n", g_failures);
    return g_failures == 0 ? 0 : 1;
}
