// lambdock -- hyper-hackable desktop dock for Wayland
//
// SPDX-License-Identifier: MIT
// Copyright © Josep Bigorra <jjbigorra@gmail.com>

//! What a dock item says about itself: `src/core/item_text.h` in Rust.
//!
//! Labels, screen-reader descriptions, tooltips and gauge readouts, as functions
//! of values: no widgets, no display, no Guile. Only strings, counts and numbers
//! go in, which is what keeps it free of GTK -- and what makes the wording of
//! every item kind testable without constructing a dock.
//!
//! Two rules come from C and stay: a `(transfer full)` string is allocated with
//! `g_strdup`, because the C callers free it with `g_free` and
//! `lambdock_item_text_clear`; and a translated message with substitutions is
//! formatted by `g_strdup_printf`, so a translation may reorder its placeholders.

use crate::glib::{g_free, g_strdup, g_strdup_printf, tr, trn};
use std::ffi::CStr;
use std::os::raw::{c_char, c_int};

/// Two strings that always belong together, so the per-kind functions can return
/// them as one value instead of writing through two out-parameters.
///
/// The field order is the contract: `LambdockItemText` in the header.
#[repr(C)]
pub struct ItemText {
    pub label: *mut c_char,
    pub description: *mut c_char,
}

/// A reading that is not a number is a broken reading, not a large one, so it
/// falls back instead of being clamped to a huge value.
fn finite_or(value: f64, fallback: f64) -> f64 {
    if value.is_finite() {
        value
    } else {
        fallback
    }
}

/// The empty string a lookup returns when there is nothing to say, so the caller
/// can still print it (the C code returned `""` the same way).
static EMPTY: [u8; 1] = [0];

/// Whether a C string argument is present and non-empty: the rule the C code
/// spelled out inline everywhere.
unsafe fn is_set(text: *const c_char) -> bool {
    !text.is_null() && *text != 0
}

/// Whether `text` begins with `prefix`, byte for byte, as `g_str_has_prefix` did.
unsafe fn starts_with(text: *const c_char, prefix: *const c_char) -> bool {
    CStr::from_ptr(text)
        .to_bytes()
        .starts_with(CStr::from_ptr(prefix).to_bytes())
}

/// What a screen reader should say about an app item besides its name.
unsafe fn running_text(window_count: u32) -> *mut c_char {
    if window_count == 0 {
        return g_strdup(tr(c"Not running"));
    }

    g_strdup_printf(
        trn(
            c"Running, %u window",
            c"Running, %u windows",
            window_count,
        ),
        window_count,
    )
}

/// Frees both strings and zeroes the struct, so an early return cannot leak it.
///
/// # Safety
///
/// `text` must be null or point to a struct produced by one of the functions here.
pub unsafe fn lambdock_item_text_clear(text: *mut ItemText) {
    if text.is_null() {
        return;
    }

    let text = &mut *text;

    if !text.label.is_null() {
        g_free(text.label.cast());
        text.label = std::ptr::null_mut();
    }

    if !text.description.is_null() {
        g_free(text.description.cast());
        text.description = std::ptr::null_mut();
    }
}

/// The "use the name, but not an empty one" rule that every kind repeats.
///
/// # Safety
///
/// Each argument must be null or a NUL-terminated string. The result is borrowed
/// from one of them.
pub unsafe fn lambdock_item_display_name(
    name: *const c_char,
    fallback: *const c_char,
) -> *const c_char {
    if is_set(name) {
        name
    } else {
        fallback
    }
}

/// The label and description of an application item.
///
/// # Safety
///
/// `name` must be null or a NUL-terminated string.
pub unsafe fn lambdock_item_text_app(
    name: *const c_char,
    window_count: u32,
) -> ItemText {
    let title = lambdock_item_display_name(name, std::ptr::null());

    ItemText {
        label: g_strdup(if is_set(title) {
            title
        } else {
            tr(c"Application")
        }),
        description: running_text(window_count),
    }
}

/// The label and description of a polling (clock, CPU, meter) item.
///
/// # Safety
///
/// Each argument must be null or a NUL-terminated string.
pub unsafe fn lambdock_item_text_dynamic(
    name: *const c_char,
    reading: *const c_char,
) -> ItemText {
    let title = lambdock_item_display_name(name, std::ptr::null());

    let label = if is_set(reading) {
        // Meter items already put "name: reading" in the tooltip, and repeating
        // the name would read as "CPU: CPU: 42%".
        let prefix = if is_set(title) {
            title
        } else {
            EMPTY.as_ptr().cast()
        };

        if starts_with(reading, prefix) {
            g_strdup(reading)
        } else {
            g_strdup_printf(
                tr(c"%s: %s"),
                if is_set(title) {
                    title
                } else {
                    tr(c"Reading")
                },
                reading,
            )
        }
    } else {
        g_strdup(if is_set(title) {
            title
        } else {
            tr(c"Reading")
        })
    };

    ItemText {
        label,
        description: g_strdup(tr(c"Live reading from the dock.")),
    }
}

/// The label and description of a folder stack item.
///
/// # Safety
///
/// `name` must be null or a NUL-terminated string.
pub unsafe fn lambdock_item_text_stack(name: *const c_char) -> ItemText {
    let title = lambdock_item_display_name(name, tr(c"Stack"));

    ItemText {
        label: g_strdup_printf(tr(c"%s, folder stack"), title),
        description: g_strdup(tr(c"Opens the folder's contents.")),
    }
}

/// The label and description of the application grid item.
///
/// # Safety
///
/// `name` must be null or a NUL-terminated string.
pub unsafe fn lambdock_item_text_launchpad(name: *const c_char) -> ItemText {
    let title = lambdock_item_display_name(name, tr(c"Applications"));

    ItemText {
        label: g_strdup(title),
        description: g_strdup(tr(c"Opens the application grid. Escape closes it.")),
    }
}

/// The label and description of the trash item.
///
/// # Safety
///
/// `name` must be null or a NUL-terminated string.
pub unsafe fn lambdock_item_text_trash(
    name: *const c_char,
    item_count: u32,
) -> ItemText {
    let title = lambdock_item_display_name(name, tr(c"Trash"));

    if item_count == 0 {
        return ItemText {
            label: g_strdup(title),
            description: g_strdup(tr(c"The trash is empty. Opens the trash.")),
        };
    }

    let count = g_strdup_printf(
        trn(
            c"%u item in the trash",
            c"%u items in the trash",
            item_count,
        ),
        item_count,
    );
    let description = g_strdup_printf(tr(c"%s. Opens the trash."), count);
    g_free(count.cast());

    ItemText {
        label: g_strdup(title),
        description,
    }
}

/// The trash item's tooltip: its name, plus the count while the trash is not
/// empty.
///
/// # Safety
///
/// `name` must be null or a NUL-terminated string; the result is `g_strdup`ed.
pub unsafe fn lambdock_item_trash_tooltip(
    name: *const c_char,
    item_count: u32,
) -> *mut c_char {
    let title = lambdock_item_display_name(name, tr(c"Trash"));

    if item_count == 0 {
        return g_strdup(title);
    }

    let items = g_strdup_printf(
        trn(c"%u item", c"%u items", item_count),
        item_count,
    );
    let tooltip = g_strdup_printf(c"%s — %s".as_ptr(), title, items);
    g_free(items.cast());

    tooltip
}

/// The gauge's 0..1 fraction, clamped, and never non-finite.
pub fn lambdock_meter_reading_fraction(reading: f64, maximum: f64) -> f64 {
    // A gauge with no configured maximum is a percentage gauge.
    let full_scale = if maximum > 0.0 { maximum } else { 100.0 };
    let raw = if full_scale > 0.0 {
        reading / full_scale
    } else {
        0.0
    };

    finite_or(raw, 0.0).clamp(0.0, 1.0)
}

/// The reading as the gauge writes it ("42%"), with non-finite readings as 0.
///
/// # Safety
///
/// `unit` must be null or a NUL-terminated string; the result is `g_strdup`ed.
pub unsafe fn lambdock_meter_reading_text(
    reading: f64,
    unit: *const c_char,
) -> *mut c_char {
    // The gauge writes whole numbers, so a wild reading would otherwise print as
    // twenty digits and break the icon's layout.
    let shown = finite_or(reading, 0.0).clamp(-1e9, 1e9);
    let unit = if unit.is_null() {
        EMPTY.as_ptr().cast()
    } else {
        unit
    };

    g_strdup_printf(c"%.0f%s".as_ptr(), shown, unit)
}

/// The text to show inside the icon, or null to hide the label.
///
/// # Safety
///
/// Each string argument must be null or NUL-terminated. The result is borrowed
/// from one of them.
pub unsafe fn lambdock_window_title_text(
    show_titles: c_int,
    has_windows: c_int,
    window_title: *const c_char,
    name: *const c_char,
    app_id: *const c_char,
) -> *const c_char {
    if show_titles == 0 || has_windows == 0 {
        return std::ptr::null();
    }

    if is_set(window_title) {
        return window_title;
    }

    if name.is_null() {
        if app_id.is_null() {
            EMPTY.as_ptr().cast()
        } else {
            app_id
        }
    } else {
        name
    }
}

/// "key, from path" -- or the fallback-dock sentence when there is no file.
///
/// # Safety
///
/// Each argument must be null or NUL-terminated; the result is `g_strdup`ed.
pub unsafe fn lambdock_inspector_launcher_text(
    config_path: *const c_char,
    item_key: *const c_char,
) -> *mut c_char {
    let key = if is_set(item_key) {
        item_key
    } else {
        c"?".as_ptr()
    };

    if !is_set(config_path) {
        return g_strdup_printf(
            tr(c"%s (no settings file: the fallback dock)"),
            key,
        );
    }

    g_strdup_printf(tr(c"%s, from %s"), key, config_path)
}

/// The icon name and where it was found.
///
/// # Safety
///
/// `icon` must be null or NUL-terminated; the result is `g_strdup`ed.
pub unsafe fn lambdock_inspector_icon_text(
    icon: *const c_char,
    found_in_theme: c_int,
) -> *mut c_char {
    if !is_set(icon) {
        return g_strdup(tr(c"no icon configured; the generic executable is drawn"));
    }

    let format = if found_in_theme != 0 {
        tr(c"%s (found in the icon theme)")
    } else {
        tr(c"%s (not in the icon theme; a fallback is drawn)")
    };

    g_strdup_printf(format, icon)
}

/// The poller and its interval, or a sentence saying the item does not poll.
///
/// # Safety
///
/// `poll_name` must be null or NUL-terminated; the result is `g_strdup`ed.
pub unsafe fn lambdock_inspector_poller_text(
    poll_name: *const c_char,
    interval_ms: u32,
) -> *mut c_char {
    if !is_set(poll_name) {
        return g_strdup(tr(c"none: this item does not poll"));
    }

    g_strdup_printf(tr(c"%s, every %u ms"), poll_name, interval_ms)
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::ffi::CString;

    fn c(text: &str) -> CString {
        CString::new(text).expect("no interior NUL")
    }

    /// Reads a `(transfer full)` string and frees it.
    fn take(pointer: *mut c_char) -> Option<String> {
        if pointer.is_null() {
            return None;
        }

        let text = unsafe { CStr::from_ptr(pointer) }
            .to_string_lossy()
            .into_owned();
        unsafe { g_free(pointer.cast()) };
        Some(text)
    }

    /// Reads a borrowed string.
    fn borrow(pointer: *const c_char) -> Option<String> {
        if pointer.is_null() {
            return None;
        }

        Some(unsafe { CStr::from_ptr(pointer) }.to_string_lossy().into_owned())
    }

    fn pair(text: ItemText) -> (Option<String>, Option<String>) {
        (take(text.label), take(text.description))
    }

    // ======================================================================
    // Display name
    // ======================================================================

    #[test]
    fn a_non_empty_name_wins_over_the_fallback() {
        let (name, fallback) = (c("Terminal"), c("fallback"));
        assert_eq!(
            borrow(unsafe { lambdock_item_display_name(name.as_ptr(), fallback.as_ptr()) }),
            Some("Terminal".to_owned())
        );
    }

    #[test]
    fn an_empty_name_is_no_name() {
        let (name, fallback) = (c(""), c("fallback"));
        assert_eq!(
            borrow(unsafe { lambdock_item_display_name(name.as_ptr(), fallback.as_ptr()) }),
            Some("fallback".to_owned())
        );
        // A missing name falls back, and a missing fallback stays missing.
        assert_eq!(
            borrow(unsafe { lambdock_item_display_name(std::ptr::null(), fallback.as_ptr()) }),
            Some("fallback".to_owned())
        );
        assert_eq!(
            borrow(unsafe { lambdock_item_display_name(std::ptr::null(), std::ptr::null()) }),
            None
        );
    }

    // ======================================================================
    // Accessible text per kind
    // ======================================================================

    #[test]
    fn an_application_reads_its_name_and_window_count() {
        let name = c("Terminal");
        let (label, description) = pair(unsafe { lambdock_item_text_app(name.as_ptr(), 0) });
        assert_eq!(label.as_deref(), Some("Terminal"));
        assert_eq!(description.as_deref(), Some("Not running"));

        let (_, one) = pair(unsafe { lambdock_item_text_app(name.as_ptr(), 1) });
        assert_eq!(one.as_deref(), Some("Running, 1 window"));

        let (_, three) = pair(unsafe { lambdock_item_text_app(name.as_ptr(), 3) });
        assert_eq!(three.as_deref(), Some("Running, 3 windows"));
    }

    #[test]
    fn an_unnamed_application_still_says_what_it_is() {
        let (label, _) = pair(unsafe { lambdock_item_text_app(std::ptr::null(), 0) });
        assert_eq!(label.as_deref(), Some("Application"));

        let empty = c("");
        let (label, _) = pair(unsafe { lambdock_item_text_app(empty.as_ptr(), 0) });
        assert_eq!(label.as_deref(), Some("Application"));
    }

    #[test]
    fn a_dynamic_item_prepends_its_name_unless_the_reading_has_it() {
        let (name, plain) = (c("CPU"), c("42%"));
        let (label, description) =
            pair(unsafe { lambdock_item_text_dynamic(name.as_ptr(), plain.as_ptr()) });
        assert_eq!(label.as_deref(), Some("CPU: 42%"));
        assert_eq!(description.as_deref(), Some("Live reading from the dock."));

        // Meter items already write "name: reading"; repeating it would read as
        // "CPU: CPU: 42%".
        let reading = c("CPU: 42%");
        let (label, _) = pair(unsafe { lambdock_item_text_dynamic(name.as_ptr(), reading.as_ptr()) });
        assert_eq!(label.as_deref(), Some("CPU: 42%"));
    }

    #[test]
    fn a_dynamic_item_without_a_reading_is_just_its_name() {
        let name = c("CPU");
        for reading in [std::ptr::null(), c("").as_ptr()] {
            let (label, _) = pair(unsafe { lambdock_item_text_dynamic(name.as_ptr(), reading) });
            assert_eq!(label.as_deref(), Some("CPU"));
        }

        // Nameless with a reading: the reading is the label.
        let reading = c("42%");
        let (label, _) = pair(unsafe { lambdock_item_text_dynamic(std::ptr::null(), reading.as_ptr()) });
        assert_eq!(label.as_deref(), Some("42%"));

        // Nameless and no reading: a generic label rather than an empty one.
        let (label, _) = pair(unsafe { lambdock_item_text_dynamic(std::ptr::null(), std::ptr::null()) });
        assert_eq!(label.as_deref(), Some("Reading"));
    }

    #[test]
    fn a_stack_reads_its_name_and_what_it_opens() {
        let downloads = c("Downloads");
        let (label, description) = pair(unsafe { lambdock_item_text_stack(downloads.as_ptr()) });
        assert_eq!(label.as_deref(), Some("Downloads, folder stack"));
        assert_eq!(description.as_deref(), Some("Opens the folder's contents."));

        let (label, _) = pair(unsafe { lambdock_item_text_stack(std::ptr::null()) });
        assert_eq!(label.as_deref(), Some("Stack, folder stack"));
    }

    #[test]
    fn the_launchpad_reads_its_name_and_how_to_close_it() {
        let applications = c("Applications");
        let (label, description) =
            pair(unsafe { lambdock_item_text_launchpad(applications.as_ptr()) });
        assert_eq!(label.as_deref(), Some("Applications"));
        assert!(description.unwrap_or_default().contains("Escape"));

        let (label, _) = pair(unsafe { lambdock_item_text_launchpad(std::ptr::null()) });
        assert_eq!(label.as_deref(), Some("Applications"));
    }

    #[test]
    fn the_trash_counts_what_is_in_it() {
        let trash = c("Trash");

        let (label, empty) = pair(unsafe { lambdock_item_text_trash(trash.as_ptr(), 0) });
        assert_eq!(label.as_deref(), Some("Trash"));
        assert_eq!(empty.as_deref(), Some("The trash is empty. Opens the trash."));

        let (_, one) = pair(unsafe { lambdock_item_text_trash(trash.as_ptr(), 1) });
        assert_eq!(one.as_deref(), Some("1 item in the trash. Opens the trash."));

        let (_, four) = pair(unsafe { lambdock_item_text_trash(trash.as_ptr(), 4) });
        assert_eq!(four.as_deref(), Some("4 items in the trash. Opens the trash."));

        let (label, _) = pair(unsafe { lambdock_item_text_trash(std::ptr::null(), 2) });
        assert_eq!(label.as_deref(), Some("Trash"));
    }

    #[test]
    fn the_trash_tooltip_names_the_count_only_while_there_is_one() {
        let trash = c("Trash");

        for (count, expected) in [
            (0, "Trash"),
            (1, "Trash — 1 item"),
            (7, "Trash — 7 items"),
        ] {
            let tooltip = take(unsafe { lambdock_item_trash_tooltip(trash.as_ptr(), count) });
            assert_eq!(tooltip.as_deref(), Some(expected));
        }

        let unnamed = take(unsafe { lambdock_item_trash_tooltip(std::ptr::null(), 0) });
        assert_eq!(unnamed.as_deref(), Some("Trash"));
    }

    // ======================================================================
    // Gauge readings
    // ======================================================================

    #[test]
    fn the_gauge_fraction_is_clamped_to_a_full_ring_or_an_empty_one() {
        assert_eq!(lambdock_meter_reading_fraction(50.0, 100.0), 0.5);
        assert_eq!(lambdock_meter_reading_fraction(0.0, 100.0), 0.0);
        assert_eq!(lambdock_meter_reading_fraction(100.0, 100.0), 1.0);

        // Out-of-range readings fill or empty the ring instead of wrapping it.
        assert_eq!(lambdock_meter_reading_fraction(250.0, 100.0), 1.0);
        assert_eq!(lambdock_meter_reading_fraction(-40.0, 100.0), 0.0);

        // A gauge with no configured maximum is a percentage gauge.
        assert_eq!(lambdock_meter_reading_fraction(25.0, 0.0), 0.25);
        assert_eq!(lambdock_meter_reading_fraction(25.0, -10.0), 0.25);
    }

    #[test]
    fn a_broken_reading_never_reaches_the_gauge() {
        assert_eq!(lambdock_meter_reading_fraction(f64::NAN, 100.0), 0.0);
        assert_eq!(lambdock_meter_reading_fraction(f64::INFINITY, 100.0), 0.0);
    }

    #[test]
    fn the_reading_text_is_whole_numbers_with_the_unit_glued_on() {
        let percent = c("%");

        assert_eq!(
            take(unsafe { lambdock_meter_reading_text(42.4, percent.as_ptr()) }).as_deref(),
            Some("42%")
        );
        // Rounded to whole numbers, which is all the icon has room for.
        assert_eq!(
            take(unsafe { lambdock_meter_reading_text(42.6, percent.as_ptr()) }).as_deref(),
            Some("43%")
        );

        let celsius = c("°C");
        assert_eq!(
            take(unsafe { lambdock_meter_reading_text(54.0, celsius.as_ptr()) }).as_deref(),
            Some("54°C")
        );

        assert_eq!(
            take(unsafe { lambdock_meter_reading_text(7.0, std::ptr::null()) }).as_deref(),
            Some("7")
        );
    }

    #[test]
    fn a_broken_reading_prints_as_zero_and_an_absurd_one_is_clamped() {
        let percent = c("%");

        // NaN prints as 0, not as "nan".
        assert_eq!(
            take(unsafe { lambdock_meter_reading_text(f64::NAN, percent.as_ptr()) }).as_deref(),
            Some("0%")
        );

        // A wild reading would otherwise print as twenty digits and break the
        // icon's layout.
        let none = c("");
        assert_eq!(
            take(unsafe { lambdock_meter_reading_text(1e300, none.as_ptr()) }).as_deref(),
            Some("1000000000")
        );
    }

    // ======================================================================
    // Window titles (taskbar mode)
    // ======================================================================

    #[test]
    fn the_title_label_is_hidden_without_the_setting_or_a_window() {
        let (title, name, app_id) = (c("Inbox"), c("Mail"), c("mail"));

        assert_eq!(
            borrow(unsafe {
                lambdock_window_title_text(0, 1, title.as_ptr(), name.as_ptr(), app_id.as_ptr())
            }),
            None
        );
        assert_eq!(
            borrow(unsafe {
                lambdock_window_title_text(1, 0, title.as_ptr(), name.as_ptr(), app_id.as_ptr())
            }),
            None
        );
    }

    #[test]
    fn the_running_window_names_itself() {
        let (title, name, app_id) = (c("Inbox"), c("Mail"), c("mail"));

        assert_eq!(
            borrow(unsafe {
                lambdock_window_title_text(1, 1, title.as_ptr(), name.as_ptr(), app_id.as_ptr())
            })
            .as_deref(),
            Some("Inbox")
        );
    }

    #[test]
    fn a_window_that_has_not_named_itself_falls_back_to_the_application() {
        let (name, app_id) = (c("Mail"), c("mail"));
        let empty = c("");

        for title in [std::ptr::null(), empty.as_ptr()] {
            assert_eq!(
                borrow(unsafe {
                    lambdock_window_title_text(1, 1, title, name.as_ptr(), app_id.as_ptr())
                })
                .as_deref(),
                Some("Mail")
            );
        }

        assert_eq!(
            borrow(unsafe {
                lambdock_window_title_text(1, 1, std::ptr::null(), std::ptr::null(), app_id.as_ptr())
            })
            .as_deref(),
            Some("mail")
        );

        // Nothing at all to say still hides nothing: an empty title is a title.
        assert_eq!(
            borrow(unsafe {
                lambdock_window_title_text(
                    1,
                    1,
                    std::ptr::null(),
                    std::ptr::null(),
                    std::ptr::null(),
                )
            })
            .as_deref(),
            Some("")
        );
    }

    // ======================================================================
    // The inspector
    // ======================================================================

    #[test]
    fn the_inspector_names_the_settings_file_that_created_the_item() {
        let (path, key) = (c("/home/me/.config/lambdock/settings.scm"), c("app:Firefox"));

        assert_eq!(
            take(unsafe { lambdock_inspector_launcher_text(path.as_ptr(), key.as_ptr()) }).as_deref(),
            Some("app:Firefox, from /home/me/.config/lambdock/settings.scm")
        );

        let trash = c("trash:Trash");
        assert_eq!(
            take(unsafe { lambdock_inspector_launcher_text(std::ptr::null(), trash.as_ptr()) })
                .as_deref(),
            Some("trash:Trash (no settings file: the fallback dock)")
        );

        assert_eq!(
            take(unsafe { lambdock_inspector_launcher_text(std::ptr::null(), std::ptr::null()) })
                .as_deref(),
            Some("? (no settings file: the fallback dock)")
        );
    }

    #[test]
    fn the_inspector_says_where_the_icon_came_from() {
        let firefox = c("firefox");
        let missing = c("no-such-icon");
        let empty = c("");

        assert_eq!(
            take(unsafe { lambdock_inspector_icon_text(firefox.as_ptr(), 1) }).as_deref(),
            Some("firefox (found in the icon theme)")
        );
        assert_eq!(
            take(unsafe { lambdock_inspector_icon_text(missing.as_ptr(), 0) }).as_deref(),
            Some("no-such-icon (not in the icon theme; a fallback is drawn)")
        );
        assert_eq!(
            take(unsafe { lambdock_inspector_icon_text(empty.as_ptr(), 0) }).as_deref(),
            Some("no icon configured; the generic executable is drawn")
        );
    }

    #[test]
    fn the_inspector_names_the_poller_and_its_interval() {
        let poller = c("get-cpu-value");
        let empty = c("");

        assert_eq!(
            take(unsafe { lambdock_inspector_poller_text(poller.as_ptr(), 2000) }).as_deref(),
            Some("get-cpu-value, every 2000 ms")
        );
        assert_eq!(
            take(unsafe { lambdock_inspector_poller_text(empty.as_ptr(), 0) }).as_deref(),
            Some("none: this item does not poll")
        );
    }

    // ======================================================================
    // Ownership
    // ======================================================================

    #[test]
    fn clearing_frees_both_strings_and_zeroes_the_struct() {
        let name = c("Terminal");
        let mut text = unsafe { lambdock_item_text_app(name.as_ptr(), 2) };

        assert!(!text.label.is_null());
        assert!(!text.description.is_null());

        unsafe { lambdock_item_text_clear(&mut text) };

        assert!(text.label.is_null());
        assert!(text.description.is_null());

        // Clearing NULL is tolerated, so a kind that has nothing to say is safe.
        unsafe { lambdock_item_text_clear(std::ptr::null_mut()) };
    }
}
