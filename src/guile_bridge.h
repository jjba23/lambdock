// lambdock -- hyper-hackable desktop dock for Wayland
//
// SPDX-License-Identifier: MIT
// Copyright © Josep Bigorra <jjbigorra@gmail.com>

#ifndef GUILE_BRIDGE_H
#define GUILE_BRIDGE_H

#include "lambdock.h"

/* =========================================================================
 * THE GUILE BRIDGE
 * =========================================================================
 * The only place C reads the Scheme configuration. Every get_*_config_for() is
 * one settings path in, one plain value out; the item builders turn <dock-item>
 * records into rows of DOCK_ITEM_BUILDERS rather than into switches.
 *
 * Contract: the public surface is plain C -- no SCM in any signature here.
 * Texts are copied out with g_strdup() and owned by the caller (g_free); the
 * defaults live in the .c file next to the getter they belong to, so C works
 * with no Scheme at all (tests, a settings file that failed to load).
 */

/**
 * setup_guile_load_path:
 *
 * Appends directory search paths to Guile's internal '%load-path' Scheme list,
 * allowing the embedded evaluator to resolve Scheme script modules.
 */
void setup_guile_load_path(void);

/**
 * Path-Aware Configuration Getters:
 * Inspect Scheme configuration records mapped to specific file paths.
 */
gboolean get_autohide_config_for(const char *config_path);
GtkLayerShellEdge get_position_config_for(const char *config_path);
char *get_theme_config_for(const char *config_path);
char *get_monitor_config_for(const char *config_path);
gboolean get_repl_config_for(const char *config_path);
gboolean get_show_when_clear_config_for(const char *config_path);
gboolean get_hover_animate_config_for(const char *config_path);
int get_icon_size_config_for(const char *config_path);

/**
 * get_item_spacing_config_for:
 * Reads `dock-item-spacing` (px between dock items, clamped to 0..64).
 */
int get_item_spacing_config_for(const char *config_path);

/**
 * get_capsule_padding_config_for:
 * Reads `dock-capsule-padding` (px between the capsule's ends and the icons,
 * clamped to 0..96). Falls back to CAPSULE_MAIN_AXIS_PADDING_PX.
 */
int get_capsule_padding_config_for(const char *config_path);

/**
 * populate_dock_items_for:
 * @state: Pointer to main LambdockState structure.
 *
 * Evaluates configured dock launchers from Scheme for state->config_path
 * and populates dock items into the GTK box container and tracking lists.
 */
void populate_dock_items_for(LambdockState *state);

/**
 * get_config_path_c:
 *
 * Calls Guile Scheme function 'get-config-path' to resolve XDG config path.
 * Returns: Heap allocated string containing path to default settings.scm.
 */
char *get_config_path_c(void);

/**
 * load_dock_config_scm:
 * Loads and registers a specific Scheme configuration file.
 */
void load_dock_config_scm(const char *file_path);

/**
 * reload_guile_config:
 *
 * Re-invokes Guile Scheme procedure 'init-dock!' to re-parse primary settings.
 */
void reload_guile_config(void);

/**
 * is_app_excluded:
 * @app_id: Application identifier or string to check against exclusion list.
 * @config_path: Path to configuration record context.
 *
 * Invokes Guile function 'get-dock-exclude-apps' for specific config_path.
 *
 * Returns: TRUE if app is in exclusion list; FALSE otherwise.
 */
gboolean is_app_excluded(const char *app_id, const char *config_path);

gboolean get_track_windows_config_for(const char *config_path);

/**
 * get_taskbar_config_for:
 * Reads the `dock-mode` Scheme symbol for a config path and returns TRUE when it
 * is 'taskbar (full-width bar) rather than 'dock (floating capsule).
 */
gboolean get_taskbar_config_for(const char *config_path);

/**
 * get_show_window_titles_config_for:
 * Returns TRUE when window title labels should be shown on running app items
 * (Windows XP-style taskbar buttons).
 */
gboolean get_show_window_titles_config_for(const char *config_path);

/**
 * get_shelf_config_for:
 * @config_path: Path to the dock's configuration record (may be NULL).
 * @out: Destination for the resolved shelf parameters (must not be NULL).
 *
 * Reads every `dock-shelf-*` / `dock-reflection*` value for @config_path and
 * fills @out with clamped values. Missing getters, missing settings and
 * non-numeric values fall back to lambdock_shelf_params_default(), so an old
 * settings file keeps the classic flat dock.
 */
void get_shelf_config_for(const char *config_path, LambdockShelfParams *out);

/**
 * get_launchpad_icon_size_config_for:
 * Reads `dock-launchpad-icon-size` (pixel size of a launchpad tile icon),
 * clamped to the range the grid can lay out.
 */
int get_launchpad_icon_size_config_for(const char *config_path);

/**
 * get_launchpad_columns_config_for:
 * Reads `dock-launchpad-columns`: 0 means "fit the screen", 3..14 is a fixed
 * number of tiles per row.
 */
int get_launchpad_columns_config_for(const char *config_path);

/**
 * get_launchpad_blur_config_for:
 * Returns TRUE when the launchpad should ask for a frosted, translucent
 * backdrop instead of its solid one.
 */
gboolean get_launchpad_blur_config_for(const char *config_path);

/**
 * get_launchpad_close_on_right_config_for:
 * Returns TRUE when the launchpad's close button belongs at the right end of its
 * header. The default, and anything unreadable, is the left end.
 */
gboolean get_launchpad_close_on_right_config_for(const char *config_path);

/**
 * get_reduce_motion_config_for:
 * Returns TRUE when the dock itself asks for reduced motion; the desktop's
 * `gtk-enable-animations` setting is checked separately, so either one is
 * enough to stop the slide and fisheye animations.
 */
gboolean get_reduce_motion_config_for(const char *config_path);

/* =========================================================================
 * DOCK LAYOUT OVERRIDES (drag and drop persistence)
 * =========================================================================
 * The dock is freely rearrangeable at runtime; these calls hand the edits to
 * the Scheme `(lambdock layout)` module, which writes the override file and
 * replays it on top of the configured items on the next load.
 */

/**
 * dock_layout_add_item:
 * Remembers an item dragged onto the dock. @arg is the exec line for app items
 * and the folder path for stacks; @view is the stack view mode (may be NULL).
 */
void dock_layout_add_item(const char *config_path, const char *kind,
                          const char *name, const char *icon, const char *arg,
                          const char *view);

/**
 * dock_layout_add_preset_item:
 * Appends one of the dock's ready-made widgets to @config_path's dock: @preset
 * is a preset name ("trash", "clock", "ram", "cpu", "cpu-temp", "battery", or
 * any launcher preset). This is what the Preferences window's "Add an item" row
 * calls; the item is stored in the same layout override file as the ones
 * dragged in, so it can be reordered and removed the same way. A NULL or empty
 * @config_path targets the default dock's layout file.
 */
void dock_layout_add_preset_item(const char *config_path, const char *preset);

/**
 * dock_layout_remove_item:
 * Remembers that @key (see dock_item_key()) was dragged out of the dock.
 */
void dock_layout_remove_item(const char *config_path, const char *key);

/**
 * dock_layout_set_order:
 * Stores the left-to-right order of the dock as a GList of item keys.
 */
void dock_layout_set_order(const char *config_path, GList *keys);

/**
 * dock_layout_set_view:
 * Stores the view mode ('grid' or 'list') of a stack item.
 */
void dock_layout_set_view(const char *config_path, const char *key,
                          const char *view);

/* =========================================================================
 * GUI PREFERENCES (written by the GTK Preferences window)
 * =========================================================================
 * Each call applies one setting to the dock identified by @config_path through
 * the ordinary `set-dock-*!' setter and remembers it in the GUI preference
 * override file, so a change made in the preferences window survives a restart
 * without touching (or being lost to) the generated settings file.
 *
 * @key is the settings-file name, e.g. "dock-icon-size" or "dock-auto-hide?".
 * A NULL or empty @config_path targets the default dock and is not persisted.
 */

void set_dock_setting_bool(const char *key, const char *config_path,
                           gboolean value);
void set_dock_setting_int(const char *key, const char *config_path, int value);
void set_dock_setting_double(const char *key, const char *config_path,
                             double value);
void set_dock_setting_symbol(const char *key, const char *config_path,
                             const char *value);

/* =========================================================================
 * ITEM POLLERS (the Scheme side of a dynamic item)
 * =========================================================================
 * A dynamic item's reading comes from a Scheme procedure. The item stores it as
 * an opaque handle and asks this bridge what it says, so no SCM value and no
 * scm_* call lives outside this module. The handle must be installed with
 * lambdock_poll_fn_protect() (which keeps the procedure alive across GC) and
 * released with lambdock_poll_fn_release() when the item goes away.
 */

/**
 * LambdockReading:
 * @kind: what the poller returned this time.
 * @number: the value when @kind is LAMBDOCK_READING_NUMBER.
 * @text: (nullable): owned text when @kind is LAMBDOCK_READING_TEXT.
 *
 * One poll's answer, as plain C.
 */
typedef enum {
  LAMBDOCK_READING_NONE = 0,
  LAMBDOCK_READING_NUMBER,
  LAMBDOCK_READING_TEXT,
} LambdockReadingKind;

typedef struct {
  LambdockReadingKind kind;
  double number;
  char *text;
} LambdockReading;

/**
 * lambdock_poll_fn_protect:
 * @procedure: the Scheme procedure the loader produced, as the bridge sees it.
 *
 * Returns: (nullable): a handle to keep, or NULL when @procedure is not a
 * procedure. A handle is protected from the GC until it is released.
 */
void *lambdock_poll_fn_protect(void *procedure);

/**
 * lambdock_poll_fn_is_callable:
 * Returns: TRUE when @handle still names a callable procedure.
 */
gboolean lambdock_poll_fn_is_callable(void *poll_fn);

/**
 * lambdock_poll_fn_call:
 * @poll_fn: a handle from lambdock_poll_fn_protect().
 * @out: (out): the reading.
 *
 * Calls the procedure once, turning a Scheme error into LAMBDOCK_READING_NONE
 * (a missing reading, not a dead daemon).
 *
 * Returns: TRUE when @out holds a reading; the caller owns @out->text.
 */
gboolean lambdock_poll_fn_call(void *poll_fn, LambdockReading *out);

/**
 * lambdock_poll_fn_release:
 * @poll_fn: (nullable): a handle from lambdock_poll_fn_protect().
 *
 * Drops the GC protection. Safe on NULL.
 */
void lambdock_poll_fn_release(void *poll_fn);

/**
 * reset_dock_preferences:
 * Forgets every GUI override of @config_path. The caller is responsible for
 * reloading the settings file afterwards.
 */
void reset_dock_preferences(const char *config_path);

#endif // GUILE_BRIDGE_H
