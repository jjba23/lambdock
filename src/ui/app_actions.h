// lambdock -- hyper-hackable desktop dock for Wayland
//
// SPDX-License-Identifier: MIT
// Copyright © Josep Bigorra <jjbigorra@gmail.com>

#ifndef APP_ACTIONS_H
#define APP_ACTIONS_H

#include <glib.h>

/* =========================================================================
 * APPLICATION ACTIONS
 * =========================================================================
 * What a right-click can do to an application: launch a new instance, close its
 * windows, and build the command lines those actions run (MPRIS playback
 * control included).
 */

/**
 * LambdockAppKind:
 * @LAMBDOCK_APP_KIND_NONE: an application with no kind-specific menu entries.
 * @LAMBDOCK_APP_KIND_BROWSER: a web browser (Firefox, Chrome, Chromium, Brave,
 *   Vivaldi, Opera, Edge...). Gets window commands in its context menu.
 * @LAMBDOCK_APP_KIND_MUSIC: a music (or media) player (Spotify, Amberol,
 *   Rhythmbox, Lollypop, Elisa...). Gets playback controls, driven over MPRIS.
 *
 * What an application *is*, as far as the dock's context menu is concerned.
 * The kind is read from the desktop entry's Categories= when there is one, and
 * from the app id, name and command line otherwise, so an entry with no
 * categories (a hand written .desktop file, a flatpak export) is still
 * recognised.
 */
typedef enum {
  LAMBDOCK_APP_KIND_NONE = 0,
  LAMBDOCK_APP_KIND_BROWSER,
  LAMBDOCK_APP_KIND_MUSIC,
} LambdockAppKind;

/**
 * LambdockAppAction:
 * @LAMBDOCK_APP_ACTION_NEW_WINDOW: open another window of the application.
 * @LAMBDOCK_APP_ACTION_PRIVATE_WINDOW: open a private/incognito window.
 * @LAMBDOCK_APP_ACTION_PLAY_PAUSE: toggle playback (MPRIS PlayPause).
 * @LAMBDOCK_APP_ACTION_PREVIOUS: previous track (MPRIS Previous).
 * @LAMBDOCK_APP_ACTION_NEXT: next track (MPRIS Next).
 * @LAMBDOCK_APP_ACTION_STOP: stop playback (MPRIS Stop).
 *
 * One entry of an application's own section in the dock's right-click menu.
 */
typedef enum {
  LAMBDOCK_APP_ACTION_NEW_WINDOW = 0,
  LAMBDOCK_APP_ACTION_PRIVATE_WINDOW,
  LAMBDOCK_APP_ACTION_PLAY_PAUSE,
  LAMBDOCK_APP_ACTION_PREVIOUS,
  LAMBDOCK_APP_ACTION_NEXT,
  LAMBDOCK_APP_ACTION_STOP,
} LambdockAppAction;

/**
 * lambdock_app_kind:
 * @app_id: (nullable): application id (or desktop entry stem).
 * @name: (nullable): display name, e.g. "Firefox Web Browser".
 * @exec: (nullable): command line of the application.
 * @categories: (nullable): Categories= of its desktop entry.
 *
 * Classifies an application. Categories win when they say something (the
 * freedesktop.org vocabulary has "WebBrowser", "Music", "Audio" and "Player");
 * the id, name and command line are the fallback for entries that carry none.
 *
 * Returns: the application's kind.
 */
LambdockAppKind lambdock_app_kind(const char *app_id, const char *name,
                                  const char *exec, const char *categories);

/**
 * lambdock_app_actions:
 * @app_id: (nullable): application id (or desktop entry stem).
 * @name: (nullable): display name.
 * @exec: (nullable): command line of the application.
 * @categories: (nullable): Categories= of its desktop entry.
 * @music_bus: (nullable): MPRIS bus name of the application's running player,
 *   as returned by lambdock_app_music_player_find().
 *
 * The kind-specific actions to offer for this application, in menu order. A
 * browser only gets them when it has a command line to launch; a player only
 * when @music_bus names a running player, so a pinned but closed player does
 * not grow controls that would do nothing. A browser whose family the dock does
 * not know (or one that has no private mode in its command line) gets the new
 * window action but not the private one.
 *
 * Returns: (transfer full): a GArray of LambdockAppAction (guint) values.
 */
GArray *lambdock_app_actions(const char *app_id, const char *name,
                             const char *exec, const char *categories,
                             const char *music_bus);

/**
 * lambdock_app_action_label:
 * @action: an action returned by lambdock_app_actions().
 *
 * The translated label of @action, for the menu button and its accessible
 * name.
 */
const char *lambdock_app_action_label(LambdockAppAction action);

/**
 * lambdock_app_action_command:
 * @exec: (nullable): command line of a browser.
 * @action: a browser action (new or private window).
 *
 * Builds the command line that carries out @action for @exec: the application's
 * own command line, cleaned of desktop-entry field codes, with the flag its
 * family expects (Firefox's --private-window, Chrome's --incognito, Edge's
 * --inprivate, Opera's --private). Field codes are dropped before the launch,
 * so a Scheme entry written as "firefox %U" works like the desktop file does.
 *
 * Returns: (transfer full) (nullable): the command line for
 *   g_spawn_command_line_async(), or NULL when @action is not a browser action
 *   or the browser has no such mode.
 */
char *lambdock_app_action_command(const char *exec, LambdockAppAction action);

/**
 * lambdock_app_action_is_music:
 * @action: an action.
 *
 * Whether @action is carried out over MPRIS rather than by launching a command.
 */
gboolean lambdock_app_action_is_music(LambdockAppAction action);

/**
 * lambdock_app_music_player_find:
 * @app_id: (nullable): application id (or desktop entry stem).
 * @name: (nullable): display name.
 *
 * Finds the MPRIS player belonging to this application on the session bus. The
 * bus names are "org.mpris.MediaPlayer2.<instance>" ("...spotify",
 * "...amberol", and for flatpak builds whatever the sandbox registers), so the
 * instance is compared with the app id, its last component and the display
 * name, ignoring case and punctuation. Bus names ending in ".instance<N>"
 * (Firefox, Chromium) compare their first component only.
 *
 * Returns: (transfer full) (nullable): the bus name of the player to drive, or
 *   NULL when the application has none running.
 */
char *lambdock_app_music_player_find(const char *app_id, const char *name);

/**
 * lambdock_app_music_invoke:
 * @bus_name: (nullable): bus name from lambdock_app_music_player_find().
 * @action: a music action.
 *
 * Sends @action to the player asynchronously; failures are logged, never
 * shown to the caller, because the menu has already been dismissed. A player
 * that quits between the menu opening and the click simply does not respond.
 */
void lambdock_app_music_invoke(const char *bus_name, LambdockAppAction action);

#endif // APP_ACTIONS_H
