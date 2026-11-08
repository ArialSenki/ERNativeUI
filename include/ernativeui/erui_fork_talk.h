#ifndef ERNATIVEUI_ERUI_FORK_TALK_H_INCLUDED
#define ERNATIVEUI_ERUI_FORK_TALK_H_INCLUDED

/*
 * NativeTweaks fork of ERNativeUI: the shared talk menu (F11 by default,
 * ERNativeUI.ini [TalkMenu] Key).
 *
 * Not part of the official ERUI_Api table. Look the exports up with
 * GetProcAddress on the loaded ERNativeUI.dll and fall back gracefully when
 * they are missing (upstream host or an older fork build). The presence of
 * ERNativeUIFork_TalkRegister means the host owns the talk-menu key: a mod
 * that had its own talk-menu hotkey should register a row instead.
 *
 * Callbacks run on the game thread (a FrameBegin task) and must return
 * quickly and must not throw. See docs/fork/api.md.
 */

#include "erui.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef uint64_t ERUI_TalkEntryHandle;
typedef struct ERUI_TalkMenu ERUI_TalkMenu; /* valid only inside a callback */

#define ERUI_INVALID_TALK_ENTRY ((ERUI_TalkEntryHandle)0u)
/* Rows a page can show (the host adds a Back row after them). */
#define ERUI_TALK_MAX_ROWS 63u
/* UTF-16 code units of a row text; longer texts are cut. */
#define ERUI_TALK_ROW_MAX_CODE_UNITS 159u

/* Fill `page` (0 = the first page) with ERNativeUIFork_TalkAddRow. */
typedef void(ERUI_CALL* ERUI_TalkBuildFn)(void* user, ERUI_TalkMenu* menu, uint32_t page);
/* The player chose `row` (your own id) on `page`. Without a navigation call
 * the same page is built and shown again (so a toggle shows its new value). */
typedef void(ERUI_CALL* ERUI_TalkChooseFn)(
    void* user, ERUI_TalkMenu* menu, uint32_t page, uint32_t row);

/* Adds a row to the top list. Any thread, any time. */
typedef ERUI_Result(ERUI_CALL* ERNativeUIFork_TalkRegister_Fn)(const ERUI_Utf16View* name,
    ERUI_TalkBuildFn build, ERUI_TalkChooseFn choose, void* user, ERUI_TalkEntryHandle* out);
typedef ERUI_Result(ERUI_CALL* ERNativeUIFork_TalkUnregister_Fn)(ERUI_TalkEntryHandle entry);
/* Inside build only. */
typedef ERUI_Result(ERUI_CALL* ERNativeUIFork_TalkAddRow_Fn)(
    ERUI_TalkMenu* menu, uint32_t row, const ERUI_Utf16View* text);
/* Inside build or choose. OpenPage pushes (Back returns), ReplacePage
 * swaps the shown page, Back goes up one level, Close closes the menu. */
typedef ERUI_Result(ERUI_CALL* ERNativeUIFork_TalkOpenPage_Fn)(ERUI_TalkMenu* menu, uint32_t page);
typedef ERUI_Result(ERUI_CALL* ERNativeUIFork_TalkReplacePage_Fn)(ERUI_TalkMenu* menu, uint32_t page);
typedef ERUI_Result(ERUI_CALL* ERNativeUIFork_TalkBack_Fn)(ERUI_TalkMenu* menu);
typedef ERUI_Result(ERUI_CALL* ERNativeUIFork_TalkClose_Fn)(ERUI_TalkMenu* menu);
/* Opens the menu on the next frame it may (in a game, no other menu open):
 * at `entry`'s first page, or the top list for ERUI_INVALID_TALK_ENTRY. */
typedef ERUI_Result(ERUI_CALL* ERNativeUIFork_TalkOpen_Fn)(ERUI_TalkEntryHandle entry);
/* As TalkOpen, at `page` of `entry` (r157): Back from it goes to the
 * entry's first page; page 0 is TalkOpen. */
typedef ERUI_Result(ERUI_CALL* ERNativeUIFork_TalkOpenAt_Fn)(
    ERUI_TalkEntryHandle entry, uint32_t page);
/* 1 while the talk menu is open, else 0. */
typedef uint32_t(ERUI_CALL* ERNativeUIFork_TalkIsOpen_Fn)(void);

#ifdef __cplusplus
}
#endif

#endif
