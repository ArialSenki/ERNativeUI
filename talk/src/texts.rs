//! Our own texts in the talk menu (from NPC Spawner's texts.rs). The game
//! reads every message through MsgRepositoryImp's lookup (WW 2.7.2.0
//! 0x142671370, found by bytes in all supported versions):
//!     lookup(repo, bnd, category, id): bnd < [repo+10h] && category < [repo+14h]
//!         -> file = [[repo+8] + bnd*8][category]; jmp fmg_lookup(file, id)
//! While the talk menu is open its first 12 bytes become `mov rax, hook;
//! jmp rax`; the hook answers our reserved ids and otherwise does exactly what
//! the original did (the code is that short, so no trampoline is needed).
//! Only then: other mods (NativeTweaks' shop, NPC Spawner) look this function
//! up by its bytes. If someone else has changed them, we leave them alone.

use std::{
    cell::UnsafeCell,
    sync::{
        Mutex,
        atomic::{AtomicBool, AtomicUsize, Ordering},
    },
};

use crate::mem;
use windows::Win32::System::{
    Diagnostics::Debug::FlushInstructionCache,
    Memory::{PAGE_EXECUTE_READWRITE, PAGE_PROTECTION_FLAGS, VirtualProtect},
    Threading::GetCurrentProcess,
};

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
static FMG_LOOKUP: AtomicUsize = AtomicUsize::new(0);
/// Address of MsgRepositoryImp::LookupEntry (0 = not found yet).
static AT: AtomicUsize = AtomicUsize::new(0);
/// Its first 12 bytes, put back when the menu closes.
static ORIGINAL: Mutex<[u8; 12]> = Mutex::new([0; 12]);
static HOOKED: AtomicBool = AtomicBool::new(false);

pub fn ready() -> bool {
    AT.load(Ordering::Acquire) != 0
}

fn write_code(at: usize, bytes: &[u8; 12]) -> bool {
    let mut old = PAGE_PROTECTION_FLAGS(0);
    unsafe {
        if VirtualProtect(at as *const _, 12, PAGE_EXECUTE_READWRITE, &mut old).is_err() {
            return false;
        }
        std::ptr::copy_nonoverlapping(bytes.as_ptr(), at as *mut u8, 12);
        let mut ignored = PAGE_PROTECTION_FLAGS(0);
        let _ = VirtualProtect(at as *const _, 12, old, &mut ignored);
        let _ = FlushInstructionCache(GetCurrentProcess(), Some(at as *const _), 12);
    }
    true
}

fn our_patch() -> [u8; 12] {
    let mut patch = [0u8; 12];
    patch[0] = 0x48;
    patch[1] = 0xB8;
    patch[2..10].copy_from_slice(&(hook_entry as *const () as usize as u64).to_le_bytes());
    patch[10] = 0xFF;
    patch[11] = 0xE0;
    patch
}

/// Puts our hook in while the menu is open. False when the lookup is not
/// known or someone else changed its bytes since we found it.
pub fn hook() -> bool {
    if HOOKED.load(Ordering::Acquire) {
        return true;
    }
    let at = AT.load(Ordering::Acquire);
    if at == 0 || !mem::readable(at, 12) {
        return false;
    }
    let now = unsafe { *(at as *const [u8; 12]) };
    if now != *ORIGINAL.lock().unwrap_or_else(|e| e.into_inner()) {
        crate::log_warn("talk menu: the message lookup was changed by another mod; not hooking it now");
        return false;
    }
    if !write_code(at, &our_patch()) {
        crate::log_error("talk menu: VirtualProtect failed on the message lookup");
        return false;
    }
    HOOKED.store(true, Ordering::Release);
    true
}

/// Puts the message lookup back (if it still has our hook).
pub fn unhook() {
    if !HOOKED.swap(false, Ordering::AcqRel) {
        return;
    }
    let at = AT.load(Ordering::Acquire);
    if unsafe { *(at as *const [u8; 12]) } == our_patch() {
        let original = *ORIGINAL.lock().unwrap_or_else(|e| e.into_inner());
        write_code(at, &original);
    }
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

extern "C" fn hook_entry(repo: usize, bnd: u32, category: u32, id: i32) -> *const u16 {
    if (BASE_ID..BASE_ID + SLOTS as i32).contains(&id) {
        return unsafe { (*TEXTS[(id - BASE_ID) as usize].0.get()).as_ptr() };
    }
    unsafe {
        if bnd >= *((repo + 0x10) as *const u32) || category >= *((repo + 0x14) as *const u32) {
            return std::ptr::null();
        }
        let banks = *((repo + 8) as *const usize);
        let files = *((banks + bnd as usize * 8) as *const usize);
        if files == 0 {
            return std::ptr::null();
        }
        let file = *((files + category as usize * 8) as *const usize);
        if file == 0 {
            return std::ptr::null();
        }
        let f: extern "C" fn(usize, i32) -> *const u16 =
            std::mem::transmute(FMG_LOOKUP.load(Ordering::Relaxed));
        f(file, id)
    }
}

/// Finds the message lookup by its bytes. False when the code is not the
/// expected one right now (another mod may have it hooked for a moment, as
/// NPC Spawner does while its own menu is open): try again later.
pub fn install() -> bool {
    if ready() {
        return true;
    }
    let pattern = mem::pattern(
        "3B 51 10 73 29 44 3B 41 14 73 23 48 8B 41 08 8B D2 48 8B 0C D0 48 85 C9 74 14 41 8B C0 48 8B 0C C1 48 85 C9 74 08 41 8B D1 E9",
    );
    let Some(at) = mem::find_in_text(&pattern) else { return false };
    let rel = unsafe { ((at + 0x2A) as *const i32).read_unaligned() } as isize;
    let fmg = (at as isize + 0x2E + rel) as usize;
    FMG_LOOKUP.store(fmg, Ordering::Relaxed);
    *ORIGINAL.lock().unwrap_or_else(|e| e.into_inner()) = unsafe { *(at as *const [u8; 12]) };
    AT.store(at, Ordering::Release);
    let image = mem::image().0;
    crate::log_info(&format!(
        "talk menu: message lookup at +{:#X} (file lookup +{:#X}); hooked only while the menu is open",
        at - image,
        fmg - image
    ));
    true
}
