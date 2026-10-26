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

#ifndef GUILE_BRIDGE_H
#define GUILE_BRIDGE_H

#include "lambdock.h"

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
 * Stores the view mode ('grid', 'list' or 'fan') of a stack item.
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

/**
 * reset_dock_preferences:
 * Forgets every GUI override of @config_path. The caller is responsible for
 * reloading the settings file afterwards.
 */
void reset_dock_preferences(const char *config_path);

#endif // GUILE_BRIDGE_H
