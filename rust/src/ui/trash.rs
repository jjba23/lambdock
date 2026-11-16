// lambdock -- hyper-hackable desktop dock for Wayland
//
// SPDX-License-Identifier: MIT
// Copyright © Josep Bigorra <jjbigorra@gmail.com>

//! The freedesktop.org trash: `src/ui/trash.c` in Rust.
//!
//! The trash is not a GNOME or KDE feature: it is specified by freedesktop.org
//! and every Linux desktop implements the same layout.
//!
//! ```text
//! $XDG_DATA_HOME/Trash/files   the trashed files themselves
//! $XDG_DATA_HOME/Trash/info    one <name>.trashinfo per trashed file
//! ```
//!
//! Files trashed from another volume do not cross it: that volume gets its own
//! trash, either `$topdir/.Trash/$uid` (when a shared, sticky `.Trash` exists) or
//! `$topdir/.Trash-$uid`. Working from that layout, instead of from `gio` or a
//! desktop-specific tool, is what makes the dock's trash icon work the same on
//! GNOME, KDE, Xfce, Cinnamon, MATE and anything else that follows the spec.
//!
//! The module is the only one allowed to touch the filesystem for trash purposes;
//! the dock just asks it questions.

use crate::glib::{g_free, strdup_string};
use crate::log_debug;
use crate::log_info;
use crate::log_warn;
use gtk4::gio;
use gtk4::glib;
use gtk4::glib::translate::IntoGlib;
use gtk4::prelude::*;
use std::ffi::CStr;
use std::os::raw::{c_char, c_int, c_uint};
use std::os::unix::fs::PermissionsExt;
use std::path::{Path, PathBuf};

const INFO_SUFFIX: &str = ".trashinfo";
const MOUNT_TABLE: &str = "/proc/mounts";

/// Filesystems that never carry user files, so a trash directory on them would
/// not be found or would not be the user's. Skipping them keeps the periodic
/// count down to the handful of real volumes and off network mounts that may not
/// answer.
const PSEUDO_FILESYSTEMS: &[&str] = &[
    "autofs",
    "bpf",
    "cgroup",
    "cgroup2",
    "configfs",
    "debugfs",
    "devpts",
    "devtmpfs",
    "efivarfs",
    "fusectl",
    "hugetlbfs",
    "mqueue",
    "nsfs",
    "proc",
    "pstore",
    "securityfs",
    "sysfs",
    "tracefs",
];

/// The home trash: `$XDG_DATA_HOME/Trash`, usually `~/.local/share/Trash`.
pub fn home_trash_path() -> PathBuf {
    glib::user_data_dir().join("Trash")
}

/// One field of `/proc/mounts` escapes whitespace and backslashes as `\ooo` (a
/// mount point with a space is written as `/media/My\040Disk`).
fn mount_field_unescape(field: &str) -> String {
    let bytes = field.as_bytes();
    let mut out = Vec::with_capacity(bytes.len());
    let mut index = 0;

    while index < bytes.len() {
        let escaped = bytes[index] == b'\\'
            && bytes
                .get(index + 1..index + 4)
                .is_some_and(|digits| digits.iter().all(|digit| (b'0'..=b'7').contains(digit)));

        if escaped {
            let digits = &bytes[index + 1..index + 4];
            out.push((digits[0] - b'0') * 64 + (digits[1] - b'0') * 8 + (digits[2] - b'0'));
            index += 4;
        } else {
            out.push(bytes[index]);
            index += 1;
        }
    }

    String::from_utf8_lossy(&out).into_owned()
}

fn is_pseudo_filesystem(filesystem_type: &str) -> bool {
    PSEUDO_FILESYSTEMS.contains(&filesystem_type)
}

/// Adds `path` to `roots` when it is a trash directory that has not been added
/// yet. A missing trash simply means that volume has never had a file trashed on
/// it, which is not an error.
fn add_trash_root(roots: &mut Vec<PathBuf>, path: PathBuf, exists: &dyn Fn(&Path) -> bool) {
    if roots.contains(&path) || !exists(&path) {
        return;
    }

    roots.push(path);
}

/// The trash directory of every real filesystem in `mounts` (the text of
/// `/proc/mounts`), for `uid`.
///
/// `exists` checks whether a directory is there. The specification prefers
/// `$topdir/.Trash/$uid` when the volume has a shared `.Trash`, and falls back to
/// `$topdir/.Trash-$uid`; both are checked so a volume whose `.Trash` appeared
/// after another desktop created it is still seen.
pub fn volume_trash_roots(mounts: &str, uid: u32, exists: &dyn Fn(&Path) -> bool) -> Vec<PathBuf> {
    let mut roots: Vec<PathBuf> = Vec::new();

    for line in mounts.lines().filter(|line| !line.is_empty()) {
        // "device mountpoint fstype options dump pass"
        let fields: Vec<&str> = line.splitn(4, ' ').collect();
        let (Some(_device), Some(mount_point), Some(filesystem_type)) =
            (fields.first(), fields.get(1), fields.get(2))
        else {
            continue;
        };

        if is_pseudo_filesystem(filesystem_type) {
            continue;
        }

        let mount_point = PathBuf::from(mount_field_unescape(mount_point));

        add_trash_root(
            &mut roots,
            mount_point.join(".Trash").join(uid.to_string()),
            exists,
        );
        add_trash_root(
            &mut roots,
            mount_point.join(format!(".Trash-{uid}")),
            exists,
        );
    }

    roots
}

/// How many `*.trashinfo` entries one trash directory holds.
pub fn count_at(root: &Path) -> u32 {
    let Ok(entries) = std::fs::read_dir(root.join("info")) else {
        return 0;
    };

    entries
        .flatten()
        .filter(|entry| entry.file_name().to_string_lossy().ends_with(INFO_SUFFIX))
        .count() as u32
}

/// Every trash directory belonging to the user: the home trash first, then the
/// per-volume trashes of the filesystems mounted right now.
pub fn trash_roots() -> Vec<PathBuf> {
    let mut roots = vec![home_trash_path()];

    let mounts = std::fs::read_to_string(MOUNT_TABLE).unwrap_or_else(|_| {
        // Pseudo filesystems have no trash of their own, and neither does a machine
        // whose mount table cannot be read: the home trash is still the user's.
        log_debug!(
            "Trash",
            "No {}: only the home trash is searched",
            MOUNT_TABLE
        );
        String::new()
    });

    for root in volume_trash_roots(&mounts, unsafe { getuid() }, &Path::is_dir) {
        if !roots.contains(&root) {
            roots.push(root);
        }
    }

    roots
}

extern "C" {
    fn getuid() -> u32;
}

/// Removes `path` and, when it is a directory, everything below it. Symlinks are
/// unlinked, never followed: a trashed symlink may point anywhere.
fn remove_tree(path: &Path) -> bool {
    let Ok(metadata) = std::fs::symlink_metadata(path) else {
        return true; // Gone already is the outcome this was asked for.
    };

    // symlink_metadata() does not follow symlinks, so a link to a directory is not
    // a directory here and is unlinked below.
    if !metadata.is_dir() {
        return match std::fs::remove_file(path) {
            Ok(()) => true,
            Err(error) => error.kind() == std::io::ErrorKind::NotFound,
        };
    }

    // A trashed directory keeps the permissions it had when it was trashed, and a
    // read-only one could not be walked into or emptied. Its contents are about to
    // be deleted anyway, so the owner's rwx bits are added first.
    let mut permissions = metadata.permissions();
    if permissions.mode() & 0o700 != 0o700 {
        permissions.set_mode((permissions.mode() & 0o7777) | 0o700);
        let _ = std::fs::set_permissions(path, permissions);
    }

    let mut ok = true;

    match std::fs::read_dir(path) {
        Ok(entries) => {
            for entry in entries.flatten() {
                let child = entry.path();
                if !remove_tree(&child) {
                    log_warn!(
                        "Trash",
                        "Could not remove trashed item: {}",
                        child.display()
                    );
                    ok = false;
                }
            }
        }
        Err(_) => {
            log_warn!(
                "Trash",
                "Could not read trashed directory: {}",
                path.display()
            );
            ok = false;
        }
    }

    if std::fs::remove_dir(path).is_err() {
        // A directory made read-only after its children were removed cannot be
        // deleted either; give the owner write access and try once more.
        if let Ok(metadata) = std::fs::metadata(path) {
            let mut permissions = metadata.permissions();
            permissions.set_mode(0o700);
            let _ = std::fs::set_permissions(path, permissions);
        }

        if std::fs::remove_dir(path).is_err() && path.exists() {
            log_warn!(
                "Trash",
                "Could not remove trashed directory: {}",
                path.display()
            );
            ok = false;
        }
    }

    ok
}

/// Empties one of the two directories of a trash, removing every child.
fn empty_directory(path: &Path) {
    let Ok(entries) = std::fs::read_dir(path) else {
        return;
    };

    for entry in entries.flatten() {
        let child = entry.path();
        if !remove_tree(&child) {
            log_warn!("Trash", "Could not remove {}", child.display());
        }
    }
}

/// Empties one trash directory, and says how many items it held.
pub fn empty_at(root: &Path) -> u32 {
    if root.as_os_str().is_empty() {
        return 0;
    }

    // The info directory drives the count, so it is counted before the files are
    // gone: it tells the caller how many items were actually in that trash.
    let removed = count_at(root);

    // files first: while the info entries still exist the desktop's own view of the
    // trash stays consistent, and the markers go last.
    empty_directory(&root.join("files"));
    empty_directory(&root.join("info"));

    removed
}

/// Empties every trash of the user, the way a file manager's "Empty Trash" does.
pub fn empty_all() -> u32 {
    let mut removed = 0;

    for root in trash_roots() {
        let there = empty_at(&root);
        if there > 0 {
            log_info!("Trash", "Removed {} item(s) from {}", there, root.display());
        }
        removed += there;
    }

    removed
}

/// Creates the home trash directories if they do not exist yet, with the 0700
/// permissions the specification asks for.
///
/// GLib does the same when the first file is trashed; doing it here means a file
/// manager opening the trash finds a directory instead of reporting that there is
/// no trash.
fn ensure_home_trash() {
    let home = home_trash_path();

    for path in [home.clone(), home.join("files"), home.join("info")] {
        let _ = std::fs::create_dir_all(&path);
        if let Ok(metadata) = std::fs::metadata(&path) {
            let mut permissions = metadata.permissions();
            permissions.set_mode(0o700);
            let _ = std::fs::set_permissions(&path, permissions);
        }
    }
}

/// Asks the application that handles folders -- the user's file manager -- to open
/// `trash:///`.
///
/// Distributions rarely register an `x-scheme-handler/trash`, so the URI's own
/// default handler is often missing even though the file manager understands the
/// URI perfectly well.
fn open_with_directory_handler() -> Result<(), glib::Error> {
    let Some(handler) = gio::AppInfo::default_for_type("inode/directory", false) else {
        return Err(glib::Error::new(
            gio::IOErrorEnum::NotFound,
            "no handler for inode/directory",
        ));
    };

    if !handler.supports_uris() {
        return Err(glib::Error::new(
            gio::IOErrorEnum::NotSupported,
            "the folder handler does not take URIs",
        ));
    }

    handler.launch_uris(&["trash:///"], None::<&gio::AppLaunchContext>)
}

/// Opens the trash in the desktop's file manager.
fn open_trash() -> bool {
    ensure_home_trash();

    // trash:/// is what the file managers themselves show, and it covers volume
    // trashes too. It needs a handler for the trash scheme, which not every
    // desktop registers.
    match gio::AppInfo::launch_default_for_uri("trash:///", None::<&gio::AppLaunchContext>) {
        Ok(()) => return true,
        Err(error) => log_debug!(
            "Trash",
            "No trash:// handler ({}); asking the file manager",
            error.message()
        ),
    }

    // The file manager is the application that handles folders; give it the trash
    // URI directly. This is the path that works on desktops without GVfs, where
    // trash:/// has no MIME handler but the file manager still knows the scheme.
    match open_with_directory_handler() {
        Ok(()) => return true,
        Err(error) => log_debug!(
            "Trash",
            "The folder handler could not open trash:/// ({}); opening the home trash",
            error.message()
        ),
    }

    // Last resort: the trash is just a directory, and any file manager can open it
    // as a path.
    let home = home_trash_path();
    let uri = match glib::filename_to_uri(&home, None::<&str>) {
        Ok(uri) => uri,
        Err(error) => {
            log_warn!(
                "Trash",
                "Could not open '{}': {}",
                home.display(),
                error.message()
            );
            return false;
        }
    };

    match gio::AppInfo::launch_default_for_uri(&uri, None::<&gio::AppLaunchContext>) {
        Ok(()) => true,
        Err(error) => {
            log_warn!(
                "Trash",
                "Could not open '{}': {}",
                home.display(),
                error.message()
            );
            false
        }
    }
}

unsafe fn path_option(pointer: *const c_char) -> Option<PathBuf> {
    if pointer.is_null() {
        return None;
    }

    Some(PathBuf::from(
        CStr::from_ptr(pointer).to_string_lossy().into_owned(),
    ))
}

/// The home trash directory, `$XDG_DATA_HOME/Trash`.
///
/// # Safety
///
/// The result is `(transfer full)` and freed with `g_free`.
#[no_mangle]
pub unsafe extern "C" fn lambdock_trash_home_path() -> *mut c_char {
    strdup_string(&home_trash_path().to_string_lossy())
}

/// Every trash directory belonging to the user, home first.
///
/// # Safety
///
/// The result is `(transfer full)` and freed with `g_ptr_array_unref`.
#[no_mangle]
pub unsafe extern "C" fn lambdock_trash_roots() -> *mut glib::ffi::GPtrArray {
    let roots = glib::ffi::g_ptr_array_new_with_free_func(Some(g_free));

    for root in trash_roots() {
        glib::ffi::g_ptr_array_add(roots, strdup_string(&root.to_string_lossy()).cast());
    }

    roots
}

/// How many items one trash directory holds.
///
/// # Safety
///
/// `root` must be null or a NUL-terminated path.
#[no_mangle]
pub unsafe extern "C" fn lambdock_trash_count_at(root: *const c_char) -> c_uint {
    path_option(root).map_or(0, |root| count_at(&root))
}

/// How many items the user's trash holds, across the home trash and the volume
/// trashes.
#[no_mangle]
pub extern "C" fn lambdock_trash_count() -> c_uint {
    trash_roots().iter().map(|root| count_at(root)).sum()
}

/// Whether the user's trash holds nothing.
#[no_mangle]
pub extern "C" fn lambdock_trash_is_empty() -> c_int {
    c_int::from(lambdock_trash_count() == 0)
}

/// Empties one trash directory.
///
/// # Safety
///
/// `root` must be null or a NUL-terminated path.
#[no_mangle]
pub unsafe extern "C" fn lambdock_trash_empty_at(root: *const c_char) -> c_uint {
    path_option(root).map_or(0, |root| empty_at(&root))
}

/// Empties every trash of the user and says how many items were removed.
#[no_mangle]
pub extern "C" fn lambdock_trash_empty() -> c_uint {
    empty_all()
}

/// Moves a file or folder to the user's trash.
///
/// GIO implements the same specification this module reads: it resolves the trash
/// of the file's own filesystem, writes the `.trashinfo` entry, moves the file --
/// copying it first when the two are on different filesystems -- and uniquifies a
/// colliding name. Hand-rolling that from the layout above would mean
/// reimplementing, and probably getting wrong, exactly the parts the specification
/// spends its time on.
///
/// # Safety
///
/// `file` must be null or a live `GFile`; `error` must be null or point at a null
/// `GError*`.
#[no_mangle]
pub unsafe extern "C" fn lambdock_trash_file(
    file: *mut gio::ffi::GFile,
    error: *mut *mut glib::ffi::GError,
) -> c_int {
    if file.is_null() {
        if !error.is_null() {
            *error = glib::ffi::g_error_new_literal(
                gio::ffi::g_io_error_quark(),
                gio::IOErrorEnum::InvalidArgument.into_glib(),
                c"No file to trash".as_ptr(),
            );
        }

        return 0;
    }

    c_int::from(gio::ffi::g_file_trash(file, std::ptr::null_mut(), error) != 0)
}

/// Opens the trash in the desktop's file manager.
#[no_mangle]
pub extern "C" fn lambdock_trash_open() -> c_int {
    c_int::from(open_trash())
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::collections::HashSet;

    /// A mount point with a space is written escaped, and the trash has to be
    /// looked for at the real path.
    #[test]
    fn a_mount_field_is_unescaped() {
        assert_eq!(mount_field_unescape(r"/media/My\040Disk"), "/media/My Disk");
        assert_eq!(mount_field_unescape(r"/a\011b"), "/a\tb");
        // Only octal escapes are collapsed: a doubled backslash stays two.
        assert_eq!(mount_field_unescape(r"/a\\b"), r"/a\\b");
        assert_eq!(mount_field_unescape("/plain"), "/plain");
        // A backslash that does not start an octal escape is kept.
        assert_eq!(mount_field_unescape(r"/a\9b"), r"/a\9b");
        assert_eq!(mount_field_unescape("/ends\\"), "/ends\\");
        assert_eq!(mount_field_unescape(""), "");
    }

    #[test]
    fn the_pseudo_filesystems_are_the_ones_with_no_user_files() {
        for filesystem in ["proc", "sysfs", "cgroup2", "devtmpfs"] {
            assert!(is_pseudo_filesystem(filesystem), "{filesystem}");
        }

        for filesystem in ["ext4", "btrfs", "xfs", "fuse.sshfs"] {
            assert!(!is_pseudo_filesystem(filesystem), "{filesystem}");
        }
    }

    /// Both layouts are looked for, the shared one first, a pseudo filesystem is
    /// skipped, and a mount point listed twice is a mount point, not two.
    #[test]
    fn the_volume_trashes_are_the_real_filesystems_ones() {
        let mounts = "/dev/sda1 / ext4 rw 0 0\n\
                      proc /proc proc rw 0 0\n\
                      /dev/sdb1 /media/My\\040Disk ext4 rw 0 0\n\
                      /dev/sda1 / ext4 rw 0 0\n\
                      server:/srv /net nfs4 rw 0 0\n";

        let existing: HashSet<PathBuf> = [
            "/.Trash-1000",
            "/media/My Disk/.Trash/1000",
            "/media/My Disk/.Trash-1000",
            "/net/.Trash-1000",
        ]
        .into_iter()
        .map(PathBuf::from)
        .collect();

        let roots = volume_trash_roots(mounts, 1000, &|path| existing.contains(path));

        assert_eq!(
            roots,
            [
                PathBuf::from("/.Trash-1000"),
                PathBuf::from("/media/My Disk/.Trash/1000"),
                PathBuf::from("/media/My Disk/.Trash-1000"),
                PathBuf::from("/net/.Trash-1000"),
            ]
        );
    }

    /// A volume that has never had a file trashed on it has no directory, and
    /// that is not an error.
    #[test]
    fn a_volume_without_a_trash_is_not_listed() {
        let mounts = "/dev/sda1 /home ext4 rw 0 0\n";
        let roots = volume_trash_roots(mounts, 1000, &|_| false);

        assert!(roots.is_empty());
    }

    /// The count is the number of metadata files, which is what the specification
    /// requires for every trashed item.
    #[test]
    fn the_count_is_the_number_of_trashinfo_files() {
        let root = std::env::temp_dir().join("lambdock-trash-count-test");
        let _ = std::fs::remove_dir_all(&root);
        std::fs::create_dir_all(root.join("info")).expect("info dir");

        std::fs::write(root.join("info/one.trashinfo"), "[Trash Info]\n").expect("write");
        std::fs::write(root.join("info/two.trashinfo"), "[Trash Info]\n").expect("write");
        // A file that is not metadata is not a trashed item.
        std::fs::write(root.join("info/notes.txt"), "not metadata").expect("write");

        assert_eq!(count_at(&root), 2);
        assert_eq!(count_at(&root.join("missing")), 0);

        let _ = std::fs::remove_dir_all(&root);
    }
}
