//! The "Mod Settings" row: opens the game's Configuration screen the way an
//! NPC talk command opens a menu (r157 research, PROGRESS.md).
//!
//! WW 2.7.2.0: the game wraps a job factory `DLRefPtr<MenuJob>(ComponentStack&)`
//! in a std::function whose impl is a plain function pointer (vtable
//! 0x2AC61B0) and hands it to 0x8141F0(out, &function), which creates the job
//! and starts it in CSMenuManImp's job manager (rebirth, talk command 113,
//! does exactly that from 0x8121D0). The in-game Configuration job factory is
//! 0x809010 (0x809050 is the title screen's). Ctrl+F12 tests on 07-11 opened
//! it 22 times without a fault. The handle returned does not own the job.
//!
//! All three are found by bytes; the same patterns match once (factory) and
//! three times (callers, same vtable and same callee) in WW 2.7.0.0, 2.7.1.0,
//! 2.7.2.0 and JP 2.7.1.1, 2.7.2.1.

use crate::mem;

#[derive(Clone, Copy, Debug)]
pub struct Opener {
    pub start_job: usize,
    pub function_vtable: usize,
    pub option_job: usize,
}

/// `mov [rsp+8],rcx; push rbx; sub rsp,30h; mov [rsp+28h],-2; mov rbx,rcx;
/// mov [rsp+20h],0; mov r8b,1; call build; nop; mov [rsp+20h],1; mov rax,rbx;
/// add rsp,30h; pop rbx; ret` (the title-screen twin passes r8b 0).
const OPTION_JOB: &str = "48 89 4C 24 08 53 48 83 EC 30 48 C7 44 24 28 FE FF FF FF 48 8B D9 C7 44 24 20 00 00 00 00 41 B0 01 E8 ?? ?? ?? ?? 90 C7 44 24 20 01 00 00 00 48 8B C3 48 83 C4 30 5B C3";
/// `lea rax,[impl vtable]; mov [r11-58h],rax; lea rax,[factory];
/// mov [r11-50h],rax; lea rax,[r11-58h]; mov [r11-20h],rax;
/// lea rdx,[r11-58h]; call start_job`
const START_CALL: &str =
    "48 8D 05 ?? ?? ?? ?? 49 89 43 A8 48 8D 05 ?? ?? ?? ?? 49 89 43 B0 49 8D 43 A8 49 89 43 E0 49 8D 53 A8 E8";
/// start_job's prologue: `mov r11,rsp; push rdi; sub rsp,90h`.
const START_JOB_PROLOGUE: [u8; 11] = [0x4C, 0x8B, 0xDC, 0x57, 0x48, 0x81, 0xEC, 0x90, 0x00, 0x00, 0x00];

fn rel32(at: usize, next: usize) -> usize {
    let rel = unsafe { (at as *const i32).read_unaligned() } as isize;
    (next as isize + rel) as usize
}

pub fn find() -> Result<Opener, String> {
    let (base, size) = mem::image();
    if base == 0 {
        return Err("no game image".into());
    }
    let inside = |a: usize| a >= base && a < base + size;
    let jobs = mem::find_all_in_text(&mem::pattern(OPTION_JOB), 4);
    let [option_job] = jobs[..] else {
        return Err(format!("Configuration job factory: {} matches", jobs.len()));
    };
    let calls = mem::find_all_in_text(&mem::pattern(START_CALL), 16);
    if calls.is_empty() {
        return Err("menu job start: no caller found".into());
    }
    let mut found: Option<(usize, usize)> = None;
    for at in &calls {
        let vtable = rel32(at + 3, at + 7);
        let start = rel32(at + 0x23, at + 0x27);
        match found {
            None => found = Some((vtable, start)),
            Some(f) if f == (vtable, start) => {}
            Some(_) => return Err(format!("menu job start: {} callers disagree", calls.len())),
        }
    }
    let (function_vtable, start_job) = found.unwrap_or_default();
    if !inside(function_vtable) || !inside(start_job) || !mem::readable(start_job, START_JOB_PROLOGUE.len()) {
        return Err("menu job start: targets outside the game".into());
    }
    let prologue = unsafe { std::slice::from_raw_parts(start_job as *const u8, START_JOB_PROLOGUE.len()) };
    if prologue != START_JOB_PROLOGUE {
        return Err("menu job start: unexpected code".into());
    }
    // The impl vtable's first slot (copy) is game code.
    if !mem::read_usize(function_vtable).is_some_and(inside) {
        return Err("menu job start: unexpected function vtable".into());
    }
    Ok(Opener { start_job, function_vtable, option_job })
}
