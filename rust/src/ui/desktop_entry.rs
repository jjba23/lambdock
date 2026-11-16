// lambdock -- hyper-hackable desktop dock for Wayland
//
// SPDX-License-Identifier: MIT
// Copyright © Josep Bigorra <jjbigorra@gmail.com>

//! Desktop entries, the pure half: `src/ui/desktop_entry.c` in Rust.
//!
//! Two things in that file are decisions about strings rather than about GKeyFile,
//! GDir or the process-wide cache, and they are the two the rest of the dock asks
//! for by name: how a command line drops its field codes before it is launched,
//! and what an application id looks like written as words. Both are tested here;
//! parsing, scanning and the cache stay in C until that half moves.
//!
//! `desktop_entry_stem` and `last_component` stay in C as well: the parser needs
//! them to fill an entry's `stem`, which happens before anything here is asked.

use crate::glib::strdup_string;
use std::ffi::CStr;
use std::os::raw::c_char;

/// Field codes that expand to a file name, URL or an empty string. They have no
/// meaning for a launcher that is not given any argument, so they are dropped.
/// The deprecated `%d %D %n %N %v %m` codes are dropped as well.
fn is_dropped_field_code(code: char) -> bool {
    matches!(
        code,
        'f' | 'F' | 'u' | 'U' | 'i' | 'c' | 'k' | 'd' | 'D' | 'n' | 'N' | 'v' | 'm'
    )
}

/// A command line with its field codes removed and its spacing tidied.
///
/// `%%` becomes one `%`, a code that names a file or a URL is dropped, an
/// unknown code is kept verbatim so nothing silently disappears, and a lone
/// trailing `%` ends the line rather than being a code.
pub fn strip_field_codes(exec: &str) -> String {
    let mut out = String::with_capacity(exec.len());
    let mut characters = exec.chars().peekable();

    while let Some(character) = characters.next() {
        if character != '%' {
            out.push(character);
            continue;
        }

        let Some(next) = characters.peek().copied() else {
            break; // A lone trailing '%' is not a field code.
        };

        if next == '%' {
            out.push('%');
            characters.next();
        } else if is_dropped_field_code(next) {
            characters.next();
        } else {
            // Unknown code: keep it verbatim so nothing silently disappears.
            out.push('%');
        }
    }

    // Dropping %f/%U leaves double spaces behind, which would make the command
    // line ugly (and, in shell-less spawning, harmless but noisy in logs).
    collapse_spaces(out.trim_matches(|character: char| character.is_ascii_whitespace()))
}

/// Trims nothing and collapses every run of ASCII whitespace to one space.
fn collapse_spaces(text: &str) -> String {
    let mut clean = String::with_capacity(text.len());
    let mut previous_space = false;

    for character in text.chars() {
        let space = character.is_ascii_whitespace();
        if space && previous_space {
            continue;
        }

        clean.push(if space { ' ' } else { character });
        previous_space = space;
    }

    clean
}

/// `id` without its `.desktop` suffix, which is what compositors report.
fn entry_stem(id: &str) -> &str {
    id.strip_suffix(".desktop").unwrap_or(id)
}

/// The last component of a reverse-DNS id: `org.gnome.SystemMonitor` ->
/// `SystemMonitor`. Ids without a dot are already the application's name, and a
/// trailing dot does not make the tail empty.
fn last_component(value: &str) -> &str {
    match value.rsplit_once('.') {
        Some((_, tail)) if !tail.is_empty() => tail,
        _ => value,
    }
}

/// How an application id reads as words: camel case and separators split,
/// reverse-DNS vendor prefixes dropped, the first letter capitalised unless the
/// word is already cased.
pub fn app_id_to_name(app_id: &str) -> Option<String> {
    if app_id.is_empty() {
        return None;
    }

    // "org.gnome.SystemMonitor.desktop" is only ever a file name, and the vendor
    // prefix is not the application's name.
    let words = app_id_words(last_component(entry_stem(app_id)));

    let mut name = String::with_capacity(app_id.len());

    for (index, word) in words.iter().enumerate() {
        if index > 0 {
            name.push(' ');
        }

        let all_lower = !word.chars().any(|character| character.is_ascii_uppercase());
        let starts_lower = word.starts_with(|character: char| character.is_ascii_lowercase());

        if all_lower && starts_lower {
            let mut characters = word.chars();
            if let Some(first) = characters.next() {
                name.extend(first.to_uppercase());
            }
            name.push_str(characters.as_str());
        } else {
            name.push_str(word);
        }
    }

    if name.is_empty() {
        None
    } else {
        Some(name)
    }
}

/// Splits camel case and separators into words: `SystemMonitor` ->
/// `["System", "Monitor"]`, `google-chrome` -> `["google", "chrome"]`,
/// `ZFSManager` -> `["ZFS", "Manager"]`.
fn app_id_words(value: &str) -> Vec<String> {
    let characters: Vec<char> = value.chars().collect();
    let mut words = Vec::new();
    let mut word = String::new();
    let mut previous: Option<char> = None;

    for (index, character) in characters.iter().copied().enumerate() {
        if matches!(character, '-' | '_' | '.' | ' ' | '+') {
            if !word.is_empty() {
                words.push(std::mem::take(&mut word));
            }

            previous = None;
            continue;
        }

        // "System|Monitor" (lower then upper) and "ZFS|Manager" (the end of an
        // acronym) both start a new word.
        let next = characters.get(index + 1).copied();
        if !word.is_empty()
            && character.is_uppercase()
            && previous.is_some_and(|previous| {
                previous.is_lowercase()
                    || (previous.is_uppercase() && next.is_some_and(|next| next.is_lowercase()))
            })
        {
            words.push(std::mem::take(&mut word));
        }

        word.push(character);
        previous = Some(character);
    }

    if !word.is_empty() {
        words.push(word);
    }

    words
}

/// A command line with its field codes stripped, as C holds it.
///
/// # Safety
///
/// `exec` must be null or a NUL-terminated string; the result is `(transfer
/// full)` and freed with `g_free`.
#[no_mangle]
pub unsafe extern "C" fn lambdock_exec_strip_field_codes(exec: *const c_char) -> *mut c_char {
    if exec.is_null() {
        return std::ptr::null_mut();
    }

    let exec = CStr::from_ptr(exec).to_string_lossy();

    strdup_string(&strip_field_codes(&exec))
}

/// An application id written as words, or null when there are none.
///
/// # Safety
///
/// `app_id` must be null or a NUL-terminated string; the result is `(transfer
/// full)` and freed with `g_free`.
#[no_mangle]
pub unsafe extern "C" fn lambdock_app_id_to_name(app_id: *const c_char) -> *mut c_char {
    if app_id.is_null() || *app_id == 0 {
        return std::ptr::null_mut();
    }

    let app_id = CStr::from_ptr(app_id).to_string_lossy();

    match app_id_to_name(&app_id) {
        Some(name) => strdup_string(&name),
        None => std::ptr::null_mut(),
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    /// `%f` and friends name a file or a URL the dock has none of, so they go --
    /// and the spaces they leave behind go with them.
    #[test]
    fn field_codes_are_dropped_from_a_command_line() {
        assert_eq!(strip_field_codes("firefox %u"), "firefox");
        assert_eq!(
            strip_field_codes("nautilus --new-window %U"),
            "nautilus --new-window"
        );
        assert_eq!(
            strip_field_codes("app %f %F %i %c %k %d %D %n %N %v"),
            "app"
        );
        assert_eq!(strip_field_codes("  app %u  "), "app");
    }

    /// Anything the dock does not understand stays: silently deleting a `%z`
    /// would change the command the application is launched with.
    #[test]
    fn only_known_codes_are_touched() {
        assert_eq!(strip_field_codes("printf 100%%"), "printf 100%");
        assert_eq!(strip_field_codes("app %z"), "app %z");
        assert_eq!(strip_field_codes("app %"), "app");
        assert_eq!(strip_field_codes(""), "");
    }

    /// The suffix is what a Launcher= line must not carry: a compositor reports
    /// the id without it.
    #[test]
    fn a_desktop_id_loses_its_suffix() {
        assert_eq!(
            entry_stem("org.gnome.Nautilus.desktop"),
            "org.gnome.Nautilus"
        );
        assert_eq!(entry_stem("firefox"), "firefox");
        assert_eq!(entry_stem("weird.desktop.desktop"), "weird.desktop");
        assert_eq!(entry_stem(""), "");
    }

    /// Reverse-DNS ids carry the vendor, not the application.
    #[test]
    fn the_last_component_is_the_application() {
        assert_eq!(last_component("org.gnome.SystemMonitor"), "SystemMonitor");
        assert_eq!(last_component("firefox"), "firefox");
        // A trailing dot does not leave an empty name behind.
        assert_eq!(last_component("org.gnome."), "org.gnome.");
        assert_eq!(last_component(""), "");
    }

    /// The table the C test used to hold, now checked without a display and
    /// without the cache behind it.
    #[test]
    fn an_app_id_reads_as_words() {
        let cases = [
            ("org.gnome.SystemMonitor", "System Monitor"),
            ("org.gnome.SystemMonitor.desktop", "System Monitor"),
            ("org.wezfurlong.wezterm", "Wezterm"),
            ("com.google.Chrome", "Chrome"),
            ("org.kde.gwenview", "Gwenview"),
            ("io.github.foo.Bar", "Bar"),
            ("firefox", "Firefox"),
            ("wezterm", "Wezterm"),
            ("google-chrome", "Google Chrome"),
            ("libreoffice-writer", "Libreoffice Writer"),
            ("org.gnome.Software_Store", "Software Store"),
            // Already-cased ids are left alone.
            ("Alacritty", "Alacritty"),
            ("Freebuff", "Freebuff"),
            // Acronyms survive, and an acronym followed by a word is split.
            ("org.gnome.ZFSManager", "ZFS Manager"),
            ("org.gnome.Nautilus", "Nautilus"),
        ];

        for (app_id, expected) in cases {
            assert_eq!(
                app_id_to_name(app_id).as_deref(),
                Some(expected),
                "{app_id}"
            );
        }

        assert_eq!(app_id_to_name(""), None);
        // An id made only of separators has no words to make a name from.
        assert_eq!(app_id_to_name("..."), None);
    }

    #[test]
    fn words_are_split_on_case_and_separators() {
        assert_eq!(app_id_words("SystemMonitor"), ["System", "Monitor"]);
        assert_eq!(app_id_words("google-chrome"), ["google", "chrome"]);
        assert_eq!(app_id_words("ZFSManager"), ["ZFS", "Manager"]);
        assert_eq!(app_id_words("a_b.c+d e"), ["a", "b", "c", "d", "e"]);
        assert_eq!(app_id_words(""), Vec::<String>::new());
        assert_eq!(app_id_words("-_-"), Vec::<String>::new());
    }
}
