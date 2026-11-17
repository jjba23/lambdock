// lambdock -- hyper-hackable desktop dock for Wayland
//
// SPDX-License-Identifier: MIT
// Copyright © Josep Bigorra <jjbigorra@gmail.com>

//! What a dock item says about itself: labels, screen-reader descriptions,
//! tooltips and gauge readouts, as functions of values.
//!
//! No widgets, no display, no Guile: only strings, counts and numbers go in,
//! which is what makes the wording of every item kind testable without
//! constructing a dock.
//!
//! The strings go *out* as Rust `String`s. They used to be `g_strdup`ed C
//! strings that the caller freed through `lambdock_item_text_clear`, which meant
//! every early return was a leak waiting to happen; a value that owns its text
//! cannot leak. The one place a C string is still needed -- the accessible
//! properties GTK caches by pointer -- makes one at the call site.
//!
//! Formatting goes through `printf` rather than `format!` for one reason: a
//! translation may reorder its placeholders, and `%s`-style formats let it.

use crate::glib::{
    formatted_number_with_unit, formatted_with_two, translated, translated_plural,
    translated_with, translated_with_count, translated_with_two,
};
use std::ffi::CStr;

/// The label and description of one item, owning their text.
#[derive(Clone, Debug, Default, PartialEq, Eq)]
pub struct ItemText {
    pub label: String,
    pub description: String,
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

/// The "use the name, but not an empty one" rule that every kind repeats.
fn display_name<'a>(name: Option<&'a CStr>, fallback: &'a CStr) -> &'a CStr {
    match name.filter(|name| !name.is_empty()) {
        Some(name) => name,
        None => fallback,
    }
}

/// A `CStr` as an owned `String`. A name that is not UTF-8 is not a name, but it
/// still gets shown lossily: the alternative is an item that says nothing.
fn owned(text: &CStr) -> String {
    text.to_string_lossy().into_owned()
}

/// The label and description of an application item.
pub fn text_app(name: Option<&CStr>, window_count: u32) -> ItemText {
    let description = if window_count == 0 {
        translated(c"Not running")
    } else {
        translated_plural(c"Running, %u window", c"Running, %u windows", window_count)
    };

    ItemText {
        label: owned(display_name(name, c"Application")),
        description,
    }
}

/// The label and description of a polling (clock, CPU, meter) item.
pub fn text_dynamic(name: Option<&CStr>, reading: Option<&CStr>) -> ItemText {
    let title = name.filter(|name| !name.is_empty());
    let reading = reading.filter(|reading| !reading.is_empty());

    let label = match (title, reading) {
        // Meter items already write "name: reading", and repeating the name
        // would read as "CPU: CPU: 42%".
        (Some(title), Some(reading)) if reading.to_bytes().starts_with(title.to_bytes()) => {
            owned(reading)
        }
        (Some(title), Some(reading)) => translated_with_two(c"%s: %s", title, reading),
        // Nothing to prepend: the reading is the label.
        (None, Some(reading)) => owned(reading),
        (Some(title), None) => owned(title),
        // Nameless and no reading: say something rather than nothing.
        (None, None) => translated(c"Reading"),
    };

    ItemText {
        label,
        description: translated(c"Live reading from the dock."),
    }
}

/// The label and description of a folder stack item.
pub fn text_stack(name: Option<&CStr>) -> ItemText {
    ItemText {
        label: translated_with(c"%s, folder stack", display_name(name, c"Stack")),
        description: translated(c"Opens the folder's contents."),
    }
}

/// The label and description of the application grid item.
pub fn text_launchpad(name: Option<&CStr>) -> ItemText {
    ItemText {
        label: owned(display_name(name, c"Applications")),
        description: translated(c"Opens the application grid. Escape closes it."),
    }
}

/// The label and description of the trash item.
pub fn text_trash(name: Option<&CStr>, item_count: u32) -> ItemText {
    let label = owned(display_name(name, c"Trash"));

    let description = if item_count == 0 {
        translated(c"The trash is empty. Opens the trash.")
    } else {
        let count = translated_plural(
            c"%u item in the trash",
            c"%u items in the trash",
            item_count,
        );
        translated_with(c"%s. Opens the trash.", &crate::glib::c_string(&count))
    };

    ItemText { label, description }
}

/// The trash item's tooltip: its name, plus the count while the trash is not
/// empty.
pub fn trash_tooltip(name: Option<&CStr>, item_count: u32) -> String {
    let title = display_name(name, c"Trash");

    if item_count == 0 {
        return owned(title);
    }

    let items = translated_plural(c"%u item", c"%u items", item_count);

    formatted_with_two(c"%s — %s", title, &crate::glib::c_string(&items))
}

/// The gauge's 0..1 fraction, clamped, and never non-finite.
pub fn meter_reading_fraction(reading: f64, maximum: f64) -> f64 {
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
pub fn meter_reading_text(reading: f64, unit: Option<&CStr>) -> String {
    // The gauge writes whole numbers, so a wild reading would otherwise print as
    // twenty digits and break the icon's layout.
    let shown = finite_or(reading, 0.0).clamp(-1e9, 1e9);

    formatted_number_with_unit(shown, unit.unwrap_or(c""))
}

/// The text to show inside the icon, or nothing to keep the label hidden.
pub fn window_title_text<'a>(
    show_titles: bool,
    has_windows: bool,
    window_title: Option<&'a CStr>,
    name: Option<&'a CStr>,
    app_id: Option<&'a CStr>,
) -> Option<&'a CStr> {
    if !show_titles || !has_windows {
        return None;
    }

    if let Some(title) = window_title.filter(|title| !title.is_empty()) {
        return Some(title);
    }

    // The application's own name, then its id, then nothing -- which is still a
    // title, so the label shows rather than disappearing.
    Some(name.or(app_id).unwrap_or(c""))
}

/// "key, from path" -- or the fallback-dock sentence when there is no file.
pub fn inspector_launcher_text(config_path: Option<&CStr>, item_key: Option<&CStr>) -> String {
    let key = item_key.filter(|key| !key.is_empty()).unwrap_or(c"?");

    match config_path.filter(|path| !path.is_empty()) {
        Some(path) => translated_with_two(c"%s, from %s", key, path),
        None => translated_with(c"%s (no settings file: the fallback dock)", key),
    }
}

/// The icon name and where it was found.
pub fn inspector_icon_text(icon: Option<&CStr>, found_in_theme: bool) -> String {
    let Some(icon) = icon.filter(|icon| !icon.is_empty()) else {
        return translated(c"no icon configured; the generic executable is drawn");
    };

    if found_in_theme {
        translated_with(c"%s (found in the icon theme)", icon)
    } else {
        translated_with(c"%s (not in the icon theme; a fallback is drawn)", icon)
    }
}

/// The poller and its interval, or a sentence saying the item does not poll.
pub fn inspector_poller_text(poll_name: Option<&CStr>, interval_ms: u32) -> String {
    let Some(name) = poll_name.filter(|name| !name.is_empty()) else {
        return translated(c"none: this item does not poll");
    };

    translated_with_count(c"%s, every %u ms", name, interval_ms)
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::ffi::CString;

    fn c(text: &str) -> CString {
        CString::new(text).expect("no interior NUL")
    }

    fn pair(text: ItemText) -> (String, String) {
        (text.label, text.description)
    }

    // ======================================================================
    // Display name
    // ======================================================================

    #[test]
    fn a_non_empty_name_wins_over_the_fallback() {
        let (name, fallback) = (c("Terminal"), c("fallback"));
        assert_eq!(display_name(Some(&name), &fallback), &*name);
    }

    #[test]
    fn an_empty_name_is_no_name() {
        let (name, fallback) = (c(""), c("fallback"));

        assert_eq!(display_name(Some(&name), &fallback), &*fallback);
        assert_eq!(display_name(None, &fallback), &*fallback);
    }

    // ======================================================================
    // Accessible text per kind
    // ======================================================================

    #[test]
    fn an_application_reads_its_name_and_window_count() {
        let name = c("Terminal");

        let (label, description) = pair(text_app(Some(&name), 0));
        assert_eq!(label, "Terminal");
        assert_eq!(description, "Not running");

        let (_, one) = pair(text_app(Some(&name), 1));
        assert_eq!(one, "Running, 1 window");

        let (_, three) = pair(text_app(Some(&name), 3));
        assert_eq!(three, "Running, 3 windows");
    }

    #[test]
    fn an_unnamed_application_still_says_what_it_is() {
        let (label, _) = pair(text_app(None, 0));
        assert_eq!(label, "Application");

        let empty = c("");
        let (label, _) = pair(text_app(Some(&empty), 0));
        assert_eq!(label, "Application");
    }

    #[test]
    fn a_dynamic_item_prepends_its_name_unless_the_reading_has_it() {
        let (name, plain) = (c("CPU"), c("42%"));
        let (label, description) = pair(text_dynamic(Some(&name), Some(&plain)));
        assert_eq!(label, "CPU: 42%");
        assert_eq!(description, "Live reading from the dock.");

        // Meter items already write "name: reading"; repeating it would read as
        // "CPU: CPU: 42%".
        let reading = c("CPU: 42%");
        let (label, _) = pair(text_dynamic(Some(&name), Some(&reading)));
        assert_eq!(label, "CPU: 42%");
    }

    #[test]
    fn a_dynamic_item_without_a_reading_is_just_its_name() {
        let name = c("CPU");
        let empty = c("");

        for reading in [None, Some(empty.as_c_str())] {
            let (label, _) = pair(text_dynamic(Some(&name), reading));
            assert_eq!(label, "CPU");
        }

        // Nameless with a reading: the reading is the label.
        let reading = c("42%");
        let (label, _) = pair(text_dynamic(None, Some(&reading)));
        assert_eq!(label, "42%");

        // Nameless and no reading: a generic label rather than an empty one.
        let (label, _) = pair(text_dynamic(None, None));
        assert_eq!(label, "Reading");
    }

    #[test]
    fn a_stack_reads_its_name_and_what_it_opens() {
        let downloads = c("Downloads");

        let (label, description) = pair(text_stack(Some(&downloads)));
        assert_eq!(label, "Downloads, folder stack");
        assert_eq!(description, "Opens the folder's contents.");

        let (label, _) = pair(text_stack(None));
        assert_eq!(label, "Stack, folder stack");
    }

    #[test]
    fn the_launchpad_reads_its_name_and_how_to_close_it() {
        let applications = c("Applications");

        let (label, description) = pair(text_launchpad(Some(&applications)));
        assert_eq!(label, "Applications");
        assert!(description.contains("Escape"));

        let (label, _) = pair(text_launchpad(None));
        assert_eq!(label, "Applications");
    }

    #[test]
    fn the_trash_counts_what_is_in_it() {
        let trash = c("Trash");

        let (label, empty) = pair(text_trash(Some(&trash), 0));
        assert_eq!(label, "Trash");
        assert_eq!(empty, "The trash is empty. Opens the trash.");

        let (_, one) = pair(text_trash(Some(&trash), 1));
        assert_eq!(one, "1 item in the trash. Opens the trash.");

        let (_, four) = pair(text_trash(Some(&trash), 4));
        assert_eq!(four, "4 items in the trash. Opens the trash.");

        let (label, _) = pair(text_trash(None, 2));
        assert_eq!(label, "Trash");
    }

    #[test]
    fn the_trash_tooltip_names_the_count_only_while_there_is_one() {
        let trash = c("Trash");

        for (count, expected) in [(0, "Trash"), (1, "Trash — 1 item"), (7, "Trash — 7 items")] {
            assert_eq!(trash_tooltip(Some(&trash), count), expected);
        }

        assert_eq!(trash_tooltip(None, 0), "Trash");
    }

    // ======================================================================
    // Gauge readings
    // ======================================================================

    #[test]
    fn the_gauge_fraction_is_clamped_to_a_full_ring_or_an_empty_one() {
        assert_eq!(meter_reading_fraction(50.0, 100.0), 0.5);
        assert_eq!(meter_reading_fraction(0.0, 100.0), 0.0);
        assert_eq!(meter_reading_fraction(100.0, 100.0), 1.0);

        // Out-of-range readings fill or empty the ring instead of wrapping it.
        assert_eq!(meter_reading_fraction(250.0, 100.0), 1.0);
        assert_eq!(meter_reading_fraction(-40.0, 100.0), 0.0);

        // A gauge with no configured maximum is a percentage gauge.
        assert_eq!(meter_reading_fraction(25.0, 0.0), 0.25);
        assert_eq!(meter_reading_fraction(25.0, -10.0), 0.25);
    }

    #[test]
    fn a_broken_reading_never_reaches_the_gauge() {
        assert_eq!(meter_reading_fraction(f64::NAN, 100.0), 0.0);
        assert_eq!(meter_reading_fraction(f64::INFINITY, 100.0), 0.0);
    }

    #[test]
    fn the_reading_text_is_whole_numbers_with_the_unit_glued_on() {
        let percent = c("%");

        assert_eq!(meter_reading_text(42.4, Some(&percent)), "42%");
        // Rounded to whole numbers, which is all the icon has room for.
        assert_eq!(meter_reading_text(42.6, Some(&percent)), "43%");

        let celsius = c("°C");
        assert_eq!(meter_reading_text(54.0, Some(&celsius)), "54°C");
        assert_eq!(meter_reading_text(7.0, None), "7");
    }

    #[test]
    fn a_broken_reading_prints_as_zero_and_an_absurd_one_is_clamped() {
        let percent = c("%");

        // NaN prints as 0, not as "nan".
        assert_eq!(meter_reading_text(f64::NAN, Some(&percent)), "0%");

        // A wild reading would otherwise print as twenty digits and break the
        // icon's layout.
        let none = c("");
        assert_eq!(meter_reading_text(1e300, Some(&none)), "1000000000");
    }

    // ======================================================================
    // Window titles (taskbar mode)
    // ======================================================================

    #[test]
    fn the_title_label_is_hidden_without_the_setting_or_a_window() {
        let (title, name, app_id) = (c("Inbox"), c("Mail"), c("mail"));

        assert_eq!(
            window_title_text(false, true, Some(&title), Some(&name), Some(&app_id)),
            None
        );
        assert_eq!(
            window_title_text(true, false, Some(&title), Some(&name), Some(&app_id)),
            None
        );
    }

    #[test]
    fn the_running_window_names_itself() {
        let (title, name, app_id) = (c("Inbox"), c("Mail"), c("mail"));

        assert_eq!(
            window_title_text(true, true, Some(&title), Some(&name), Some(&app_id)),
            Some(&*title)
        );
    }

    #[test]
    fn a_window_that_has_not_named_itself_falls_back_to_the_application() {
        let (name, app_id) = (c("Mail"), c("mail"));
        let empty = c("");

        for title in [None, Some(empty.as_c_str())] {
            assert_eq!(
                window_title_text(true, true, title, Some(&name), Some(&app_id)),
                Some(&*name)
            );
        }

        assert_eq!(
            window_title_text(true, true, None, None, Some(&app_id)),
            Some(&*app_id)
        );

        // Nothing at all to say still hides nothing: an empty title is a title.
        assert_eq!(window_title_text(true, true, None, None, None), Some(c""));
    }

    // ======================================================================
    // The inspector
    // ======================================================================

    #[test]
    fn the_inspector_names_the_settings_file_that_created_the_item() {
        let (path, key) = (c("/home/me/.config/lambdock/settings.scm"), c("app:Firefox"));

        assert_eq!(
            inspector_launcher_text(Some(&path), Some(&key)),
            "app:Firefox, from /home/me/.config/lambdock/settings.scm"
        );

        let trash = c("trash:Trash");
        assert_eq!(
            inspector_launcher_text(None, Some(&trash)),
            "trash:Trash (no settings file: the fallback dock)"
        );

        assert_eq!(
            inspector_launcher_text(None, None),
            "? (no settings file: the fallback dock)"
        );
    }

    #[test]
    fn the_inspector_says_where_the_icon_came_from() {
        let firefox = c("firefox");
        let missing = c("no-such-icon");
        let empty = c("");

        assert_eq!(
            inspector_icon_text(Some(&firefox), true),
            "firefox (found in the icon theme)"
        );
        assert_eq!(
            inspector_icon_text(Some(&missing), false),
            "no-such-icon (not in the icon theme; a fallback is drawn)"
        );
        assert_eq!(
            inspector_icon_text(Some(&empty), false),
            "no icon configured; the generic executable is drawn"
        );
    }

    #[test]
    fn the_inspector_names_the_poller_and_its_interval() {
        let poller = c("get-cpu-value");
        let empty = c("");

        assert_eq!(
            inspector_poller_text(Some(&poller), 2000),
            "get-cpu-value, every 2000 ms"
        );
        assert_eq!(
            inspector_poller_text(Some(&empty), 0),
            "none: this item does not poll"
        );
    }
}
