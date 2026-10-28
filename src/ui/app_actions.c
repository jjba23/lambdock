// lambdock -- hyper-hackable desktop dock for Wayland
//
// SPDX-License-Identifier: MIT
// Copyright © Josep Bigorra <jjbigorra@gmail.com>

#include "app_actions.h"
#include "../logger.h"
#include "desktop_entry.h"
#include <gio/gio.h>
#include <glib/gi18n.h>
#include <string.h>

/* The MPRIS spelling of a media player, from the freedesktop.org MPRIS D-Bus
 * interface specification. */
#define MPRIS_PREFIX "org.mpris.MediaPlayer2."
#define MPRIS_OBJECT_PATH "/org/mpris/MediaPlayer2"
#define MPRIS_PLAYER_INTERFACE "org.mpris.MediaPlayer2.Player"
#define MPRIS_CALL_TIMEOUT_MS 500

/* ============================================================================
 * APPLICATION KINDS
 * ============================================================================
 */

/* Names that say "browser" when the desktop entry has no categories, or when
 * the item was written by hand in Scheme and never had a desktop file. Matched
 * as substrings against the app id, the display name and the command line. */
static const char *const browser_words[] = {
    "firefox", "librewolf", "waterfox", "chromium", "chrome",
    "brave",   "vivaldi",   "opera",     "epiphany", "falkon",
    "qutebrowser", "midori", "zen-browser", NULL};

/* The same for players. Kept to applications that really speak MPRIS, because
 * the playback controls are only offered when a player is found on the bus. */
static const char *const music_words[] = {
    "spotify",   "amberol",   "rhythmbox", "lollypop", "elisa",
    "audacious", "clementine", "strawberry", "quodlibet", "banshee",
    "deadbeef",  "tauon",     "feishin",   "sonixd",   "tidal",
    "deezer",    "youtube-music", "mpv",   "vlc",      "celluloid",
    NULL};

static gboolean categories_have(const char *categories, const char *category) {
  if (!categories || *categories == '\0' || !category || *category == '\0')
    return FALSE;

  char **parts = g_strsplit(categories, ";", -1);
  gboolean found = FALSE;
  for (int i = 0; parts[i] != NULL && !found; i++) {
    if (*parts[i] != '\0' && g_ascii_strcasecmp(parts[i], category) == 0)
      found = TRUE;
  }
  g_strfreev(parts);
  return found;
}

/* Whether @text mentions one of @words, ignoring case. */
static gboolean text_mentions(const char *text, const char *const *words) {
  if (!text || *text == '\0')
    return FALSE;

  char *lower = g_ascii_strdown(text, -1);
  gboolean found = FALSE;
  for (int i = 0; words[i] != NULL && !found; i++) {
    if (strstr(lower, words[i]) != NULL)
      found = TRUE;
  }
  g_free(lower);
  return found;
}

static gboolean app_mentions(const char *app_id, const char *name,
                             const char *exec, const char *const *words) {
  return text_mentions(app_id, words) || text_mentions(name, words) ||
         text_mentions(exec, words);
}

LambdockAppKind lambdock_app_kind(const char *app_id, const char *name,
                                  const char *exec, const char *categories) {
  /* Categories first: they are the application's own claim about what it is,
   * and they cover every browser and player that ships a desktop file. */
  if (categories_have(categories, "WebBrowser"))
    return LAMBDOCK_APP_KIND_BROWSER;
  if (categories_have(categories, "Music"))
    return LAMBDOCK_APP_KIND_MUSIC;
  if (categories_have(categories, "Audio") &&
      categories_have(categories, "Player"))
    return LAMBDOCK_APP_KIND_MUSIC;

  /* Then the names. A browser check comes first so a browser whose command
   * line happens to mention media (Firefox's own media session, a browser
   * packaged with a player) is still a browser. */
  if (app_mentions(app_id, name, exec, browser_words))
    return LAMBDOCK_APP_KIND_BROWSER;
  if (app_mentions(app_id, name, exec, music_words))
    return LAMBDOCK_APP_KIND_MUSIC;

  return LAMBDOCK_APP_KIND_NONE;
}

/* ============================================================================
 * BROWSER WINDOW COMMANDS
 * ============================================================================
 */

typedef enum {
  BROWSER_FAMILY_UNKNOWN = 0,
  BROWSER_FAMILY_FIREFOX,
  BROWSER_FAMILY_CHROMIUM,
  BROWSER_FAMILY_EDGE,
  BROWSER_FAMILY_OPERA,
} BrowserFamily;

/* Splits an Exec= line into an argv, dropping the freedesktop.org field codes
 * first so both a desktop entry ("firefox %u") and a Scheme item ("firefox
 * %U") describe the same launch. */
static gboolean split_exec(const char *exec, char ***argv, int *argc) {
  *argv = NULL;
  *argc = 0;
  if (!exec || *exec == '\0')
    return FALSE;

  char *clean = lambdock_exec_strip_field_codes(exec);
  gboolean ok = clean && *clean != '\0' &&
                g_shell_parse_argv(clean, argc, argv, NULL) && *argc > 0;
  if (!ok) {
    g_strfreev(*argv);
    *argv = NULL;
    *argc = 0;
  }
  g_free(clean);
  return ok;
}

static BrowserFamily browser_family(const char *program) {
  char *base = g_path_get_basename(program);
  char *lower = g_ascii_strdown(base, -1);
  g_free(base);

  BrowserFamily family = BROWSER_FAMILY_UNKNOWN;
  if (strstr(lower, "firefox") || strstr(lower, "librewolf") ||
      strstr(lower, "waterfox"))
    family = BROWSER_FAMILY_FIREFOX;
  else if (strstr(lower, "chromium") || strstr(lower, "chrome") ||
           strstr(lower, "brave") || strstr(lower, "vivaldi"))
    family = BROWSER_FAMILY_CHROMIUM;
  else if (strstr(lower, "edge"))
    family = BROWSER_FAMILY_EDGE;
  else if (strstr(lower, "opera"))
    family = BROWSER_FAMILY_OPERA;

  g_free(lower);
  return family;
}

/* Characters that need no quoting for g_shell_parse_argv() to give the same
 * word back. */
static gboolean word_needs_quotes(const char *word) {
  if (!word || *word == '\0')
    return TRUE;

  for (const char *p = word; *p != '\0'; p++) {
    if (!g_ascii_isalnum(*p) && strchr("-._/:=+,@%", *p) == NULL)
      return TRUE;
  }
  return FALSE;
}

/* The flag that opens a window of @private_mode, or NULL when this browser has
 * no such mode (or no known flag for it). */
static const char *browser_flag(BrowserFamily family, gboolean private_mode) {
  switch (family) {
  case BROWSER_FAMILY_FIREFOX:
    return private_mode ? "--private-window" : "--new-window";
  case BROWSER_FAMILY_CHROMIUM:
    return private_mode ? "--incognito" : "--new-window";
  case BROWSER_FAMILY_EDGE:
    return private_mode ? "--inprivate" : "--new-window";
  case BROWSER_FAMILY_OPERA:
    return private_mode ? "--private" : "--new-window";
  case BROWSER_FAMILY_UNKNOWN:
    /* Still worth launching: most browsers open another window when started
     * again. There is no private mode to promise. */
    return NULL;
  }
  return NULL;
}

char *lambdock_app_action_command(const char *exec, LambdockAppAction action) {
  gboolean private_mode;
  if (action == LAMBDOCK_APP_ACTION_NEW_WINDOW)
    private_mode = FALSE;
  else if (action == LAMBDOCK_APP_ACTION_PRIVATE_WINDOW)
    private_mode = TRUE;
  else
    return NULL;

  char **argv = NULL;
  int argc = 0;
  if (!split_exec(exec, &argv, &argc))
    return NULL;

  BrowserFamily family = browser_family(argv[0]);
  const char *flag = browser_flag(family, private_mode);

  /* An unknown browser has no private mode the dock can promise, and a browser
   * that was never asked for a private window is launched as it is. */
  if (private_mode && !flag) {
    g_strfreev(argv);
    return NULL;
  }

  GString *command = g_string_new(NULL);
  for (int i = 0; i < argc; i++) {
    /* Quote a word only when the shell would split it: a command line that
     * reads like the desktop entry it came from is easier to follow in the
     * log when a launch fails. */
    char *quoted = word_needs_quotes(argv[i]) ? g_shell_quote(argv[i])
                                              : g_strdup(argv[i]);
    if (i > 0)
      g_string_append_c(command, ' ');
    g_string_append(command, quoted);
    g_free(quoted);
  }
  if (flag)
    g_string_append_printf(command, " %s", flag);

  g_strfreev(argv);
  return g_string_free(command, FALSE);
}

/* ============================================================================
 * ACTIONS
 * ============================================================================
 */

gboolean lambdock_app_action_is_music(LambdockAppAction action) {
  return action == LAMBDOCK_APP_ACTION_PLAY_PAUSE ||
         action == LAMBDOCK_APP_ACTION_PREVIOUS ||
         action == LAMBDOCK_APP_ACTION_NEXT || action == LAMBDOCK_APP_ACTION_STOP;
}

const char *lambdock_app_action_label(LambdockAppAction action) {
  switch (action) {
  case LAMBDOCK_APP_ACTION_NEW_WINDOW:
    return _("New Window");
  case LAMBDOCK_APP_ACTION_PRIVATE_WINDOW:
    return _("New Private Window");
  case LAMBDOCK_APP_ACTION_PLAY_PAUSE:
    return _("Play/Pause");
  case LAMBDOCK_APP_ACTION_PREVIOUS:
    return _("Previous Track");
  case LAMBDOCK_APP_ACTION_NEXT:
    return _("Next Track");
  case LAMBDOCK_APP_ACTION_STOP:
    return _("Stop Playback");
  }
  return "";
}

GArray *lambdock_app_actions(const char *app_id, const char *name,
                             const char *exec, const char *categories,
                             const char *music_bus) {
  GArray *actions = g_array_new(FALSE, FALSE, sizeof(guint));

  switch (lambdock_app_kind(app_id, name, exec, categories)) {
  case LAMBDOCK_APP_KIND_BROWSER:
    if (exec && *exec != '\0') {
      guint new_window = LAMBDOCK_APP_ACTION_NEW_WINDOW;
      g_array_append_val(actions, new_window);

      char *private_command =
          lambdock_app_action_command(exec, LAMBDOCK_APP_ACTION_PRIVATE_WINDOW);
      if (private_command) {
        guint private_window = LAMBDOCK_APP_ACTION_PRIVATE_WINDOW;
        g_array_append_val(actions, private_window);
        g_free(private_command);
      }
    }
    break;

  case LAMBDOCK_APP_KIND_MUSIC:
    /* With no player on the bus these would be buttons that do nothing, which
     * is worse than an item whose menu is only its windows and launcher. */
    if (music_bus && *music_bus != '\0') {
      const guint playback[] = {
          LAMBDOCK_APP_ACTION_PLAY_PAUSE, LAMBDOCK_APP_ACTION_PREVIOUS,
          LAMBDOCK_APP_ACTION_NEXT, LAMBDOCK_APP_ACTION_STOP};
      for (guint i = 0; i < G_N_ELEMENTS(playback); i++)
        g_array_append_val(actions, playback[i]);
    }
    break;

  case LAMBDOCK_APP_KIND_NONE:
    break;
  }

  return actions;
}

/* ============================================================================
 * MPRIS PLAYBACK
 * ============================================================================
 */

/* Lower case letters and digits only, so "org.mpris.MediaPlayer2.Rhythmbox",
 * "io.bassi.Amberol" and "YouTube Music" all compare as words. */
static void append_normalized(GString *out, const char *value) {
  if (!value)
    return;

  for (const char *p = value; *p != '\0'; p = g_utf8_next_char(p)) {
    gunichar c = g_utf8_get_char(p);
    if (c < 128 && g_ascii_isalnum((gchar)c))
      g_string_append_c(out, g_ascii_tolower((gchar)c));
  }
}

static char *normalize(const char *value) {
  GString *out = g_string_new(NULL);
  append_normalized(out, value);
  return g_string_free(out, FALSE);
}

/* Whether the two normalized names name the same application. Equality is the
 * common case ("spotify" and "spotify"); a prefix covers the players that
 * append something of their own ("amberol" and "amberolplayer"). */
static gboolean tokens_match(const char *a, const char *b) {
  if (!a || !b || *a == '\0' || *b == '\0')
    return FALSE;
  if (g_strcmp0(a, b) == 0)
    return TRUE;

  gsize shortest = MIN(strlen(a), strlen(b));
  if (shortest < 4)
    return FALSE;
  return g_str_has_prefix(a, b) || g_str_has_prefix(b, a);
}

/* Whether the bus name's instance ("spotify", "chromium.instance1234")
 * belongs to this application. */
static gboolean player_instance_matches(const char *instance, const char *app_id,
                                        const char *name) {
  if (!instance || *instance == '\0')
    return FALSE;

  /* The first component is the player's own name ("spotify"), the last one is
   * often the reverse-DNS tail ("io.bassi.Amberol" -> "Amberol"), and the
   * "...instance<N>" suffix that Firefox and Chromium append is neither. */
  char *instance_head = g_strdup(instance);
  char *dot = strchr(instance_head, '.');
  if (dot)
    *dot = '\0';

  const char *instance_tail = g_strrstr(instance, ".");
  instance_tail = instance_tail ? instance_tail + 1 : instance;

  const char *app_leaf = NULL;
  if (app_id) {
    const char *dot_in_id = g_strrstr(app_id, ".");
    app_leaf = dot_in_id ? dot_in_id + 1 : app_id;
  }

  char *head_norm = normalize(instance_head);
  char *full_norm = normalize(instance);
  char *id_norm = normalize(app_id);
  char *leaf_norm = normalize(app_leaf);
  char *name_norm = normalize(name);
  char *tail_norm = normalize(instance_tail);

  gboolean matched = tokens_match(head_norm, id_norm) ||
                     tokens_match(head_norm, leaf_norm) ||
                     tokens_match(head_norm, name_norm) ||
                     tokens_match(tail_norm, id_norm) ||
                     tokens_match(tail_norm, leaf_norm) ||
                     tokens_match(tail_norm, name_norm) ||
                     tokens_match(full_norm, id_norm) ||
                     tokens_match(full_norm, leaf_norm);

  g_free(tail_norm);
  g_free(head_norm);
  g_free(full_norm);
  g_free(id_norm);
  g_free(leaf_norm);
  g_free(name_norm);
  g_free(instance_head);
  return matched;
}

char *lambdock_app_music_player_find(const char *app_id, const char *name) {
  if ((!app_id || *app_id == '\0') && (!name || *name == '\0'))
    return NULL;

  GError *error = NULL;
  GDBusConnection *connection =
      g_bus_get_sync(G_BUS_TYPE_SESSION, NULL, &error);
  if (!connection) {
    g_clear_error(&error);
    return NULL;
  }

  GVariant *reply = g_dbus_connection_call_sync(
      connection, "org.freedesktop.DBus", "/org/freedesktop/DBus",
      "org.freedesktop.DBus", "ListNames", NULL, G_VARIANT_TYPE("(as)"),
      G_DBUS_CALL_FLAGS_NONE, MPRIS_CALL_TIMEOUT_MS, NULL, &error);
  if (!reply) {
    g_clear_error(&error);
    g_object_unref(connection);
    return NULL;
  }

  char **names = NULL;
  g_variant_get(reply, "(^as)", &names);

  char *found = NULL;
  for (int i = 0; names[i] != NULL && !found; i++) {
    if (!g_str_has_prefix(names[i], MPRIS_PREFIX))
      continue;

    const char *instance = names[i] + strlen(MPRIS_PREFIX);
    if (player_instance_matches(instance, app_id, name))
      found = g_strdup(names[i]);
  }

  g_strfreev(names);
  g_variant_unref(reply);
  g_object_unref(connection);
  return found;
}

static const char *mpris_method(LambdockAppAction action) {
  switch (action) {
  case LAMBDOCK_APP_ACTION_PLAY_PAUSE:
    return "PlayPause";
  case LAMBDOCK_APP_ACTION_PREVIOUS:
    return "Previous";
  case LAMBDOCK_APP_ACTION_NEXT:
    return "Next";
  case LAMBDOCK_APP_ACTION_STOP:
    return "Stop";
  case LAMBDOCK_APP_ACTION_NEW_WINDOW:
  case LAMBDOCK_APP_ACTION_PRIVATE_WINDOW:
    return NULL;
  }
  return NULL;
}

static void on_mpris_call_done(GObject *source, GAsyncResult *result,
                               gpointer user_data) {
  char *method = (char *)user_data;
  GError *error = NULL;
  GVariant *reply =
      g_dbus_connection_call_finish(G_DBUS_CONNECTION(source), result, &error);

  if (error) {
    LOG_C_WARN("UI", "MPRIS %s failed: %s", method, error->message);
    g_clear_error(&error);
  }
  if (reply)
    g_variant_unref(reply);
  g_free(method);
}

void lambdock_app_music_invoke(const char *bus_name, LambdockAppAction action) {
  const char *method = mpris_method(action);
  if (!bus_name || *bus_name == '\0' || !method)
    return;

  GError *error = NULL;
  GDBusConnection *connection =
      g_bus_get_sync(G_BUS_TYPE_SESSION, NULL, &error);
  if (!connection) {
    LOG_C_WARN("UI", "No session bus for MPRIS: %s", error->message);
    g_clear_error(&error);
    return;
  }

  g_dbus_connection_call(connection, bus_name, MPRIS_OBJECT_PATH,
                         MPRIS_PLAYER_INTERFACE, method, NULL, NULL,
                         G_DBUS_CALL_FLAGS_NONE, MPRIS_CALL_TIMEOUT_MS, NULL,
                         on_mpris_call_done, g_strdup(method));
  g_object_unref(connection);
}
