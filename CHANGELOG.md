# Changelog

## NativeTweaks fork

Fork builds are numbered rNNN (`ERNATIVEUI_FORK_BUILD` in `CMakeLists.txt`;
DLL `FileVersion` 1.3.0.NNN). They are based on upstream ERNativeUI 1.3.0,
whose entries follow below. Fork-only exports are documented in
[docs/fork/api.md](docs/fork/api.md).

### r150 (in progress)

* Wide help: the `LineHelpWide` / `LineHelpWide2` twins get the game's native
  text DropShadow, matching the native `LineHelp`.
* Client header: `connect_api` improvements.
* Logs name the module that owns a foreign hook found on a known function.
* GFx rows are inserted in batches when a page is built.
* The alert queue no longer stays stuck after quitting to the title screen.
* Research probe build fixed.
* Documentation: fork section in the README, fork API reference, this
  changelog section.

### r149

* `native_tabs.cpp` (16.8k lines) split into nine translation units with a
  shared internal header; mechanical move, no behaviour change (globals keep
  their order, so construction and destruction order are unchanged).

### r148

* Static CRT (`/MT`) in every target: no MSVCP140/VCRUNTIME140 dependency,
  the runtime that broke the host on old Proton prefixes.
* Version resource: `FileVersion` 1.3.0.148, "ERNativeUI host (NativeTweaks
  fork r148)"; `ERNATIVEUI_FORK_BUILD` is the one build number (log banner,
  `ERNativeUI-status.txt`).
* Single host: a second `ERNativeUI.dll` in the same process stays passive and
  its exports report failure; the host pins itself at attach.
* With an exact build profile, a scan hit away from the profile address is
  refused with a warning (another mod hooked the prologue).
* safetyhook and minhook pinned to tagged commits.
* Smaller fixes: one warning per image adopted by layout, draft providers named
  when the readiness wait times out, UTF-8 module path in crash text.

### r147

* Every build gets a PDB (`/Z7`, `/DEBUG:FULL`) without growing the DLL.
* Clean exit: buffered log lines are written at process detach and logging
  stops before global destructors run.
* `ERNativeUI-status.txt` is rewritten on every start, even with logging off:
  fork build, game version, executable timestamp, and ready / unsupported game
  version / failed with what to do.
* An unknown game version is reported at startup and the fork's runtime GFX is
  skipped.
* The default `ERNativeUI.ini` starts with comments, so a UTF-8 BOM cannot hide
  the `[Logging]` section.

### r146

* Startup fault handler ignores breakpoints and walks the stack safely.
* Runtime GFX: a short read or an allocation failure serves the original file.
* Dialog input ownership checks the live popup slot; keybinds no longer stay
  blocked when the popup stops updating.
* Text input trims a lone trailing high surrogate; identical slider texts are
  not republished; button text released when a row fails to build.
* Storage saves no longer hold the outer lock during disk I/O; stale temp files
  of exited processes are removed on load.
* `ERUI_INTERNAL_ERROR` for unexpected exceptions.

### r145

* Wide help: only the page that drew the status bar re-hides the native
  `LineHelp`, and a bar not drawn for 2 s is dropped instead of being used
  after it was freed.
* Value polling writes a normalised value back with a compare-exchange, so a
  change the player made in between is kept.
* Known addresses are checked against the full pattern first, skipping about
  70 `.text` scans at startup.
* `ToggleIndexOf -1` no longer selects out of range.

### r135-r144

* Mod Settings tab and Game Options rebuild when reopening on it; layout
  fingerprint adoption of unlisted executables; wide help keeps the native
  `LineHelp` hidden on rebinds; native Back accepts an entry already hooked by
  another mod; startup faults logged; live switch for the coloured labels
  (`ERNativeUIFork_SetHighlightEnabled`); the item pop-up stays where the game
  puts it with menus at the screen edges.

## Upstream ERNativeUI

## [1.3.0](https://github.com/Flammrock/ERNativeUI/compare/ERNativeUI-v1.2.1...ERNativeUI-v1.3.0) (2026-10-20)


### Features

* publish ERUI API 1.3 ([#14](https://github.com/Flammrock/ERNativeUI/issues/14)) ([b6930ca](https://github.com/Flammrock/ERNativeUI/commit/b6930ca2af5d4b4a9fbcc83b962cb479832f5136))

## [1.2.1](https://github.com/Flammrock/ERNativeUI/compare/ERNativeUI-v1.2.0...ERNativeUI-v1.2.1) (2026-10-14)


### Bug Fixes

* avoid injecting bindings into Solid Uncapper menus ([#12](https://github.com/Flammrock/ERNativeUI/issues/12)) ([4daf17e](https://github.com/Flammrock/ERNativeUI/commit/4daf17e62341d997400a507ef143c2763ddf4b61))

## [1.2.0](https://github.com/Flammrock/ERNativeUI/compare/ERNativeUI-v1.1.1...ERNativeUI-v1.2.0) (2026-10-14)


### Features

* add configurable localization language override ([#8](https://github.com/Flammrock/ERNativeUI/issues/8)) ([a7baad7](https://github.com/Flammrock/ERNativeUI/commit/a7baad7b6f3fc68113527687e9881c59459c08e0))
* add numeric and custom sliders with ERUI API 1.2 ([#10](https://github.com/Flammrock/ERNativeUI/issues/10)) ([0566e29](https://github.com/Flammrock/ERNativeUI/commit/0566e294143a5d7ec73d834b2a3c202a58556a8c))
* add Vietnamese host locale ([#6](https://github.com/Flammrock/ERNativeUI/issues/6)) ([f5977ba](https://github.com/Flammrock/ERNativeUI/commit/f5977baf4accbe04572c0a3e5de9bd6fc44cd47a))

## [1.1.1](https://github.com/Flammrock/ERNativeUI/compare/ERNativeUI-v1.1.0...ERNativeUI-v1.1.1) (2026-09-11)


### Bug Fixes

* restore Elden Ring 2.7.1.0 compatibility ([#4](https://github.com/Flammrock/ERNativeUI/issues/4)) ([4d1682f](https://github.com/Flammrock/ERNativeUI/commit/4d1682f30340a05df93e1ddac69a9ec172805dd6))

## [1.1.0](https://github.com/Flammrock/ERNativeUI/compare/ERNativeUI-v1.0.0...ERNativeUI-v1.1.0) (2026-09-06)


### Features

* add native text input rows ([0ee11f0](https://github.com/Flammrock/ERNativeUI/commit/0ee11f03c5edec58dc5ca91b3fc2bd3e256afb95))
* complete ERUI API 1.1 ([74b209f](https://github.com/Flammrock/ERNativeUI/commit/74b209fd16b8d6028b978a74e608347c46cf0922))
* complete ERUI API 1.1 ([fecb1d3](https://github.com/Flammrock/ERNativeUI/commit/fecb1d332fb16f476f7d25478ee0e2a2856d76a6))

## [1.0.0](https://github.com/Flammrock/ERNativeUI/compare/ERNativeUI-v0.9.0...ERNativeUI-v1.0.0) (2026-08-30)


### Miscellaneous Chores

* prepare first stable release ([234b175](https://github.com/Flammrock/ERNativeUI/commit/234b175828f58d41dcdd16aee20eb032df215f09))
