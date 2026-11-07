//! Pages of the talk menu: a stack that starts with the top list.

/// Deepest stack kept (Open past it replaces the top page instead).
pub const MAX_DEPTH: usize = 32;

#[derive(Clone, Copy, PartialEq, Eq, Debug)]
pub enum Level {
    /// The "NativeTweaks" list: one row per provider, Mod Settings, Leave.
    Top,
    /// A page of a provider (0 = its first page).
    Provider { entry: u64, page: u32 },
}

/// What a row of the shown page does.
#[derive(Clone, Copy, PartialEq, Eq, Debug)]
pub enum Row {
    Entry(u64),
    ModSettings,
    Leave,
    Back,
    /// A provider's own row id.
    Provider(u32),
}

/// Where to go after a choice (or a provider's build).
#[derive(Clone, Copy, PartialEq, Eq, Debug)]
pub enum Nav {
    /// Show the same page again.
    Stay,
    /// Another page of the same provider, on top (Back returns).
    Open(u32),
    /// Another page of the same provider, in place of this one.
    Replace(u32),
    Back,
    Close,
    /// A provider's first page, from the top list.
    Enter(u64),
}

/// Applies `nav`; false when the menu closes.
pub fn apply(stack: &mut Vec<Level>, nav: Nav) -> bool {
    match nav {
        Nav::Stay => {}
        Nav::Close => return false,
        Nav::Back => {
            stack.pop();
        }
        Nav::Enter(entry) => stack.push(Level::Provider { entry, page: 0 }),
        Nav::Open(page) | Nav::Replace(page) => {
            if let Some(Level::Provider { entry, .. }) = stack.last().copied() {
                if matches!(nav, Nav::Replace(_)) || stack.len() >= MAX_DEPTH {
                    stack.pop();
                }
                stack.push(Level::Provider { entry, page });
            }
        }
    }
    !stack.is_empty()
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn pages() {
        let mut s = vec![Level::Top];
        assert!(apply(&mut s, Nav::Enter(7)));
        assert!(apply(&mut s, Nav::Open(3)));
        assert_eq!(s.last(), Some(&Level::Provider { entry: 7, page: 3 }));
        assert!(apply(&mut s, Nav::Replace(4)));
        assert_eq!(s.len(), 3);
        assert!(apply(&mut s, Nav::Back));
        assert_eq!(s.last(), Some(&Level::Provider { entry: 7, page: 0 }));
        assert!(apply(&mut s, Nav::Back));
        assert_eq!(s, vec![Level::Top]);
        // Open on the top list does nothing.
        assert!(apply(&mut s, Nav::Open(1)));
        assert_eq!(s, vec![Level::Top]);
        assert!(!apply(&mut s, Nav::Back));
        let mut s = vec![Level::Top];
        assert!(!apply(&mut s, Nav::Close));
    }

    #[test]
    fn depth_is_bounded() {
        let mut s = vec![Level::Top, Level::Provider { entry: 1, page: 0 }];
        for p in 0..100 {
            assert!(apply(&mut s, Nav::Open(p)));
        }
        assert_eq!(s.len(), MAX_DEPTH);
    }
}
