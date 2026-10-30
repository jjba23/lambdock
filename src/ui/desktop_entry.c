// lambdock -- hyper-hackable desktop dock for Wayland
//
// SPDX-License-Identifier: MIT
// Copyright © Josep Bigorra <jjbigorra@gmail.com>

#include "desktop_entry.h"
#include "../logger.h"
#include <glib/gi18n.h>

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
 * DESKTOP IDS
 * ============================================================================
 */

static char *desktop_entry_stem(const char *id) {
  if (!id)
    return NULL;

  if (g_str_has_suffix(id, ".desktop"))
    return g_strndup(id, strlen(id) - strlen(".desktop"));

  return g_strdup(id);
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

  /* StartupWMClass may list several classes separated by semicolons; the first
   * one is what compositors actually report. */
  char *wm_class = g_key_file_get_string(kf, DESKTOP_GROUP, "StartupWMClass",
                                         NULL);
  if (wm_class && *wm_class != '\0') {
    char **parts = g_strsplit(wm_class, ";", -1);
    if (parts[0] && *parts[0] != '\0') {
      g_free(wm_class);
      wm_class = g_strdup(parts[0]);
    }
    g_strfreev(parts);
  } else {
    g_clear_pointer(&wm_class, g_free);
  }

  /* Categories= is a semicolon separated list of main and additional
   * categories. It is kept as written; lambdock_desktop_entry_has_category()
   * does the matching. */
  char *categories = g_key_file_get_string(kf, DESKTOP_GROUP, "Categories",
                                           NULL);
  if (categories && *categories == '\0')
    g_clear_pointer(&categories, g_free);

  LambdockDesktopEntry *entry = g_new0(LambdockDesktopEntry, 1);
  entry->id = g_strdup(id);
  entry->stem = desktop_entry_stem(id);
  entry->name = name;
  entry->exec = exec;
  if (icon && *icon != '\0') {
    entry->icon = icon;
  } else {
    entry->icon = g_strdup("application-x-executable");
    g_free(icon);
  }
  entry->wm_class = wm_class;
  entry->categories = categories;
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

  /* Where the file lives is part of what an entry is: it is what tells the
   * copies of an application apart (see lambdock_desktop_entry_source_label). */
  if (entry)
    entry->path = g_strdup(path);

  return entry;
}

void lambdock_desktop_entry_free(LambdockDesktopEntry *entry) {
  if (!entry)
    return;

  g_free(entry->id);
  g_free(entry->stem);
  g_free(entry->name);
  g_free(entry->exec);
  g_free(entry->icon);
  g_free(entry->wm_class);
  g_free(entry->categories);
  g_free(entry->path);
  g_clear_pointer(&entry->alternates, g_ptr_array_unref);
  g_free(entry);
}

char *lambdock_desktop_entry_source_label(const LambdockDesktopEntry *entry) {
  if (!entry)
    return NULL;

  const char *path = entry->path;
  if (!path || *path == '\0')
    return g_strdup(_("Application"));

  /* "~/.local/share/flatpak/exports/share/applications/x.desktop" and
   * "/var/lib/flatpak/..." are both Flatpak exports. */
  if (strstr(path, "/flatpak/") != NULL)
    return g_strdup(_("Flatpak"));

  const char *user_dir = g_get_user_data_dir();
  if (user_dir && *user_dir != '\0' && g_str_has_prefix(path, user_dir))
    return g_strdup(_("Your files"));

  return g_strdup(_("System"));
}

gboolean lambdock_desktop_entry_has_category(const LambdockDesktopEntry *entry,
                                             const char *category) {
  if (!entry || !entry->categories || !category || *category == '\0')
    return FALSE;

  char **parts = g_strsplit(entry->categories, ";", -1);
  gboolean found = FALSE;
  for (int i = 0; parts[i] != NULL && !found; i++) {
    if (*parts[i] != '\0' && g_ascii_strcasecmp(parts[i], category) == 0)
      found = TRUE;
  }
  g_strfreev(parts);
  return found;
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

/* ============================================================================
 * ONE TILE PER APPLICATION
 * ============================================================================
 *
 * A desktop file name is not an application. The same program installed as a
 * distro package and as a Flatpak exports two differently named files --
 * "libreoffice-writer.desktop" and "org.libreoffice.LibreOffice.writer.desktop"
 * -- that a launcher would show as two identical "LibreOffice Writer" tiles.
 *
 * Entries are therefore collapsed when they agree on the name *and* on the
 * strongest identity they both declare: the window class a compositor reports,
 * or, for entries that declare no class at all, the icon they draw. Two
 * same-named applications stay separate as long as their classes or icons
 * differ, because they really are two applications (a GNOME settings panel and
 * a third-party tool may both be called "Displays").
 */

static char *application_identity_key(const LambdockDesktopEntry *entry) {
  char *name = g_utf8_casefold(entry->name ? entry->name : "", -1);

  /* "org.gnome.Nautilus" and "org-gnome-nautilus" are the same class spelled
   * differently, and Flatpak exports often name the icon after the app id. */
  gboolean has_class = entry->wm_class && *entry->wm_class != '\0';
  const char *signal = has_class ? entry->wm_class : entry->icon;
  char *folded = g_utf8_casefold(signal ? signal : "", -1);

  /* \x1f cannot appear in a desktop file value, so it cannot make two
   * different names collide. */
  char *key = g_strdup_printf("%s\x1f%s", name, folded);

  g_free(folded);
  g_free(name);
  return key;
}

/* Attaches COPY to KEEPER as another installed copy of the same application.
 * The menu of a launchpad tile is what shows them (see launchpad.c). */
static void add_alternate(LambdockDesktopEntry *keeper,
                          LambdockDesktopEntry *copy) {
  if (!keeper || !copy)
    return;

  if (!keeper->alternates)
    keeper->alternates = g_ptr_array_new_with_free_func(
        (GDestroyNotify)lambdock_desktop_entry_free);

  g_ptr_array_add(keeper->alternates, copy);
}

/* Lists each application once, keeping the first copy of it. Scan order is the
 * precedence order -- user entries, then Flatpak exports, then the system ones
 * -- so the one that stays on the grid is the copy the user's own directory or
 * Flatpak put there, exactly like the id-based deduplication above. The copies
 * that lose are not thrown away: they move to the winner's alternates, because
 * a launcher should offer them rather than decide silently. */
static void collapse_duplicate_applications(GPtrArray *entries) {
  GHashTable *kept = g_hash_table_new_full(g_str_hash, g_str_equal, g_free,
                                           NULL);

  for (guint i = 0; i < entries->len;) {
    LambdockDesktopEntry *entry = g_ptr_array_index(entries, i);
    char *key = application_identity_key(entry);
    LambdockDesktopEntry *keeper = g_hash_table_lookup(kept, key);

    if (keeper) {
      g_free(key);
      /* Stealing hands the entry over instead of freeing it. */
      add_alternate(keeper, g_ptr_array_steal_index(entries, i));
      continue;
    }

    g_hash_table_insert(kept, key, entry);
    i++;
  }

  g_hash_table_destroy(kept);
}

GPtrArray *lambdock_desktop_entries_scan(void) {
  GPtrArray *entries =
      g_ptr_array_new_with_free_func((GDestroyNotify)lambdock_desktop_entry_free);
  GHashTable *seen_ids = g_hash_table_new_full(g_str_hash, g_str_equal, g_free,
                                               NULL);
  GPtrArray *dirs = applications_dirs();

  for (guint i = 0; i < dirs->len; i++)
    scan_one_dir(g_ptr_array_index(dirs, i), entries, seen_ids);

  /* Before the sort: collapsing in scan order is what makes the precedence
   * above predictable, since the sort only orders by name. */
  collapse_duplicate_applications(entries);

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
 * THE CACHE
 * ============================================================================
 *
 * A window's app id and its desktop file rarely agree on spelling: the
 * compositor reports "org.gnome.Nautilus" for a file called
 * "org.gnome.Nautilus.desktop", but "wezterm", "firefox" or "SystemMonitor" for
 * others. The cache indexes every entry under the names it could be reported
 * as -- desktop id, StartupWMClass, executable base name, the last component of
 * a reverse-DNS id, and a punctuation-insensitive form -- so a lookup is one
 * hash probe instead of a heuristic.
 */

/* The cache is one value, not three loose file statics: everything that reads
 * or replaces it goes through this struct, so it has a single owner, a single
 * lifetime, and a single place to look when "the entries are stale" comes up. */
typedef struct {
  GPtrArray *entries; /* owned; LambdockDesktopEntry*, never NULL once built */
  GHashTable *index;  /* owned; normalized alias -> entry (borrowed) */
  gint64 built_at;    /* monotonic time of the build, 0 when never built */
} DesktopEntryCache;

static DesktopEntryCache g_desktop_cache;

static void cache_clear(void) {
  g_clear_pointer(&g_desktop_cache.entries, g_ptr_array_unref);
  g_clear_pointer(&g_desktop_cache.index, g_hash_table_unref);
  g_desktop_cache.built_at = 0;
}

void lambdock_desktop_cache_invalidate(void) { cache_clear(); }

/* Case- and punctuation-insensitive form: "org.gnome.Nautilus" and
 * "org-gnome-nautilus" and "ORGGNOMENAUTILUS" all collapse to one key. */
static char *normalize_key(const char *value) {
  if (!value)
    return NULL;

  GString *out = g_string_new(NULL);
  for (const char *p = value; *p != '\0'; p++) {
    if (g_ascii_isalnum(*p))
      g_string_append_c(out, g_ascii_tolower(*p));
  }
  return g_string_free(out, FALSE);
}

/* The last component of a reverse-DNS id: "org.gnome.SystemMonitor" -> the key
 * for "SystemMonitor". Short single-component ids are returned unchanged. */
static char *last_component(const char *value) {
  if (!value)
    return NULL;

  const char *dot = strrchr(value, '.');
  return g_strdup((dot && dot[1] != '\0') ? dot + 1 : value);
}

static void cache_index_add(GHashTable *index, const char *alias,
                            LambdockDesktopEntry *entry) {
  if (!alias || *alias == '\0')
    return;

  char *key = normalize_key(alias);
  if (!key || *key == '\0') {
    g_free(key);
    return;
  }

  /* First one wins: the scan is already id-deduplicated and name-sorted, and an
   * exact alias is inserted before the looser ones below. */
  if (!g_hash_table_contains(index, key))
    g_hash_table_insert(index, key, entry);
  else
    g_free(key);
}

static char *exec_base_name(const char *exec) {
  if (!exec || *exec == '\0')
    return NULL;

  char **argv = g_strsplit_set(exec, " \t", -1);
  char *base = NULL;
  for (int i = 0; argv[i] != NULL && !base; i++) {
    const char *token = argv[i];
    if (*token == '\0' || g_str_has_prefix(token, "env=") ||
        g_strcmp0(token, "env") == 0 || g_strcmp0(token, "flatpak") == 0 ||
        g_strcmp0(token, "run") == 0 || g_str_has_prefix(token, "-"))
      continue;
    base = g_path_get_basename(token);
  }
  g_strfreev(argv);
  return base;
}

/* Every name an entry could be reported under: the desktop id, the window class
 * a compositor announces, the executable's base name, the last component of a
 * reverse-DNS id. */
static void cache_index_entry(GHashTable *index, LambdockDesktopEntry *entry) {
  cache_index_add(index, entry->id, entry);
  cache_index_add(index, entry->stem, entry);
  cache_index_add(index, entry->wm_class, entry);

  char *base = exec_base_name(entry->exec);
  cache_index_add(index, base, entry);
  g_free(base);

  char *tail = last_component(entry->stem);
  cache_index_add(index, tail, entry);
  g_free(tail);
}

static void cache_build(void) {
  cache_clear();

  g_desktop_cache.entries = lambdock_desktop_entries_scan();
  g_desktop_cache.index =
      g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);

  for (guint i = 0; i < g_desktop_cache.entries->len; i++) {
    LambdockDesktopEntry *entry = g_ptr_array_index(g_desktop_cache.entries, i);

    cache_index_entry(g_desktop_cache.index, entry);

    /* The copies the grid does not list are indexed too, so a window of the
     * copy that lost still resolves to the application and its icon. The one on
     * the grid goes first, and the index keeps the first entry of a key. */
    for (guint a = 0; entry->alternates && a < entry->alternates->len; a++)
      cache_index_entry(g_desktop_cache.index,
                        g_ptr_array_index(entry->alternates, a));
  }

  g_desktop_cache.built_at = g_get_monotonic_time();
  LOG_C_INFO("UI", "Desktop entry cache built: %u applications, %u keys",
             g_desktop_cache.entries->len,
             g_hash_table_size(g_desktop_cache.index));
}

static void cache_ensure(void) {
  if (!g_desktop_cache.entries || !g_desktop_cache.index) {
    cache_build();
    return;
  }

  gint64 age_ms = (g_get_monotonic_time() - g_desktop_cache.built_at) / 1000;
  if (age_ms > DESKTOP_CACHE_TTL_MS)
    cache_build();
}

GPtrArray *lambdock_desktop_entries_cached(void) {
  cache_ensure();
  return g_desktop_cache.entries;
}

const LambdockDesktopEntry *lambdock_desktop_entry_lookup(const char *app_id) {
  if (!app_id || *app_id == '\0')
    return NULL;

  cache_ensure();

  char *key = normalize_key(app_id);
  if (!key || *key == '\0') {
    g_free(key);
    return NULL;
  }

  const LambdockDesktopEntry *entry =
      g_hash_table_lookup(g_desktop_cache.index, key);
  if (entry) {
    g_free(key);
    return entry;
  }

  /* One more try without a vendor prefix, so "SystemMonitor" still finds
   * "org.gnome.SystemMonitor" when the compositor reports the short form. */
  char *tail = last_component(app_id);
  char *tail_key = normalize_key(tail);
  g_free(tail);

  if (tail_key && g_strcmp0(tail_key, key) != 0)
    entry = g_hash_table_lookup(g_desktop_cache.index, tail_key);

  g_free(tail_key);
  g_free(key);
  return entry;
}

/* ============================================================================
 * NAMES FOR APP IDS
 * ============================================================================
 */

/* Splits camel case and separators into words: "SystemMonitor" -> "System
 * Monitor", "google-chrome" -> "google chrome", "ZFSManager" -> "ZFS Manager". */
static GPtrArray *app_id_words(const char *value) {
  GPtrArray *words = g_ptr_array_new_with_free_func(g_free);
  if (!value || *value == '\0')
    return words;

  GString *word = g_string_new(NULL);
  gunichar previous = 0;
  const char *p = value;

  while (*p != '\0') {
    gunichar c = g_utf8_get_char(p);
    const char *next_p = g_utf8_next_char(p);
    gunichar next = (*next_p != '\0') ? g_utf8_get_char(next_p) : 0;

    if (c == '-' || c == '_' || c == '.' || c == ' ' || c == '+') {
      if (word->len > 0) {
        g_ptr_array_add(words, g_strdup(word->str));
        g_string_truncate(word, 0);
      }
      previous = 0;
      p = next_p;
      continue;
    }

    /* "System|Monitor" (lower then upper) and "ZFS|Manager" (the end of an
     * acronym) both start a new word. */
    if (word->len > 0 && previous != 0 && g_unichar_isupper(c) &&
        (g_unichar_islower(previous) ||
         (g_unichar_isupper(previous) && next != 0 &&
          g_unichar_islower(next)))) {
      g_ptr_array_add(words, g_strdup(word->str));
      g_string_truncate(word, 0);
    }

    g_string_append_unichar(word, c);
    previous = c;
    p = next_p;
  }

  if (word->len > 0)
    g_ptr_array_add(words, g_strdup(word->str));

  g_string_free(word, TRUE);
  return words;
}

char *lambdock_app_id_to_name(const char *app_id) {
  if (!app_id || *app_id == '\0')
    return NULL;

  /* "org.gnome.SystemMonitor.desktop" is only ever a file name. */
  char *stem = desktop_entry_stem(app_id);

  /* Reverse-DNS ids carry the vendor, not the application: keep the last
   * component. Ids without dots are already the application's name. */
  char *tail = last_component(stem);
  g_free(stem);

  GPtrArray *words = app_id_words(tail);
  g_free(tail);

  GString *name = g_string_new(NULL);
  for (guint i = 0; i < words->len; i++) {
    const char *word = g_ptr_array_index(words, i);
    gboolean all_lower = TRUE;
    for (const char *p = word; *p != '\0'; p++) {
      if (g_ascii_isupper(*p)) {
        all_lower = FALSE;
        break;
      }
    }

    if (i > 0)
      g_string_append_c(name, ' ');

    if (all_lower && g_ascii_islower(word[0])) {
      g_string_append_c(name, g_ascii_toupper(word[0]));
      g_string_append(name, word + 1);
    } else {
      g_string_append(name, word);
    }
  }

  g_ptr_array_unref(words);

  char *result = g_string_free(name, FALSE);
  if (*result == '\0') {
    g_free(result);
    return NULL;
  }
  return result;
}

char *lambdock_app_name_for(const char *app_id) {
  const LambdockDesktopEntry *entry = lambdock_desktop_entry_lookup(app_id);
  if (entry && entry->name && *entry->name != '\0')
    return g_strdup(entry->name);

  char *derived = lambdock_app_id_to_name(app_id);
  if (derived)
    return derived;

  return (app_id && *app_id != '\0') ? g_strdup(app_id) : NULL;
}

char *lambdock_app_icon_for(const char *app_id) {
  const LambdockDesktopEntry *entry = lambdock_desktop_entry_lookup(app_id);
  if (entry && entry->icon && *entry->icon != '\0')
    return g_strdup(entry->icon);

  return (app_id && *app_id != '\0') ? g_strdup(app_id) : NULL;
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
