// lambdock -- hyper-hackable desktop dock for Wayland
//
// SPDX-License-Identifier: MIT
// Copyright © Josep Bigorra <jjbigorra@gmail.com>

#include "trash.h"
#include "../logger.h"

#include <errno.h>
#include <gio/gio.h>
#include <glib/gstdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

/* ============================================================================
 * FREEDESKTOP.ORG TRASH SPECIFICATION
 * ============================================================================
 * The trash is not a GNOME or KDE feature: it is specified by freedesktop.org
 * and every Linux desktop implements the same layout.
 *
 *   $XDG_DATA_HOME/Trash/files   the trashed files themselves
 *   $XDG_DATA_HOME/Trash/info    one <name>.trashinfo per trashed file
 *
 * Files trashed from another volume do not cross it: that volume gets its own
 * trash, either $topdir/.Trash/$uid (when a shared, sticky .Trash exists) or
 * $topdir/.Trash-$uid. Each has the same files/ + info/ pair.
 *
 * Working from that layout, instead of from `gio` or a desktop-specific tool,
 * is what makes the dock's trash icon work the same on GNOME, KDE, Xfce,
 * Cinnamon, MATE and anything else that follows the specification.
 * ============================================================================
 */

#define TRASH_INFO_SUFFIX ".trashinfo"
#define MOUNT_TABLE "/proc/mounts"

/* Filesystems that never carry user files, so a trash directory on them would
 * not be found or would not be the user's. Skipping them keeps the periodic
 * count down to the handful of real volumes and off network mounts that may
 * not answer. */
static const char *const pseudo_filesystems[] = {
    "autofs",   "bpf",      "cgroup",   "cgroup2",  "configfs",
    "debugfs",  "devpts",   "devtmpfs", "efivarfs", "fusectl",
    "hugetlbfs", "mqueue",  "nsfs",     "proc",     "pstore",
    "securityfs", "sysfs",  "tracefs",  NULL};

/* ============================================================================
 * HOME TRASH
 * ============================================================================
 */

char *lambdock_trash_home_path(void) {
  return g_build_filename(g_get_user_data_dir(), "Trash", NULL);
}

/* ============================================================================
 * VOLUME TRASHES
 * ============================================================================
 */

/* One field of /proc/mounts escapes whitespace and backslashes as \ooo (for
 * example a mount point with a space is written as "/media/My\040Disk"). */
static char *mount_field_unescape(const char *field) {
  GString *out = g_string_sized_new(strlen(field));

  for (const char *p = field; *p != '\0'; p++) {
    if (p[0] == '\\' && p[1] >= '0' && p[1] <= '7' && p[2] >= '0' &&
        p[2] <= '7' && p[3] >= '0' && p[3] <= '7') {
      g_string_append_c(out, (char)((p[1] - '0') * 64 + (p[2] - '0') * 8 +
                                    (p[3] - '0')));
      p += 3;
    } else {
      g_string_append_c(out, *p);
    }
  }

  return g_string_free(out, FALSE);
}

static gboolean is_pseudo_filesystem(const char *type) {
  return type && g_strv_contains(pseudo_filesystems, type);
}

/* Adds PATH to the roots when it is a trash directory that has not been added
 * yet. A missing trash simply means that volume has never had a file trashed
 * on it, which is not an error. */
static void add_trash_root(GPtrArray *roots, GHashTable *seen,
                           const char *path) {
  if (!path || g_hash_table_contains(seen, path))
    return;

  if (!g_file_test(path, G_FILE_TEST_IS_DIR))
    return;

  g_hash_table_add(seen, g_strdup(path));
  g_ptr_array_add(roots, g_strdup(path));
}

/* Walks /proc/mounts, adding the trash directory of every real filesystem.
 * The specification prefers $topdir/.Trash/$uid when the volume has a shared
 * .Trash directory, and falls back to $topdir/.Trash-$uid; both are checked so
 * a volume whose .Trash appeared after another desktop created it is still
 * seen. */
static void add_volume_trashes(GPtrArray *roots, GHashTable *seen) {
  char *contents = NULL;
  if (!g_file_get_contents(MOUNT_TABLE, &contents, NULL, NULL)) {
    LOG_C_DEBUG("Trash", "No " MOUNT_TABLE ": only the home trash is searched");
    return;
  }

  char *uid = g_strdup_printf("%u", (guint)getuid());
  char **lines = g_strsplit(contents, "\n", -1);

  for (int i = 0; lines[i] != NULL; i++) {
    if (*lines[i] == '\0')
      continue;

    /* "device mountpoint fstype options dump pass" */
    char **fields = g_strsplit(lines[i], " ", 4);
    if (fields[0] && fields[1] && fields[2] &&
        !is_pseudo_filesystem(fields[2])) {
      char *mount_point = mount_field_unescape(fields[1]);

      char *shared_name = g_build_filename(".Trash", uid, NULL);
      char *shared = g_build_filename(mount_point, shared_name, NULL);
      add_trash_root(roots, seen, shared);

      char *per_user_name = g_strdup_printf(".Trash-%s", uid);
      char *per_user = g_build_filename(mount_point, per_user_name, NULL);
      add_trash_root(roots, seen, per_user);

      g_free(per_user);
      g_free(per_user_name);
      g_free(shared);
      g_free(shared_name);
      g_free(mount_point);
    }
    g_strfreev(fields);
  }

  g_strfreev(lines);
  g_free(uid);
  g_free(contents);
}

GPtrArray *lambdock_trash_roots(void) {
  GPtrArray *roots = g_ptr_array_new_with_free_func(g_free);
  GHashTable *seen = g_hash_table_new_full(g_str_hash, g_str_equal, g_free,
                                           NULL);

  /* The home trash is always first, and always listed, even before it holds
   * anything: it is the one every file manager uses. */
  char *home = lambdock_trash_home_path();
  g_hash_table_add(seen, g_strdup(home));
  g_ptr_array_add(roots, home);

  add_volume_trashes(roots, seen);

  g_hash_table_destroy(seen);
  return roots;
}

/* ============================================================================
 * COUNTING
 * ============================================================================
 */

guint lambdock_trash_count_at(const char *root) {
  if (!root)
    return 0;

  char *info_dir = g_build_filename(root, "info", NULL);
  GDir *dir = g_dir_open(info_dir, 0, NULL);

  if (!dir) {
    g_free(info_dir);
    return 0;
  }

  guint count = 0;
  const char *name = NULL;
  while ((name = g_dir_read_name(dir)) != NULL) {
    if (g_str_has_suffix(name, TRASH_INFO_SUFFIX))
      count++;
  }

  g_dir_close(dir);
  g_free(info_dir);
  return count;
}

guint lambdock_trash_count(void) {
  GPtrArray *roots = lambdock_trash_roots();
  guint count = 0;

  for (guint i = 0; i < roots->len; i++)
    count += lambdock_trash_count_at(g_ptr_array_index(roots, i));

  g_ptr_array_free(roots, TRUE);
  return count;
}

gboolean lambdock_trash_is_empty(void) { return lambdock_trash_count() == 0; }

/* ============================================================================
 * EMPTYING
 * ============================================================================
 */

/* Removes PATH and, when it is a directory, everything below it. Symlinks are
 * unlinked, never followed: a trashed symlink may point anywhere.
 *
 * A trashed directory keeps the permissions it had when it was trashed, and a
 * read-only one could not be walked into or emptied. Since its contents are
 * about to be deleted anyway, the owner's rwx bits are added before the
 * directory is removed. */
static gboolean remove_tree(const char *path) {
  GStatBuf st;

  if (g_lstat(path, &st) != 0)
    return errno == ENOENT;

  /* lstat() does not follow symlinks, so a link to a directory is not a
   * directory here and is unlinked below. */
  if (!S_ISDIR(st.st_mode))
    return g_unlink(path) == 0 || errno == ENOENT;

  if ((st.st_mode & (S_IRUSR | S_IWUSR | S_IXUSR)) !=
      (S_IRUSR | S_IWUSR | S_IXUSR))
    g_chmod(path, (st.st_mode & 07777) | S_IRWXU);

  gboolean ok = TRUE;
  GDir *dir = g_dir_open(path, 0, NULL);

  if (dir) {
    const char *name = NULL;
    while ((name = g_dir_read_name(dir)) != NULL) {
      char *child = g_build_filename(path, name, NULL);
      if (!remove_tree(child)) {
        LOG_C_WARN("Trash", "Could not remove trashed item: %s", child);
        ok = FALSE;
      }
      g_free(child);
    }
    g_dir_close(dir);
  } else {
    LOG_C_WARN("Trash", "Could not read trashed directory: %s", path);
    ok = FALSE;
  }

  if (g_rmdir(path) != 0 && errno != ENOENT) {
    /* A directory made read-only after its children were removed cannot be
     * deleted either; give the owner write access and try once more. */
    g_chmod(path, 0700);
    if (g_rmdir(path) != 0 && errno != ENOENT) {
      LOG_C_WARN("Trash", "Could not remove trashed directory: %s", path);
      ok = FALSE;
    }
  }

  return ok;
}

/* Empties one of the two directories of a trash, removing every child. */
static void empty_directory(const char *path) {
  GDir *dir = g_dir_open(path, 0, NULL);
  if (!dir)
    return;

  const char *name = NULL;
  while ((name = g_dir_read_name(dir)) != NULL) {
    char *child = g_build_filename(path, name, NULL);
    if (!remove_tree(child))
      LOG_C_WARN("Trash", "Could not remove %s", child);
    g_free(child);
  }

  g_dir_close(dir);
}

guint lambdock_trash_empty_at(const char *root) {
  if (!root || *root == '\0')
    return 0;

  /* The info directory drives the count, so it is counted before the files are
   * gone: it tells the caller how many items were actually in that trash. */
  guint removed = lambdock_trash_count_at(root);

  char *files_dir = g_build_filename(root, "files", NULL);
  char *info_dir = g_build_filename(root, "info", NULL);

  /* files first: while the info entries still exist the desktop's own view of
   * the trash stays consistent, and the markers go last. */
  empty_directory(files_dir);
  empty_directory(info_dir);

  g_free(files_dir);
  g_free(info_dir);
  return removed;
}

guint lambdock_trash_empty(void) {
  GPtrArray *roots = lambdock_trash_roots();
  guint removed = 0;

  for (guint i = 0; i < roots->len; i++) {
    const char *root = g_ptr_array_index(roots, i);
    guint there = lambdock_trash_empty_at(root);
    if (there > 0)
      LOG_C_INFO("Trash", "Removed %u item(s) from %s", there, root);
    removed += there;
  }

  g_ptr_array_free(roots, TRUE);
  return removed;
}

/* ============================================================================
 * ADDING
 * ============================================================================
 */

gboolean lambdock_trash_file(GFile *file, GError **error) {
  if (!file) {
    g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
                        "No file to trash");
    return FALSE;
  }

  /* GIO implements the same specification this module reads: it resolves the
   * trash of the file's own filesystem (home or volume), writes the .trashinfo
   * entry, moves the file -- copying it first when the two are on different
   * filesystems -- and uniquifies a colliding name. Hand-rolling that from the
   * layout above would mean reimplementing, and probably getting wrong, exactly
   * the parts the specification spends its time on. */
  return g_file_trash(file, NULL, error);
}

/* ============================================================================
 * OPENING
 * ============================================================================
 */

/* Creates the home trash directories if they do not exist yet, with the 0700
 * permissions the specification asks for. GLib does the same when the first
 * file is trashed; doing it here means a file manager opening the trash finds
 * a directory instead of reporting that there is no trash. */
static void ensure_home_trash(void) {
  char *home = lambdock_trash_home_path();
  char *files_dir = g_build_filename(home, "files", NULL);
  char *info_dir = g_build_filename(home, "info", NULL);

  g_mkdir_with_parents(home, 0700);
  g_mkdir_with_parents(files_dir, 0700);
  g_mkdir_with_parents(info_dir, 0700);

  g_free(info_dir);
  g_free(files_dir);
  g_free(home);
}

/* Launches a handler with one URI argument. File managers accept the trash URI
 * on their command line (nautilus trash:///, dolphin trash:/, thunar trash:///),
 * which is what makes the trash visible as the file manager's own trash view
 * rather than as the hidden files directory behind it. */
static gboolean launch_with_uri(GAppInfo *handler, const char *uri,
                                GError **error) {
  GList *uris = g_list_prepend(NULL, (gpointer)uri);
  gboolean opened = g_app_info_launch_uris(handler, uris, NULL, error);
  g_list_free(uris);
  return opened;
}

/* Asks the application that handles folders -- the user's file manager -- to
 * open trash:///. Distributions rarely register an x-scheme-handler/trash, so
 * the URI's own default handler is often missing even though the file manager
 * understands the URI perfectly well. */
static gboolean open_with_directory_handler(GError **error) {
  GAppInfo *handler = g_app_info_get_default_for_type("inode/directory", FALSE);
  if (!handler)
    return FALSE;

  gboolean opened = FALSE;
  if (g_app_info_supports_uris(handler))
    opened = launch_with_uri(handler, "trash:///", error);

  g_object_unref(handler);
  return opened;
}

gboolean lambdock_trash_open(void) {
  ensure_home_trash();

  GError *error = NULL;

  /* trash:/// is what the file managers themselves show, and it covers volume
   * trashes too. It needs a handler for the trash scheme, which not every
   * desktop registers. */
  if (g_app_info_launch_default_for_uri("trash:///", NULL, &error))
    return TRUE;

  if (error) {
    LOG_C_DEBUG("Trash", "No trash:// handler (%s); asking the file manager",
                error->message);
    g_clear_error(&error);
  }

  /* The file manager is the application that handles folders; give it the trash
   * URI directly. This is the path that works on desktops without GVfs, where
   * trash:/// has no MIME handler but the file manager still knows the scheme. */
  if (open_with_directory_handler(&error))
    return TRUE;

  if (error) {
    LOG_C_DEBUG("Trash", "The folder handler could not open trash:/// (%s); "
                          "opening the home trash",
                error->message);
    g_clear_error(&error);
  }

  /* Last resort: the trash is just a directory, and any file manager can open
   * it as a path. */
  char *home = lambdock_trash_home_path();
  char *uri = g_filename_to_uri(home, NULL, &error);
  gboolean opened = FALSE;

  if (uri)
    opened = g_app_info_launch_default_for_uri(uri, NULL, &error);

  if (!opened && error) {
    LOG_C_WARN("Trash", "Could not open '%s': %s", home, error->message);
    g_clear_error(&error);
  }

  g_free(uri);
  g_free(home);
  return opened;
}
