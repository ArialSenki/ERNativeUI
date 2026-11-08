//! The shared talk menu of the NativeTweaks fork of ERNativeUI.
//!
//! A key (F11 by default, ERNativeUI.ini [TalkMenu] Key; hotkey.rs), in a
//! game with no other menu open, opens the game's own
//! talk-list dialog, as a merchant's, with one row per mod: the Mod Settings
//! pages of every ERNativeUI provider (shown by src/talk_menu.cpp, values
//! changed live) and the talk pages of mods that registered through the
//! ERNativeUIFork_Talk* exports (documented in docs/fork/api.md), and Leave.
//! Choosing a mod's row opens its pages, filled from callbacks on the game
//! thread.
//!
//! The dialog is driven through TalkScript (fromsoftware-rs, as in its
//! examples/invoke-esd and NPC Spawner's dialog.rs), with our own texts
//! served by a chained hook on the game's message lookup (texts.rs).

mod hotkey;
mod mem;
mod nav;
mod texts;

use std::{
    ffi::c_void,
    panic::AssertUnwindSafe,
    sync::{
        Mutex, OnceLock,
        atomic::{AtomicBool, AtomicPtr, AtomicU64, AtomicUsize, Ordering},
    },
    time::Duration,
};

use eldenring::{
    cs::{BlockId, CSMenuManImp, CSTaskGroupIndex, CSTaskImp, EzStateInvokeError, MenuType, TalkScript, WorldChrMan},
    ez_state::EzStateValue,
    fd4::FD4TaskData,
};
use fromsoftware_shared::{FromStatic, SharedTaskImpExt};
use windows::Win32::{
    System::Threading::GetCurrentProcessId,
    UI::{
        Input::KeyboardAndMouse::GetAsyncKeyState,
        WindowsAndMessaging::{GetForegroundWindow, GetWindowThreadProcessId},
    },
};

use nav::{Level, Nav, Row};

// ---- results (ERUI_Result values of erui.h)

pub const ERUI_OK: u32 = 0;
pub const ERUI_INVALID_ARGUMENT: u32 = 1;
pub const ERUI_HOST_NOT_READY: u32 = 2;
pub const ERUI_INVALID_HANDLE: u32 = 6;
pub const ERUI_INTERNAL_ERROR: u32 = 11;
pub const ERUI_QUEUE_FULL: u32 = 12;
pub const ERUI_NOT_FOUND: u32 = 16;

// ---- ESD commands (fromsoftware-rs examples/invoke-esd)

const SHOW_SHOP_MESSAGE: i32 = 10;
const ADD_TALK_LIST_DATA: i32 = 19;
const CLEAR_TALK_LIST_DATA: i32 = 20;
const GET_TALK_LIST_MENU_RESULT: i32 = 23;
const CHECK_SPECIFIC_PERSON_GENERIC_DIALOG_IS_OPEN: i32 = 58;
const CHECK_SPECIFIC_PERSON_MENU_IS_OPEN: i32 = 59;
/// The game's own "Leave" text (localized).
const MSG_LEAVE: i32 = 20000009;
/// Most rows on the top list (it also has Leave).
const MAX_ENTRIES: usize = 48;
/// Rows of a provider page (one more slot is its Back row).
const MAX_ROWS: usize = texts::SLOTS - 1;

// ---- logging into ERNativeUI.log (the C++ side hands us its sink)

type LogFn = unsafe extern "C" fn(level: u32, text: *const u8, len: usize);
static LOG: AtomicUsize = AtomicUsize::new(0);

fn log_at(level: u32, text: &str) {
    let f = LOG.load(Ordering::Acquire);
    if f != 0 {
        let f: LogFn = unsafe { std::mem::transmute(f) };
        unsafe { f(level, text.as_ptr(), text.len()) };
    }
}
pub fn log_info(text: &str) {
    log_at(0, text);
}
pub fn log_warn(text: &str) {
    log_at(1, text);
}
pub fn log_error(text: &str) {
    log_at(2, text);
}

// ---- providers

/// Fills a page: call erui_talk_add_row (and optionally a navigation call).
pub type BuildFn = unsafe extern "C" fn(user: *mut c_void, menu: *mut Ctx, page: u32);
/// The player chose `row` of `page`. Without a navigation call the same page
/// is built and shown again.
pub type ChooseFn = unsafe extern "C" fn(user: *mut c_void, menu: *mut Ctx, page: u32, row: u32);

#[derive(Clone)]
struct Entry {
    id: u64,
    /// A provider's Mod Settings pages (talk_menu.cpp); a talk entry of the
    /// same name hides it.
    settings: bool,
    name: Vec<u16>,
    build: BuildFn,
    choose: ChooseFn,
    user: usize,
}

static ENTRIES: Mutex<Vec<Entry>> = Mutex::new(Vec::new());
static NEXT_ID: AtomicU64 = AtomicU64::new(1);
/// Open request from erui_talk_open: u64::MAX none, 0 top list, else entry.
static PENDING_OPEN: AtomicU64 = AtomicU64::new(u64::MAX);
/// Frames an open request waits for the game (another menu still closing,
/// a loading screen) before it is dropped: about one second.
const REQUEST_FRAMES: u32 = 60;
/// Called on the game thread before a page is built, so the C++ side
/// can add rows for providers committed since (talk_menu.cpp).
type BeforeOpenFn = unsafe extern "C" fn();
static BEFORE_OPEN: AtomicUsize = AtomicUsize::new(0);
static HOTKEY: OnceLock<hotkey::Hotkey> = OnceLock::new();
static STARTED: AtomicBool = AtomicBool::new(false);
/// The context of the callback being run (null outside callbacks).
static CURRENT: AtomicPtr<Ctx> = AtomicPtr::new(std::ptr::null_mut());

fn entry(id: u64) -> Option<Entry> {
    ENTRIES.lock().unwrap_or_else(|e| e.into_inner()).iter().find(|e| e.id == id).cloned()
}

/// What a callback hands back: rows (build) and where to go next.
pub struct Ctx {
    rows: Vec<(u32, Vec<u16>)>,
    nav: Nav,
    building: bool,
    overflow: bool,
}

fn text_of(text: *const u16, len: usize) -> Option<Vec<u16>> {
    if text.is_null() {
        return (len == 0).then(Vec::new);
    }
    if len > 4096 {
        return None;
    }
    let s = unsafe { std::slice::from_raw_parts(text, len) };
    if s.contains(&0) {
        return None;
    }
    Some(s.to_vec())
}

fn current(ctx: *mut Ctx) -> Option<&'static mut Ctx> {
    if ctx.is_null() || CURRENT.load(Ordering::Acquire) != ctx {
        return None;
    }
    Some(unsafe { &mut *ctx })
}

// ---- C interface (wrapped by src/talk_menu.cpp)

/// Starts the menu (once): the log sink, the before-open hook (may be null),
/// the key, and the game's frame task, from a background thread.
#[unsafe(no_mangle)]
pub extern "C" fn erui_talk_start(
    log: Option<LogFn>,
    before_open: Option<BeforeOpenFn>,
    install_text_hook: Option<texts::InstallFn>,
    key: *const u16,
    key_len: usize,
) -> u32 {
    if let Some(f) = install_text_hook {
        texts::set_installer(f);
    }
    if let Some(f) = log {
        LOG.store(f as usize, Ordering::Release);
    }
    let text = text_of(key, key_len).map(|k| String::from_utf16_lossy(&k)).unwrap_or_default();
    let parsed = if text.trim().is_empty() { None } else { hotkey::parse(&text) };
    if parsed.is_none() && !text.trim().is_empty() {
        log_warn(&format!("talk menu: [TalkMenu] Key = \"{text}\" is not a key this build knows; using {}", hotkey::DEFAULT));
    }
    let _ = HOTKEY.set(parsed.or_else(|| hotkey::parse(hotkey::DEFAULT)).expect("default key"));
    if let Some(f) = before_open {
        BEFORE_OPEN.store(f as usize, Ordering::Release);
    }
    if STARTED.swap(true, Ordering::AcqRel) {
        return ERUI_OK;
    }
    let spawned = std::thread::Builder::new().name("ERNativeUI talk menu".into()).spawn(|| {
        if std::panic::catch_unwind(start_task).is_err() {
            log_error("talk menu: start-up failed (this game version is not supported by the dialog engine)");
        }
    });
    if spawned.is_err() {
        return ERUI_INTERNAL_ERROR;
    }
    ERUI_OK
}

fn start_task() {
    // The message lookup is found now (or before an open, if not yet).
    texts::install();
    let start = std::time::Instant::now();
    let task = loop {
        match CSTaskImp::wait_for_instance(Duration::ZERO) {
            Ok(t) => break t,
            Err(_) if start.elapsed() < Duration::from_secs(300) => std::thread::sleep(Duration::from_millis(250)),
            Err(e) => {
                log_error(&format!("talk menu: the game's task scheduler never appeared ({e:?}); the talk menu is off"));
                return;
            }
        }
    };
    std::mem::forget(task.run_recurring(frame as fn(&FD4TaskData), CSTaskGroupIndex::FrameBegin));
    log_info(&format!(
        "talk menu: frame task installed; {} opens it ({} talk rows registered so far)",
        HOTKEY.get().map_or("?", |h| h.name.as_str()),
        ENTRIES.lock().unwrap_or_else(|e| e.into_inner()).len(),
    ));
}

#[unsafe(no_mangle)]
pub extern "C" fn erui_talk_register(
    name: *const u16,
    name_len: usize,
    build: Option<BuildFn>,
    choose: Option<ChooseFn>,
    user: *mut c_void,
    out: *mut u64,
) -> u32 {
    register(name, name_len, build, choose, user, out, false)
}

/// A provider's Mod Settings pages (talk_menu.cpp only).
#[unsafe(no_mangle)]
pub extern "C" fn erui_talk_register_settings(
    name: *const u16,
    name_len: usize,
    build: Option<BuildFn>,
    choose: Option<ChooseFn>,
    user: *mut c_void,
    out: *mut u64,
) -> u32 {
    register(name, name_len, build, choose, user, out, true)
}

fn register(
    name: *const u16,
    name_len: usize,
    build: Option<BuildFn>,
    choose: Option<ChooseFn>,
    user: *mut c_void,
    out: *mut u64,
    settings: bool,
) -> u32 {
    let (Some(build), Some(choose)) = (build, choose) else { return ERUI_INVALID_ARGUMENT };
    let Some(name) = text_of(name, name_len).filter(|n| !n.is_empty()) else { return ERUI_INVALID_ARGUMENT };
    let mut entries = ENTRIES.lock().unwrap_or_else(|e| e.into_inner());
    if entries.len() >= MAX_ENTRIES {
        return ERUI_QUEUE_FULL;
    }
    let id = NEXT_ID.fetch_add(1, Ordering::Relaxed);
    log_info(&format!(
        "talk menu: row \"{}\" registered (id {id}{})",
        String::from_utf16_lossy(&name),
        if settings { ", Mod Settings pages" } else { "" }
    ));
    entries.push(Entry { id, settings, name, build, choose, user: user as usize });
    if !out.is_null() {
        unsafe { *out = id };
    }
    ERUI_OK
}

#[unsafe(no_mangle)]
pub extern "C" fn erui_talk_unregister(id: u64) -> u32 {
    let mut entries = ENTRIES.lock().unwrap_or_else(|e| e.into_inner());
    let before = entries.len();
    entries.retain(|e| e.id != id);
    if entries.len() == before { ERUI_INVALID_HANDLE } else { ERUI_OK }
}

#[unsafe(no_mangle)]
pub extern "C" fn erui_talk_add_row(ctx: *mut Ctx, row: u32, text: *const u16, len: usize) -> u32 {
    let Some(ctx) = current(ctx) else { return ERUI_INVALID_HANDLE };
    if !ctx.building {
        return ERUI_INVALID_HANDLE;
    }
    let Some(text) = text_of(text, len) else { return ERUI_INVALID_ARGUMENT };
    if ctx.rows.len() >= MAX_ROWS {
        ctx.overflow = true;
        return ERUI_QUEUE_FULL;
    }
    ctx.rows.push((row, text));
    ERUI_OK
}

fn set_nav(ctx: *mut Ctx, nav: Nav) -> u32 {
    let Some(ctx) = current(ctx) else { return ERUI_INVALID_HANDLE };
    ctx.nav = nav;
    ERUI_OK
}

#[unsafe(no_mangle)]
pub extern "C" fn erui_talk_open_page(ctx: *mut Ctx, page: u32) -> u32 {
    set_nav(ctx, Nav::Open(page))
}

#[unsafe(no_mangle)]
pub extern "C" fn erui_talk_replace_page(ctx: *mut Ctx, page: u32) -> u32 {
    set_nav(ctx, Nav::Replace(page))
}

#[unsafe(no_mangle)]
pub extern "C" fn erui_talk_back(ctx: *mut Ctx) -> u32 {
    set_nav(ctx, Nav::Back)
}

#[unsafe(no_mangle)]
pub extern "C" fn erui_talk_close(ctx: *mut Ctx) -> u32 {
    set_nav(ctx, Nav::Close)
}

/// Asks for the menu to open (at a provider's first page, or the top list
/// for 0) on the next frame where it may. Any thread.
#[unsafe(no_mangle)]
pub extern "C" fn erui_talk_open(id: u64) -> u32 {
    if !STARTED.load(Ordering::Acquire) {
        return ERUI_HOST_NOT_READY;
    }
    if id != 0 && entry(id).is_none() {
        return ERUI_NOT_FOUND;
    }
    PENDING_OPEN.store(id, Ordering::Release);
    ERUI_OK
}

/// The Back row's text (talk_menu.cpp, in the host's language; "Back" until
/// set). Called on the game thread before a page is built.
#[unsafe(no_mangle)]
pub extern "C" fn erui_talk_set_back_text(text: *const u16, len: usize) {
    if let Some(t) = text_of(text, len).filter(|t| !t.is_empty()) {
        *BACK_TEXT.lock().unwrap_or_else(|e| e.into_inner()) = t;
    }
}

static BACK_TEXT: Mutex<Vec<u16>> = Mutex::new(Vec::new());

fn back_text() -> Vec<u16> {
    let t = BACK_TEXT.lock().unwrap_or_else(|e| e.into_inner());
    if t.is_empty() { "Back".encode_utf16().collect() } else { t.clone() }
}

/// 1 while the menu is open.
#[unsafe(no_mangle)]
pub extern "C" fn erui_talk_is_open() -> u32 {
    IS_OPEN.load(Ordering::Acquire) as u32
}

static IS_OPEN: AtomicBool = AtomicBool::new(false);

// ---- the frame task

#[derive(Clone, Copy, PartialEq, Eq)]
enum State {
    Idle,
    Enter,
    Open,
}

struct Menu {
    talk: Option<Box<TalkScript>>,
    state: State,
    stack: Vec<Level>,
    rows: Vec<Row>,
    key_prev: bool,
    /// An open request (erui_talk_open) waiting for its frame, and how many
    /// frames it may still wait.
    request: u64,
    request_frames: u32,
}

// Only touched from the game's frame task.
unsafe impl Send for Menu {}

static MENU: Mutex<Option<Menu>> = Mutex::new(None);

fn key_down(key: u16) -> bool {
    (unsafe { GetAsyncKeyState(key as i32) } as u16) & 0x8000 != 0
}

fn game_in_front() -> bool {
    unsafe {
        let mut pid = 0u32;
        GetWindowThreadProcessId(GetForegroundWindow(), Some(&mut pid));
        pid == GetCurrentProcessId()
    }
}

/// The menu job of the top in-game menu (inventory, map, pause...), 0 when
/// none is open. CSPopupMenu+0xB0 (fromsoftware-rs menu_man.rs).
fn top_menu_job() -> Option<usize> {
    let man = unsafe { CSMenuManImp::instance() }.ok()?;
    let popup = man.popup_menu?.as_ptr() as usize;
    mem::read_usize(popup + 0xB0)
}

fn frame(_: &FD4TaskData) {
    if std::panic::catch_unwind(AssertUnwindSafe(frame_inner)).is_err() {
        log_error("talk menu: panic in the frame task (caught); menu closed");
        IS_OPEN.store(false, Ordering::Release);
        let mut g = MENU.lock().unwrap_or_else(|e| e.into_inner());
        if let Some(m) = g.as_mut() {
            m.state = State::Idle;
            m.stack.clear();
            m.rows.clear();
            m.request = u64::MAX;
        }
    }
}

fn frame_inner() {
    let mut guard = MENU.lock().unwrap_or_else(|e| e.into_inner());
    let m = guard.get_or_insert_with(|| Menu {
        talk: None,
        state: State::Idle,
        stack: Vec::new(),
        rows: Vec::new(),
        key_prev: false,
        request: u64::MAX,
        request_frames: 0,
    });

    let down = HOTKEY
        .get()
        .is_some_and(|h| key_down(h.key) && h.modifiers.iter().all(|m| key_down(*m)) && game_in_front());
    let pressed = down && !m.key_prev;
    m.key_prev = down;
    let new_request = PENDING_OPEN.swap(u64::MAX, Ordering::AcqRel);
    if new_request != u64::MAX && m.state == State::Idle {
        // (a request while the menu is open is dropped, as it always was)
        m.request = new_request;
        m.request_frames = REQUEST_FRAMES;
    } else if m.request != u64::MAX {
        m.request_frames = m.request_frames.saturating_sub(1);
        if m.request_frames == 0 {
            log_info("talk menu: an open request was dropped (the game was busy for a second)");
            m.request = u64::MAX;
        }
    }
    let request = m.request;

    let Ok(world) = (unsafe { WorldChrMan::instance() }) else {
        close(m);
        return;
    };
    let Some(player) = world.main_player.as_ref() else {
        close(m);
        return;
    };
    let handle = player.chr_ins.field_ins_handle;
    let alive = player.chr_ins.modules.data.hp > 0;

    if m.state == State::Idle && (pressed || request != u64::MAX) && !alive {
        // not while dying or dead (a request waits, as for another menu)
        if pressed {
            log_info("talk menu: not opened, the player is dead");
        }
        return;
    }
    if m.state == State::Idle && (pressed || request != u64::MAX) {
        let top = top_menu_job();
        if top.is_some_and(|j| j != 0) {
            // once per press or request, not on every frame a request waits
            if pressed || m.request_frames == REQUEST_FRAMES {
                log_info(&format!("talk menu: not opened, another menu is open (top menu job {:#X})", top.unwrap_or(0)));
            }
            return;
        }
        if !texts::install() {
            // (a scan of the game's code: not again on every waiting frame)
            m.request = u64::MAX;
            log_warn("talk menu: not opened, the game's message lookup was not found");
            return;
        }
        m.request = u64::MAX;
        m.stack = vec![Level::Top];
        if request != u64::MAX && request != 0 {
            m.stack.push(Level::Provider { entry: request, page: 0 });
        }
        log_info(&format!(
            "talk menu: opening ({}; top menu job {:#X})",
            if pressed { HOTKEY.get().map_or("key", |h| h.name.as_str()) } else { "request" },
            top.unwrap_or(0)
        ));
        m.state = State::Enter;
    }
    if m.state == State::Idle {
        return;
    }

    let talk = m.talk.get_or_insert_with(|| Box::new(TalkScript::new(BlockId::none(), 1000, handle)));
    talk.npc_talk.base.field_ins_handle = handle;
    let result = match m.state {
        State::Enter => enter(m),
        State::Open => poll(m, alive),
        State::Idle => Ok(()),
    };
    if let Err(e) = result {
        log_error(&format!("talk menu: dialog error {e:?}; closed"));
        close(m);
    }
}

fn close(m: &mut Menu) {
    if m.state != State::Idle {
        m.state = State::Idle;
        m.stack.clear();
        m.rows.clear();
    }
    IS_OPEN.store(false, Ordering::Release);
}

fn before_open() {
    let f = BEFORE_OPEN.load(Ordering::Acquire);
    if f != 0 {
        let f: BeforeOpenFn = unsafe { std::mem::transmute(f) };
        unsafe { f() };
    }
}

/// The top list: Mod Settings pages first, then talk entries; a provider's
/// Mod Settings pages are left out when it registered talk pages of the same
/// name (NPC Spawner).
fn top_entries() -> Vec<Entry> {
    let entries = ENTRIES.lock().unwrap_or_else(|e| e.into_inner()).clone();
    let mut out: Vec<Entry> = entries
        .iter()
        .filter(|e| !e.settings || !entries.iter().any(|t| !t.settings && t.name == e.name))
        .cloned()
        .collect();
    out.sort_by_key(|e| (!e.settings, e.id));
    out
}

/// Runs a provider callback with a fresh context.
fn call(build: bool, e: &Entry, page: u32, row: u32) -> Ctx {
    let mut ctx = Ctx { rows: Vec::new(), nav: Nav::Stay, building: build, overflow: false };
    let p: *mut Ctx = &mut ctx;
    CURRENT.store(p, Ordering::Release);
    unsafe {
        if build {
            (e.build)(e.user as *mut c_void, p, page);
        } else {
            (e.choose)(e.user as *mut c_void, p, page, row);
        }
    }
    CURRENT.store(std::ptr::null_mut(), Ordering::Release);
    ctx
}

/// Builds and shows the page on top of the stack.
fn enter(m: &mut Menu) -> Result<(), EzStateInvokeError> {
    // A provider's build may navigate (Back, another page...): follow it a
    // few times at most.
    // New Mod Settings rows and the host's words (Back) before any page.
    before_open();
    let mut rows: Vec<(Row, Vec<u16>)> = Vec::new();
    let mut built = false;
    for _ in 0..8 {
        let Some(level) = m.stack.last().copied() else {
            close(m);
            return Ok(());
        };
        rows.clear();
        match level {
            Level::Top => {
                for e in top_entries() {
                    rows.push((Row::Entry(e.id), e.name));
                }
                rows.push((Row::Leave, Vec::new()));
                built = true;
                break;
            }
            Level::Provider { entry: id, page } => {
                let Some(e) = entry(id) else {
                    m.stack.truncate(1);
                    continue;
                };
                let ctx = call(true, &e, page, 0);
                if ctx.overflow {
                    log_warn(&format!(
                        "talk menu: \"{}\" page {page} has more than {MAX_ROWS} rows; the rest are not shown",
                        String::from_utf16_lossy(&e.name)
                    ));
                }
                if ctx.nav != Nav::Stay {
                    if !nav::apply(&mut m.stack, ctx.nav) {
                        close(m);
                        return Ok(());
                    }
                    continue;
                }
                for (row, text) in ctx.rows {
                    rows.push((Row::Provider(row), text));
                }
                rows.push((Row::Back, back_text()));
                built = true;
                break;
            }
        }
    }
    if !built {
        log_warn("talk menu: a page kept navigating while it was built; closed");
        close(m);
        return Ok(());
    }
    if !texts::hook() {
        log_warn("talk menu: our texts cannot be shown; closed");
        close(m);
        return Ok(());
    }
    let talk = m.talk.as_mut().expect("talk script");
    talk.event(CLEAR_TALK_LIST_DATA)?;
    m.rows.clear();
    let mut slot = 0;
    for (row, text) in rows {
        let msg = if row == Row::Leave {
            MSG_LEAVE
        } else if slot < texts::SLOTS {
            texts::set(slot, &text);
            slot += 1;
            texts::id(slot - 1)
        } else {
            continue;
        };
        m.rows.push(row);
        let index = m.rows.len() as i32;
        talk.event((
            ADD_TALK_LIST_DATA,
            [EzStateValue::Int32(index), EzStateValue::Int32(msg), EzStateValue::Int32(-1)],
        ))?;
    }
    talk.event((SHOW_SHOP_MESSAGE, [EzStateValue::Int32(1)]))?;
    m.state = State::Open;
    IS_OPEN.store(true, Ordering::Release);
    Ok(())
}

/// Waits for the player's choice and acts on it.
fn poll(m: &mut Menu, alive: bool) -> Result<(), EzStateInvokeError> {
    let talk = m.talk.as_mut().expect("talk script");
    let menu: i32 = talk
        .env((
            CHECK_SPECIFIC_PERSON_MENU_IS_OPEN,
            [EzStateValue::Int32(MenuType::TalkList as i32), EzStateValue::Int32(0)],
        ))?
        .into();
    let generic: i32 = talk.env((CHECK_SPECIFIC_PERSON_GENERIC_DIALOG_IS_OPEN, [EzStateValue::Int32(0)]))?.into();
    if menu == 1 && generic == 0 {
        return Ok(());
    }
    let r: i32 = talk.env(GET_TALK_LIST_MENU_RESULT)?.into();
    let chosen = (r >= 1).then(|| m.rows.get(r as usize - 1).copied()).flatten();
    let level = m.stack.last().copied();
    // Closed without a choice because the player died: close instead of
    // going up a level, which would show the list again. (Not the top menu
    // job: the talk list's own job may still be closing after Esc.)
    if chosen.is_none() && !alive {
        log_info("talk menu: closed, the player died");
        close(m);
        return Ok(());
    }
    let next = match (chosen, level) {
        (Some(Row::Entry(id)), _) => Nav::Enter(id),
        (Some(Row::Provider(row)), Some(Level::Provider { entry: id, page })) => match entry(id) {
            Some(e) => call(false, &e, page, row).nav,
            None => Nav::Back,
        },
        (Some(Row::Leave), _) | (None, Some(Level::Top)) => Nav::Close,
        // Back, or cancelled (Esc) on a provider page: one level up.
        _ => Nav::Back,
    };
    if nav::apply(&mut m.stack, next) {
        m.state = State::Enter;
    } else {
        close(m);
    }
    Ok(())
}
