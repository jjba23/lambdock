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

#include "desktop_entry.h"
#include "../logger.h"

#define DESKTOP_GROUP "Desktop Entry"

/* ============================================================================
 * EXEC FIELD CODES
 * ============================================================================
 */

/* Field codes that expand to a file name, URL or an empty string. They have no
 * meaning for a launcher that is not given any argument, so they are dropped.
 * The deprecated %d %D %n %N %v %m codes are dropped as well. */
static gboolean is_dropped_field_code(char code) {
  switch (code) {
  case 'f':
  case 'F':
  case 'u':
  case 'U':
  case 'i':
  case 'c':
  case 'k':
  case 'd':
  case 'D':
  case 'n':
  case 'N':
  case 'v':
  case 'm':
    return TRUE;
  default:
    return FALSE;
  }
}

char *lambdock_exec_strip_field_codes(const char *exec) {
  if (!exec)
    return NULL;

  GString *out = g_string_new(NULL);

  for (const char *p = exec; *p != '\0'; p++) {
    if (*p != '%') {
      g_string_append_c(out, *p);
      continue;
    }

    char next = p[1];
    if (next == '\0')
      break; /* A lone trailing '%' is not a field code. */

    if (next == '%') {
      g_string_append_c(out, '%');
      p++;
    } else if (is_dropped_field_code(next)) {
      p++;
    } else {
      /* Unknown code: keep it verbatim so nothing silently disappears. */
      g_string_append_c(out, '%');
    }
  }

  /* Dropping %f/%U leaves double spaces behind, which would make the command
   * line ugly (and, in shell-less spawning, harmless but noisy in logs). */
  char *raw = g_string_free(out, FALSE);
  char *collapsed = g_strdup(raw);
  g_strstrip(collapsed);

  GString *clean = g_string_new(NULL);
  gboolean previous_space = FALSE;
  for (const char *p = collapsed; *p != '\0'; p++) {
    gboolean space = g_ascii_isspace(*p);
    if (space && previous_space)
      continue;
    g_string_append_c(clean, space ? ' ' : *p);
    previous_space = space;
  }

  g_free(raw);
  g_free(collapsed);
  return g_string_free(clean, FALSE);
}

/* ============================================================================
 * PARSING
 * ============================================================================
 */

static gboolean key_file_flag(GKeyFile *kf, const char *key) {
  GError *error = NULL;
  gboolean value = g_key_file_get_boolean(kf, DESKTOP_GROUP, key, &error);
  if (error) {
    g_clear_error(&error);
    return FALSE;
  }
  return value;
}

LambdockDesktopEntry *lambdock_desktop_entry_new_from_data(const char *data,
                                                           const char *id) {
  if (!data || *data == '\0')
    return NULL;

  GKeyFile *kf = g_key_file_new();
  if (!g_key_file_load_from_data(kf, data, strlen(data), G_KEY_FILE_NONE,
                                 NULL)) {
    g_key_file_free(kf);
    return NULL;
  }

  if (!g_key_file_has_group(kf, DESKTOP_GROUP)) {
    g_key_file_free(kf);
    return NULL;
  }

  char *type = g_key_file_get_string(kf, DESKTOP_GROUP, "Type", NULL);
  gboolean is_application =
      (!type || g_ascii_strcasecmp(type, "Application") == 0);
  g_free(type);

  if (!is_application) {
    g_key_file_free(kf);
    return NULL;
  }

  char *name = g_key_file_get_locale_string(kf, DESKTOP_GROUP, "Name", NULL,
                                            NULL);
  if (!name || *name == '\0') {
    g_free(name);
    g_key_file_free(kf);
    return NULL;
  }

  char *raw_exec = g_key_file_get_string(kf, DESKTOP_GROUP, "Exec", NULL);
  char *exec = lambdock_exec_strip_field_codes(raw_exec);
  g_free(raw_exec);

  if (!exec || *exec == '\0') {
    g_free(name);
    g_free(exec);
    g_key_file_free(kf);
    return NULL;
  }

  char *icon = g_key_file_get_string(kf, DESKTOP_GROUP, "Icon", NULL);

  LambdockDesktopEntry *entry = g_new0(LambdockDesktopEntry, 1);
  entry->id = g_strdup(id);
  entry->name = name;
  entry->exec = exec;
  if (icon && *icon != '\0') {
    entry->icon = icon;
  } else {
    entry->icon = g_strdup("application-x-executable");
    g_free(icon);
  }
  entry->no_display = key_file_flag(kf, "NoDisplay");
  entry->hidden = key_file_flag(kf, "Hidden");

  g_key_file_free(kf);
  return entry;
}

LambdockDesktopEntry *lambdock_desktop_entry_new_from_file(const char *path) {
  if (!path)
    return NULL;

  char *data = NULL;
  if (!g_file_get_contents(path, &data, NULL, NULL))
    return NULL;

  char *id = g_path_get_basename(path);
  LambdockDesktopEntry *entry =
      lambdock_desktop_entry_new_from_data(data, id);
  g_free(id);
  g_free(data);
  return entry;
}

void lambdock_desktop_entry_free(LambdockDesktopEntry *entry) {
  if (!entry)
    return;

  g_free(entry->id);
  g_free(entry->name);
  g_free(entry->exec);
  g_free(entry->icon);
  g_free(entry);
}

/* ============================================================================
 * SCANNING THE XDG APPLICATION DIRECTORIES
 * ============================================================================
 */

static void add_applications_dir(GPtrArray *dirs, GHashTable *seen,
                                const char *share_dir) {
  if (!share_dir || *share_dir == '\0')
    return;

  char *path = g_build_filename(share_dir, "applications", NULL);
  if (g_hash_table_contains(seen, path) ||
      !g_file_test(path, G_FILE_TEST_IS_DIR)) {
    g_free(path);
    return;
  }

  g_hash_table_add(seen, g_strdup(path));
  g_ptr_array_add(dirs, path);
}

static GPtrArray *applications_dirs(void) {
  GPtrArray *dirs = g_ptr_array_new_with_free_func(g_free);
  GHashTable *seen = g_hash_table_new_full(g_str_hash, g_str_equal, g_free,
                                           NULL);

  /* User first: a .desktop file in ~/.local/share overrides the system one. */
  add_applications_dir(dirs, seen, g_get_user_data_dir());

  /* Flatpak exports next, so flatpak apps win over distro packages. */
  char *user_share = g_build_filename(g_get_user_data_dir(), "flatpak",
                                      "exports", "share", NULL);
  add_applications_dir(dirs, seen, user_share);
  g_free(user_share);
  add_applications_dir(dirs, seen, "/var/lib/flatpak/exports/share");

  const char *const *system_dirs = g_get_system_data_dirs();
  for (int i = 0; system_dirs && system_dirs[i] != NULL; i++)
    add_applications_dir(dirs, seen, system_dirs[i]);

  g_hash_table_destroy(seen);
  return dirs;
}

static void scan_one_dir(const char *dir, GPtrArray *out, GHashTable *seen_ids) {
  GDir *handle = g_dir_open(dir, 0, NULL);
  if (!handle)
    return;

  const char *filename;
  while ((filename = g_dir_read_name(handle)) != NULL) {
    if (!g_str_has_suffix(filename, ".desktop"))
      continue;

    if (g_hash_table_contains(seen_ids, filename))
      continue;

    char *path = g_build_filename(dir, filename, NULL);
    LambdockDesktopEntry *entry = lambdock_desktop_entry_new_from_file(path);
    g_free(path);

    if (!entry)
      continue;

    /* Record the id even when the entry is hidden: the user's override is
     * supposed to hide the system entry, not to resurface it later. */
    g_hash_table_add(seen_ids, g_strdup(filename));

    if (entry->hidden || entry->no_display) {
      lambdock_desktop_entry_free(entry);
      continue;
    }

    g_ptr_array_add(out, entry);
  }

  g_dir_close(handle);
}

static gint compare_entries(gconstpointer a, gconstpointer b) {
  const LambdockDesktopEntry *entry_a = *(const LambdockDesktopEntry *const *)a;
  const LambdockDesktopEntry *entry_b = *(const LambdockDesktopEntry *const *)b;
  return g_utf8_collate(entry_a->name ? entry_a->name : "",
                        entry_b->name ? entry_b->name : "");
}

GPtrArray *lambdock_desktop_entries_scan(void) {
  GPtrArray *entries =
      g_ptr_array_new_with_free_func((GDestroyNotify)lambdock_desktop_entry_free);
  GHashTable *seen_ids = g_hash_table_new_full(g_str_hash, g_str_equal, g_free,
                                               NULL);
  GPtrArray *dirs = applications_dirs();

  for (guint i = 0; i < dirs->len; i++)
    scan_one_dir(g_ptr_array_index(dirs, i), entries, seen_ids);

  g_ptr_array_sort(entries, compare_entries);

  g_hash_table_destroy(seen_ids);
  g_ptr_array_free(dirs, TRUE);
  return entries;
}

void lambdock_desktop_entries_free(GPtrArray *entries) {
  if (entries)
    g_ptr_array_free(entries, TRUE);
}

/* ============================================================================
 * SPAWNING
 * ============================================================================
 */

gboolean lambdock_spawn_command(const char *command) {
  if (!command || *command == '\0')
    return FALSE;

  GError *error = NULL;
  if (!g_spawn_command_line_async(command, &error)) {
    LOG_C_WARN("UI", "Failed to launch '%s': %s", command, error->message);
    g_clear_error(&error);
    return FALSE;
  }
  return TRUE;
}

gboolean lambdock_open_path(const char *path) {
  if (!path || *path == '\0')
    return FALSE;

  char *argv[] = {(char *)"xdg-open", (char *)path, NULL};
  GError *error = NULL;

  if (!g_spawn_async(NULL, argv, NULL, G_SPAWN_SEARCH_PATH, NULL, NULL, NULL,
                     &error)) {
    LOG_C_WARN("UI", "Failed to open '%s': %s", path, error->message);
    g_clear_error(&error);
    return FALSE;
  }
  return TRUE;
}
