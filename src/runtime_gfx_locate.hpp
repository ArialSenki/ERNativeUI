#pragma once

// Fork: build-independent lookup of the Scaleform types used by runtime GFX
// patching. Kept free of hooks so tests can run it on a mapped game image.

#include "module.hpp"

#include <string_view>

namespace erui::host {

struct RuntimeGfxTargets {
    void** file_opener_vtable{};   // CS::CSScaleformFileOpener
    void* open_file{};             // its OpenFile override (slot 1)
    void** memory_file_vtable{};   // Scaleform::MemoryFile
    const char* failure{};         // set when the lookup or validation fails
};

// Finds a class's primary vtable from its MSVC RTTI decorated name
// (".?AVName@Namespace@@").
[[nodiscard]] void** find_rtti_vtable(
    const erui::native::ModuleView& game, std::string_view decorated_name) noexcept;

// Locates and validates every runtime GFX target. Returns false and sets
// targets.failure when anything does not match the validated layout.
[[nodiscard]] bool locate_runtime_gfx_targets(
    const erui::native::ModuleView& game, RuntimeGfxTargets& targets) noexcept;

} // namespace erui::host
