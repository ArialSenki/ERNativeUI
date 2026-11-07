//! Memory helpers: readable checks and byte patterns in the game's .text.

use windows::Win32::System::{
    LibraryLoader::GetModuleHandleW,
    Memory::{MEM_COMMIT, MEMORY_BASIC_INFORMATION, PAGE_GUARD, PAGE_NOACCESS, VirtualQuery},
};

pub fn readable(addr: usize, len: usize) -> bool {
    if addr < 0x10000 {
        return false;
    }
    let mut info = MEMORY_BASIC_INFORMATION::default();
    let n = unsafe {
        VirtualQuery(Some(addr as *const core::ffi::c_void), &mut info, size_of::<MEMORY_BASIC_INFORMATION>())
    };
    if n == 0 || info.State != MEM_COMMIT || info.Protect.0 & (PAGE_GUARD.0 | PAGE_NOACCESS.0) != 0 {
        return false;
    }
    addr + len <= info.BaseAddress as usize + info.RegionSize
}

pub fn read_usize(addr: usize) -> Option<usize> {
    readable(addr, 8).then(|| unsafe { (addr as *const usize).read_unaligned() })
}

/// "48 8B ?? 05" -> bytes with wildcards.
pub fn pattern(s: &str) -> Vec<Option<u8>> {
    s.split_whitespace()
        .map(|b| if b == "??" { None } else { Some(u8::from_str_radix(b, 16).unwrap_or(0)) })
        .collect()
}

/// The game's image base and size.
pub fn image() -> (usize, usize) {
    let base = unsafe { GetModuleHandleW(None) }.map_or(0, |h| h.0 as usize);
    if base == 0 || !readable(base, 0x400) {
        return (0, 0);
    }
    unsafe {
        let pe = base + *((base + 0x3C) as *const u32) as usize;
        let size = *((pe + 0x18 + 0x38) as *const u32) as usize;
        (base, size)
    }
}

/// The .text section (start, length).
fn text() -> Option<(usize, usize)> {
    let (base, _) = image();
    if base == 0 {
        return None;
    }
    unsafe {
        let pe = base + *((base + 0x3C) as *const u32) as usize;
        let sections = *((pe + 6) as *const u16) as usize;
        let optional = *((pe + 0x14) as *const u16) as usize;
        let first = pe + 0x18 + optional;
        for i in 0..sections {
            let h = first + i * 40;
            if std::slice::from_raw_parts(h as *const u8, 8).starts_with(b".text") {
                let size = *((h + 8) as *const u32) as usize;
                let rva = *((h + 12) as *const u32) as usize;
                return Some((base + rva, size));
            }
        }
    }
    None
}

fn find_from(start: usize, len: usize, pattern: &[Option<u8>], out: &mut Vec<usize>, max: usize) {
    if pattern.is_empty() || len < pattern.len() {
        return;
    }
    let bytes = unsafe { std::slice::from_raw_parts(start as *const u8, len) };
    let first = pattern[0];
    let mut i = 0;
    while i + pattern.len() <= bytes.len() && out.len() < max {
        if first.is_none_or(|b| b == bytes[i])
            && bytes[i..i + pattern.len()].iter().zip(pattern).all(|(b, p)| p.is_none_or(|p| p == *b))
        {
            out.push(start + i);
        }
        i += 1;
    }
}

/// Every match (up to `max`) in .text, region by region.
pub fn find_all_in_text(pattern: &[Option<u8>], max: usize) -> Vec<usize> {
    let mut out = Vec::new();
    let Some((start, len)) = text() else { return out };
    let end = start + len;
    let mut addr = start;
    while addr < end && out.len() < max {
        let mut info = MEMORY_BASIC_INFORMATION::default();
        let n = unsafe {
            VirtualQuery(Some(addr as *const core::ffi::c_void), &mut info, size_of::<MEMORY_BASIC_INFORMATION>())
        };
        if n == 0 {
            break;
        }
        let region_end = (info.BaseAddress as usize + info.RegionSize).min(end);
        let chunk = region_end - addr;
        if readable(addr, chunk) {
            // Matches across a region boundary are missed; .text is one
            // committed region in practice.
            find_from(addr, chunk, pattern, &mut out, max);
        }
        addr = region_end;
    }
    out
}

pub fn find_in_text(pattern: &[Option<u8>]) -> Option<usize> {
    find_all_in_text(pattern, 1).first().copied()
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn finds_with_wildcards() {
        let data = [1u8, 2, 3, 4, 5, 3, 9, 5];
        let mut out = Vec::new();
        find_from(data.as_ptr() as usize, data.len(), &pattern("03 ?? 05"), &mut out, 8);
        let base = data.as_ptr() as usize;
        assert_eq!(out, vec![base + 2, base + 5]);
    }
}
