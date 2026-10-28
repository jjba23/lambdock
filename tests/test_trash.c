// lambdock -- hyper-hackable desktop dock for Wayland
//
// SPDX-License-Identifier: MIT
// Copyright © Josep Bigorra <jjbigorra@gmail.com>

/* Tests for the trash module. They only ever empty throwaway trash directories
 * they created themselves: `lambdock_trash_empty()` walks the volumes mounted on
 * the machine, and a test has no business deleting the developer's trash. */

#include "../src/ui/trash.h"
#include <gio/gio.h>
#include <glib/gstdio.h>
#include <string.h>
#include <unistd.h>

/* XDG_DATA_HOME for the whole binary; the home trash and its count depend on it. */
static char *data_home;

static void remove_tree(const char *path) {
  GDir *dir = g_dir_open(path, 0, NULL);
  if (dir != NULL) {
    const char *name;
    while ((name = g_dir_read_name(dir)) != NULL) {
      char *child = g_build_filename(path, name, NULL);
      if (g_file_test(child, G_FILE_TEST_IS_DIR))
        remove_tree(child);
      else
        unlink(child);
      g_free(child);
    }
    g_dir_close(dir);
  }
  rmdir(path);
}

/* Creates a fresh, empty directory to act as a trash root. */
static char *make_trash_root(const char *label) {
  char *pattern = g_strdup_printf("lambdock-trash-%s-XXXXXX", label);
  char *root = g_dir_make_tmp(pattern, NULL);
  g_free(pattern);
  g_assert_nonnull(root);
  return root;
}

/* Writes the `info/<name>.trashinfo` metadata file the specification requires
 * for every trashed item. */
static void add_entry(const char *root, const char *name) {
  char *info_dir = g_build_filename(root, "info", NULL);
  g_assert_cmpint(g_mkdir_with_parents(info_dir, 0700), ==, 0);

  char *info_name = g_strdup_printf("%s.trashinfo", name);
  char *info_path = g_build_filename(info_dir, info_name, NULL);
  g_assert_true(
      g_file_set_contents(info_path,
                          "[Trash Info]\n"
                          "Path=/home/user/example\n"
                          "DeletionDate=2026-10-26T00:00:00\n",
                          -1, NULL));

  g_free(info_path);
  g_free(info_name);
  g_free(info_dir);
}

/* Writes a file inside the trash's files/ directory, creating parents. */
static void add_file(const char *root, const char *relative) {
  char *files_dir = g_build_filename(root, "files", NULL);
  char *path = g_build_filename(files_dir, relative, NULL);
  char *parent = g_path_get_dirname(path);

  g_assert_cmpint(g_mkdir_with_parents(parent, 0700), ==, 0);
  g_assert_true(g_file_set_contents(path, "trashed\n", -1, NULL));

  g_free(parent);
  g_free(path);
  g_free(files_dir);
}

/* How many entries DIR holds (a missing directory holds none). */
static guint count_entries(const char *path) {
  GDir *dir = g_dir_open(path, 0, NULL);
  if (!dir)
    return 0;

  guint count = 0;
  while (g_dir_read_name(dir) != NULL)
    count++;

  g_dir_close(dir);
  return count;
}

/* TRUE when DIR contains no entries at all (missing counts as empty). */
static gboolean directory_is_empty(const char *path) {
  GDir *dir = g_dir_open(path, 0, NULL);
  if (!dir)
    return TRUE;

  const char *name = g_dir_read_name(dir);
  g_dir_close(dir);
  return name == NULL;
}

/* ============================================================================
 * HOME PATH
 * ============================================================================ */

static void test_home_path(void) {
  char *path = lambdock_trash_home_path();
  char *expected = g_build_filename(data_home, "Trash", NULL);

  g_assert_cmpstr(path, ==, expected);

  g_free(expected);
  g_free(path);
}

static void test_roots_start_with_home_trash(void) {
  GPtrArray *roots = lambdock_trash_roots();
  g_assert_nonnull(roots);
  g_assert_cmpuint(roots->len, >=, 1);

  char *home = lambdock_trash_home_path();
  g_assert_cmpstr(g_ptr_array_index(roots, 0), ==, home);

  /* The list is deduplicated: /proc/mounts repeats a filesystem under every
   * bind mount it has. */
  GHashTable *seen =
      g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
  for (guint i = 0; i < roots->len; i++) {
    const char *root = g_ptr_array_index(roots, i);
    g_assert_false(g_hash_table_contains(seen, root));
    g_hash_table_add(seen, g_strdup(root));
  }
  g_hash_table_destroy(seen);

  g_free(home);
  g_ptr_array_free(roots, TRUE);
}

/* ============================================================================
 * COUNTING
 * ============================================================================ */

static void test_count_at(void) {
  char *root = make_trash_root("count");

  add_entry(root, "one");
  add_entry(root, "two");
  add_entry(root, "three");

  /* Only the metadata files count: the trash also holds the files themselves
   * and may hold unrelated notes in info/. */
  add_file(root, "one");
  char *info_dir = g_build_filename(root, "info", NULL);
  char *notes = g_build_filename(info_dir, "notes.txt", NULL);
  g_assert_true(g_file_set_contents(notes, "not a trashed item\n", -1, NULL));

  g_assert_cmpuint(lambdock_trash_count_at(root), ==, 3);

  /* Missing and invalid roots are not an error, they are just empty. */
  g_assert_cmpuint(lambdock_trash_count_at(NULL), ==, 0);
  g_assert_cmpuint(lambdock_trash_count_at("/nonexistent/lambdock-trash"),
                   ==, 0);

  g_free(notes);
  g_free(info_dir);
  remove_tree(root);
  g_free(root);
}

static void test_public_count_sees_the_home_trash(void) {
  char *home = lambdock_trash_home_path();

  add_entry(home, "home-one");
  add_entry(home, "home-two");
  add_file(home, "home-one");

  /* Volume trashes may add to this count, so the home entries are a floor. */
  g_assert_cmpuint(lambdock_trash_count_at(home), ==, 2);
  g_assert_cmpuint(lambdock_trash_count(), >=, 2);
  g_assert_false(lambdock_trash_is_empty());

  g_assert_cmpuint(lambdock_trash_empty_at(home), ==, 2);
  g_assert_cmpuint(lambdock_trash_count_at(home), ==, 0);

  remove_tree(home);
  g_free(home);
}

/* ============================================================================
 * EMPTYING
 * ============================================================================ */

static void test_empty_at_removes_everything(void) {
  char *root = make_trash_root("empty");

  add_entry(root, "document.txt");
  add_file(root, "document.txt");
  add_entry(root, "folder");
  add_file(root, "folder/nested/file.txt");

  /* Orphans: files without metadata, and metadata that is not a trash entry.
   * Emptying the trash leaves nothing behind. */
  add_file(root, "orphan-without-metadata");
  char *info_dir = g_build_filename(root, "info", NULL);
  char *junk = g_build_filename(info_dir, "stale.junk", NULL);
  g_assert_true(g_file_set_contents(junk, "junk\n", -1, NULL));

  g_assert_cmpuint(lambdock_trash_empty_at(root), ==, 2);
  g_assert_cmpuint(lambdock_trash_count_at(root), ==, 0);

  char *files_dir = g_build_filename(root, "files", NULL);
  g_assert_true(directory_is_empty(files_dir));
  g_assert_true(directory_is_empty(info_dir));

  /* Emptying an already empty trash is a no-op that reports nothing removed. */
  g_assert_cmpuint(lambdock_trash_empty_at(root), ==, 0);
  g_assert_cmpuint(lambdock_trash_empty_at(NULL), ==, 0);

  g_free(files_dir);
  g_free(junk);
  g_free(info_dir);
  remove_tree(root);
  g_free(root);
}

static void test_empty_at_handles_read_only_directories(void) {
  char *root = make_trash_root("read-only");

  add_entry(root, "locked");
  add_file(root, "locked/child.txt");

  /* A trashed directory keeps the permissions it had when it was trashed. */
  char *locked = g_build_filename(root, "files", "locked", NULL);
  g_assert_cmpint(g_chmod(locked, 0500), ==, 0);

  g_assert_cmpuint(lambdock_trash_empty_at(root), ==, 1);

  char *files_dir = g_build_filename(root, "files", NULL);
  g_assert_true(directory_is_empty(files_dir));

  g_free(files_dir);
  g_free(locked);
  remove_tree(root);
  g_free(root);
}

/* ============================================================================
 * ADDING (WHAT A DROP ON THE TRASH ICON DOES)
 * ============================================================================ */

/* Trashes a file with lambdock_trash_file() and checks it arrived, with the
 * metadata the specification requires, in the throwaway home trash. */
static void test_trash_file_moves_it_into_the_home_trash(void) {
  char *home = lambdock_trash_home_path();
  lambdock_trash_empty_at(home);

  char *work = g_dir_make_tmp("lambdock-trash-drop-XXXXXX", NULL);
  g_assert_nonnull(work);
  char *path = g_build_filename(work, "dropped.txt", NULL);
  g_assert_true(g_file_set_contents(path, "dropped\n", -1, NULL));

  GFile *file = g_file_new_for_path(path);
  GError *error = NULL;
  g_assert_true(lambdock_trash_file(file, &error));
  g_assert_no_error(error);

  /* The original is gone and GIO moved the file into the trash... */
  g_assert_false(g_file_test(path, G_FILE_TEST_EXISTS));
  char *trashed = g_build_filename(home, "files", "dropped.txt", NULL);
  g_assert_true(g_file_test(trashed, G_FILE_TEST_IS_REGULAR));

  /* ...with the Path/DeletionDate metadata every file manager reads. */
  char *info = g_build_filename(home, "info", "dropped.txt.trashinfo", NULL);
  char *contents = NULL;
  g_assert_true(g_file_get_contents(info, &contents, NULL, NULL));
  g_assert_nonnull(strstr(contents, "[Trash Info]"));
  char *path_line = g_strconcat("Path=", path, "\n", NULL);
  g_assert_nonnull(strstr(contents, path_line));
  g_assert_nonnull(strstr(contents, "DeletionDate="));
  g_assert_cmpuint(lambdock_trash_count_at(home), ==, 1);

  g_free(path_line);
  g_free(contents);
  g_free(info);
  g_free(trashed);
  g_object_unref(file);

  lambdock_trash_empty_at(home);
  remove_tree(work);
  g_free(path);
  g_free(work);
  g_free(home);
}

/* Dropping a folder takes the folder and everything inside it. */
static void test_trash_file_trashes_a_folder(void) {
  char *home = lambdock_trash_home_path();
  lambdock_trash_empty_at(home);

  char *work = g_dir_make_tmp("lambdock-trash-drop-XXXXXX", NULL);
  g_assert_nonnull(work);
  char *folder = g_build_filename(work, "folder", NULL);
  char *nested = g_build_filename(folder, "nested.txt", NULL);
  g_assert_cmpint(g_mkdir_with_parents(folder, 0700), ==, 0);
  g_assert_true(g_file_set_contents(nested, "nested\n", -1, NULL));

  GFile *file = g_file_new_for_path(folder);
  GError *error = NULL;
  g_assert_true(lambdock_trash_file(file, &error));
  g_assert_no_error(error);

  g_assert_false(g_file_test(folder, G_FILE_TEST_EXISTS));
  char *trashed_nested =
      g_build_filename(home, "files", "folder", "nested.txt", NULL);
  g_assert_true(g_file_test(trashed_nested, G_FILE_TEST_IS_REGULAR));
  g_assert_cmpuint(lambdock_trash_count_at(home), ==, 1);

  g_free(trashed_nested);
  g_object_unref(file);

  lambdock_trash_empty_at(home);
  remove_tree(work);
  g_free(nested);
  g_free(folder);
  g_free(work);
  g_free(home);
}

/* Two files with the same name are both kept: GIO gives the second one a fresh
 * name instead of overwriting the first. */
static void test_trash_file_keeps_colliding_names(void) {
  char *home = lambdock_trash_home_path();
  lambdock_trash_empty_at(home);

  char *first_dir = g_dir_make_tmp("lambdock-trash-drop-a-XXXXXX", NULL);
  char *second_dir = g_dir_make_tmp("lambdock-trash-drop-b-XXXXXX", NULL);
  g_assert_nonnull(first_dir);
  g_assert_nonnull(second_dir);

  char *first = g_build_filename(first_dir, "same.txt", NULL);
  char *second = g_build_filename(second_dir, "same.txt", NULL);
  g_assert_true(g_file_set_contents(first, "first\n", -1, NULL));
  g_assert_true(g_file_set_contents(second, "second\n", -1, NULL));

  GFile *first_file = g_file_new_for_path(first);
  GFile *second_file = g_file_new_for_path(second);
  g_assert_true(lambdock_trash_file(first_file, NULL));
  g_assert_true(lambdock_trash_file(second_file, NULL));

  char *files_dir = g_build_filename(home, "files", NULL);
  g_assert_cmpuint(lambdock_trash_count_at(home), ==, 2);
  g_assert_cmpuint(count_entries(files_dir), ==, 2);

  g_free(files_dir);
  g_object_unref(second_file);
  g_object_unref(first_file);

  lambdock_trash_empty_at(home);
  remove_tree(first_dir);
  remove_tree(second_dir);
  g_free(second);
  g_free(first);
  g_free(second_dir);
  g_free(first_dir);
  g_free(home);
}

static void test_trash_file_reports_failures(void) {
  char *work = g_dir_make_tmp("lambdock-trash-drop-XXXXXX", NULL);
  g_assert_nonnull(work);
  char *path = g_build_filename(work, "gone.txt", NULL);

  GFile *file = g_file_new_for_path(path);
  GError *error = NULL;
  g_assert_false(lambdock_trash_file(file, &error));
  g_assert_nonnull(error);
  g_clear_error(&error);

  /* A missing GFile is refused before it reaches GIO. */
  g_assert_false(lambdock_trash_file(NULL, &error));
  g_assert_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT);
  g_clear_error(&error);

  g_object_unref(file);
  remove_tree(work);
  g_free(path);
  g_free(work);
}

int main(int argc, char **argv) {
  /* g_get_user_data_dir() reads XDG_DATA_HOME once and caches it, so point it at
   * a throwaway directory before anything asks for it. */
  data_home = g_dir_make_tmp("lambdock-trash-home-XXXXXX", NULL);
  g_assert_nonnull(data_home);
  g_setenv("XDG_DATA_HOME", data_home, TRUE);
  /* GIO only uses the home trash for files on the home filesystem, and refuses
   * "system internal" volume mounts (a /tmp tmpfs or the build sandbox's
   * filesystem, for instance). Pointing HOME at the same throwaway directory
   * keeps the trashing tests on the ordinary home-trash path wherever the
   * tests are built and run. */
  g_setenv("HOME", data_home, TRUE);

  g_test_init(&argc, &argv, NULL);

  g_test_add_func("/ui/trash/home_path", test_home_path);
  g_test_add_func("/ui/trash/roots", test_roots_start_with_home_trash);
  g_test_add_func("/ui/trash/count_at", test_count_at);
  g_test_add_func("/ui/trash/public_count",
                  test_public_count_sees_the_home_trash);
  g_test_add_func("/ui/trash/empty_at", test_empty_at_removes_everything);
  g_test_add_func("/ui/trash/empty_at_read_only",
                  test_empty_at_handles_read_only_directories);
  g_test_add_func("/ui/trash/trash_file",
                  test_trash_file_moves_it_into_the_home_trash);
  g_test_add_func("/ui/trash/trash_file_folder",
                  test_trash_file_trashes_a_folder);
  g_test_add_func("/ui/trash/trash_file_collision",
                  test_trash_file_keeps_colliding_names);
  g_test_add_func("/ui/trash/trash_file_failure",
                  test_trash_file_reports_failures);

  int result = g_test_run();

  remove_tree(data_home);
  g_free(data_home);
  return result;
}
