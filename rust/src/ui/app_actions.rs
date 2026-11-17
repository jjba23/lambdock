// lambdock -- hyper-hackable desktop dock for Wayland
//
// SPDX-License-Identifier: MIT
// Copyright © Josep Bigorra <jjbigorra@gmail.com>

//! What an application's menu can offer: `src/ui/app_actions.c` in Rust.
//!
//! Two questions, answered from strings the dock already has: what kind of
//! application is this (a browser, a player, neither), and what command opens
//! another window of it. The playback controls come from MPRIS on the session
//! bus, which is the one part of this module that talks to the outside world.
//!
//! The kind and the command are pure -- names in, a decision out -- so they are
//! tested without a bus: a browser is matched by its categories first and its
//! names second, and a browser's private window is only offered when the dock
//! knows the flag for that family.

use crate::glib::{g_free, strdup_string, tr};
use crate::log_warn;
use crate::ui::desktop_entry::strip_field_codes;
use gtk4::gio;
use gtk4::glib::{self, VariantTy};
use std::ffi::{CStr, CString};
use std::os::raw::{c_char, c_int, c_uint};

/// `LAMBDOCK_APP_KIND_*` from the header.
pub const KIND_NONE: c_int = 0;
pub const KIND_BROWSER: c_int = 1;
pub const KIND_MUSIC: c_int = 2;

/// `LAMBDOCK_APP_ACTION_*` from the header.
pub const ACTION_NEW_WINDOW: c_int = 0;
pub const ACTION_PRIVATE_WINDOW: c_int = 1;
pub const ACTION_PLAY_PAUSE: c_int = 2;
pub const ACTION_PREVIOUS: c_int = 3;
pub const ACTION_NEXT: c_int = 4;
pub const ACTION_STOP: c_int = 5;

/// The MPRIS spelling of a media player, from the freedesktop.org D-Bus
/// interface specification.
const MPRIS_PREFIX: &str = "org.mpris.MediaPlayer2.";
const MPRIS_OBJECT_PATH: &str = "/org/mpris/MediaPlayer2";
const MPRIS_PLAYER_INTERFACE: &str = "org.mpris.MediaPlayer2.Player";
const MPRIS_CALL_TIMEOUT_MS: i32 = 500;

/// Names that say "browser" when the desktop entry has no categories, or when the
/// item was written by hand in Scheme and never had a desktop file.
const BROWSER_WORDS: &[&str] = &[
    "firefox",
    "librewolf",
    "waterfox",
    "chromium",
    "chrome",
    "brave",
    "vivaldi",
    "opera",
    "epiphany",
    "falkon",
    "qutebrowser",
    "midori",
    "zen-browser",
];

/// The same for players, kept to applications that really speak MPRIS: the
/// playback controls are only offered when a player is found on the bus.
const MUSIC_WORDS: &[&str] = &[
    "spotify",
    "amberol",
    "rhythmbox",
    "lollypop",
    "elisa",
    "audacious",
    "clementine",
    "strawberry",
    "quodlibet",
    "banshee",
    "deadbeef",
    "tauon",
    "feishin",
    "sonixd",
    "tidal",
    "deezer",
    "youtube-music",
    "mpv",
    "vlc",
    "celluloid",
];

/// Whether `categories` lists `category`, case-insensitively.
fn categories_have(categories: Option<&str>, category: &str) -> bool {
    let Some(categories) = categories.filter(|categories| !categories.is_empty()) else {
        return false;
    };

    categories
        .split(';')
        .any(|part| !part.is_empty() && part.eq_ignore_ascii_case(category))
}

/// Whether one of the names mentions one of `words`, ignoring case.
fn text_mentions(text: Option<&str>, words: &[&str]) -> bool {
    let Some(text) = text.filter(|text| !text.is_empty()) else {
        return false;
    };

    let text = text.to_lowercase();

    words.iter().any(|word| text.contains(word))
}

fn app_mentions(
    app_id: Option<&str>,
    name: Option<&str>,
    exec: Option<&str>,
    words: &[&str],
) -> bool {
    text_mentions(app_id, words) || text_mentions(name, words) || text_mentions(exec, words)
}

/// What kind of application this is: a browser, a player, or neither.
pub fn app_kind(
    app_id: Option<&str>,
    name: Option<&str>,
    exec: Option<&str>,
    categories: Option<&str>,
) -> c_int {
    // Categories first: they are the application's own claim about what it is,
    // and they cover every browser and player that ships a desktop file.
    if categories_have(categories, "WebBrowser") {
        return KIND_BROWSER;
    }
    if categories_have(categories, "Music")
        || (categories_have(categories, "Audio") && categories_have(categories, "Player"))
    {
        return KIND_MUSIC;
    }

    // Then the names. The browser check comes first so a browser whose command
    // line happens to mention media is still a browser.
    if app_mentions(app_id, name, exec, BROWSER_WORDS) {
        return KIND_BROWSER;
    }
    if app_mentions(app_id, name, exec, MUSIC_WORDS) {
        return KIND_MUSIC;
    }

    KIND_NONE
}

/// The browser family, which decides the flag that opens a window.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
enum Family {
    Unknown,
    Firefox,
    Chromium,
    Edge,
    Opera,
}

fn browser_family(program: &str) -> Family {
    let base = std::path::Path::new(program)
        .file_name()
        .map(|name| name.to_string_lossy().into_owned())
        .unwrap_or_default();
    let base = base.to_lowercase();

    if base.contains("firefox") || base.contains("librewolf") || base.contains("waterfox") {
        Family::Firefox
    } else if base.contains("chromium")
        || base.contains("chrome")
        || base.contains("brave")
        || base.contains("vivaldi")
    {
        Family::Chromium
    } else if base.contains("edge") {
        Family::Edge
    } else if base.contains("opera") {
        Family::Opera
    } else {
        Family::Unknown
    }
}

/// The flag that opens a window of `private_mode`, or `None` when this browser
/// has no such mode (or no known flag for it).
fn browser_flag(family: Family, private_mode: bool) -> Option<&'static str> {
    match (family, private_mode) {
        (Family::Firefox, true) => Some("--private-window"),
        (Family::Firefox, false) => Some("--new-window"),
        (Family::Chromium, true) => Some("--incognito"),
        (Family::Chromium, false) => Some("--new-window"),
        (Family::Edge, true) => Some("--inprivate"),
        (Family::Edge, false) => Some("--new-window"),
        (Family::Opera, true) => Some("--private"),
        (Family::Opera, false) => Some("--new-window"),
        // Still worth launching: most browsers open another window when started
        // again. There is no private mode to promise.
        (Family::Unknown, _) => None,
    }
}

/// Splits an `Exec=` line into an argv, dropping the field codes first so both a
/// desktop entry ("firefox %u") and a Scheme item ("firefox %U") describe the same
/// launch.
fn split_exec(exec: &str) -> Option<Vec<String>> {
    let clean = strip_field_codes(exec);
    if clean.is_empty() {
        return None;
    }

    unsafe {
        let line = CString::new(clean).ok()?;
        let mut argv: *mut *mut c_char = std::ptr::null_mut();
        let mut argc: c_int = 0;

        if glib::ffi::g_shell_parse_argv(line.as_ptr(), &mut argc, &mut argv, std::ptr::null_mut())
            == 0
            || argv.is_null()
            || argc <= 0
        {
            glib::ffi::g_strfreev(argv);
            return None;
        }

        let words = (0..argc)
            .map(|index| {
                CStr::from_ptr(*argv.add(index as usize))
                    .to_string_lossy()
                    .into_owned()
            })
            .collect();

        glib::ffi::g_strfreev(argv);
        Some(words)
    }
}

/// Characters a shell leaves alone, so the word is written as it was.
fn word_needs_quotes(word: &str) -> bool {
    if word.is_empty() {
        return true;
    }

    word.chars()
        .any(|character| !character.is_ascii_alphanumeric() && !"-._/:=+,@%".contains(character))
}

/// The command that opens a window of the browser `exec` starts.
///
/// `None` when the browser has no private mode the dock can promise, or when the
/// action is not a window action at all.
pub fn action_command(exec: Option<&str>, action: c_int) -> Option<String> {
    let private_mode = match action {
        ACTION_NEW_WINDOW => false,
        ACTION_PRIVATE_WINDOW => true,
        _ => return None,
    };

    let argv = split_exec(exec?)?;
    let program = argv.first()?;
    let flag = browser_flag(browser_family(program), private_mode);

    // An unknown browser has no private mode the dock can promise, and a browser
    // asked for a plain window is launched as it is.
    if private_mode && flag.is_none() {
        return None;
    }

    let mut command = String::new();

    for (index, word) in argv.iter().enumerate() {
        // Quote a word only when the shell would split it: a command line that
        // reads like the desktop entry it came from is easier to follow in the
        // log when a launch fails.
        let word = if word_needs_quotes(word) {
            shell_quote(word)
        } else {
            word.clone()
        };

        if index > 0 {
            command.push(' ');
        }
        command.push_str(&word);
    }

    if let Some(flag) = flag {
        command.push(' ');
        command.push_str(flag);
    }

    Some(command)
}

/// `g_shell_quote`, so a quoted word is written the way the shell and the C side
/// write it.
fn shell_quote(word: &str) -> String {
    let Ok(word) = CString::new(word) else {
        return String::new();
    };

    unsafe {
        let quoted = glib::ffi::g_shell_quote(word.as_ptr());
        if quoted.is_null() {
            return String::new();
        }

        let text = CStr::from_ptr(quoted).to_string_lossy().into_owned();
        g_free(quoted.cast());

        text
    }
}

/// Whether `action` is one of the playback controls.
pub fn action_is_music(action: c_int) -> bool {
    matches!(
        action,
        ACTION_PLAY_PAUSE | ACTION_PREVIOUS | ACTION_NEXT | ACTION_STOP
    )
}

/// The menu label of an action, translated; empty for an action outside the enum.
pub fn action_label(action: c_int) -> &'static std::ffi::CStr {
    match action {
        ACTION_NEW_WINDOW => c"New Window",
        ACTION_PRIVATE_WINDOW => c"New Private Window",
        ACTION_PLAY_PAUSE => c"Play/Pause",
        ACTION_PREVIOUS => c"Previous Track",
        ACTION_NEXT => c"Next Track",
        ACTION_STOP => c"Stop Playback",
        _ => c"",
    }
}

/// The actions an application of this kind offers, in menu order.
pub fn actions(
    app_id: Option<&str>,
    name: Option<&str>,
    exec: Option<&str>,
    categories: Option<&str>,
    music_bus: Option<&str>,
) -> Vec<c_int> {
    let mut actions = Vec::new();

    match app_kind(app_id, name, exec, categories) {
        KIND_BROWSER => {
            if exec.is_some_and(|exec| !exec.is_empty()) {
                actions.push(ACTION_NEW_WINDOW);

                // Only when the dock knows how to ask this browser for one.
                if action_command(exec, ACTION_PRIVATE_WINDOW).is_some() {
                    actions.push(ACTION_PRIVATE_WINDOW);
                }
            }
        }
        // With no player on the bus these would be buttons that do nothing,
        // which is worse than an item whose menu is only its launcher.
        KIND_MUSIC if music_bus.is_some_and(|bus| !bus.is_empty()) => {
            actions.extend([ACTION_PLAY_PAUSE, ACTION_PREVIOUS, ACTION_NEXT, ACTION_STOP]);
        }
        _ => {}
    }

    actions
}

/// Whether the bus name's instance (`spotify`, `chromium.instance1234`) belongs
/// to this application.
pub fn player_instance_matches(instance: &str, app_id: Option<&str>, name: Option<&str>) -> bool {
    if instance.is_empty() {
        return false;
    }

    // The first component is the player's own name ("spotify"), the last one is
    // often the reverse-DNS tail ("io.bassi.Amberol" -> "Amberol"), and the
    // "...instance<N>" suffix that Firefox and Chromium append is neither.
    let head = instance.split('.').next().unwrap_or(instance);
    let tail = instance.rsplit_once('.').map_or(instance, |(_, tail)| tail);

    let leaf = app_id.map(|app_id| app_id.rsplit_once('.').map_or(app_id, |(_, leaf)| leaf));

    let head = normalize(Some(head));
    let full = normalize(Some(instance));
    let id = normalize(app_id);
    let leaf = normalize(leaf);
    let name = normalize(name);
    let tail = normalize(Some(tail));

    let matches = |left: &str, right: &str| tokens_match(left, right);

    matches(&head, &id)
        || matches(&head, &leaf)
        || matches(&head, &name)
        || matches(&tail, &id)
        || matches(&tail, &leaf)
        || matches(&tail, &name)
        || matches(&full, &id)
        || matches(&full, &leaf)
}

/// Lower case letters and digits only, so `org.mpris.MediaPlayer2.Rhythmbox`,
/// `io.bassi.Amberol` and `YouTube Music` all compare as words.
fn normalize(value: Option<&str>) -> String {
    value
        .unwrap_or_default()
        .chars()
        .filter(|character| character.is_ascii_alphanumeric())
        .map(|character| character.to_ascii_lowercase())
        .collect()
}

/// Whether two normalized names name the same application: equality is the
/// common case, and a prefix covers the players that append something of their
/// own (`amberol`, `amberolplayer`).
fn tokens_match(left: &str, right: &str) -> bool {
    if left.is_empty() || right.is_empty() {
        return false;
    }
    if left == right {
        return true;
    }

    // A prefix of four characters or fewer is not evidence of anything.
    if left.len().min(right.len()) < 4 {
        return false;
    }

    left.starts_with(right) || right.starts_with(left)
}

/// The MPRIS method behind an action, or `None` for a window action.
fn mpris_method(action: c_int) -> Option<&'static str> {
    match action {
        ACTION_PLAY_PAUSE => Some("PlayPause"),
        ACTION_PREVIOUS => Some("Previous"),
        ACTION_NEXT => Some("Next"),
        ACTION_STOP => Some("Stop"),
        _ => None,
    }
}

/// The bus name of the MPRIS player behind this application, when one is running.
pub fn music_player_find(app_id: Option<&str>, name: Option<&str>) -> Option<String> {
    if app_id.is_none_or(|app_id| app_id.is_empty()) && name.is_none_or(|name| name.is_empty()) {
        return None;
    }

    let connection = gio::bus_get_sync(gio::BusType::Session, None::<&gio::Cancellable>).ok()?;

    let reply = connection
        .call_sync(
            Some("org.freedesktop.DBus"),
            "/org/freedesktop/DBus",
            "org.freedesktop.DBus",
            "ListNames",
            None,
            Some(VariantTy::new("(as)").ok()?),
            gio::DBusCallFlags::NONE,
            MPRIS_CALL_TIMEOUT_MS,
            None::<&gio::Cancellable>,
        )
        .ok()?;

    let (names,): (Vec<String>,) = reply.get()?;

    names
        .into_iter()
        .filter_map(|bus_name| {
            let instance = bus_name.strip_prefix(MPRIS_PREFIX)?;
            player_instance_matches(instance, app_id, name).then_some(bus_name)
        })
        .next()
}

/// Asks the player at `bus_name` to run the method behind `action`.
fn music_invoke(bus_name: &str, action: c_int) {
    let Some(method) = mpris_method(action) else {
        return;
    };

    let Ok(connection) = gio::bus_get_sync(gio::BusType::Session, None::<&gio::Cancellable>) else {
        log_warn!("UI", "No session bus for MPRIS");
        return;
    };

    connection.call(
        Some(bus_name),
        MPRIS_OBJECT_PATH,
        MPRIS_PLAYER_INTERFACE,
        method,
        None,
        None,
        gio::DBusCallFlags::NONE,
        MPRIS_CALL_TIMEOUT_MS,
        None::<&gio::Cancellable>,
        move |result| {
            if let Err(error) = result {
                log_warn!("UI", "MPRIS {method} failed: {error}");
            }
        },
    );
}

/// What kind of application this is.
///
/// # Safety
///
/// Every argument must be null or a NUL-terminated string.
#[no_mangle]
pub unsafe extern "C" fn lambdock_app_kind(
    app_id: *const c_char,
    name: *const c_char,
    exec: *const c_char,
    categories: *const c_char,
) -> c_int {
    app_kind(
        crate::cstr::text(app_id).as_deref(),
        crate::cstr::text(name).as_deref(),
        crate::cstr::text(exec).as_deref(),
        crate::cstr::text(categories).as_deref(),
    )
}

/// The actions this application offers, as a `GArray` of `LambdockAppAction`.
///
/// # Safety
///
/// Every argument must be null or a NUL-terminated string; the result is
/// `(transfer full)` and freed with `g_array_unref`.
#[no_mangle]
pub unsafe extern "C" fn lambdock_app_actions(
    app_id: *const c_char,
    name: *const c_char,
    exec: *const c_char,
    categories: *const c_char,
    music_bus: *const c_char,
) -> *mut glib::ffi::GArray {
    let actions = actions(
        crate::cstr::text(app_id).as_deref(),
        crate::cstr::text(name).as_deref(),
        crate::cstr::text(exec).as_deref(),
        crate::cstr::text(categories).as_deref(),
        crate::cstr::text(music_bus).as_deref(),
    );

    let array = glib::ffi::g_array_new(glib::ffi::GFALSE, glib::ffi::GFALSE, c_uint::BITS / 8);

    for action in actions {
        let value = action as c_uint;
        glib::ffi::g_array_append_vals(array, std::ptr::from_ref(&value).cast(), 1);
    }

    array
}

/// The menu label of an action, translated. The pointer is gettext's, so it is
/// borrowed, exactly as the header says.
///
/// # Safety
///
/// The result must not be freed.
#[no_mangle]
pub unsafe extern "C" fn lambdock_app_action_label(action: c_int) -> *const c_char {
    let label = action_label(action);

    if label.is_empty() {
        return c"".as_ptr();
    }

    tr(label) as *const c_char
}

/// The command that opens a window of the browser `exec` starts.
///
/// # Safety
///
/// `exec` must be null or a NUL-terminated string; the result is `(transfer
/// full)` and freed with `g_free`.
#[no_mangle]
pub unsafe extern "C" fn lambdock_app_action_command(
    exec: *const c_char,
    action: c_int,
) -> *mut c_char {
    match action_command(crate::cstr::text(exec).as_deref(), action) {
        Some(command) => strdup_string(&command),
        None => std::ptr::null_mut(),
    }
}

/// Whether the action is one of the playback controls.
#[no_mangle]
pub extern "C" fn lambdock_app_action_is_music(action: c_int) -> c_int {
    c_int::from(action_is_music(action))
}

/// The MPRIS bus name of the player behind this application, when one is running.
///
/// # Safety
///
/// Every argument must be null or a NUL-terminated string; the result is
/// `(transfer full)` and freed with `g_free`.
#[no_mangle]
pub unsafe extern "C" fn lambdock_app_music_player_find(
    app_id: *const c_char,
    name: *const c_char,
) -> *mut c_char {
    match music_player_find(crate::cstr::text(app_id).as_deref(), crate::cstr::text(name).as_deref()) {
        Some(bus_name) => strdup_string(&bus_name),
        None => std::ptr::null_mut(),
    }
}

/// Asks the player at `bus_name` to run the method behind `action`.
///
/// # Safety
///
/// `bus_name` must be null or a NUL-terminated string.
#[no_mangle]
pub unsafe extern "C" fn lambdock_app_music_invoke(bus_name: *const c_char, action: c_int) {
    if let Some(bus_name) = crate::cstr::text(bus_name).filter(|bus_name| !bus_name.is_empty()) {
        music_invoke(&bus_name, action);
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    /// The categories are the application's own claim, and they win over the
    /// names -- which is what makes a browser packaged with a player still a
    /// browser.
    #[test]
    fn the_categories_decide_first() {
        assert_eq!(
            app_kind(None, None, None, Some("Network;WebBrowser;")),
            KIND_BROWSER
        );
        assert_eq!(
            app_kind(None, None, None, Some("AudioVideo;Audio;Player;")),
            KIND_MUSIC
        );
        assert_eq!(app_kind(None, None, None, Some("Audio;Editor;")), KIND_NONE);

        // A name that says player does not override a WebBrowser category.
        assert_eq!(
            app_kind(Some("spotify"), Some("Spotify"), None, Some("WebBrowser;")),
            KIND_BROWSER
        );
    }

    /// Without categories, the names decide: the app id, the display name or the
    /// command line.
    #[test]
    fn the_names_decide_second() {
        assert_eq!(app_kind(Some("firefox"), None, None, None), KIND_BROWSER);
        assert_eq!(
            app_kind(Some("org.mozilla.Firefox"), None, None, None),
            KIND_BROWSER
        );
        assert_eq!(app_kind(None, Some("LibreWolf"), None, None), KIND_BROWSER);
        assert_eq!(
            app_kind(None, None, Some("/usr/bin/chromium %U"), None),
            KIND_BROWSER
        );

        assert_eq!(app_kind(Some("spotify"), None, None, None), KIND_MUSIC);
        assert_eq!(
            app_kind(None, None, Some("vlc --started-from-file %U"), None),
            KIND_MUSIC
        );

        assert_eq!(
            app_kind(
                Some("org.gnome.Nautilus"),
                Some("Files"),
                Some("nautilus"),
                None
            ),
            KIND_NONE
        );
        assert_eq!(app_kind(None, None, None, None), KIND_NONE);
    }

    /// A browser whose command line mentions media is still a browser.
    #[test]
    fn a_browser_beats_a_player() {
        assert_eq!(
            app_kind(
                Some("firefox"),
                None,
                Some("firefox --media-session vlc"),
                None
            ),
            KIND_BROWSER
        );
    }

    /// Each family gets its own flag, and an unknown browser gets none: the dock
    /// does not promise a private window it cannot open.
    #[test]
    fn a_browser_window_gets_its_family_flag() {
        let new_window = |exec: &str| action_command(Some(exec), ACTION_NEW_WINDOW);
        let private = |exec: &str| action_command(Some(exec), ACTION_PRIVATE_WINDOW);

        assert_eq!(
            new_window("firefox %u").as_deref(),
            Some("firefox --new-window")
        );
        assert_eq!(
            private("firefox %u").as_deref(),
            Some("firefox --private-window")
        );
        assert_eq!(
            private("librewolf %U").as_deref(),
            Some("librewolf --private-window")
        );
        assert_eq!(private("chromium").as_deref(), Some("chromium --incognito"));
        assert_eq!(
            private("brave-browser").as_deref(),
            Some("brave-browser --incognito")
        );
        assert_eq!(
            private("microsoft-edge-stable").as_deref(),
            Some("microsoft-edge-stable --inprivate")
        );
        assert_eq!(private("opera").as_deref(), Some("opera --private"));

        // An unknown family: another window, but no private mode.
        assert_eq!(new_window("epiphany %U").as_deref(), Some("epiphany"));
        assert_eq!(private("epiphany %U"), None);

        // Not a window action, no command; no exec, no command.
        assert_eq!(action_command(Some("firefox"), ACTION_PLAY_PAUSE), None);
        assert_eq!(action_command(None, ACTION_NEW_WINDOW), None);
        assert_eq!(action_command(Some(""), ACTION_NEW_WINDOW), None);
    }

    /// The command line keeps its arguments, and quotes only the words the shell
    /// would split -- so a failing launch logs a line that reads like the entry it
    /// came from.
    #[test]
    fn the_command_keeps_its_arguments() {
        assert_eq!(
            action_command(
                Some("firefox --profile '/home/u/My Profile' %u"),
                ACTION_NEW_WINDOW
            )
            .as_deref(),
            Some("firefox --profile '/home/u/My Profile' --new-window")
        );

        // An unquoted space is a word boundary in the entry: it splits the path
        // into two arguments, and rejoining them gives the same line back.
        assert_eq!(
            action_command(
                Some("firefox --profile /home/u/My Profile"),
                ACTION_NEW_WINDOW
            )
            .as_deref(),
            Some("firefox --profile /home/u/My Profile --new-window")
        );
    }

    /// A browser offers a new window and, when it has one, a private window; a
    /// player offers its playback controls only while one is on the bus.
    #[test]
    fn the_actions_follow_the_kind() {
        assert_eq!(
            actions(Some("firefox"), None, Some("firefox %u"), None, None),
            [ACTION_NEW_WINDOW, ACTION_PRIVATE_WINDOW]
        );
        assert_eq!(
            actions(Some("epiphany"), None, Some("epiphany %U"), None, None),
            [ACTION_NEW_WINDOW]
        );

        let playback = [ACTION_PLAY_PAUSE, ACTION_PREVIOUS, ACTION_NEXT, ACTION_STOP];
        assert_eq!(
            actions(
                Some("spotify"),
                None,
                None,
                None,
                Some("org.mpris.MediaPlayer2.spotify")
            ),
            playback
        );
        // No player on the bus: buttons that do nothing are worse than no buttons.
        assert!(actions(Some("spotify"), None, None, None, None).is_empty());

        assert!(actions(Some("nautilus"), None, Some("nautilus"), None, None).is_empty());
    }

    #[test]
    fn only_the_playback_actions_are_music() {
        for action in [ACTION_PLAY_PAUSE, ACTION_PREVIOUS, ACTION_NEXT, ACTION_STOP] {
            assert!(action_is_music(action), "{action}");
        }
        for action in [ACTION_NEW_WINDOW, ACTION_PRIVATE_WINDOW] {
            assert!(!action_is_music(action), "{action}");
        }
    }

    /// Every action in the enum has a label, and an action outside it gets none.
    #[test]
    fn every_action_has_a_label() {
        for action in [
            ACTION_NEW_WINDOW,
            ACTION_PRIVATE_WINDOW,
            ACTION_PLAY_PAUSE,
            ACTION_PREVIOUS,
            ACTION_NEXT,
            ACTION_STOP,
        ] {
            assert!(!action_label(action).to_bytes().is_empty(), "{action}");
        }

        assert_eq!(action_label(99).to_bytes(), b"");
    }

    /// A bus name's instance has to name the same application: the head covers
    /// "spotify", the tail covers "io.bassi.Amberol", and a four-letter floor
    /// keeps "mpv" from matching everything that starts with it.
    #[test]
    fn a_bus_instance_has_to_name_its_application() {
        assert!(player_instance_matches("spotify", Some("spotify"), None));
        assert!(player_instance_matches(
            "chromium.instance1234",
            Some("chromium"),
            None
        ));
        assert!(player_instance_matches(
            "io.bassi.Amberol",
            Some("io.bassi.Amberol"),
            None
        ));
        assert!(player_instance_matches(
            "amberolplayer",
            Some("amberol"),
            None
        ));
        assert!(player_instance_matches(
            "instance1.spotify",
            None,
            Some("Spotify")
        ));

        assert!(!player_instance_matches("spotify", Some("vlc"), None));
        assert!(!player_instance_matches("", Some("spotify"), None));
        assert!(!player_instance_matches("mpv", Some("mp"), None));
    }
}
