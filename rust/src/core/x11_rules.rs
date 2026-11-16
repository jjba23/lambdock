// lambdock -- hyper-hackable desktop dock for Wayland
//
// SPDX-License-Identifier: MIT
// Copyright © Josep Bigorra <jjbigorra@gmail.com>

//! X11 / EWMH rules: `src/core/x11_rules.h` in Rust.
//!
//! What an X11 window means to a dock, as functions of plain values: no Xlib, no
//! widget, no global state. Reading `_NET_CLIENT_LIST` and the properties of each
//! window is `x11_tracker.c`'s job; deciding which windows the dock lists, what
//! their state is, and where the dock puts itself is here, where it can be tested
//! without a display.

use super::config_values::{EDGE_BOTTOM, EDGE_LEFT, EDGE_RIGHT, EDGE_TOP};
use std::ffi::CStr;
use std::os::raw::{c_char, c_int, c_ulong};

/// Window types the desktop owns. Listing them would put an icon for the panel,
/// the desktop or a menu inside the dock.
const DESKTOP_CHROME_TYPES: [&str; 10] = [
    "_NET_WM_WINDOW_TYPE_DESKTOP",
    "_NET_WM_WINDOW_TYPE_DOCK",
    "_NET_WM_WINDOW_TYPE_TOOLBAR",
    "_NET_WM_WINDOW_TYPE_MENU",
    "_NET_WM_WINDOW_TYPE_UTILITY",
    "_NET_WM_WINDOW_TYPE_SPLASH",
    "_NET_WM_WINDOW_TYPE_NOTIFICATION",
    "_NET_WM_WINDOW_TYPE_TOOLTIP",
    "_NET_WM_WINDOW_TYPE_COMBO",
    "_NET_WM_WINDOW_TYPE_DND",
];

/// A rectangle in root (whole-desktop) coordinates, in pixels.
///
/// The field order is the contract: `LambdockX11Rect` in the header.
#[repr(C)]
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub struct Rect {
    pub x: c_int,
    pub y: c_int,
    pub width: c_int,
    pub height: c_int,
}

/// The two flags a dock reads off a window: the same pair `AppWindow` holds.
#[repr(C)]
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub struct WindowState {
    pub minimized: c_int,
    pub activated: c_int,
}

/// Where the dock's window goes and how much desktop it reserves, as one value.
///
/// The field order is the contract: `LambdockX11Placement` in the header.
#[repr(C)]
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub struct Placement {
    pub x: c_int,
    pub y: c_int,
    pub width: c_int,
    pub height: c_int,
    pub strut_left: c_int,
    pub strut_right: c_int,
    pub strut_top: c_int,
    pub strut_bottom: c_int,
    pub strut_left_start_y: c_int,
    pub strut_left_end_y: c_int,
    pub strut_right_start_y: c_int,
    pub strut_right_end_y: c_int,
    pub strut_top_start_x: c_int,
    pub strut_top_end_x: c_int,
    pub strut_bottom_start_x: c_int,
    pub strut_bottom_end_x: c_int,
}

/// Whether a `_NET_WM_WINDOW_TYPE_*` atom name is one the desktop owns.
///
/// # Safety
///
/// `window_type` must be null or a NUL-terminated string.
#[no_mangle]
pub unsafe extern "C" fn lambdock_x11_type_is_desktop_chrome(
    window_type: *const c_char,
) -> c_int {
    if window_type.is_null() || *window_type == 0 {
        return 0;
    }

    let window_type = CStr::from_ptr(window_type).to_bytes();
    let chrome = DESKTOP_CHROME_TYPES
        .iter()
        .any(|known| window_type == known.as_bytes());

    chrome as c_int
}

/// Whether the dock should list the window.
#[no_mangle]
pub extern "C" fn lambdock_x11_shows_window(
    desktop_chrome: c_int,
    skip_taskbar: c_int,
    own_window: c_int,
    has_wm_class: c_int,
) -> c_int {
    (desktop_chrome == 0 && skip_taskbar == 0 && own_window == 0 && has_wm_class != 0) as c_int
}

/// The two flags the dock's click policy reads.
#[no_mangle]
pub extern "C" fn lambdock_x11_window_state(
    hidden: c_int,
    window: c_ulong,
    active_window: c_ulong,
) -> WindowState {
    WindowState {
        minimized: hidden,
        activated: (active_window != 0 && window == active_window) as c_int,
    }
}

/// @value bounded to `1..=@limit`, so a dock whose content or thickness outgrew
/// its monitor still gets a window that is on screen.
fn clamp_axis(value: c_int, limit: c_int) -> c_int {
    if limit < 1 {
        return 1;
    }

    if value > limit {
        return limit;
    }

    if value < 1 {
        1
    } else {
        value
    }
}

/// The reserved strip along `edge`, in root coordinates.
fn reserved_strip(
    placement: &Placement,
    edge: c_int,
    monitor: Rect,
    main_px: c_int,
    reserve_px: c_int,
) -> Rect {
    match edge {
        EDGE_TOP => Rect {
            x: placement.x,
            y: monitor.y,
            width: main_px,
            height: reserve_px,
        },
        EDGE_LEFT => Rect {
            x: monitor.x,
            y: placement.y,
            width: reserve_px,
            height: main_px,
        },
        EDGE_RIGHT => Rect {
            x: monitor.x + monitor.width - reserve_px,
            y: placement.y,
            width: reserve_px,
            height: main_px,
        },
        _ => Rect {
            x: placement.x,
            y: monitor.y + monitor.height - reserve_px,
            width: main_px,
            height: reserve_px,
        },
    }
}

/// Where the window goes and what it reserves.
///
/// The window hugs the monitor's edge on the dock's side and is centred along the
/// other axis; a size larger than the monitor is clamped to it, so the window is
/// always on screen.
#[no_mangle]
pub extern "C" fn lambdock_x11_placement_compute(
    edge: c_int,
    root: Rect,
    monitor: Rect,
    desired_main_px: c_int,
    window_cross_px: c_int,
    reserve_px: c_int,
) -> Placement {
    let horizontal = edge == EDGE_TOP || edge == EDGE_BOTTOM;
    let at_start = edge == EDGE_TOP || edge == EDGE_LEFT;

    let monitor_main = if horizontal {
        monitor.width
    } else {
        monitor.height
    };
    let monitor_cross = if horizontal {
        monitor.height
    } else {
        monitor.width
    };

    // Nothing measured yet means "as wide as the monitor": collapsing to a
    // one-pixel bar on the first frame would show a dock that flickers in.
    let main_px = if desired_main_px < 1 {
        monitor_main
    } else {
        clamp_axis(desired_main_px, monitor_main)
    };
    let cross_px = clamp_axis(window_cross_px, monitor_cross);
    let reserve = if reserve_px < 1 {
        0
    } else {
        clamp_axis(reserve_px, monitor_cross)
    };

    let main_origin =
        (if horizontal { monitor.x } else { monitor.y }) + (monitor_main - main_px) / 2;
    let cross_origin = if at_start {
        if horizontal {
            monitor.y
        } else {
            monitor.x
        }
    } else {
        (if horizontal {
            monitor.y + monitor.height
        } else {
            monitor.x + monitor.width
        }) - cross_px
    };

    let mut placement = Placement {
        x: if horizontal { main_origin } else { cross_origin },
        y: if horizontal { cross_origin } else { main_origin },
        width: if horizontal { main_px } else { cross_px },
        height: if horizontal { cross_px } else { main_px },
        ..Placement::default()
    };

    // No reservation, no struts: a window may ask for its edge and nothing else.
    if reserve < 1 {
        return placement;
    }

    let reserved = reserved_strip(&placement, edge, monitor, main_px, reserve);

    // The four distances are measured from the desktop's own edges, so that a
    // window manager can subtract the reservation from the work area.
    match edge {
        EDGE_TOP => {
            placement.strut_top = reserved.y + reserved.height - root.y;
            placement.strut_top_start_x = reserved.x;
            placement.strut_top_end_x = reserved.x + reserved.width - 1;
        }
        EDGE_LEFT => {
            placement.strut_left = reserved.x + reserved.width - root.x;
            placement.strut_left_start_y = reserved.y;
            placement.strut_left_end_y = reserved.y + reserved.height - 1;
        }
        EDGE_RIGHT => {
            placement.strut_right = root.x + root.width - reserved.x;
            placement.strut_right_start_y = reserved.y;
            placement.strut_right_end_y = reserved.y + reserved.height - 1;
        }
        _ => {
            placement.strut_bottom = root.y + root.height - reserved.y;
            placement.strut_bottom_start_x = reserved.x;
            placement.strut_bottom_end_x = reserved.x + reserved.width - 1;
        }
    }

    placement
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::ffi::CString;

    fn rect(x: c_int, y: c_int, width: c_int, height: c_int) -> Rect {
        Rect {
            x,
            y,
            width,
            height,
        }
    }

    fn chrome(window_type: Option<&str>) -> bool {
        // The CString has to outlive the call: a temporary here would leave a
        // dangling pointer for the duration of the assertion.
        let text = window_type.map(|text| CString::new(text).expect("no interior NUL"));
        let pointer = text
            .as_ref()
            .map_or(std::ptr::null(), |text| text.as_ptr() as *const c_char);

        unsafe { lambdock_x11_type_is_desktop_chrome(pointer) != 0 }
    }

    fn shows(chrome: bool, skip_taskbar: bool, own: bool, wm_class: bool) -> bool {
        lambdock_x11_shows_window(
            chrome as c_int,
            skip_taskbar as c_int,
            own as c_int,
            wm_class as c_int,
        ) != 0
    }

    fn placement(
        edge: c_int,
        root: Rect,
        monitor: Rect,
        desired_main_px: c_int,
        window_cross_px: c_int,
        reserve_px: c_int,
    ) -> Placement {
        lambdock_x11_placement_compute(
            edge,
            root,
            monitor,
            desired_main_px,
            window_cross_px,
            reserve_px,
        )
    }

    // ======================================================================
    // Which windows a dock lists
    // ======================================================================

    #[test]
    fn every_desktop_owned_window_type_is_chrome() {
        for window_type in [
            "_NET_WM_WINDOW_TYPE_DESKTOP",
            "_NET_WM_WINDOW_TYPE_DOCK",
            "_NET_WM_WINDOW_TYPE_TOOLBAR",
            "_NET_WM_WINDOW_TYPE_MENU",
            "_NET_WM_WINDOW_TYPE_UTILITY",
            "_NET_WM_WINDOW_TYPE_SPLASH",
            "_NET_WM_WINDOW_TYPE_NOTIFICATION",
            "_NET_WM_WINDOW_TYPE_TOOLTIP",
            "_NET_WM_WINDOW_TYPE_COMBO",
            "_NET_WM_WINDOW_TYPE_DND",
        ] {
            assert!(chrome(Some(window_type)), "{window_type}");
        }
    }

    #[test]
    fn applications_are_not_chrome() {
        // This is what the dock is for.
        assert!(!chrome(Some("_NET_WM_WINDOW_TYPE_NORMAL")));
        assert!(!chrome(Some("_NET_WM_WINDOW_TYPE_DIALOG")));
    }

    #[test]
    fn a_window_that_declares_no_type_is_an_ordinary_window() {
        assert!(!chrome(None));
        assert!(!chrome(Some("")));
    }

    #[test]
    fn an_ordinary_application_window_is_listed() {
        assert!(shows(false, false, false, true));
    }

    #[test]
    fn desktop_chrome_is_never_listed() {
        assert!(!shows(true, false, false, true));
    }

    #[test]
    fn a_window_that_asked_the_taskbar_to_skip_it_is_honoured() {
        assert!(!shows(false, true, false, true));
    }

    #[test]
    fn the_docks_own_windows_are_never_listed() {
        // Otherwise the dock, its preferences and its launchpad show up as a
        // running application called lambdock.
        assert!(!shows(false, false, true, true));
    }

    #[test]
    fn a_window_without_a_wm_class_has_no_app_id_to_attach_to() {
        assert!(!shows(false, false, false, false));
    }

    // ======================================================================
    // Window state
    // ======================================================================

    #[test]
    fn the_focused_window_is_not_minimized_and_is_activated() {
        let state = lambdock_x11_window_state(0, 42, 42);
        assert_eq!(state.minimized, 0);
        assert_ne!(state.activated, 0);
    }

    #[test]
    fn a_hidden_window_is_how_ewmh_spells_minimized() {
        let state = lambdock_x11_window_state(1, 42, 42);
        assert_ne!(state.minimized, 0);
    }

    #[test]
    fn a_window_that_is_not_the_active_one_is_not_activated() {
        let state = lambdock_x11_window_state(0, 42, 7);
        assert_eq!(state.minimized, 0);
        assert_eq!(state.activated, 0);
    }

    #[test]
    fn with_nothing_focused_no_window_is_activated() {
        let state = lambdock_x11_window_state(0, 42, 0);
        assert_eq!(state.activated, 0);
    }

    // ======================================================================
    // Placement and struts
    // ======================================================================

    #[test]
    fn a_bottom_taskbar_spans_the_screen_and_reserves_its_capsule() {
        let screen = rect(0, 0, 1920, 1080);
        let p = placement(EDGE_BOTTOM, screen, screen, 1920, 96, 72);

        assert_eq!(p.x, 0);
        assert_eq!(p.y, 1080 - 96);
        assert_eq!(p.width, 1920);
        assert_eq!(p.height, 96);

        assert_eq!(p.strut_bottom, 72);
        assert_eq!(p.strut_bottom_start_x, 0);
        assert_eq!(p.strut_bottom_end_x, 1919);

        // Only the edge the dock is on gets a strut.
        assert_eq!(p.strut_top, 0);
        assert_eq!(p.strut_left, 0);
        assert_eq!(p.strut_right, 0);
    }

    #[test]
    fn a_floating_capsule_is_centred_on_its_own_monitor() {
        let root = rect(0, 0, 3840, 1080);
        let monitor = rect(1920, 0, 1920, 1080);
        let p = placement(EDGE_BOTTOM, root, monitor, 600, 96, 72);

        assert_eq!(p.x, 1920 + (1920 - 600) / 2);
        assert_eq!(p.y, 1080 - 96);
        assert_eq!(p.width, 600);
        assert_eq!(p.height, 96);

        // The strut ranges are absolute, which is what lets a window manager
        // shrink the work area of that monitor and not the other one.
        assert_eq!(p.strut_bottom, 72);
        assert_eq!(p.strut_bottom_start_x, 2580);
        assert_eq!(p.strut_bottom_end_x, 3179);
    }

    #[test]
    fn a_top_dock_sits_on_the_top_edge() {
        let screen = rect(0, 0, 1920, 1080);
        let p = placement(EDGE_TOP, screen, screen, 800, 90, 60);

        assert_eq!(p.y, 0);
        assert_eq!(p.strut_top, 60);
        assert_eq!(p.strut_top_start_x, (1920 - 800) / 2);
        assert_eq!(p.strut_top_end_x, (1920 - 800) / 2 + 799);
        assert_eq!(p.strut_bottom, 0);
    }

    #[test]
    fn a_left_dock_is_centred_down_the_screen() {
        let screen = rect(0, 0, 1920, 1080);
        let p = placement(EDGE_LEFT, screen, screen, 400, 80, 60);

        assert_eq!(p.x, 0);
        assert_eq!(p.y, (1080 - 400) / 2);
        assert_eq!(p.width, 80);
        assert_eq!(p.height, 400);

        assert_eq!(p.strut_left, 60);
        assert_eq!(p.strut_left_start_y, (1080 - 400) / 2);
        assert_eq!(p.strut_left_end_y, (1080 - 400) / 2 + 399);
    }

    #[test]
    fn a_right_dock_hugs_the_right_edge_of_the_desktop() {
        let root = rect(0, 0, 3840, 1080);
        let monitor = rect(1920, 0, 1920, 1080);
        let p = placement(EDGE_RIGHT, root, monitor, 400, 80, 60);

        assert_eq!(p.x, 3840 - 80);
        assert_eq!(p.strut_right, 60);
        assert_eq!(p.strut_right_start_y, (1080 - 400) / 2);
        assert_eq!(p.strut_right_end_y, (1080 - 400) / 2 + 399);
        assert_eq!(p.strut_left, 0);
    }

    #[test]
    fn a_row_wider_than_its_monitor_is_clamped_to_the_monitor() {
        let screen = rect(0, 0, 1920, 1080);
        let p = placement(EDGE_BOTTOM, screen, screen, 4000, 96, 72);

        assert_eq!(p.width, 1920);
        assert_eq!(p.x, 0);
        assert_eq!(p.strut_bottom_end_x, 1919);
    }

    #[test]
    fn nothing_measured_yet_spans_the_monitor_instead_of_collapsing() {
        let screen = rect(0, 0, 1920, 1080);
        let p = placement(EDGE_BOTTOM, screen, screen, 0, 96, 72);

        assert_eq!(p.width, 1920);
    }

    #[test]
    fn a_cross_axis_taller_than_the_monitor_is_clamped_too() {
        // Otherwise the window is positioned above the top edge.
        let screen = rect(0, 0, 1920, 1080);
        let p = placement(EDGE_BOTTOM, screen, screen, 800, 2000, 72);

        assert_eq!(p.height, 1080);
        assert_eq!(p.y, 0);
    }

    #[test]
    fn a_dock_that_reserves_nothing_gets_no_struts() {
        let screen = rect(0, 0, 1920, 1080);
        let p = placement(EDGE_BOTTOM, screen, screen, 1920, 96, 0);

        assert_eq!(p.strut_bottom, 0);
        assert_eq!(p.strut_top, 0);
        assert_eq!(p.strut_left, 0);
        assert_eq!(p.strut_right, 0);
    }
}
