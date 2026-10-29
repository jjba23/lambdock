// lambdock -- hyper-hackable desktop dock for Wayland
//
// SPDX-License-Identifier: MIT
// Copyright © Josep Bigorra <jjbigorra@gmail.com>

#ifndef TRASH_H
#define TRASH_H

#include <gio/gio.h>
#include <glib.h>

/* =========================================================================
 * FREEDESKTOP TRASH
 * =========================================================================
 * Find the trash roots, count what is in them, empty them, open them, and move
 * dropped paths into them.
 *
 * The only module in the tree allowed to touch the filesystem for trash
 * purposes; the dock just asks it questions.
 */

/* How often a trash dock item re-counts the trash, in milliseconds. Deleting a
 * file from a file manager has to show up on the dock without the user doing
 * anything, but there is no notification every trash implementation emits. */
#define TRASH_REFRESH_INTERVAL_MS 2000

/**
 * lambdock_trash_home_path:
 *
 * The home trash directory, `$XDG_DATA_HOME/Trash` (usually
 * `~/.local/share/Trash`), as required by the freedesktop.org Trash
 * specification.
 *
 * Returns: (transfer full): the directory path.
 */
char *lambdock_trash_home_path(void);

/**
 * lambdock_trash_roots:
 *
 * Every trash directory belonging to the user: the home trash first, then the
 * per-volume trashes (`$topdir/.Trash/$uid` or `$topdir/.Trash-$uid`) of the
 * filesystems mounted when the list is built, as described by the freedesktop.org
 * Trash specification and found in `/proc/mounts`.
 *
 * Pseudo filesystems have no trash of their own and are skipped, and duplicate
 * mount points (bind mounts) are listed once.
 *
 * Returns: (transfer full) (element-type utf8): the trash directories. The home
 * trash is always present, whether or not it exists yet.
 */
GPtrArray *lambdock_trash_roots(void);

/**
 * lambdock_trash_count:
 *
 * How many items the user's trash holds, across the home trash and the volume
 * trashes. An item is counted from its `*.trashinfo` metadata file, which the
 * specification requires for every trashed file.
 *
 * Returns: the number of trashed items.
 */
guint lambdock_trash_count(void);

/**
 * lambdock_trash_count_at:
 * @root: A single trash directory.
 *
 * Counts the items of one trash directory only. Exposed because it is what the
 * tests can make deterministic: the machine's mounted volumes are not theirs to
 * control.
 *
 * Returns: the number of `*.trashinfo` entries in @root, or 0 for NULL or a
 * missing directory.
 */
guint lambdock_trash_count_at(const char *root);

/**
 * lambdock_trash_is_empty:
 *
 * Returns: TRUE when the user's trash holds nothing.
 */
gboolean lambdock_trash_is_empty(void);

/**
 * lambdock_trash_empty:
 *
 * Empties every trash of the user, the way a file manager's "Empty Trash" does:
 * the contents of each trash's `files` and `info` directories are removed. No
 * desktop-specific helper is involved, so this works on any Linux desktop that
 * follows the freedesktop.org Trash specification.
 *
 * Returns: how many trashed items were removed (their `*.trashinfo` entries).
 */
guint lambdock_trash_empty(void);

/**
 * lambdock_trash_empty_at:
 * @root: A single trash directory.
 *
 * Empties one trash directory. Failures (a trash owned by another user, a file
 * that cannot be unlinked) are logged and do not stop the rest from being
 * removed.
 *
 * Returns: how many trashed items of @root were removed.
 */
guint lambdock_trash_empty_at(const char *root);

/**
 * lambdock_trash_file:
 * @file: A file or folder to move to the trash.
 * @error: (optional): Where a failure is reported.
 *
 * Moves @file -- and, for a folder, everything below it -- to the user's trash
 * using GIO's g_file_trash(), which implements the same freedesktop.org Trash
 * specification this module reads: it picks the trash of the file's own
 * filesystem, writes the `*.trashinfo` entry, gives a colliding name a fresh one
 * instead of overwriting, and copies the file when its trash lives on another
 * filesystem.
 *
 * Blocking: a large folder, or a file that has to be copied rather than
 * renamed, takes as long as the data takes. The dock calls it from a worker
 * thread; do the same anywhere else that has a main loop to keep responsive.
 *
 * Returns: TRUE when @file ended up in the trash.
 */
gboolean lambdock_trash_file(GFile *file, GError **error);

/**
 * lambdock_trash_open:
 *
 * Opens the trash in the desktop's file manager. `trash:///` is tried first, so
 * GNOME Files, Dolphin, Thunar and the other GVfs-based managers show the same
 * view they show themselves. Most distributions do not register a handler for
 * that scheme, so the application that handles folders (the file manager) is
 * then asked to open the URI directly; when neither works, the home trash
 * directory is opened instead.
 *
 * Returns: TRUE when a handler could be launched.
 */
gboolean lambdock_trash_open(void);

#endif // TRASH_H
