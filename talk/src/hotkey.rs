//! The key that opens the talk menu: ERNativeUI.ini [TalkMenu] Key, such as
//! "F11" (the default), "Home", "Ctrl+F11" or "Alt+Shift+F10". Modifiers named
//! must be held; others are ignored. Ctrl+F11 was the first default, but Ctrl
//! is an attack key for some players.

pub const DEFAULT: &str = "F11";

pub const VK_SHIFT: u16 = 0x10;
pub const VK_CONTROL: u16 = 0x11;
pub const VK_MENU: u16 = 0x12;

#[derive(Clone, Debug, PartialEq, Eq)]
pub struct Hotkey {
    pub key: u16,
    /// Virtual keys that must be held with it.
    pub modifiers: Vec<u16>,
    /// As written in logs ("F11", "Ctrl+Home").
    pub name: String,
}

fn key_code(name: &str) -> Option<(u16, &'static str)> {
    let upper = name.to_ascii_uppercase();
    if let Some(n) = upper.strip_prefix('F').and_then(|n| n.parse::<u16>().ok()) {
        if (1..=24).contains(&n) {
            let names = [
                "F1", "F2", "F3", "F4", "F5", "F6", "F7", "F8", "F9", "F10", "F11", "F12", "F13", "F14", "F15",
                "F16", "F17", "F18", "F19", "F20", "F21", "F22", "F23", "F24",
            ];
            return Some((0x6F + n, names[n as usize - 1]));
        }
    }
    Some(match upper.as_str() {
        "HOME" | "INICIO" => (0x24, "Home"),
        "END" | "FIN" => (0x23, "End"),
        "INSERT" | "INS" => (0x2D, "Insert"),
        "DELETE" | "DEL" | "SUPR" => (0x2E, "Delete"),
        "PAGEUP" | "PGUP" => (0x21, "PageUp"),
        "PAGEDOWN" | "PGDN" => (0x22, "PageDown"),
        "PAUSE" => (0x13, "Pause"),
        "SCROLLLOCK" => (0x91, "ScrollLock"),
        _ => return None,
    })
}

/// Parses "Ctrl+F11"-style text; None when it names no usable key.
pub fn parse(text: &str) -> Option<Hotkey> {
    let mut modifiers = Vec::new();
    let mut names = Vec::new();
    let mut key = None;
    for part in text.split('+').map(str::trim).filter(|p| !p.is_empty()) {
        let (vk, name) = match part.to_ascii_uppercase().as_str() {
            "CTRL" | "CONTROL" => (VK_CONTROL, "Ctrl"),
            "ALT" => (VK_MENU, "Alt"),
            "SHIFT" => (VK_SHIFT, "Shift"),
            _ => {
                if key.is_some() {
                    return None;
                }
                let (vk, name) = key_code(part)?;
                key = Some((vk, name));
                continue;
            }
        };
        if !modifiers.contains(&vk) {
            modifiers.push(vk);
            names.push(name);
        }
    }
    let (key, key_name) = key?;
    names.push(key_name);
    Some(Hotkey { key, modifiers, name: names.join("+") })
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn keys() {
        assert_eq!(parse("F11"), Some(Hotkey { key: 0x7A, modifiers: vec![], name: "F11".into() }));
        assert_eq!(parse(" ctrl + f11 ").map(|h| (h.key, h.modifiers, h.name)), Some((0x7A, vec![VK_CONTROL], "Ctrl+F11".into())));
        assert_eq!(parse("Alt+Shift+Home").map(|h| h.name), Some("Alt+Shift+Home".into()));
        assert_eq!(parse("inicio").map(|h| h.key), Some(0x24));
        assert_eq!(parse("F25"), None);
        assert_eq!(parse("Ctrl"), None);
        assert_eq!(parse("F11+F12"), None);
        assert_eq!(parse(""), None);
        assert_eq!(parse(DEFAULT).map(|h| h.key), Some(0x7A));
    }
}
