// lambdock -- hyper-hackable desktop dock for Wayland
//
// SPDX-License-Identifier: MIT
// Copyright © Josep Bigorra <jjbigorra@gmail.com>

#ifndef ICON_RESOLVER_H
#define ICON_RESOLVER_H

#include <gtk/gtk.h>

/* =========================================================================
 * ICON LOOKUP
 * =========================================================================
 * The fallback pipeline: GResource path, absolute path, icon theme, XDG
 * pixmaps, a -symbolic variant, then the generic icon. Also sets up the icon
 * theme.
 */

/**
 * lambdock_icon_search_paths_build:
 * @system_data_dirs: (nullable): NULL-terminated XDG data directories (what
 *   g_get_system_data_dirs() returns), or NULL for none.
 * @home: (nullable): the user's home directory, or NULL for none.
 *
 * The icon directories the dock should search, in the order it should search
 * them: each data directory's =icons=, then =$HOME/.local/share/icons=, then the
 * legacy =$HOME/.icons=. Only directories that exist are listed, and none is
 * listed twice.
 *
 * Split out of setup_icon_theme() so the list can be built from explicit inputs
 * and checked without a display or the process environment.
 *
 * Returns: (transfer full): a NULL-terminated array of owned paths.
 */
char **lambdock_icon_search_paths_build(const char *const *system_data_dirs,
                                        const char *home);

/**
 * lambdock_icon_search_paths_new:
 * The same list for the running process (the real XDG data dirs and $HOME).
 *
 * Returns: (transfer full): a NULL-terminated array of owned paths.
 */
char **lambdock_icon_search_paths_new(void);

/**
 * setup_icon_theme:
 *
 * Registers system and user XDG icon search paths (~/.local/share/icons, etc.)
 * and detects GNOME desktop icon preferences.
 */
void setup_icon_theme(void);

/**
 * create_image_for_icon:
 * @theme: Active GtkIconTheme instance.
 * @icon: Icon name, GResource URI, file path, or symbolic identifier.
 * @icon_size: Pixel size for rendering the image widget.
 *
 * Executes a 6-stage fallback pipeline to resolve and construct a GtkImage
 * widget.
 *
 * Returns: Newly created GtkWidget image.
 */
GtkWidget *create_image_for_icon(GtkIconTheme *theme, const char *icon,
                                 int icon_size);

#endif // ICON_RESOLVER_H
