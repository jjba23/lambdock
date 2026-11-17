// lambdock -- hyper-hackable desktop dock for Wayland
//
// SPDX-License-Identifier: MIT
// Copyright © Josep Bigorra <jjbigorra@gmail.com>

//! Desktop entries: `src/ui/desktop_entry.c` in Rust.
//!
//! Parse `.desktop` files, index them under every name a window might report,
//! and answer name/icon/exec lookups. The dock asks this module three questions
//! -- what is this app id called, what icon does it wear, what does it run -- and
//! every layer above it (the item builder, the launchers, the popovers) goes
//! through the same cache.
//!
//! The half that is a decision about strings (the field codes, the wording of an
//! id) is at the top and is where the tests are sharpest; the half that touches
//! the filesystem is below it and is shaped by one rule: a window's app id and
//! its desktop file rarely agree on spelling, so the cache indexes every entry
//! under every name it could be reported as.

use crate::glib::{g_free, g_malloc0, strdup_string};
use crate::log_info;
use crate::log_warn;
use gtk4::glib::{self, KeyFile, KeyFileFlags};
use std::collections::{HashMap, HashSet};
use std::ffi::{CStr, CString};
use std::os::raw::{c_char, c_int, c_void};
use std::path::{Path, PathBuf};
use std::sync::{Mutex, MutexGuard};

/// Milliseconds a built cache is trusted before it is rebuilt on demand.
/// `DESKTOP_CACHE_TTL_MS` in the header.
const CACHE_TTL_MS: i64 = 60_000;

/// The group every desktop file keeps its launcher in.
const GROUP: &str = "Desktop Entry";

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
pub unsafe fn lambdock_exec_strip_field_codes(exec: *const c_char) -> *mut c_char {
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
pub unsafe fn lambdock_app_id_to_name(app_id: *const c_char) -> *mut c_char {
    if app_id.is_null() || *app_id == 0 {
        return std::ptr::null_mut();
    }

    let app_id = CStr::from_ptr(app_id).to_string_lossy();

    match app_id_to_name(&app_id) {
        Some(name) => strdup_string(&name),
        None => std::ptr::null_mut(),
    }
}

/// One extra launcher an application declares in its own desktop file:
/// `LambdockDesktopAction` from the header.
#[repr(C)]
pub struct DesktopAction {
    pub id: *mut c_char,
    pub name: *mut c_char,
    pub exec: *mut c_char,
}

/// One installed application: `LambdockDesktopEntry` from the header, field for
/// field. C reads these fields directly, so the order is the contract.
#[repr(C)]
pub struct DesktopEntry {
    pub id: *mut c_char,
    pub stem: *mut c_char,
    pub name: *mut c_char,
    pub exec: *mut c_char,
    pub icon: *mut c_char,
    pub wm_class: *mut c_char,
    pub categories: *mut c_char,
    pub no_display: c_int,
    pub hidden: c_int,
    pub path: *mut c_char,
    pub actions: *mut glib::ffi::GPtrArray,
    pub alternates: *mut glib::ffi::GPtrArray,
}

/// Reads a GLib-allocated string and frees it.
unsafe fn read_and_free(pointer: *mut c_char) -> String {
    if pointer.is_null() {
        return String::new();
    }

    let text = CStr::from_ptr(pointer).to_string_lossy().into_owned();
    g_free(pointer.cast());

    text
}



unsafe fn ptr_array_len(array: *const glib::ffi::GPtrArray) -> usize {
    if array.is_null() {
        0
    } else {
        (*array).len as usize
    }
}

unsafe fn ptr_array_at(array: *const glib::ffi::GPtrArray, index: usize) -> *mut c_void {
    *(*array).pdata.add(index)
}

unsafe extern "C" fn free_action(data: *mut c_void) {
    if data.is_null() {
        return;
    }

    let action = data.cast::<DesktopAction>();
    g_free((*action).id.cast());
    g_free((*action).name.cast());
    g_free((*action).exec.cast());
    g_free(action.cast());
}

unsafe extern "C" fn free_entry(data: *mut c_void) {
    lambdock_desktop_entry_free(data.cast());
}

/// Case-insensitive, locale-aware string folding, the way the C used
/// `g_utf8_casefold` -- `to_lowercase` is not the same function.
fn casefold(value: &str) -> String {
    let Ok(value) = CString::new(value) else {
        return String::new();
    };

    unsafe { read_and_free(glib::ffi::g_utf8_casefold(value.as_ptr(), -1)) }
}

/// Name ordering the way the user's locale asks for it, not byte ordering.
fn collate(left: &str, right: &str) -> std::cmp::Ordering {
    let left = CString::new(left).unwrap_or_default();
    let right = CString::new(right).unwrap_or_default();

    unsafe { glib::ffi::g_utf8_collate(left.as_ptr(), right.as_ptr()).cmp(&0) }
}

/// The first of a semicolon-separated list, when it is not empty.
fn first_component(value: &str) -> Option<String> {
    value
        .split(';')
        .next()
        .filter(|part| !part.is_empty())
        .map(str::to_owned)
}

/// Parses the entry's own `Actions=` list.
///
/// Each name in it points at a `[Desktop Action <name>]` group with a `Name=` and
/// an `Exec=`; a group missing either is not a launcher and is dropped instead of
/// showing a blank menu row.
unsafe fn parse_actions(key_file: &KeyFile) -> *mut glib::ffi::GPtrArray {
    let Ok(list) = key_file.string(GROUP, "Actions") else {
        return std::ptr::null_mut();
    };

    let actions = glib::ffi::g_ptr_array_new_with_free_func(Some(free_action));

    for id in list.split(';').filter(|id| !id.is_empty()) {
        let group = format!("Desktop Action {id}");
        if !key_file.has_group(&group) {
            continue;
        }

        let name = key_file
            .locale_string(&group, "Name", None)
            .ok()
            .filter(|name| !name.is_empty());
        let exec = key_file
            .string(&group, "Exec")
            .ok()
            .map(|exec| strip_field_codes(&exec))
            .filter(|exec| !exec.is_empty());

        let (Some(name), Some(exec)) = (name, exec) else {
            continue;
        };

        let action = g_malloc0(std::mem::size_of::<DesktopAction>()).cast::<DesktopAction>();
        (*action).id = strdup_string(id);
        (*action).name = strdup_string(&name);
        (*action).exec = strdup_string(&exec);
        glib::ffi::g_ptr_array_add(actions, action.cast());
    }

    if ptr_array_len(actions) == 0 {
        glib::ffi::g_ptr_array_free(actions, glib::ffi::GTRUE);
        return std::ptr::null_mut();
    }

    actions
}

/// Parses a `[Desktop Entry]` group.
///
/// # Safety
///
/// `data` and `id` must be null or NUL-terminated strings; the result is
/// `(transfer full)` and freed with `lambdock_desktop_entry_free`.
pub unsafe fn lambdock_desktop_entry_new_from_data(
    data: *const c_char,
    id: *const c_char,
) -> *mut DesktopEntry {
    if data.is_null() || *data == 0 {
        return std::ptr::null_mut();
    }

    let data = CStr::from_ptr(data).to_string_lossy();
    let key_file = KeyFile::new();

    if key_file.load_from_data(&data, KeyFileFlags::NONE).is_err() || !key_file.has_group(GROUP) {
        return std::ptr::null_mut();
    }

    // Only applications are launchers; a Link= or Directory= entry is not.
    let is_application = key_file
        .string(GROUP, "Type")
        .ok()
        .is_none_or(|kind| kind.eq_ignore_ascii_case("Application"));
    if !is_application {
        return std::ptr::null_mut();
    }

    let name = key_file
        .locale_string(GROUP, "Name", None)
        .ok()
        .filter(|name| !name.is_empty());
    let Some(name) = name else {
        return std::ptr::null_mut();
    };

    let exec = key_file
        .string(GROUP, "Exec")
        .ok()
        .map(|exec| strip_field_codes(&exec))
        .filter(|exec| !exec.is_empty());
    let Some(exec) = exec else {
        return std::ptr::null_mut();
    };

    let icon = key_file
        .string(GROUP, "Icon")
        .ok()
        .filter(|icon| !icon.is_empty());
    let wm_class = key_file
        .string(GROUP, "StartupWMClass")
        .ok()
        .and_then(|class| first_component(&class));
    let categories = key_file
        .string(GROUP, "Categories")
        .ok()
        .filter(|categories| !categories.is_empty());

    let entry = g_malloc0(std::mem::size_of::<DesktopEntry>()).cast::<DesktopEntry>();

    (*entry).id = crate::glib::g_strdup(id);
    (*entry).stem = match crate::cstr::text(id) {
        Some(id) => strdup_string(entry_stem(&id)),
        None => std::ptr::null_mut(),
    };
    (*entry).name = strdup_string(&name);
    (*entry).exec = strdup_string(&exec);
    (*entry).icon = strdup_string(icon.as_deref().unwrap_or("application-x-executable"));
    (*entry).wm_class = wm_class.map_or(std::ptr::null_mut(), |class| strdup_string(&class));
    (*entry).categories = categories.map_or(std::ptr::null_mut(), |categories| {
        strdup_string(&categories)
    });
    (*entry).actions = parse_actions(&key_file);
    (*entry).no_display = c_int::from(key_file.boolean(GROUP, "NoDisplay").unwrap_or(false));
    (*entry).hidden = c_int::from(key_file.boolean(GROUP, "Hidden").unwrap_or(false));

    entry
}

/// Reads and parses a `.desktop` file; null for missing, unreadable or invalid
/// files.
///
/// # Safety
///
/// `path` must be null or a NUL-terminated string; the result is `(transfer
/// full)`.
pub unsafe fn lambdock_desktop_entry_new_from_file(
    path: *const c_char,
) -> *mut DesktopEntry {
    let Some(path) = crate::cstr::text(path) else {
        return std::ptr::null_mut();
    };

    let Ok(data) = std::fs::read(&path) else {
        return std::ptr::null_mut();
    };
    let data = String::from_utf8_lossy(&data);

    let id = Path::new(&path)
        .file_name()
        .map(|name| name.to_string_lossy().into_owned())
        .unwrap_or_default();

    let data = match CString::new(data.as_bytes()) {
        Ok(data) => data,
        Err(_) => return std::ptr::null_mut(),
    };
    let id = CString::new(id).unwrap_or_default();

    let entry = lambdock_desktop_entry_new_from_data(data.as_ptr(), id.as_ptr());

    // Where the file lives is part of what an entry is: it is what tells the
    // copies of an application apart (see lambdock_desktop_entry_source_label).
    if !entry.is_null() {
        (*entry).path = strdup_string(&path);
    }

    entry
}

/// Frees one entry and everything it owns; null is a no-op.
///
/// # Safety
///
/// `entry` must be null or a record this module allocated, not freed twice.
pub unsafe fn lambdock_desktop_entry_free(entry: *mut DesktopEntry) {
    if entry.is_null() {
        return;
    }

    for field in [
        (*entry).id,
        (*entry).stem,
        (*entry).name,
        (*entry).exec,
        (*entry).icon,
        (*entry).wm_class,
        (*entry).categories,
        (*entry).path,
    ] {
        if !field.is_null() {
            g_free(field.cast());
        }
    }

    if !(*entry).actions.is_null() {
        glib::ffi::g_ptr_array_unref((*entry).actions);
    }
    if !(*entry).alternates.is_null() {
        glib::ffi::g_ptr_array_unref((*entry).alternates);
    }

    g_free(entry.cast());
}

/// Where an installed copy came from: "Flatpak", the user's own files, the
/// distribution, or "Application" for an entry that was never read from a file.
///
/// # Safety
///
/// `entry` must be null or a live entry; the result is `(transfer full)`.
pub unsafe fn lambdock_desktop_entry_source_label(
    entry: *const DesktopEntry,
) -> *mut c_char {
    if entry.is_null() {
        return std::ptr::null_mut();
    }

    let Some(path) = crate::cstr::text((*entry).path) else {
        return crate::cstr::translated_dup(c"Application");
    };

    // "~/.local/share/flatpak/exports/share/applications/x.desktop" and
    // "/var/lib/flatpak/..." are both Flatpak exports.
    if path.contains("/flatpak/") {
        return crate::cstr::translated_dup(c"Flatpak");
    }

    let user_dir = glib::user_data_dir().to_string_lossy().into_owned();
    if !user_dir.is_empty() && path.starts_with(&user_dir) {
        return crate::cstr::translated_dup(c"Your files");
    }

    crate::cstr::translated_dup(c"System")
}

/// Whether the entry lists `category` in its `Categories=`.
///
/// # Safety
///
/// `entry` and `category` must be null or valid; the comparison is
/// case-insensitive, as the freedesktop.org categories are written.
pub unsafe fn lambdock_desktop_entry_has_category(
    entry: *const DesktopEntry,
    category: *const c_char,
) -> c_int {
    if entry.is_null() || category.is_null() || *category == 0 {
        return 0;
    }

    let Some(categories) = crate::cstr::text((*entry).categories) else {
        return 0;
    };
    let category = CStr::from_ptr(category).to_string_lossy();

    c_int::from(
        categories
            .split(';')
            .any(|part| !part.is_empty() && part.eq_ignore_ascii_case(&category)),
    )
}

/// The `applications` directory of every XDG data dir, in precedence order.
///
/// The user's own directory comes first, then Flatpak exports, then the system
/// ones: the first entry found for an id is the one that wins.
fn applications_dirs() -> Vec<PathBuf> {
    let mut dirs = Vec::new();
    let mut seen = HashSet::new();

    let add = |share_dir: PathBuf, dirs: &mut Vec<PathBuf>, seen: &mut HashSet<PathBuf>| {
        if share_dir.as_os_str().is_empty() {
            return;
        }

        let path = share_dir.join("applications");
        if seen.contains(&path) || !path.is_dir() {
            return;
        }

        seen.insert(path.clone());
        dirs.push(path);
    };

    add(glib::user_data_dir(), &mut dirs, &mut seen);
    add(
        glib::user_data_dir()
            .join("flatpak")
            .join("exports")
            .join("share"),
        &mut dirs,
        &mut seen,
    );
    add(
        PathBuf::from("/var/lib/flatpak/exports/share"),
        &mut dirs,
        &mut seen,
    );

    for share_dir in glib::system_data_dirs() {
        add(share_dir, &mut dirs, &mut seen);
    }

    dirs
}

/// Every `.desktop` file in one directory, first copy of each id winning.
unsafe fn scan_one_dir(
    dir: &Path,
    out: &mut Vec<*mut DesktopEntry>,
    seen_ids: &mut HashSet<String>,
) {
    let Ok(dir) = std::fs::read_dir(dir) else {
        return;
    };

    for entry in dir.flatten() {
        let name = entry.file_name().to_string_lossy().into_owned();

        if !name.ends_with(".desktop") || seen_ids.contains(&name) {
            continue;
        }

        let path = entry.path();
        let Ok(path) = CString::new(path.to_string_lossy().as_bytes()) else {
            continue;
        };

        let parsed = lambdock_desktop_entry_new_from_file(path.as_ptr());
        if parsed.is_null() {
            continue;
        }

        // Record the id even when the entry is hidden: the user's override is
        // supposed to hide the system entry, not to resurface it later.
        seen_ids.insert(name);

        if (*parsed).hidden != 0 || (*parsed).no_display != 0 {
            lambdock_desktop_entry_free(parsed);
            continue;
        }

        out.push(parsed);
    }
}

/// What makes two entries the same application: its name *and* the strongest
/// identity both declare -- the window class a compositor reports, or, for
/// entries that declare no class, the icon they draw.
///
/// Two same-named applications stay separate as long as their classes or icons
/// differ, because they really are two applications (a GNOME settings panel and a
/// third-party tool may both be called "Displays").
unsafe fn application_identity_key(entry: *const DesktopEntry) -> String {
    let name = casefold(&crate::cstr::text((*entry).name).unwrap_or_default());

    let wm_class = crate::cstr::text((*entry).wm_class).filter(|class| !class.is_empty());
    let signal = wm_class.or_else(|| crate::cstr::text((*entry).icon));

    // \x1f cannot appear in a desktop file value, so it cannot make two
    // different names collide.
    format!("{name}\x1f{}", casefold(&signal.unwrap_or_default()))
}

/// Attaches a copy of an application to the one that stays on the grid.
unsafe fn add_alternate(keeper: *mut DesktopEntry, copy: *mut DesktopEntry) {
    if keeper.is_null() || copy.is_null() {
        return;
    }

    if (*keeper).alternates.is_null() {
        (*keeper).alternates = glib::ffi::g_ptr_array_new_with_free_func(Some(free_entry));
    }

    glib::ffi::g_ptr_array_add((*keeper).alternates, copy.cast());
}

/// Collapses the copies of one application into the first one seen.
///
/// Scan order is the precedence order -- user entries, then Flatpak exports, then
/// the system ones -- so the one that stays is the copy the user's own directory
/// or Flatpak put there. The copies that lose are not thrown away: they move to
/// the winner's `alternates`, because a launcher should offer them rather than
/// decide silently.
unsafe fn collapse_duplicates(entries: &mut Vec<*mut DesktopEntry>) {
    let mut kept: HashMap<String, *mut DesktopEntry> = HashMap::new();
    let mut index = 0;

    while index < entries.len() {
        let entry = entries[index];
        let key = application_identity_key(entry);

        match kept.get(&key) {
            Some(keeper) => {
                entries.remove(index);
                add_alternate(*keeper, entry);
            }
            None => {
                kept.insert(key, entry);
                index += 1;
            }
        }
    }
}

/// Turns owned entries into the `GPtrArray` that holds them, with the free
/// function that releases each one.
unsafe fn into_ptr_array(entries: Vec<*mut DesktopEntry>) -> *mut glib::ffi::GPtrArray {
    let array = glib::ffi::g_ptr_array_new_with_free_func(Some(free_entry));

    for entry in entries {
        glib::ffi::g_ptr_array_add(array, entry.cast());
    }

    array
}

/// Every installed application, once each, sorted by display name.
///
/// # Safety
///
/// The result is `(transfer full)`; free it with `lambdock_desktop_entries_free`.
pub unsafe fn lambdock_desktop_entries_scan() -> *mut glib::ffi::GPtrArray {
    let mut entries = Vec::new();
    let mut seen_ids = HashSet::new();

    for dir in applications_dirs() {
        scan_one_dir(&dir, &mut entries, &mut seen_ids);
    }

    // Before the sort: collapsing in scan order is what makes the precedence
    // predictable, since the sort only orders by name.
    collapse_duplicates(&mut entries);

    entries.sort_by(|left, right| {
        collate(
            &crate::cstr::text((**left).name).unwrap_or_default(),
            &crate::cstr::text((**right).name).unwrap_or_default(),
        )
    });

    into_ptr_array(entries)
}

/// Frees an array returned by the scan or the cache.
///
/// # Safety
///
/// `entries` must be null or an array this module created.
pub unsafe fn lambdock_desktop_entries_free(entries: *mut glib::ffi::GPtrArray) {
    if !entries.is_null() {
        glib::ffi::g_ptr_array_free(entries, glib::ffi::GTRUE);
    }
}

/// The cache is one value, not three loose globals: everything that reads or
/// replaces it goes through this struct, so it has one owner, one lifetime, and
/// one place to look when "the entries are stale" comes up.
struct Cache {
    /// Owned; never null once built.
    entries: *mut glib::ffi::GPtrArray,
    /// Normalized alias -> entry, borrowed from `entries` and their alternates.
    index: HashMap<String, *mut DesktopEntry>,
    /// Monotonic time of the build, 0 when never built.
    built_at: i64,
}

// The pointers are into memory this module owns and the dock only touches from
// the main thread; the mutex is what makes that true for the cache itself.
unsafe impl Send for Cache {}

static CACHE: Mutex<Option<Cache>> = Mutex::new(None);

fn cache() -> MutexGuard<'static, Option<Cache>> {
    CACHE
        .lock()
        .unwrap_or_else(|poisoned| poisoned.into_inner())
}

/// Case- and punctuation-insensitive form: "org.gnome.Nautilus" and
/// "org-gnome-nautilus" and "ORGGNOMENAUTILUS" all collapse to one key.
fn normalize_key(value: &str) -> String {
    value
        .chars()
        .filter(|character| character.is_ascii_alphanumeric())
        .map(|character| character.to_ascii_lowercase())
        .collect()
}

unsafe fn cache_index_add(
    index: &mut HashMap<String, *mut DesktopEntry>,
    alias: Option<String>,
    entry: *mut DesktopEntry,
) {
    let Some(alias) = alias.filter(|alias| !alias.is_empty()) else {
        return;
    };

    let key = normalize_key(&alias);
    if key.is_empty() {
        return;
    }

    // First one wins: the exact aliases are inserted before the looser ones, and
    // the entry on the grid before its alternates.
    index.entry(key).or_insert(entry);
}

/// The first argument of a command line that is an executable, as a file name.
fn exec_base_name(exec: Option<String>) -> Option<String> {
    let exec = exec.filter(|exec| !exec.is_empty())?;

    for token in exec.split([' ', '\t']).filter(|token| !token.is_empty()) {
        if token.starts_with("env=")
            || token == "env"
            || token == "flatpak"
            || token == "run"
            || token.starts_with('-')
        {
            continue;
        }

        return Path::new(token)
            .file_name()
            .map(|name| name.to_string_lossy().into_owned());
    }

    None
}

/// Every name an entry could be reported under: the desktop id, the window class
/// a compositor announces, the executable's base name, the last component of a
/// reverse-DNS id.
unsafe fn cache_index_entry(
    index: &mut HashMap<String, *mut DesktopEntry>,
    entry: *mut DesktopEntry,
) {
    cache_index_add(index, crate::cstr::text((*entry).id), entry);

    let stem = crate::cstr::text((*entry).stem);
    cache_index_add(index, stem.clone(), entry);
    cache_index_add(index, crate::cstr::text((*entry).wm_class), entry);
    cache_index_add(index, exec_base_name(crate::cstr::text((*entry).exec)), entry);
    cache_index_add(
        index,
        stem.as_deref().map(|stem| last_component(stem).to_owned()),
        entry,
    );
}

unsafe fn cache_build() {
    let entries = lambdock_desktop_entries_scan();
    let mut index: HashMap<String, *mut DesktopEntry> = HashMap::new();

    for position in 0..ptr_array_len(entries) {
        let entry = ptr_array_at(entries, position).cast::<DesktopEntry>();
        cache_index_entry(&mut index, entry);

        // The copies the grid does not list are indexed too, so a window of the
        // copy that lost still resolves to the application and its icon.
        for alternate in 0..ptr_array_len((*entry).alternates) {
            cache_index_entry(
                &mut index,
                ptr_array_at((*entry).alternates, alternate).cast::<DesktopEntry>(),
            );
        }
    }

    log_info!(
        "UI",
        "Desktop entry cache built: {} applications, {} keys",
        ptr_array_len(entries),
        index.len()
    );

    *cache() = Some(Cache {
        entries,
        index,
        built_at: glib::monotonic_time(),
    });
}

/// Builds the cache when it is missing or older than its TTL.
unsafe fn cache_ensure() {
    {
        let guard = cache();

        if let Some(cache) = guard.as_ref() {
            let age_ms = (glib::monotonic_time() - cache.built_at) / 1000;
            if age_ms <= CACHE_TTL_MS {
                return;
            }
        }
    }

    cache_build();
}

/// Drops the cache, so the next lookup rescans the applications directories.
///
/// # Safety
///
/// Safe to call from any thread: the cache is behind a mutex, and the entries it
/// held are freed here.
pub unsafe fn lambdock_desktop_cache_invalidate() {
    let mut guard = cache();

    if let Some(cache) = guard.take() {
        lambdock_desktop_entries_free(cache.entries);
    }
}

/// The cached, name-sorted array of entries that should be shown in launchers.
///
/// # Safety
///
/// The result is `(transfer none)`: the cache owns it, and it is stale after the
/// next rebuild.
pub unsafe fn lambdock_desktop_entries_cached() -> *mut glib::ffi::GPtrArray {
    cache_ensure();

    cache()
        .as_ref()
        .map_or(std::ptr::null_mut(), |cache| cache.entries)
}

/// The entry a reported app id resolves to.
///
/// # Safety
///
/// `app_id` must be null or a NUL-terminated string; the result is owned by the
/// cache and is stale after the next rebuild.
pub unsafe fn lambdock_desktop_entry_lookup(
    app_id: *const c_char,
) -> *const DesktopEntry {
    let Some(app_id) = crate::cstr::text(app_id).filter(|app_id| !app_id.is_empty()) else {
        return std::ptr::null();
    };

    cache_ensure();

    let key = normalize_key(&app_id);
    if key.is_empty() {
        return std::ptr::null();
    }

    let guard = cache();
    let Some(cache) = guard.as_ref() else {
        return std::ptr::null();
    };

    if let Some(entry) = cache.index.get(&key) {
        return *entry;
    }

    // One more try without a vendor prefix, so "SystemMonitor" still finds
    // "org.gnome.SystemMonitor" when the compositor reports the short form.
    let tail_key = normalize_key(last_component(&app_id));
    if !tail_key.is_empty() && tail_key != key {
        if let Some(entry) = cache.index.get(&tail_key) {
            return *entry;
        }
    }

    std::ptr::null()
}

/// The display name of the application behind `app_id`: the localized name from
/// its desktop entry when there is one, the derived name when there is not.
///
/// # Safety
///
/// `app_id` must be null or a NUL-terminated string; the result is `(transfer
/// full)`.
pub unsafe fn lambdock_app_name_for(app_id: *const c_char) -> *mut c_char {
    let Some(id) = crate::cstr::text(app_id).filter(|id| !id.is_empty()) else {
        return std::ptr::null_mut();
    };

    let entry = lambdock_desktop_entry_lookup(app_id);
    if !entry.is_null() {
        if let Some(name) = crate::cstr::text((*entry).name).filter(|name| !name.is_empty()) {
            return strdup_string(&name);
        }
    }

    match app_id_to_name(&id) {
        Some(name) => strdup_string(&name),
        None => strdup_string(&id),
    }
}

/// The icon of the application behind `app_id`: the entry's `Icon=` when there is
/// a desktop file, the app id itself otherwise (icon themes often ship the app id
/// as an icon name).
///
/// # Safety
///
/// `app_id` must be null or a NUL-terminated string; the result is `(transfer
/// full)`.
pub unsafe fn lambdock_app_icon_for(app_id: *const c_char) -> *mut c_char {
    let Some(id) = crate::cstr::text(app_id).filter(|id| !id.is_empty()) else {
        return std::ptr::null_mut();
    };

    let entry = lambdock_desktop_entry_lookup(app_id);
    if !entry.is_null() {
        if let Some(icon) = crate::cstr::text((*entry).icon).filter(|icon| !icon.is_empty()) {
            return strdup_string(&icon);
        }
    }

    strdup_string(&id)
}

/// Spawns a command line asynchronously, logging (instead of crashing) on
/// failure.
///
/// # Safety
///
/// `command` must be null or a NUL-terminated string.
pub unsafe fn lambdock_spawn_command(command: *const c_char) -> c_int {
    let Some(command) = crate::cstr::text(command).filter(|command| !command.is_empty()) else {
        return 0;
    };

    match glib::spawn_command_line_async(&command) {
        Ok(()) => 1,
        Err(error) => {
            log_warn!("UI", "Failed to launch '{command}': {error}");
            0
        }
    }
}

/// Opens a path with the desktop's default handler (`xdg-open`). Fire and forget.
///
/// # Safety
///
/// `path` must be null or a NUL-terminated string.
pub unsafe fn lambdock_open_path(path: *const c_char) -> c_int {
    let Some(path) = crate::cstr::text(path).filter(|path| !path.is_empty()) else {
        return 0;
    };

    let argv = [Path::new("xdg-open"), Path::new(&path)];

    match glib::spawn_async(
        None::<&Path>,
        &argv,
        &[],
        glib::SpawnFlags::SEARCH_PATH,
        None,
    ) {
        Ok(_) => 1,
        Err(error) => {
            log_warn!("UI", "Failed to open '{path}': {error}");
            0
        }
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

    // ======================================================================
    // The record, and parsing one
    // ======================================================================

    /// The record C reads field by field: seven strings, two flags, the path, and
    /// the two arrays it walks itself. The numbers come from a
    /// `printf("%zu", sizeof/offsetof)` against the header.
    #[test]
    fn the_entry_mirror_matches_the_header() {
        use std::mem::{offset_of, size_of};

        assert_eq!(size_of::<DesktopEntry>(), 88);
        assert_eq!(offset_of!(DesktopEntry, no_display), 56);
        assert_eq!(offset_of!(DesktopEntry, hidden), 60);
        assert_eq!(offset_of!(DesktopEntry, path), 64);
        assert_eq!(offset_of!(DesktopEntry, actions), 72);
        assert_eq!(offset_of!(DesktopEntry, alternates), 80);
        assert_eq!(size_of::<DesktopAction>(), 24);
    }

    /// A parsed entry keeps what the owner wrote, and its own extra launcher.
    #[test]
    fn an_entry_is_parsed_from_its_desktop_file() {
        let data = "[Desktop Entry]\n\
                    Type=Application\n\
                    Name=Files\n\
                    Exec=nautilus --new-window %U\n\
                    Icon=org.gnome.Nautilus\n\
                    StartupWMClass=Org.gnome.Nautilus;other;\n\
                    Categories=System;FileManager;\n\
                    Actions=new-window;\n\
                    \n\
                    [Desktop Action new-window]\n\
                    Name=New Window\n\
                    Exec=nautilus --new-window\n";

        let data = CString::new(data).expect("no interior NUL");
        let id = CString::new("org.gnome.Nautilus.desktop").expect("no interior NUL");

        unsafe {
            let entry = lambdock_desktop_entry_new_from_data(data.as_ptr(), id.as_ptr());
            let record = entry.as_ref().expect("a valid entry parses");

            assert_eq!(
                crate::cstr::text(record.stem).as_deref(),
                Some("org.gnome.Nautilus")
            );
            assert_eq!(crate::cstr::text(record.name).as_deref(), Some("Files"));
            // The field code goes, and the space it leaves with it.
            assert_eq!(
                crate::cstr::text(record.exec).as_deref(),
                Some("nautilus --new-window")
            );
            // Only the first class is what a compositor reports.
            assert_eq!(
                crate::cstr::text(record.wm_class).as_deref(),
                Some("Org.gnome.Nautilus")
            );
            assert_eq!(
                crate::cstr::text(record.categories).as_deref(),
                Some("System;FileManager;")
            );
            assert_eq!(record.no_display, 0);
            assert_eq!(record.hidden, 0);
            assert_eq!(ptr_array_len(record.actions), 1);

            let action = ptr_array_at(record.actions, 0).cast::<DesktopAction>();
            let action = action.as_ref().expect("one action");
            assert_eq!(crate::cstr::text(action.id).as_deref(), Some("new-window"));
            assert_eq!(crate::cstr::text(action.name).as_deref(), Some("New Window"));

            assert_ne!(
                lambdock_desktop_entry_has_category(entry, c"filemanager".as_ptr()),
                0
            );
            assert_eq!(
                lambdock_desktop_entry_has_category(entry, c"Game".as_ptr()),
                0
            );

            lambdock_desktop_entry_free(entry);
        }
    }

    /// An entry without an icon still has one to draw, and one that is not an
    /// application is not a launcher at all.
    #[test]
    fn an_entry_defaults_its_icon_and_rejects_non_applications() {
        let data = CString::new("[Desktop Entry]\nName=Thing\nExec=thing\n").expect("no NUL");
        let id = CString::new("thing.desktop").expect("no NUL");

        unsafe {
            let entry = lambdock_desktop_entry_new_from_data(data.as_ptr(), id.as_ptr());
            let entry_ref = entry.as_ref().expect("a valid entry parses");
            assert_eq!(
                crate::cstr::text(entry_ref.icon).as_deref(),
                Some("application-x-executable")
            );
            assert!(entry_ref.actions.is_null(), "no Actions= means no array");
            lambdock_desktop_entry_free(entry);
        }

        let link =
            CString::new("[Desktop Entry]\nType=Link\nName=Site\nURL=https://x\n").expect("no NUL");
        let id = CString::new("site.desktop").expect("no NUL");

        unsafe {
            assert!(lambdock_desktop_entry_new_from_data(link.as_ptr(), id.as_ptr()).is_null());

            // No Name=, no Exec=, no file at all: none of them is a launcher.
            let unnamed = CString::new("[Desktop Entry]\nExec=thing\n").expect("no NUL");
            assert!(lambdock_desktop_entry_new_from_data(unnamed.as_ptr(), id.as_ptr()).is_null());

            let empty = CString::new("").expect("no NUL");
            assert!(lambdock_desktop_entry_new_from_data(empty.as_ptr(), id.as_ptr()).is_null());
            assert!(lambdock_desktop_entry_new_from_data(std::ptr::null(), id.as_ptr()).is_null());
        }
    }

    /// A file on disk knows where it came from, which is what tells the copies of
    /// an application apart.
    #[test]
    fn a_file_entry_knows_its_path() {
        let path = std::env::temp_dir().join("lambdock-entry-test.desktop");
        std::fs::write(&path, "[Desktop Entry]\nName=Frost\nExec=frost\n").expect("temp file");
        let path_text = CString::new(path.to_string_lossy().as_bytes()).expect("no NUL");

        unsafe {
            let entry = lambdock_desktop_entry_new_from_file(path_text.as_ptr());
            let record = entry.as_ref().expect("a valid file gives an entry");
            assert_eq!(
                crate::cstr::text(record.path),
                Some(path.to_string_lossy().into_owned())
            );
            assert_eq!(
                crate::cstr::text(record.id).as_deref(),
                Some("lambdock-entry-test.desktop")
            );

            let label = lambdock_desktop_entry_source_label(entry);
            assert!(!label.is_null(), "every entry has a source label");
            g_free(label.cast());

            lambdock_desktop_entry_free(entry);
        }

        let _ = std::fs::remove_file(&path);

        let missing = CString::new("/nonexistent/thing.desktop").expect("no NUL");
        unsafe {
            assert!(lambdock_desktop_entry_new_from_file(missing.as_ptr()).is_null());
            assert!(lambdock_desktop_entry_new_from_file(std::ptr::null()).is_null());
        }
    }

    /// The cache answers a window's id through its aliases, and falls back to the
    /// derived name when no application is installed under it.
    #[test]
    fn the_cache_answers_only_what_it_knows() {
        unsafe {
            lambdock_desktop_cache_invalidate();

            // The machine always has some .desktop files, so this builds a cache.
            assert!(!lambdock_desktop_entries_cached().is_null());

            // An id nobody installed resolves to nothing rather than crashing.
            let unknown = CString::new("org.example.NotInstalled").expect("no NUL");
            assert!(lambdock_desktop_entry_lookup(unknown.as_ptr()).is_null());
            assert!(lambdock_desktop_entry_lookup(std::ptr::null()).is_null());
            assert!(lambdock_desktop_entry_lookup(c"".as_ptr()).is_null());

            let name = lambdock_app_name_for(unknown.as_ptr());
            assert_eq!(
                CStr::from_ptr(name).to_string_lossy(),
                "Not Installed",
                "the derived name answers when the cache cannot"
            );
            g_free(name.cast());

            let icon = lambdock_app_icon_for(unknown.as_ptr());
            assert_eq!(
                CStr::from_ptr(icon).to_string_lossy(),
                "org.example.NotInstalled"
            );
            g_free(icon.cast());

            assert!(lambdock_app_name_for(std::ptr::null()).is_null());
            assert!(lambdock_app_icon_for(c"".as_ptr()).is_null());
        }
    }
}
