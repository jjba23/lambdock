// lambdock

// Copyright © Josep Bigorra <jjbigorra@gmail.com>

// lambdock is free software : you can redistribute it and / or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.

// lambdock is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
// GNU General Public License for more details.

// You should have received a copy of the GNU General Public License
// along with lambdock. If not, see <https://www.gnu.org/licenses/>.

#include "../src/ui/desktop_entry.h"
#include <gio/gio.h>
#include <glib.h>
#include <stdio.h>

/* ============================================================================
 * EXEC FIELD CODES
 * ============================================================================ */

static void test_strip_field_codes(void) {
  char *exec = lambdock_exec_strip_field_codes("firefox %u");
  g_assert_cmpstr(exec, ==, "firefox");
  g_free(exec);

  exec = lambdock_exec_strip_field_codes("nautilus --new-window %U");
  g_assert_cmpstr(exec, ==, "nautilus --new-window");
  g_free(exec);

  /* Everything the spec says expands to nothing disappears, whatever the code's
   * case, and the leftover gaps are collapsed. */
  exec = lambdock_exec_strip_field_codes("app %f %F %i %c %k %d %D %n %N %v");
  g_assert_cmpstr(exec, ==, "app");
  g_free(exec);

  /* A literal percent stays, written as %%. */
  exec = lambdock_exec_strip_field_codes("printf 100%%");
  g_assert_cmpstr(exec, ==, "printf 100%");
  g_free(exec);

  /* Unknown codes are kept verbatim instead of silently vanishing. */
  exec = lambdock_exec_strip_field_codes("app %z");
  g_assert_cmpstr(exec, ==, "app %z");
  g_free(exec);

  /* Leading/trailing whitespace and a lone trailing '%' are handled. */
  exec = lambdock_exec_strip_field_codes("  app %u  ");
  g_assert_cmpstr(exec, ==, "app");
  g_free(exec);

  exec = lambdock_exec_strip_field_codes("app %");
  g_assert_cmpstr(exec, ==, "app");
  g_free(exec);

  g_assert_null(lambdock_exec_strip_field_codes(NULL));
}

/* ============================================================================
 * PARSING
 * ============================================================================ */

static const char *VALID_ENTRY = "[Desktop Entry]\n"
                                 "Type=Application\n"
                                 "Name=Web Browser\n"
                                 "Exec=firefox %u\n"
                                 "Icon=firefox\n";

static void test_parse_valid(void) {
  LambdockDesktopEntry *entry =
      lambdock_desktop_entry_new_from_data(VALID_ENTRY, "firefox.desktop");

  g_assert_nonnull(entry);
  g_assert_cmpstr(entry->id, ==, "firefox.desktop");
  g_assert_cmpstr(entry->name, ==, "Web Browser");
  g_assert_cmpstr(entry->exec, ==, "firefox");
  g_assert_cmpstr(entry->icon, ==, "firefox");
  g_assert_false(entry->no_display);
  g_assert_false(entry->hidden);
  lambdock_desktop_entry_free(entry);
  lambdock_desktop_entry_free(NULL);
}

static void test_parse_rejects(void) {
  /* Not a key file at all. */
  g_assert_null(lambdock_desktop_entry_new_from_data("this is not a desktop",
                                                     "x.desktop"));
  /* Empty data. */
  g_assert_null(lambdock_desktop_entry_new_from_data("", "x.desktop"));
  g_assert_null(lambdock_desktop_entry_new_from_data(NULL, "x.desktop"));

  /* Wrong group: only [Desktop Entry] describes the application itself. */
  g_assert_null(lambdock_desktop_entry_new_from_data(
      "[Desktop Action new]\nName=Nope\nExec=x\n", "x.desktop"));

  /* Not an application (links, directories...). */
  g_assert_null(lambdock_desktop_entry_new_from_data(
      "[Desktop Entry]\nType=Link\nName=A link\nURL=https://example.org\n",
      "x.desktop"));

  /* No name / no exec. */
  g_assert_null(lambdock_desktop_entry_new_from_data(
      "[Desktop Entry]\nType=Application\nExec=x\n", "x.desktop"));
  g_assert_null(lambdock_desktop_entry_new_from_data(
      "[Desktop Entry]\nType=Application\nName=No exec\n", "x.desktop"));
}

static void test_parse_flags_and_icon_default(void) {
  LambdockDesktopEntry *entry = lambdock_desktop_entry_new_from_data(
      "[Desktop Entry]\n"
      "Type=Application\n"
      "Name=Hidden Thing\n"
      "Exec=hidden\n"
      "NoDisplay=true\n"
      "Hidden=true\n",
      "hidden.desktop");

  g_assert_nonnull(entry);
  /* Parsing keeps the flags: hiding is the scanner's decision, because a
   * dropped .desktop file should still be able to add the app. */
  g_assert_true(entry->no_display);
  g_assert_true(entry->hidden);
  g_assert_cmpstr(entry->icon, ==, "application-x-executable");
  lambdock_desktop_entry_free(entry);

  /* An empty Icon= falls back to the generic executable icon too. */
  entry = lambdock_desktop_entry_new_from_data("[Desktop Entry]\n"
                                               "Type=Application\n"
                                               "Name=No icon\n"
                                               "Exec=x\n"
                                               "Icon=\n",
                                               "noicon.desktop");
  g_assert_nonnull(entry);
  g_assert_cmpstr(entry->icon, ==, "application-x-executable");
  lambdock_desktop_entry_free(entry);
}

static void test_parse_localized_name(void) {
  LambdockDesktopEntry *entry = lambdock_desktop_entry_new_from_data(
      "[Desktop Entry]\n"
      "Type=Application\n"
      "Name=Default Name\n"
      "Name[de]=Deutscher Name\n"
      "Exec=app\n",
      "i18n.desktop");

  g_assert_nonnull(entry);
  /* The default locale is "C", so the untranslated Name wins. */
  g_assert_cmpstr(entry->name, ==, "Default Name");
  lambdock_desktop_entry_free(entry);
}

static void test_parse_file(void) {
  char *dir = g_dir_make_tmp("lambdock-desktop-XXXXXX", NULL);
  g_assert_nonnull(dir);

  char *path = g_build_filename(dir, "sample.desktop", NULL);
  g_assert_true(g_file_set_contents(path, VALID_ENTRY, -1, NULL));

  LambdockDesktopEntry *entry = lambdock_desktop_entry_new_from_file(path);
  g_assert_nonnull(entry);
  g_assert_cmpstr(entry->id, ==, "sample.desktop");
  g_assert_cmpstr(entry->exec, ==, "firefox");
  lambdock_desktop_entry_free(entry);

  /* Missing files are simply not entries. */
  char *missing = g_build_filename(dir, "missing.desktop", NULL);
  g_assert_null(lambdock_desktop_entry_new_from_file(missing));
  g_assert_null(lambdock_desktop_entry_new_from_file(NULL));

  remove(path);
  g_free(missing);
  g_free(path);
  remove(dir);
  g_free(dir);
}

/* ============================================================================
 * SCANNING
 * ============================================================================ */

static void write_entry(const char *dir, const char *filename, const char *name,
                        const char *exec) {
  char *path = g_build_filename(dir, filename, NULL);
  char *data = g_strdup_printf("[Desktop Entry]\n"
                               "Type=Application\n"
                               "Name=%s\n"
                               "Exec=%s\n"
                               "Icon=%s\n",
                               name, exec, exec);
  g_assert_true(g_file_set_contents(path, data, -1, NULL));
  g_free(data);
  g_free(path);
}

static gboolean entries_contain(GPtrArray *entries, const char *name) {
  for (guint i = 0; i < entries->len; i++) {
    LambdockDesktopEntry *entry = g_ptr_array_index(entries, i);
    if (g_strcmp0(entry->name, name) == 0)
      return TRUE;
  }
  return FALSE;
}

static int entries_index_of(GPtrArray *entries, const char *name) {
  for (guint i = 0; i < entries->len; i++) {
    LambdockDesktopEntry *entry = g_ptr_array_index(entries, i);
    if (g_strcmp0(entry->name, name) == 0)
      return (int)i;
  }
  return -1;
}

static void test_scan(const char *data_dir) {
  char *apps = g_build_filename(data_dir, "applications", NULL);

  write_entry(apps, "zebra.desktop", "Zebra", "zebra");
  write_entry(apps, "alpha.desktop", "Alpha", "alpha");

  char *path = g_build_filename(apps, "hidden.desktop", NULL);
  char *data = g_strdup_printf("[Desktop Entry]\n"
                               "Type=Application\n"
                               "Name=Hidden\n"
                               "Exec=hidden\n"
                               "Hidden=true\n");
  g_assert_true(g_file_set_contents(path, data, -1, NULL));
  g_free(data);
  g_free(path);

  path = g_build_filename(apps, "nodisplay.desktop", NULL);
  data = g_strdup_printf("[Desktop Entry]\n"
                         "Type=Application\n"
                         "Name=NoDisplay\n"
                         "Exec=nodisplay\n"
                         "NoDisplay=true\n");
  g_assert_true(g_file_set_contents(path, data, -1, NULL));
  g_free(data);
  g_free(path);

  GPtrArray *entries = lambdock_desktop_entries_scan();
  g_assert_nonnull(entries);

  /* The two visible applications are listed, in name order, and the ones the
   * user or the distribution hid are not. */
  g_assert_true(entries_contain(entries, "Alpha"));
  g_assert_true(entries_contain(entries, "Zebra"));
  g_assert_false(entries_contain(entries, "Hidden"));
  g_assert_false(entries_contain(entries, "NoDisplay"));
  g_assert_cmpint(entries_index_of(entries, "Alpha"), <,
                  entries_index_of(entries, "Zebra"));

  lambdock_desktop_entries_free(entries);
  lambdock_desktop_entries_free(NULL);

  g_free(apps);
}

int main(int argc, char **argv) {
  g_test_init(&argc, &argv, NULL);

  /* Point every XDG data lookup at a private tree *before* glib caches the
   * values, so the scan sees only the entries written here. */
  char *data_dir = g_dir_make_tmp("lambdock-xdg-XXXXXX", NULL);
  g_assert_nonnull(data_dir);

  char *apps = g_build_filename(data_dir, "applications", NULL);
  g_assert_cmpint(g_mkdir_with_parents(apps, 0755), ==, 0);
  g_free(apps);

  g_setenv("XDG_DATA_HOME", data_dir, TRUE);
  g_setenv("XDG_DATA_DIRS", data_dir, TRUE);
  g_setenv("HOME", data_dir, TRUE);

  g_test_add_func("/desktop_entry/strip_field_codes", test_strip_field_codes);
  g_test_add_func("/desktop_entry/parse_valid", test_parse_valid);
  g_test_add_func("/desktop_entry/parse_rejects", test_parse_rejects);
  g_test_add_func("/desktop_entry/parse_flags",
                  test_parse_flags_and_icon_default);
  g_test_add_func("/desktop_entry/parse_localized", test_parse_localized_name);
  g_test_add_func("/desktop_entry/parse_file", test_parse_file);
  g_test_add_data_func("/desktop_entry/scan", data_dir,
                       (GTestDataFunc)test_scan);

  return g_test_run();
}
