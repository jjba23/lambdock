// lambdock -- hyper-hackable desktop dock for Wayland
//
// SPDX-License-Identifier: MIT
// Copyright © Josep Bigorra <jjbigorra@gmail.com>

#include "../src/ui/app_actions.h"
#include <gio/gio.h>
#include <glib.h>
#include <string.h>

/* ============================================================================
 * APPLICATION KINDS
 * ============================================================================
 */

static void test_kind_from_categories(void) {
  /* The freedesktop.org vocabulary is the first source of truth. */
  g_assert_cmpint(lambdock_app_kind("org.example.Web", "Some Browser",
                                    "browser", "Network;WebBrowser;"),
                  ==, LAMBDOCK_APP_KIND_BROWSER);
  g_assert_cmpint(lambdock_app_kind("org.example.Player", "Some Player",
                                    "player", "Audio;Music;Player;"),
                  ==, LAMBDOCK_APP_KIND_MUSIC);
  g_assert_cmpint(lambdock_app_kind("org.example.Audio", "Audio Thing",
                                    "thing", "Audio;Player;"),
                  ==, LAMBDOCK_APP_KIND_MUSIC);
  /* Case does not matter, and an empty category does not count. */
  g_assert_cmpint(lambdock_app_kind("x", "x", "x", "webbrowser;"),
                  ==, LAMBDOCK_APP_KIND_BROWSER);
  g_assert_cmpint(lambdock_app_kind("x", "x", "x", "Utility;"),
                  ==, LAMBDOCK_APP_KIND_NONE);
}

static void test_kind_from_names(void) {
  /* No categories at all: the id, name and command line still classify. */
  g_assert_cmpint(lambdock_app_kind("firefox", "Firefox", "firefox %u", NULL),
                  ==, LAMBDOCK_APP_KIND_BROWSER);
  g_assert_cmpint(lambdock_app_kind("google-chrome", "Google Chrome",
                                    "/usr/bin/google-chrome-stable %U", NULL),
                  ==, LAMBDOCK_APP_KIND_BROWSER);
  g_assert_cmpint(lambdock_app_kind("brave-browser", "Brave", "brave", NULL),
                  ==, LAMBDOCK_APP_KIND_BROWSER);
  g_assert_cmpint(lambdock_app_kind("spotify", "Spotify", "spotify", NULL),
                  ==, LAMBDOCK_APP_KIND_MUSIC);
  g_assert_cmpint(lambdock_app_kind("io.bassi.Amberol", "Amberol",
                                    "amberol", NULL),
                  ==, LAMBDOCK_APP_KIND_MUSIC);
  g_assert_cmpint(lambdock_app_kind("org.gnome.Rhythmbox3", "Rhythmbox",
                                    "rhythmbox", NULL),
                  ==, LAMBDOCK_APP_KIND_MUSIC);
}

static void test_kind_none(void) {
  g_assert_cmpint(lambdock_app_kind("org.gnome.Calculator", "Calculator",
                                    "gnome-calculator", "Utility;Calculator;"),
                  ==, LAMBDOCK_APP_KIND_NONE);
  g_assert_cmpint(lambdock_app_kind(NULL, NULL, NULL, NULL), ==,
                  LAMBDOCK_APP_KIND_NONE);
  /* A browser's command line is the browser's, even when a name is shared
   * with something else. */
  g_assert_cmpint(lambdock_app_kind("zen-browser", "Zen", "zen-browser", NULL),
                  ==, LAMBDOCK_APP_KIND_BROWSER);
}

/* ============================================================================
 * BROWSER COMMANDS
 * ============================================================================
 */

static void test_browser_commands_firefox(void) {
  char *command =
      lambdock_app_action_command("firefox", LAMBDOCK_APP_ACTION_NEW_WINDOW);
  g_assert_cmpstr(command, ==, "firefox --new-window");
  g_free(command);

  command =
      lambdock_app_action_command("firefox", LAMBDOCK_APP_ACTION_PRIVATE_WINDOW);
  g_assert_cmpstr(command, ==, "firefox --private-window");
  g_free(command);

  /* A Scheme item may still carry the desktop entry's field codes. */
  command = lambdock_app_action_command("firefox %u",
                                        LAMBDOCK_APP_ACTION_PRIVATE_WINDOW);
  g_assert_cmpstr(command, ==, "firefox --private-window");
  g_free(command);
}

static void test_browser_commands_chromium_families(void) {
  char *command = lambdock_app_action_command(
      "google-chrome-stable", LAMBDOCK_APP_ACTION_PRIVATE_WINDOW);
  g_assert_cmpstr(command, ==, "google-chrome-stable --incognito");
  g_free(command);

  command = lambdock_app_action_command("chromium",
                                        LAMBDOCK_APP_ACTION_PRIVATE_WINDOW);
  g_assert_cmpstr(command, ==, "chromium --incognito");
  g_free(command);

  command = lambdock_app_action_command("brave-browser",
                                        LAMBDOCK_APP_ACTION_PRIVATE_WINDOW);
  g_assert_cmpstr(command, ==, "brave-browser --incognito");
  g_free(command);

  command = lambdock_app_action_command("microsoft-edge-stable",
                                        LAMBDOCK_APP_ACTION_PRIVATE_WINDOW);
  g_assert_cmpstr(command, ==, "microsoft-edge-stable --inprivate");
  g_free(command);

  command =
      lambdock_app_action_command("opera", LAMBDOCK_APP_ACTION_PRIVATE_WINDOW);
  g_assert_cmpstr(command, ==, "opera --private");
  g_free(command);
}

static void test_browser_commands_keep_arguments(void) {
  /* Every argument survives, quoted, and the flag goes last. */
  char *command = lambdock_app_action_command(
      "google-chrome-stable --profile-directory=Default %U",
      LAMBDOCK_APP_ACTION_NEW_WINDOW);
  g_assert_cmpstr(command, ==,
                  "google-chrome-stable --profile-directory=Default "
                  "--new-window");
  g_free(command);

  /* An argument with a space stays one word, quoted the way it came in. */
  command = lambdock_app_action_command(
      "firefox --profile '/home/joe/My Profile'",
      LAMBDOCK_APP_ACTION_NEW_WINDOW);
  g_assert_cmpstr(command, ==,
                  "firefox --profile '/home/joe/My Profile' --new-window");
  g_free(command);
}

static void test_browser_commands_unknown_family(void) {
  /* A browser the dock has no flags for still gets its window command, but is
   * not promised a private mode. */
  char *command =
      lambdock_app_action_command("epiphany", LAMBDOCK_APP_ACTION_NEW_WINDOW);
  g_assert_cmpstr(command, ==, "epiphany");
  g_free(command);

  g_assert_null(lambdock_app_action_command(
      "epiphany", LAMBDOCK_APP_ACTION_PRIVATE_WINDOW));

  /* Non-browser actions are not commands. */
  g_assert_null(lambdock_app_action_command(
      "firefox", LAMBDOCK_APP_ACTION_PLAY_PAUSE));

  /* No command line, no command. */
  g_assert_null(lambdock_app_action_command(NULL,
                                            LAMBDOCK_APP_ACTION_NEW_WINDOW));
  g_assert_null(lambdock_app_action_command("",
                                            LAMBDOCK_APP_ACTION_NEW_WINDOW));
}

/* ============================================================================
 * ACTION LISTS
 * ============================================================================
 */

static void test_browser_actions(void) {
  GArray *actions = lambdock_app_actions("firefox", "Firefox", "firefox %u",
                                         "Network;WebBrowser;", NULL);
  g_assert_cmpuint(actions->len, ==, 2);
  g_assert_cmpint(g_array_index(actions, guint, 0), ==,
                  LAMBDOCK_APP_ACTION_NEW_WINDOW);
  g_assert_cmpint(g_array_index(actions, guint, 1), ==,
                  LAMBDOCK_APP_ACTION_PRIVATE_WINDOW);
  g_array_free(actions, TRUE);

  /* A browser with an unknown family offers the window command only. */
  actions =
      lambdock_app_actions("epiphany", "Web", "epiphany", NULL, NULL);
  g_assert_cmpuint(actions->len, ==, 1);
  g_assert_cmpint(g_array_index(actions, guint, 0), ==,
                  LAMBDOCK_APP_ACTION_NEW_WINDOW);
  g_array_free(actions, TRUE);

  /* Nothing to launch, nothing to offer. */
  actions = lambdock_app_actions("firefox", "Firefox", NULL, NULL, NULL);
  g_assert_cmpuint(actions->len, ==, 0);
  g_array_free(actions, TRUE);
}

static void test_music_actions(void) {
  /* Controls only exist while the player is on the bus. */
  GArray *actions = lambdock_app_actions("spotify", "Spotify", "spotify", NULL,
                                         NULL);
  g_assert_cmpuint(actions->len, ==, 0);
  g_array_free(actions, TRUE);

  actions = lambdock_app_actions("spotify", "Spotify", "spotify", NULL,
                                 "org.mpris.MediaPlayer2.spotify");
  g_assert_cmpuint(actions->len, ==, 4);
  g_assert_cmpint(g_array_index(actions, guint, 0), ==,
                  LAMBDOCK_APP_ACTION_PLAY_PAUSE);
  g_assert_cmpint(g_array_index(actions, guint, 1), ==,
                  LAMBDOCK_APP_ACTION_PREVIOUS);
  g_assert_cmpint(g_array_index(actions, guint, 2), ==,
                  LAMBDOCK_APP_ACTION_NEXT);
  g_assert_cmpint(g_array_index(actions, guint, 3), ==,
                  LAMBDOCK_APP_ACTION_STOP);
  g_array_free(actions, TRUE);

  /* An application of no kind gets nothing, whatever bus it names. */
  actions = lambdock_app_actions("org.gnome.Calculator", "Calculator",
                                 "gnome-calculator", "Utility;Calculator;",
                                 "org.mpris.MediaPlayer2.spotify");
  g_assert_cmpuint(actions->len, ==, 0);
  g_array_free(actions, TRUE);
}

static void test_action_labels(void) {
  const LambdockAppAction actions[] = {
      LAMBDOCK_APP_ACTION_NEW_WINDOW, LAMBDOCK_APP_ACTION_PRIVATE_WINDOW,
      LAMBDOCK_APP_ACTION_PLAY_PAUSE, LAMBDOCK_APP_ACTION_PREVIOUS,
      LAMBDOCK_APP_ACTION_NEXT,       LAMBDOCK_APP_ACTION_STOP};

  for (guint i = 0; i < G_N_ELEMENTS(actions); i++) {
    const char *label = lambdock_app_action_label(actions[i]);
    g_assert_nonnull(label);
    g_assert_cmpuint(strlen(label), >, 0);
    for (guint j = i + 1; j < G_N_ELEMENTS(actions); j++)
      g_assert_cmpstr(label, !=, lambdock_app_action_label(actions[j]));
  }

  g_assert_true(lambdock_app_action_is_music(LAMBDOCK_APP_ACTION_PLAY_PAUSE));
  g_assert_true(lambdock_app_action_is_music(LAMBDOCK_APP_ACTION_NEXT));
  g_assert_false(lambdock_app_action_is_music(LAMBDOCK_APP_ACTION_NEW_WINDOW));
  g_assert_false(
      lambdock_app_action_is_music(LAMBDOCK_APP_ACTION_PRIVATE_WINDOW));
}

/* ============================================================================
 * MPRIS
 * ============================================================================
 */

typedef struct {
  char *last_method;
  guint calls;
} MockPlayer;

static const char mock_player_xml[] =
    "<node>"
    "  <interface name='org.mpris.MediaPlayer2.Player'>"
    "    <method name='PlayPause'/>"
    "    <method name='Previous'/>"
    "    <method name='Next'/>"
    "    <method name='Stop'/>"
    "  </interface>"
    "</node>";

static void on_mock_player_method(GDBusConnection *connection,
                                  const char *sender, const char *object_path,
                                  const char *interface_name,
                                  const char *method_name,
                                  GVariant *parameters,
                                  GDBusMethodInvocation *invocation,
                                  gpointer user_data) {
  (void)connection;
  (void)sender;
  (void)object_path;
  (void)interface_name;
  (void)parameters;

  MockPlayer *player = (MockPlayer *)user_data;
  g_free(player->last_method);
  player->last_method = g_strdup(method_name);
  player->calls++;
  g_dbus_method_invocation_return_value(invocation, NULL);
}

/* Spins the default context until the mock saw @calls calls or two seconds
 * went by. The dock's calls are asynchronous, so the reply and the incoming
 * message both need the loop. */
static gboolean pump_until_calls(MockPlayer *player, guint calls) {
  gint64 deadline = g_get_monotonic_time() + 2 * G_TIME_SPAN_SECOND;
  while (player->calls < calls && g_get_monotonic_time() < deadline) {
    g_main_context_iteration(NULL, FALSE);
    g_usleep(1000);
  }
  return player->calls >= calls;
}

static void test_music_player_find_and_invoke(void) {
  if (!g_find_program_in_path("dbus-daemon")) {
    g_test_skip("dbus-daemon is not installed");
    return;
  }

  GTestDBus *bus = g_test_dbus_new(G_TEST_DBUS_NONE);
  g_test_dbus_up(bus);

  GError *error = NULL;
  GDBusConnection *connection =
      g_bus_get_sync(G_BUS_TYPE_SESSION, NULL, &error);
  g_assert_no_error(error);
  g_assert_nonnull(connection);

  MockPlayer player = {0};
  GDBusNodeInfo *node = g_dbus_node_info_new_for_xml(mock_player_xml, &error);
  g_assert_no_error(error);

  const GDBusInterfaceVTable vtable = {on_mock_player_method, NULL, NULL, {0}};
  guint registration = g_dbus_connection_register_object(
      connection, "/org/mpris/MediaPlayer2",
      g_dbus_node_info_lookup_interface(node,
                                        "org.mpris.MediaPlayer2.Player"),
      &vtable, &player, NULL, &error);
  g_assert_no_error(error);
  g_assert_cmpuint(registration, >, 0);

  GVariant *reply = g_dbus_connection_call_sync(
      connection, "org.freedesktop.DBus", "/org/freedesktop/DBus",
      "org.freedesktop.DBus", "RequestName",
      g_variant_new("(su)", "org.mpris.MediaPlayer2.spotify", 0),
      G_VARIANT_TYPE("(u)"), G_DBUS_CALL_FLAGS_NONE, 2000, NULL, &error);
  g_assert_no_error(error);
  g_variant_unref(reply);

  /* The player is found from the app id, from the desktop entry's name, and
   * from the flatpak-style reverse-DNS id; unrelated apps find nothing. */
  char *found = lambdock_app_music_player_find("spotify", "Spotify");
  g_assert_cmpstr(found, ==, "org.mpris.MediaPlayer2.spotify");
  g_free(found);

  found = lambdock_app_music_player_find("com.spotify.Client", "Spotify");
  g_assert_cmpstr(found, ==, "org.mpris.MediaPlayer2.spotify");
  g_free(found);

  g_assert_null(lambdock_app_music_player_find("io.bassi.Amberol", "Amberol"));
  g_assert_null(lambdock_app_music_player_find(NULL, NULL));

  /* And its controls reach it. */
  found = lambdock_app_music_player_find("spotify", "Spotify");
  lambdock_app_music_invoke(found, LAMBDOCK_APP_ACTION_NEXT);
  g_assert_true(pump_until_calls(&player, 1));
  g_assert_cmpstr(player.last_method, ==, "Next");

  lambdock_app_music_invoke(found, LAMBDOCK_APP_ACTION_PLAY_PAUSE);
  g_assert_true(pump_until_calls(&player, 2));
  g_assert_cmpstr(player.last_method, ==, "PlayPause");
  g_free(found);

  g_free(player.last_method);
  g_dbus_connection_unregister_object(connection, registration);
  g_dbus_node_info_unref(node);
  g_object_unref(connection);
  g_test_dbus_down(bus);
  g_object_unref(bus);
}

int main(int argc, char **argv) {
  g_test_init(&argc, &argv, NULL);

  g_test_add_func("/app_actions/kind/categories", test_kind_from_categories);
  g_test_add_func("/app_actions/kind/names", test_kind_from_names);
  g_test_add_func("/app_actions/kind/none", test_kind_none);

  g_test_add_func("/app_actions/browser/firefox",
                  test_browser_commands_firefox);
  g_test_add_func("/app_actions/browser/chromium",
                  test_browser_commands_chromium_families);
  g_test_add_func("/app_actions/browser/arguments",
                  test_browser_commands_keep_arguments);
  g_test_add_func("/app_actions/browser/unknown",
                  test_browser_commands_unknown_family);

  g_test_add_func("/app_actions/actions/browser", test_browser_actions);
  g_test_add_func("/app_actions/actions/music", test_music_actions);
  g_test_add_func("/app_actions/actions/labels", test_action_labels);

  g_test_add_func("/app_actions/mpris/find_and_invoke",
                  test_music_player_find_and_invoke);

  return g_test_run();
}
