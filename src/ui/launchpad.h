// lambdock -- hyper-hackable desktop dock for Wayland
//
// SPDX-License-Identifier: MIT
// Copyright © Josep Bigorra <jjbigorra@gmail.com>

#ifndef LAUNCHPAD_H
#define LAUNCHPAD_H

#include "../lambdock.h"
#include "desktop_entry.h"

/* =========================================================================
 * THE LAUNCHPAD
 * =========================================================================
 * The full-screen application grid: its model, search, tiles, copy menu and key
 * movement.
 *
 * There is one launchpad (there is one screen), so the entry points reach the
 * single instance; everything else takes the model as a parameter.
 */

/* Icon size of a launchpad tile, in pixels. */
#define LAUNCHPAD_ICON_SIZE 64
#define LAUNCHPAD_ICON_SIZE_MIN 32
#define LAUNCHPAD_ICON_SIZE_MAX 160

/* Tiles per row. 0 lets the launcher choose from the screen width, which is
 * what a display of any size wants; a fixed number is for people who want the
 * same grid everywhere. */
#define LAUNCHPAD_COLUMNS_AUTO 0
#define LAUNCHPAD_COLUMNS_MIN 3
#define LAUNCHPAD_COLUMNS_MAX 14

/* Bounds for the width-derived column count: fewer than four wastes the screen,
 * more than ten makes the labels unreadable. */
#define LAUNCHPAD_AUTO_COLUMNS_MIN 4
#define LAUNCHPAD_AUTO_COLUMNS_MAX 10

/* Columns used when the screen width cannot be read yet (first allocation,
 * headless test). */
#define LAUNCHPAD_FALLBACK_COLUMNS 8

/**
 * lambdock_launchpad_show:
 * @state: Any dock state; its window provides the GtkApplication and monitor.
 *
 * Shows the launchpad: a full screen layer-shell overlay with a search field and
 * a grid of every installed application, macOS Launchpad style. The window is
 * created on first use and reused afterwards; the application list is rescanned
 * on every show so freshly installed apps appear. The tile size, column count
 * and frosted background are resolved from @state's dock settings on each show.
 *
 * Escape, the close button and a click on the backdrop all hide it.
 */
void lambdock_launchpad_show(LambdockState *state);

/**
 * lambdock_launchpad_hide:
 * Hides the launchpad overlay if it is currently shown.
 */
void lambdock_launchpad_hide(void);

/**
 * lambdock_launchpad_is_open:
 * Returns: TRUE when the launchpad overlay is mapped.
 */
gboolean lambdock_launchpad_is_open(void);

/**
 * lambdock_launchpad_shutdown:
 * Destroys the launchpad window (used when the last dock goes away).
 */
void lambdock_launchpad_shutdown(void);

/**
 * lambdock_launchpad_grid_new:
 * @entries: Applications to show (borrowed).
 *
 * Builds the searchable application grid widget. Exposed so the layout can be
 * exercised without a display server.
 *
 * Returns: (transfer none): a GtkFlowBox.
 */
GtkWidget *lambdock_launchpad_grid_new(GPtrArray *entries);

/**
 * lambdock_launchpad_entry_matches:
 * @entry: an installed application.
 * @query: what the user typed, NULL or "" for "everything".
 *
 * The search filter, on its own: matches case-insensitively against the
 * application's name and its command line, so both "fire" and "--private" find
 * the entry that names Firefox and runs `firefox --private-window`. Exposed so
 * the filter can be tested without a display.
 *
 * Returns: TRUE when @entry belongs in the grid.
 */
gboolean lambdock_launchpad_entry_matches(const LambdockDesktopEntry *entry,
                                          const char *query);

/**
 * lambdock_launchpad_columns_for:
 * @usable_width: pixels available between the grid's margins.
 * @icon_size: the configured tile icon size.
 * @configured_columns: requested tiles per row, 0 for "as many as fit".
 *
 * Tiles grow with the icon size, so the column count is derived from the width
 * instead of fixed; a configured count is an upper bound, because asking for 14
 * tiles per row on a laptop must not push the last ones off the screen. Exposed
 * so this arithmetic can be tested without a display.
 *
 * Returns: how many tiles fit in a row.
 */
int lambdock_launchpad_columns_for(int usable_width, int icon_size,
                                   int configured_columns);

/**
 * lambdock_launchpad_tile_new:
 * @entry: (nullable): the installed application to show.
 * @icon_size: pixel size of its icon.
 *
 * Builds one tile of the grid: the icon, the name, the accessibility text and
 * the tooltip. An application that is installed more than once (see
 * lambdock_desktop_entries_scan()) also gets a context menu, opened with the
 * right mouse button, the Menu key or Shift+F10, that lists the copies so the
 * user picks the one to start instead of the scan picking for them. Tiles of an
 * application with a single copy have no menu. Exposed so the tiles can be
 * exercised without a display server.
 *
 * Returns: (transfer none) (nullable): a GtkButton, or NULL for a NULL entry.
 */
GtkWidget *lambdock_launchpad_tile_new(const LambdockDesktopEntry *entry,
                                       int icon_size);

#endif // LAUNCHPAD_H
