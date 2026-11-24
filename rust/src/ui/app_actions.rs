// lambdock -- hyper-hackable desktop dock for Wayland
//
// SPDX-License-Identifier: MIT
// Copyright © Josep Bigorra <jjbigorra@gmail.com>

//! What an application's menu can offer: `src/ui/app_actions.c` in Rust.
//!
//! Three questions, answered from strings the dock already has: what kind of
//! application is this (a browser, a player, a file manager, neither), what
//! command opens another window of it, and -- for a browser the dock can read a
//! profile list for -- which profiles it could open. The playback controls come
//! from MPRIS on the session bus, which is the one part of this module that talks
//! to the outside world.
//!
//! The kind and the commands are pure -- names in, a decision out -- so they are
//! tested without a bus: a browser is matched by its categories first and its
//! names second, a browser's private window is only offered when the dock knows
//! the flag for that family, and a profile becomes one more row that opens a
//! window of its own.

use crate::glib::g_free;
use crate::log_warn;
use crate::ui::desktop_entry::strip_field_codes;
use gtk4::gio;
use gtk4::glib::{self, VariantTy};
use std::ffi::{CStr, CString};
use std::os::raw::{c_char, c_int};

/// `LAMBDOCK_APP_KIND_*` from the header.
pub const KIND_NONE: c_int = 0;
pub const KIND_BROWSER: c_int = 1;
pub const KIND_MUSIC: c_int = 2;
pub const KIND_FILES: c_int = 3;

/// `LAMBDOCK_APP_ACTION_*` from the header.
pub const ACTION_NEW_WINDOW: c_int = 0;
pub const ACTION_PRIVATE_WINDOW: c_int = 1;
pub const ACTION_PLAY_PAUSE: c_int = 2;
pub const ACTION_PREVIOUS: c_int = 3;
pub const ACTION_NEXT: c_int = 4;
pub const ACTION_STOP: c_int = 5;
pub const ACTION_HOME_FOLDER: c_int = 6;
pub const ACTION_PROFILE: c_int = 7;

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
    "floorp",
    "mullvad",
    "chromium",
    "chrome",
    "brave",
    "vivaldi",
    "thorium",
    "epiphany",
    "falkon",
    "qutebrowser",
    "midori",
    "zen-browser",
    "zen_browser",
];

/// The same for file managers: the one extra thing the dock offers them is a
/// window on the home folder, which every one of these opens from an argument.
const FILES_WORDS: &[&str] = &[
    "nautilus",
    "dolphin",
    "thunar",
    "nemo",
    "pcmanfm",
    "caja",
    "files",
    "file-manager",
    "org.gnome.files",
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

/// One row of an application's menu.
///
/// The label is a `String` because not every row is one of the fixed actions: a
/// browser profile is one more way to open a window, and it is named after the
/// profile.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct AppAction {
    /// The action this row performs, which is what picks its icon.
    pub action: c_int,
    /// What the row says.
    pub label: String,
    /// The command this row runs, when it is not the one the action's flag builds
    /// from the item's own `Exec`.
    pub command: Option<String>,
}

impl AppAction {
    /// A row for one of the fixed actions, labelled in the user's language.
    fn simple(action: c_int) -> Self {
        Self {
            action,
            label: crate::cstr::translated(action_label(action)),
            command: None,
        }
    }
}

/// One profile of a Mozilla-family browser, from its `profiles.ini`.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct Profile {
    /// What the browser calls the profile ("default", "work").
    pub name: String,
    /// Where the profile lives, absolute.
    pub path: String,
}

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

/// What kind of application this is: a browser, a player, a file manager, or
/// neither.
pub fn app_kind(
    app_id: Option<&str>,
    name: Option<&str>,
    exec: Option<&str>,
    categories: Option<&str>,
) -> c_int {
    // Categories first: they are the application's own claim about what it is,
    // and they cover every browser, player and file manager that ships a desktop
    // file.
    if categories_have(categories, "WebBrowser") {
        return KIND_BROWSER;
    }
    if categories_have(categories, "FileManager") {
        return KIND_FILES;
    }
    if categories_have(categories, "Music")
        || (categories_have(categories, "Audio") && categories_have(categories, "Player"))
    {
        return KIND_MUSIC;
    }

    // Then the names. The browser check comes first so a browser whose command
    // line happens to mention media is still a browser, and the file manager
    // check comes before the player one because "Files" is not a player.
    if app_mentions(app_id, name, exec, BROWSER_WORDS) {
        return KIND_BROWSER;
    }
    if app_mentions(app_id, name, exec, FILES_WORDS) {
        return KIND_FILES;
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

/// The program's file name, lower case: what the family is decided on.
fn program_name(program: &str) -> String {
    std::path::Path::new(program)
        .file_name()
        .map(|name| name.to_string_lossy().into_owned())
        .unwrap_or_default()
        .to_lowercase()
}

fn browser_family(program: &str) -> Family {
    let base = program_name(program);

    // The Firefox family speaks the same command line: every fork of it keeps
    // `--new-window` and `--private-window`.
    if ["firefox", "librewolf", "waterfox", "zen", "floorp", "mullvad", "tor-browser", "icecat"]
        .iter()
        .any(|name| base.contains(name))
    {
        Family::Firefox
    } else if ["chromium", "chrome", "brave", "vivaldi", "thorium", "yandex"]
        .iter()
        .any(|name| base.contains(name))
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

/// `argv` written as one line the GLib spawner parses back into the same words,
/// with `extra` appended.
///
/// Quoting only the words the shell would split keeps a command line that reads
/// like the desktop entry it came from, which matters when a launch fails and the
/// log is the only thing to look at.
fn command_line(argv: &[String], extra: &[&str]) -> String {
    let mut command = String::new();

    for word in argv.iter().map(String::as_str).chain(extra.iter().copied()) {
        let word = if word_needs_quotes(word) {
            shell_quote(word)
        } else {
            word.to_owned()
        };

        if !command.is_empty() {
            command.push(' ');
        }
        command.push_str(&word);
    }

    command
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

    Some(command_line(&argv, flag.as_slice()))
}

/// The command that opens a window of `profile` in the browser `exec` starts.
///
/// `--profile` takes a path, so the name a browser shows in its own profile
/// manager never has to be guessed at, and `--new-window` makes the profile get a
/// window even when it is already running.
fn profile_command(exec: Option<&str>, profile: &Profile) -> Option<String> {
    let argv = split_exec(exec?)?;

    Some(command_line(&argv, &["--profile", profile.path.as_str(), "--new-window"]))
}

/// The command that opens `exec` on the user's home folder: the one row a file
/// manager gets that its plain launcher cannot express.
fn home_command(exec: Option<&str>) -> Option<String> {
    let argv = split_exec(exec?)?;
    let home = std::env::var("HOME").ok().filter(|home| !home.is_empty())?;

    Some(command_line(&argv, &[home.as_str()]))
}

/// The directory a browser of this family keeps its profile list in, relative to
/// the user's home, when the dock knows it.
fn profile_home(program: &str) -> Option<&'static str> {
    if browser_family(program) != Family::Firefox {
        return None;
    }

    let base = program_name(program);

    if base.contains("librewolf") {
        Some(".librewolf")
    } else if base.contains("waterfox") {
        Some(".waterfox")
    } else if base.contains("zen") {
        Some(".zen")
    } else if base.contains("firefox") {
        Some(".mozilla/firefox")
    } else {
        // Floorp, Mullvad and the rest keep theirs where the dock does not look,
        // and a wrong path would offer profiles that are not there.
        None
    }
}

/// The Mozilla profile list, parsed: one entry per `[ProfileN]` section that names
/// both a name and a path.
///
/// `Install…` sections name a *default path* rather than a profile and are
/// skipped, which is also why a section without a `Name=` is dropped.
fn parse_profiles(text: &str, dir: &std::path::Path) -> Vec<Profile> {
    let mut profiles = Vec::new();
    let mut name: Option<String> = None;
    let mut path: Option<String> = None;
    let mut relative = true;
    let mut in_profile = false;

    // One section is finished: keep it when it describes a profile.
    let mut finish = |name: &mut Option<String>,
                      path: &mut Option<String>,
                      relative: &mut bool,
                      in_profile: bool| {
        if let (true, Some(name), Some(path)) = (in_profile, name.take(), path.take()) {
            let path = if *relative {
                dir.join(&path).to_string_lossy().into_owned()
            } else {
                path
            };

            profiles.push(Profile { name, path });
        }

        *relative = true;
    };

    for line in text.lines() {
        let line = line.trim();

        if line.is_empty() || line.starts_with('#') || line.starts_with(';') {
            continue;
        }

        if let Some(section) = line.strip_prefix('[').and_then(|rest| rest.strip_suffix(']')) {
            finish(&mut name, &mut path, &mut relative, in_profile);
            in_profile = section.starts_with("Profile");
            continue;
        }

        let Some((key, value)) = line.split_once('=') else {
            continue;
        };

        match key.trim().to_ascii_lowercase().as_str() {
            "name" => name = Some(value.trim().to_owned()),
            "path" => path = Some(value.trim().to_owned()),
            "isrelative" => relative = value.trim() != "0",
            _ => {}
        }
    }

    finish(&mut name, &mut path, &mut relative, in_profile);

    profiles
}

/// The profiles the browser `exec` starts could open, in the order its own
/// `profiles.ini` declares them.
///
/// An unreadable list is no profiles rather than an error: the rest of the menu
/// still works, and a browser that has never been started has nothing to offer.
pub fn profiles(exec: Option<&str>) -> Vec<Profile> {
    let Some(argv) = exec.and_then(split_exec) else {
        return Vec::new();
    };
    let Some(program) = argv.first() else {
        return Vec::new();
    };
    let Some(home) = profile_home(program) else {
        return Vec::new();
    };
    let Some(home_dir) = std::env::var_os("HOME") else {
        return Vec::new();
    };

    let dir = std::path::Path::new(&home_dir).join(home);

    match std::fs::read_to_string(dir.join("profiles.ini")) {
        // A profile whose directory is gone is not one a window can open: a
        // browser asked for it fails with a dialog nobody asked for.
        Ok(text) => parse_profiles(&text, &dir)
            .into_iter()
            .filter(|profile| std::path::Path::new(&profile.path).is_dir())
            .collect(),
        Err(_) => Vec::new(),
    }
}

/// `g_shell_quote`, so a quoted word is written the way the shell writes it.
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
        ACTION_HOME_FOLDER => c"Home Folder",
        ACTION_PROFILE => c"Profile",
        _ => c"",
    }
}

/// The actions an application of this kind offers, in menu order.
///
/// What each kind gets is what the dock can actually deliver: a browser window it
/// knows the flag for, a profile window when the browser publishes its profiles, a
/// player's transport only while one is on the bus, and a file manager's home
/// folder.
pub fn actions(
    app_id: Option<&str>,
    name: Option<&str>,
    exec: Option<&str>,
    categories: Option<&str>,
    music_bus: Option<&str>,
) -> Vec<AppAction> {
    let mut actions = Vec::new();

    match app_kind(app_id, name, exec, categories) {
        KIND_BROWSER => {
            let Some(exec) = exec.filter(|exec| !exec.is_empty()) else {
                return actions;
            };

            actions.push(AppAction::simple(ACTION_NEW_WINDOW));

            // Only when the dock knows how to ask this browser for one.
            if action_command(Some(exec), ACTION_PRIVATE_WINDOW).is_some() {
                actions.push(AppAction::simple(ACTION_PRIVATE_WINDOW));
            }

            // One row per profile: another window, in another set of tabs, logins
            // and settings -- the thing a "new window" cannot give a browser user.
            let profile_label = crate::cstr::translated(action_label(ACTION_PROFILE));
            for profile in profiles(Some(exec)) {
                actions.push(AppAction {
                    action: ACTION_PROFILE,
                    label: format!("{profile_label}: {}", profile.name),
                    command: profile_command(Some(exec), &profile),
                });
            }
        }
        KIND_FILES => {
            if let Some(command) = home_command(exec) {
                actions.push(AppAction {
                    action: ACTION_HOME_FOLDER,
                    command: Some(command),
                    ..AppAction::simple(ACTION_HOME_FOLDER)
                });
            }
        }
        // With no player on the bus these would be buttons that do nothing,
        // which is worse than an item whose menu is only its launcher.
        KIND_MUSIC if music_bus.is_some_and(|bus| !bus.is_empty()) => {
            actions.extend(
                [ACTION_PLAY_PAUSE, ACTION_PREVIOUS, ACTION_NEXT, ACTION_STOP]
                    .map(AppAction::simple),
            );
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
///
/// Fire and forget: MPRIS answers on the session bus, and a player that does not
/// is a log line rather than a dialog.
pub fn music_invoke(bus_name: &str, action: c_int) {
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
        assert_eq!(
            app_kind(None, None, None, Some("System;FileManager;")),
            KIND_FILES
        );
        assert_eq!(app_kind(None, None, None, Some("Audio;Editor;")), KIND_NONE);

        // A name that says player does not override a WebBrowser category.
        assert_eq!(
            app_kind(Some("spotify"), Some("Spotify"), None, Some("WebBrowser;")),
            KIND_BROWSER
        );
        // A name that says browser does not override a FileManager category.
        assert_eq!(
            app_kind(Some("firefox"), None, None, Some("FileManager;")),
            KIND_FILES
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
            KIND_FILES
        );
        assert_eq!(
            app_kind(None, None, Some("dolphin %U"), None),
            KIND_FILES
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
    /// does not promise a private window it cannot open. The forks keep their
    /// parent's command line, so they keep the parent's flags.
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
        assert_eq!(
            private("zen-browser %U").as_deref(),
            Some("zen-browser --private-window")
        );
        assert_eq!(private("chromium").as_deref(), Some("chromium --incognito"));
        assert_eq!(
            private("brave-browser").as_deref(),
            Some("brave-browser --incognito")
        );
        assert_eq!(
            private("thorium-browser").as_deref(),
            Some("thorium-browser --incognito")
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

    /// A file manager gets one row its launcher cannot express: the home folder.
    #[test]
    fn a_file_manager_gets_the_home_folder() {
        let home = std::env::var("HOME").unwrap_or_else(|_| "/home/u".to_owned());
        let as_word = if word_needs_quotes(&home) {
            shell_quote(&home)
        } else {
            home
        };

        assert_eq!(
            home_command(Some("nautilus --new-window")),
            Some(format!("nautilus --new-window {as_word}"))
        );
        // The field codes go, the program keeps its own arguments.
        assert_eq!(
            home_command(Some("thunar %U")),
            Some(format!("thunar {as_word}"))
        );

        // Nothing to open, no row.
        assert_eq!(home_command(None), None);
        assert_eq!(home_command(Some("")), None);
    }

    /// A browser offers a new window and, when it has one, a private window; a
    /// player offers its playback controls only while one is on the bus; a file
    /// manager offers the home folder.
    #[test]
    fn the_actions_follow_the_kind() {
        let actions_of = |app_id: &str, exec: &str, bus: Option<&str>| {
            actions(Some(app_id), None, Some(exec), None, bus)
                .iter()
                .map(|action| action.action)
                .collect::<Vec<_>>()
        };

        // Chromium rather than Firefox: the Mozilla profiles are read from the
        // home directory, and a test may not depend on what is installed there.
        assert_eq!(
            actions_of("chromium", "chromium %U", None),
            [ACTION_NEW_WINDOW, ACTION_PRIVATE_WINDOW]
        );
        assert_eq!(
            actions_of("epiphany", "epiphany %U", None),
            [ACTION_NEW_WINDOW]
        );

        let playback = [ACTION_PLAY_PAUSE, ACTION_PREVIOUS, ACTION_NEXT, ACTION_STOP];
        assert_eq!(
            actions_of("spotify", "spotify", Some("org.mpris.MediaPlayer2.spotify")),
            playback
        );
        // No player on the bus: buttons that do nothing are worse than no buttons.
        assert!(actions_of("spotify", "spotify", None).is_empty());

        assert_eq!(
            actions_of("nautilus", "nautilus -w", None),
            [ACTION_HOME_FOLDER]
        );
    }

    /// The profile list is the browser's own, and the `Install…` sections are not
    /// profiles: they name a default path, not a name.
    #[test]
    fn the_profiles_are_read_from_the_ini_that_declares_them() {
        let dir = std::path::Path::new("/home/u/.mozilla/firefox");
        let ini = "\
[General]
StartWithLastProfile=1

[Profile0]
Name=default
IsRelative=1
Path=abc123.default
Default=1

[Profile1]
Name=work
Path=/mnt/keys/firefox/work

[Install4F96D1932A9F858E]
Default=abc123.default
Locked=1
";

        assert_eq!(
            parse_profiles(ini, dir),
            [
                Profile {
                    name: "default".to_owned(),
                    path: "/home/u/.mozilla/firefox/abc123.default".to_owned(),
                },
                Profile {
                    name: "work".to_owned(),
                    path: "/mnt/keys/firefox/work".to_owned(),
                },
            ]
        );

        // A file that is not a profile list, and one with no sections at all.
        assert!(parse_profiles("", dir).is_empty());
        assert!(parse_profiles("[General]\nStartWithLastProfile=1\n", dir).is_empty());
    }

    /// A profile row opens a window of that profile, with the path quoted for the
    /// shell's parser.
    #[test]
    fn a_profile_row_opens_a_window_of_that_profile() {
        let profile = Profile {
            name: "work".to_owned(),
            path: "/mnt/My Keys/work".to_owned(),
        };

        assert_eq!(
            profile_command(Some("firefox %u"), &profile).as_deref(),
            Some("firefox --profile '/mnt/My Keys/work' --new-window")
        );
        assert_eq!(profile_command(None, &profile), None);
    }

    #[test]
    fn only_the_playback_actions_are_music() {
        for action in [ACTION_PLAY_PAUSE, ACTION_PREVIOUS, ACTION_NEXT, ACTION_STOP] {
            assert!(action_is_music(action), "{action}");
        }
        for action in [
            ACTION_NEW_WINDOW,
            ACTION_PRIVATE_WINDOW,
            ACTION_HOME_FOLDER,
            ACTION_PROFILE,
        ] {
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
            ACTION_HOME_FOLDER,
            ACTION_PROFILE,
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
