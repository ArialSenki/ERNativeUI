//! Our own texts in the talk menu. The game reads every message through
//! MsgRepositoryImp::LookupEntry (WW 2.7.2.0 0x142671370). src/talk_menu.cpp
//! hooks it with a chained SafetyHook inline hook the first time the menu
//! opens and keeps it: the hook answers our reserved ids (erui_talk_text) and
//! passes everything else on, so it lives with other mods that hook the same
//! function, before or after us (Glorious Merchant hooks it with MinHook at
//! start-up; NativeTweaks and NPC Spawner call it).
//!
//! The function is found by its bytes or, when another mod has already
//! hooked it, through the item-name getters that call it.

use std::{
    cell::UnsafeCell,
    sync::atomic::{AtomicBool, AtomicUsize, Ordering},
};

use crate::mem;

/// First id of our texts (any category). NPC Spawner uses 1_990_000_000.
pub const BASE_ID: i32 = 1_991_000_000;
pub const SLOTS: usize = 64;
/// UTF-16 units per text, terminator included.
pub const SLOT_LEN: usize = 160;

struct Slot(UnsafeCell<[u16; SLOT_LEN]>);
// Written by the frame task, read by the game's lookups; a torn read can only
// show a mixed text for one frame.
unsafe impl Sync for Slot {}

static TEXTS: [Slot; SLOTS] = [const { Slot(UnsafeCell::new([0; SLOT_LEN])) }; SLOTS];
/// Address of LookupEntry (0 = not found yet).
static AT: AtomicUsize = AtomicUsize::new(0);
static HOOKED: AtomicBool = AtomicBool::new(false);
static FAILED: AtomicBool = AtomicBool::new(false);

/// Installs the chained hook at an address (C++, talk_menu.cpp). 1 = done.
pub type InstallFn = unsafe extern "C" fn(at: usize) -> u32;
static INSTALL: AtomicUsize = AtomicUsize::new(0);

pub fn set_installer(f: InstallFn) {
    INSTALL.store(f as usize, Ordering::Release);
}

/// The text for one of our ids, or null (the hook passes the call on).
#[unsafe(no_mangle)]
pub extern "C" fn erui_talk_text(id: i32) -> *const u16 {
    if (BASE_ID..BASE_ID + SLOTS as i32).contains(&id) {
        unsafe { (*TEXTS[(id - BASE_ID) as usize].0.get()).as_ptr() }
    } else {
        std::ptr::null()
    }
}

/// Our texts are served (the hook is in). Installs it the first time.
pub fn hook() -> bool {
    if HOOKED.load(Ordering::Acquire) {
        return true;
    }
    if FAILED.load(Ordering::Acquire) || !install() {
        return false;
    }
    let f = INSTALL.load(Ordering::Acquire);
    if f == 0 {
        return false;
    }
    let f: InstallFn = unsafe { std::mem::transmute(f) };
    if unsafe { f(AT.load(Ordering::Acquire)) } == 0 {
        FAILED.store(true, Ordering::Release);
        crate::log_error("talk menu: the message lookup could not be hooked; the menu stays off");
        return false;
    }
    HOOKED.store(true, Ordering::Release);
    crate::log_info("talk menu: message lookup hooked (chained, kept from now on)");
    true
}

pub fn id(slot: usize) -> i32 {
    BASE_ID + slot as i32
}

/// Sets the text of `slot` (cut to SLOT_LEN - 1 UTF-16 units).
pub fn set(slot: usize, text: &[u16]) {
    let Some(s) = TEXTS.get(slot) else { return };
    let buf = unsafe { &mut *s.0.get() };
    let n = text.len().min(SLOT_LEN - 1);
    buf[..n].copy_from_slice(&text[..n]);
    buf[n] = 0;
}

fn rel32_target(call: usize) -> usize {
    let rel = unsafe { ((call + 1) as *const i32).read_unaligned() } as isize;
    (call as isize + 5 + rel) as usize
}

/// Finds LookupEntry: its own bytes, or the call in the item-name getters
/// (`mov rdi,rcx; lea r8d,[rdx+6Fh]; call LookupEntry`, two getters, same
/// target), which still works when another mod hooked its first bytes.
pub fn install() -> bool {
    if AT.load(Ordering::Acquire) != 0 {
        return true;
    }
    let own = mem::pattern(
        "3B 51 10 73 29 44 3B 41 14 73 23 48 8B 41 08 8B D2 48 8B 0C D0 48 85 C9 74 14 41 8B C0 48 8B 0C C1 48 85 C9 74 08 41 8B D1 E9",
    );
    let (at, how) = match mem::find_in_text(&own) {
        Some(at) => (at, "by its bytes"),
        None => {
            let callers = mem::find_all_in_text(&mem::pattern("48 8B F9 44 8D 42 6F E8"), 4);
            let targets: Vec<usize> = callers.iter().map(|c| rel32_target(c + 7)).collect();
            match targets.first() {
                Some(&t) if targets.iter().all(|&x| x == t) && mem::readable(t, 16) => {
                    (t, "through the item-name getters (already hooked by another mod)")
                }
                _ => {
                    crate::log_warn(&format!(
                        "talk menu: message lookup not found ({} getter calls); trying again later",
                        callers.len()
                    ));
                    return false;
                }
            }
        }
    };
    AT.store(at, Ordering::Release);
    crate::log_info(&format!("talk menu: message lookup at +{:#X}, found {how}", at - mem::image().0));
    true
}
