# NativeTweaks fork: `ERNativeUIFork_*` exports

The NativeTweaks fork of ERNativeUI exports a few extra C functions from
`ERNativeUI.dll`. They are **not** part of the official `ERUI_Api` table
returned by `ERUI_GetApi`, and the upstream host does not have them.

- Look them up with `GetProcAddress` on the loaded `ERNativeUI.dll` and fall
  back gracefully when a symbol is missing (upstream host or an older fork
  build).
- All use the C ABI of `include/ernativeui/erui.h`: `ERUI_CALL` (`__cdecl`),
  `ERUI_Result` (`uint32_t`) return values, `ERUI_Utf16View` for visible text.
- Handles (`ERUI_ProviderHandle`, `ERUI_RowHandle`, `ERUI_PageHandle`) are the
  ones the official API returned to your provider.
- Every text argument is borrowed for the call only; the host copies it.

The implementations live in `src/host_api.cpp`, `src/host_dllmain.cpp`
(`SetHostLanguage`) and `src/runtime_gfx.cpp` (`SetHudLayout`).

## Summary

| Export | Purpose |
|---|---|
| [`ERNativeUIFork_SetRowText`](#ernativeuifork_setrowtext) | Change a row's label and/or help text at run time |
| [`ERNativeUIFork_SetSubmenuText`](#ernativeuifork_setsubmenutext) | Change a submenu row's texts and its page title/help at run time |
| [`ERNativeUIFork_SetTextInputReadOnly`](#ernativeuifork_settextinputreadonly) | Make a TextInput a read-only live display (no editor) |
| [`ERNativeUIFork_SetCustomSliderValues`](#ernativeuifork_setcustomslidervalues) | Replace the per-position texts of a custom or numeric slider |
| [`ERNativeUIFork_SetRowDefault`](#ernativeuifork_setrowdefault) | Register a row's default value for the changed-value label colour |
| [`ERNativeUIFork_SetRowDefaultEx`](#ernativeuifork_setrowdefaultex) | As above, with flags (inverted sliders) |
| [`ERNativeUIFork_SetHighlightEnabled`](#ernativeuifork_sethighlightenabled) | Turn the changed-value label colours on or off, live |
| [`ERNativeUIFork_SetHostLanguage`](#ernativeuifork_sethostlanguage) | Switch the host's own texts to another language |
| [`ERNativeUIFork_SetHudLayout`](#ernativeuifork_sethudlayout) | Ultrawide HUD/menu layout and HUD scale |
| [`ERNativeUIFork_Talk*`](#talk-menu-f11) | Rows and pages in the shared talk menu (F11) |

## Threading and timing

- Every fork export may be called from **any thread**. The registry-backed
  ones (`SetRowText`, `SetSubmenuText`, `SetCustomSliderValues`,
  `SetRowDefault[Ex]`, `SetHostLanguage`) take the registry mutex for a short
  time; `SetHighlightEnabled` and `SetHudLayout` only store atomics.
- The registry mutex is not held while client callbacks run (see
  [Lifecycle, errors, and limits](../reference/lifecycle-errors-and-limits.md#callback-threads)),
  so calling these from a change callback on the ERNativeUI worker, or from a
  button callback on the game's UI thread, is allowed. Keep such calls short.
- Do not call them from `DllMain`.
- Text changes reach the screen at the next page frame; a row that is
  currently visible is relabelled in place.

Timing windows for the text/value exports:

| Host state | Result |
|---|---|
| Host still initializing | `ERUI_HOST_NOT_READY` |
| Provider committed, registry not frozen yet | `ERUI_OK`; the change goes into the model and is compiled into the menu |
| Registry frozen, menu still compiling | `ERUI_HOST_NOT_READY`; retry later (except `SetHostLanguage`, which is kept and applied when the menu is published) |
| Menu published | `ERUI_OK`; the published menu's texts are replaced |
| Host failed, or a second passive copy of the DLL | `ERUI_HOST_FAILED` |

A second `ERNativeUI.dll` loaded into the same process (r148 and later) stays
passive: its exports report `ERUI_HOST_FAILED`, and `SetHudLayout` reports
`ERUI_NOT_SUPPORTED`. Clients should use the module that `ERUI_GetApi` came
from.

## Exports

### `ERNativeUIFork_SetRowText`

```c
ERUI_Result ERUI_CALL ERNativeUIFork_SetRowText(
    ERUI_ProviderHandle provider,
    ERUI_RowHandle row,
    const ERUI_Utf16View* label,   /* NULL or data == NULL: keep */
    const ERUI_Utf16View* help);   /* NULL or data == NULL: keep */
```

Replaces the label and/or help of any row of a committed provider. Passing
`NULL` (or a view whose `data` is `NULL`) for one of them keeps the current
text.

The new text is used the next time the row is built: a page that is open
keeps showing the old text until it is opened again. For a value that changes
while the page is open, use a read-only TextInput
([`SetTextInputReadOnly`](#ernativeuifork_settextinputreadonly)).

| Result | When |
|---|---|
| `ERUI_OK` | Applied (or stored for compilation) |
| `ERUI_INVALID_ARGUMENT` | Invalid UTF-16, embedded NUL, or text longer than the host limit |
| `ERUI_INVALID_HANDLE` | Unknown provider, provider not committed, or row not owned by it |
| `ERUI_HOST_NOT_READY` | Host initializing, or menu compiling |
| `ERUI_HOST_FAILED` | Host failed or passive copy |
| `ERUI_OUT_OF_MEMORY`, `ERUI_INTERNAL_ERROR` | Allocation failure or unexpected exception |

### `ERNativeUIFork_SetSubmenuText`

```c
ERUI_Result ERUI_CALL ERNativeUIFork_SetSubmenuText(
    ERUI_ProviderHandle provider,
    ERUI_PageHandle page,             /* the submenu's child page */
    const ERUI_Utf16View* label,      /* opener row label, NULL: keep */
    const ERUI_Utf16View* help,       /* opener row help,  NULL: keep */
    const ERUI_Utf16View* page_title, /* child page title, NULL: keep */
    const ERUI_Utf16View* page_help); /* child page help,  NULL: keep */
```

`page` is the page handle returned for the submenu. The host finds the submenu
row that opens it and relabels both. A paginated child page also gets its
"Title (n/m)" slices renamed. A page with a custom page-title presentation
(formatter or `page_title` presentation) keeps that title; `page_title` is
then ignored.

Results as for `SetRowText`, plus `ERUI_INVALID_ARGUMENT` when no submenu row
of that provider opens `page`.

### `ERNativeUIFork_SetTextInputReadOnly`

```c
ERUI_Result ERUI_CALL ERNativeUIFork_SetTextInputReadOnly(
    ERUI_ProviderHandle provider,
    ERUI_RowHandle row,      /* a row from add_text_input */
    uint32_t read_only);     /* 1: read-only, 0: editable */
```

A read-only TextInput keeps showing its value, and `set_text_input_value`
still updates it at once while the page is open, but selecting it opens no
editor. It is meant for status lines ("Status", "Selected", counters). The
value keeps the normal (white) text colour. The flag is read when the row is
built, that is each time its page opens: set it before `commit_provider` so
the row is read-only from the start; a change made while the page is open
applies the next time it opens. Values are limited to the TextInput maximum
length (35 UTF-16 units).

| Result | When |
|---|---|
| `ERUI_OK` | Applied |
| `ERUI_INVALID_ARGUMENT` | The row is not a TextInput |
| `ERUI_INVALID_HANDLE` | Unknown provider, or row not owned by it |
| `ERUI_NOT_SUPPORTED` | Provider registered with API 1.0 |
| `ERUI_HOST_NOT_READY`, `ERUI_HOST_FAILED` | Host initializing, failed or passive copy |

### `ERNativeUIFork_SetCustomSliderValues`

```c
ERUI_Result ERUI_CALL ERNativeUIFork_SetCustomSliderValues(
    ERUI_ProviderHandle provider,
    ERUI_RowHandle row,
    const ERUI_Utf16View* values,
    uint32_t count);
```

Replaces the text shown for each position of a Custom Slider, or of a Numeric
Slider (for example to show a multiplied value). `count` must equal the row's
current number of positions; the position count itself cannot change.

| Result | When |
|---|---|
| `ERUI_OK` | Applied |
| `ERUI_INVALID_ARGUMENT` | `values` is `NULL`; `count` outside `ERUI_SLIDER_MIN_VALUE_COUNT`..`ERUI_SLIDER_MAX_VALUE_COUNT`; an empty, invalid or longer than `ERUI_CUSTOM_SLIDER_MAX_TEXT_LENGTH` text; the row is not a custom/numeric slider; `count` differs from the row's position count |
| `ERUI_INVALID_HANDLE` | Unknown provider, not committed, or unknown row |
| `ERUI_HOST_NOT_READY` | Host initializing, menu compiling, or the slider bridge not ready |
| `ERUI_HOST_FAILED`, `ERUI_OUT_OF_MEMORY`, `ERUI_INTERNAL_ERROR` | As above |

### `ERNativeUIFork_SetRowDefault`

```c
ERUI_Result ERUI_CALL ERNativeUIFork_SetRowDefault(
    ERUI_ProviderHandle provider,
    ERUI_RowHandle row,
    int64_t value);
```

Registers the default value of a row. While the row's value differs from it,
its label is tinted: gold for a toggle, soft green above the default and soft
red below it for a slider. Supported rows and the meaning of `value`:

| Row kind | `value` |
|---|---|
| Toggle | `0` = off, anything else = on |
| Numeric Slider | Scaled value, within the row's minimum/maximum (converted to a position by its step) |
| Custom Slider | Position index, `0`..count-1 |

| Result | When |
|---|---|
| `ERUI_OK` | Registered |
| `ERUI_INVALID_ARGUMENT` | `value` out of range for the slider |
| `ERUI_NOT_SUPPORTED` | Any other row kind (button, legacy slider, choices, text input...) |
| `ERUI_INVALID_HANDLE` | Unknown provider or row |
| `ERUI_HOST_NOT_READY`, `ERUI_HOST_FAILED` | Host state (see above) |

The colours need the native value-refresh hooks; when their prologue checks
fail on a game build, the call still succeeds but no colour is drawn.

### `ERNativeUIFork_SetRowDefaultEx`

```c
ERUI_Result ERUI_CALL ERNativeUIFork_SetRowDefaultEx(
    ERUI_ProviderHandle provider,
    ERUI_RowHandle row,
    int64_t value,
    uint32_t flags);
```

As `SetRowDefault`. `flags` bit 0 marks an inverted slider (higher is worse):
its label turns red above the default and green below it. Other bits are
ignored. Results as for `SetRowDefault`.

### `ERNativeUIFork_SetHighlightEnabled`

```c
ERUI_Result ERUI_CALL ERNativeUIFork_SetHighlightEnabled(uint32_t enabled);
```

Turns the changed-value label colours on (`enabled != 0`, the default) or off
for every row, live. Registered defaults are kept, so turning it on again
needs nothing else; labels change at the next page frame. Callable at any
time, also before the API is ready. Always returns `ERUI_OK`.

### `ERNativeUIFork_SetHostLanguage`

```c
ERUI_Result ERUI_CALL ERNativeUIFork_SetHostLanguage(const char* identifier);
```

Switches the host's own texts (Mod Settings title and help, Previous/Next
Page) to a Steam language identifier such as `"english"` or `"spanish"`,
using the built-in table plus an optional `locales\<identifier>.ini` next to
the DLL. The tab help keeps the game's language on purpose.

- Before the registry freezes: sets the texts the menu is built with.
- While the menu compiles: kept and applied right after the menu is published
  (returns `ERUI_OK`).
- After publication: relabels the published menu at once.

| Result | When |
|---|---|
| `ERUI_OK` | Applied or queued |
| `ERUI_INVALID_ARGUMENT` | `NULL`, empty, or not a safe locale identifier |
| `ERUI_HOST_FAILED` | Host failed or passive copy |
| `ERUI_OUT_OF_MEMORY`, `ERUI_INTERNAL_ERROR` | Allocation failure or unexpected exception |

### `ERNativeUIFork_SetHudLayout`

```c
ERUI_Result ERUI_CALL ERNativeUIFork_SetHudLayout(
    uint32_t flags,
    uint32_t scale_percent);
```

Layout used by the in-memory HUD/menu movie patch on screens wider than 16:9.

| `flags` bit | Meaning |
|---|---|
| 0 | Move the edge-anchored HUD blocks to the screen edges |
| 1 | Move the menus that have a layout table to the screen edges (menus are never scaled) |

`scale_percent` (50-150) resizes the HUD blocks around their corner. The
layout applies the next time the game loads the HUD/menu movies (normally at
startup), so a change made later needs a game restart; the host logs a
warning when the new layout differs from the one the movies were opened with.
These values override `[GFX] HudToScreenEdges`, `HudScale` and
`MenusToScreenEdges` in `ERNativeUI.ini`.

Callable at any time, also before the API is ready. The OpenFile hook is
installed before the API becomes ready, so a client that has connected gets
the definitive answer.

| Result | When |
|---|---|
| `ERUI_OK` | Stored |
| `ERUI_INVALID_ARGUMENT` | `scale_percent` outside 50-150, or unknown `flags` bits |
| `ERUI_NOT_SUPPORTED` | The runtime GFX hook is not installed (`[GFX] EnableRuntimePatching = 0`, a failed install, an unknown game version, a passive copy, or a call before the host thread installed it); nothing is stored |

## Talk menu (F11)

r157. **F11** (or the `[TalkMenu] Key` below), in a game with no other menu
open, opens the game's own
talk-list dialog (as a merchant's): one row per mod, then **Leave**.
Typedefs are in `include/ernativeui/erui_fork_talk.h`; the dialog is a Rust
static library (`talk/`) linked into `ERNativeUI.dll`, the exports and the
Mod Settings pages are in `src/talk_menu.cpp`.

**Mod Settings pages, automatically.** Every committed provider whose Mod
Settings page has rows gets a row named after it, with nothing to call: its
page and submenus become talk pages, live. A toggle flips when chosen
(`Name: On`), a slider, numeric or custom slider or choice opens a page with
its values (`(current)` marks the one in use; picking one goes back), a
submenu opens its page, a button runs its callback and closes the menu (it
may open a game menu), a text input shows its value. Long pages are split
into pages of 60 rows. A value is changed as a player edit in the
Configuration screen would change it, so the provider's change callback runs
on the ERNativeUI worker within about 50 ms and saving works as usual.
Disabled rows are shown but do nothing. These rows come first in the list,
in Mod Settings order; a provider that also registers talk pages
(`ERNativeUIFork_TalkRegister`) under the same name keeps only those.

The presence of `ERNativeUIFork_TalkRegister` means the host owns the
talk-menu key: a mod with its own talk-menu hotkey (NPC Spawner's Ctrl+F11)
should register a row instead of listening to a key.

```c
ERUI_Result ERNativeUIFork_TalkRegister(const ERUI_Utf16View* name,
    ERUI_TalkBuildFn build, ERUI_TalkChooseFn choose, void* user,
    ERUI_TalkEntryHandle* out);                 /* any thread, any time */
ERUI_Result ERNativeUIFork_TalkUnregister(ERUI_TalkEntryHandle entry);

/* callbacks, on the game thread */
void build(void* user, ERUI_TalkMenu* menu, uint32_t page);       /* 0 = first page */
void choose(void* user, ERUI_TalkMenu* menu, uint32_t page, uint32_t row);

/* inside build only */
ERUI_Result ERNativeUIFork_TalkAddRow(ERUI_TalkMenu* menu, uint32_t row,
    const ERUI_Utf16View* text);
/* inside build or choose */
ERUI_Result ERNativeUIFork_TalkOpenPage(ERUI_TalkMenu* menu, uint32_t page);    /* push */
ERUI_Result ERNativeUIFork_TalkReplacePage(ERUI_TalkMenu* menu, uint32_t page); /* swap */
ERUI_Result ERNativeUIFork_TalkBack(ERUI_TalkMenu* menu);
ERUI_Result ERNativeUIFork_TalkClose(ERUI_TalkMenu* menu);

/* any thread */
ERUI_Result ERNativeUIFork_TalkOpen(ERUI_TalkEntryHandle entry); /* 0: the top list */
uint32_t    ERNativeUIFork_TalkIsOpen(void);
```

- Pages are numbers of your own; the first one is 0. Row ids are yours too
  and come back in `choose`.
- `build` adds up to 63 rows (`ERUI_QUEUE_FULL` after that); the host adds a
  **Back** row after them. Texts longer than 159 UTF-16 units are cut.
- After `choose` without a navigation call, the same page is built and shown
  again, so a row like "Toggle: on" shows its new value at once.
- `OpenPage` pushes a page (Back returns to this one), `ReplacePage` shows
  another page in place of this one (next/previous pages of a list), `Back`
  goes up one level (from your first page, to the top list), `Close` closes
  the menu. A navigation call inside `build` is followed instead of showing
  the page.
- **Esc** on one of your pages goes up one level; on the top list it closes
  the menu.
- `ERUI_TalkMenu*` is only valid during the callback it was given to; other
  calls return `ERUI_INVALID_HANDLE`.
- Callbacks must be quick and must not throw or unwind across the boundary.
  The registry is not locked while they run; they may call any export.
  To open a game menu (a shop, for instance), call `Close` and open it from
  `choose` or later.
- `TalkOpen` while the menu is closed waits up to about one second for a
  frame where it may open (another menu still closing, no player yet), then
  is dropped; while the menu is open it is ignored. `Close` + `TalkOpen`
  from `choose` reopens the menu on the next frame.
- Up to 48 providers.

Texts are served by a chained SafetyHook inline hook on the game's message
lookup (`MsgRepositoryImp::LookupEntry`), put in the first time the menu opens
and kept; it answers ids 1991000000-1991000063 and passes every other call on,
so it works with mods that hook the same function before or after it
(Glorious Merchant). The function is found by its bytes or, if another mod
hooked it first, through the item-name getters that call it.

`ERNativeUI.ini`:

```ini
[TalkMenu]
Enable=1        ; 0 turns the key and the exports' menu off
Key=F11         ; F1-F24, Home, End, Insert, Delete, PageUp, PageDown, Pause,
                ; ScrollLock, with optional Ctrl+ / Alt+ / Shift+ (e.g. Ctrl+Home)
```

Supported game versions: those of fromsoftware-rs (WW 2.7.0.0, 2.7.1.0,
2.7.2.0, JP 2.7.0.1, 2.7.1.1, 2.7.2.1) and of ERNativeUI.

## Example

```c
typedef ERUI_Result (ERUI_CALL *SetRowTextFn)(
    ERUI_ProviderHandle, ERUI_RowHandle,
    const ERUI_Utf16View*, const ERUI_Utf16View*);

HMODULE host = GetModuleHandleW(L"ERNativeUI.dll");
SetRowTextFn set_row_text = host
    ? (SetRowTextFn)GetProcAddress(host, "ERNativeUIFork_SetRowText")
    : NULL;
if (set_row_text) {
    static const wchar_t text[] = L"Damage dealt (x2)";
    ERUI_Utf16View label = { (const uint16_t*)text, 17, 0 };
    ERUI_Result r = set_row_text(provider, row, &label, NULL);
    if (r == ERUI_HOST_NOT_READY) { /* retry later */ }
}
```

## Versioning

- Fork builds are numbered **rNNN** (for example r149). The number is set in
  one place: `ERNATIVEUI_FORK_BUILD` in `CMakeLists.txt`.
- It feeds the DLL's version resource: `FileVersion` is
  `1.3.0.NNN` (the upstream ERNativeUI version plus the fork build) and the
  file description reads "ERNativeUI host (NativeTweaks fork rNNN)".
- The same tag appears in the `ERNativeUI.log` banner and in
  `ERNativeUI-status.txt`.
- The official ERUI API version stays the upstream one (1.3); the fork exports
  are versioned only by their presence. A client should check each symbol with
  `GetProcAddress` rather than compare build numbers.
