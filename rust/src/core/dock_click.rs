// lambdock -- hyper-hackable desktop dock for Wayland
//
// SPDX-License-Identifier: MIT
// Copyright © Josep Bigorra <jjbigorra@gmail.com>

//! The click policy of an application item: `src/core/dock_click.h` in Rust.
//!
//! It is the one part of the dock that behaves differently on every desktop:
//! taskbar people expect the icon of the focused app to tuck it away, launcher
//! people expect it to stay put. As a value it is testable without a
//! compositor, and `ui/dock_item.c` only applies the answer.

use std::os::raw::{c_char, c_int};

/// What a left click on an application item should do.
///
/// The discriminants are the contract: they were the C enum's values, the item
/// kinds are numbered by the same table, and a configuration file may name one.
#[repr(C)]
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum ClickAction {
    Nothing = 0,
    Launch = 1,
    Minimize = 2,
    Activate = 3,
    Menu = 4,
}

/// The action a click should perform.
///
/// # Safety
///
/// `launcher_command` must be null or a NUL-terminated string.
pub unsafe fn lambdock_click_action_for(
    window_count: u32,
    window_minimized: c_int,
    window_active: c_int,
    launcher_command: *const c_char,
) -> ClickAction {
    if window_count == 0 {
        let has_command = !launcher_command.is_null() && *launcher_command != 0;
        return if has_command {
            ClickAction::Launch
        } else {
            ClickAction::Nothing
        };
    }

    if window_count > 1 {
        return ClickAction::Menu;
    }

    // Exactly one window: the click toggles it. An active window tucks away; a
    // minimized one -- or one that is merely behind another app -- comes
    // forward. A minimized window is never reported as active, but checking
    // both keeps the rule right for compositors that lag on focus events.
    if window_active != 0 && window_minimized == 0 {
        ClickAction::Minimize
    } else {
        ClickAction::Activate
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn click(
        window_count: u32,
        minimized: bool,
        active: bool,
        command: Option<&'static [u8]>,
    ) -> ClickAction {
        let command = match command {
            Some(bytes) => bytes.as_ptr() as *const c_char,
            None => std::ptr::null(),
        };

        unsafe {
            lambdock_click_action_for(
                window_count,
                minimized as c_int,
                active as c_int,
                command,
            )
        }
    }

    #[test]
    fn a_pinned_launcher_with_nothing_open_starts_the_app() {
        assert_eq!(
            click(0, false, false, Some(b"firefox\0")),
            ClickAction::Launch
        );
    }

    #[test]
    fn without_a_command_there_is_nothing_to_do() {
        assert_eq!(click(0, false, false, None), ClickAction::Nothing);
        assert_eq!(click(0, false, false, Some(b"\0")), ClickAction::Nothing);
    }

    #[test]
    fn a_focused_single_window_is_tucked_away() {
        assert_eq!(
            click(1, false, true, Some(b"x\0")),
            ClickAction::Minimize
        );
    }

    #[test]
    fn a_minimized_or_hidden_single_window_comes_forward() {
        assert_eq!(
            click(1, true, false, Some(b"x\0")),
            ClickAction::Activate
        );
        // Compositors lag on focus events, so both flags can disagree for a
        // frame: a minimized window is never the one to tuck away.
        assert_eq!(
            click(1, true, true, Some(b"x\0")),
            ClickAction::Activate
        );
        assert_eq!(
            click(1, false, false, Some(b"x\0")),
            ClickAction::Activate
        );
    }

    #[test]
    fn a_running_app_is_never_launched_again() {
        assert_eq!(click(1, false, true, None), ClickAction::Minimize);
        assert_eq!(click(1, true, false, None), ClickAction::Activate);
    }

    #[test]
    fn several_windows_ask_the_user_which_one() {
        assert_eq!(
            click(2, false, true, Some(b"x\0")),
            ClickAction::Menu
        );
        assert_eq!(click(9, true, false, None), ClickAction::Menu);
    }
}
