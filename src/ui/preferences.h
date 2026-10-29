// lambdock -- hyper-hackable desktop dock for Wayland
//
// SPDX-License-Identifier: MIT
// Copyright © Josep Bigorra <jjbigorra@gmail.com>

#ifndef PREFERENCES_H
#define PREFERENCES_H

#include "../lambdock.h"

/* =========================================================================
 * THE SETTINGS WINDOW
 * =========================================================================
 * A declarative table of settings rows mapped onto Scheme setters, with
 * debounced apply, plus a registry of open windows keyed by settings path.
 *
 * The row tables are the source of truth: a new option is a row, not a new
 * callback.
 */

/**
 * lambdock_preferences_show:
 * @config_path: Settings file of the dock to edit (NULL or "" for the default).
 *
 * Opens (or re-presents) the GTK preferences window for one dock instance.
 * Every control applies its value to the running dock and remembers it in the
 * GUI preference override file, so the generated settings file stays untouched.
 */
void lambdock_preferences_show(const char *config_path);

/**
 * on_preferences_action:
 * @action: Triggered GSimpleAction object.
 * @parameter: Action target: the settings path as a string (may be absent).
 * @user_data: Pointer to the running GtkApplication.
 *
 * Action handler behind the dock's "Preferences..." menu entry. The string
 * target is what makes the menu open the pane for *that* dock when several
 * settings files are in use.
 */
void on_preferences_action(GSimpleAction *action, GVariant *parameter,
                           gpointer user_data);

/**
 * lambdock_preferences_shutdown:
 * Destroys every open preferences window. Called on application teardown.
 */
void lambdock_preferences_shutdown(void);

/**
 * lambdock_preferences_content_for:
 * @config_path: Settings file of the dock (NULL or "" for the default).
 *
 * Builds the preferences form without creating a window. This is the widget
 * tree the window wraps; it is exposed so the controls' "edit -> setter" wiring
 * can be exercised in a unit test.
 *
 * Returns: (transfer none): a new GtkBox owned by the caller.
 */
GtkWidget *lambdock_preferences_content_for(const char *config_path);

#endif // PREFERENCES_H
