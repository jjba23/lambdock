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

/* * INCLUDE GUARD: Prevents double-inclusion errors during compilation.
 * Ensures the declarations below are processed only once per C compilation
 * unit.
 */
#ifndef GUILE_BRIDGE_H
#define GUILE_BRIDGE_H

/* Include core project definitions and global state structures */
#include "lambdock.h"

/* =========================================================================
 * GUILE SCHEME INTEGRATION PROTOTYPES
 * ========================================================================= */

/**
 * setup_guile_load_path:
 *
 * Configures GNU Guile's internal '%load-path' global variable so the embedded
 * Scheme interpreter can locate Lambdock's .scm script modules on disk.
 */
void setup_guile_load_path(void);

/**
 * get_autohide_config:
 *
 * Queries the embedded Guile Scheme engine to fetch the current auto-hide
 * preference setting set in Scheme configuration.
 *
 * Returns: 'TRUE' if auto-hide is enabled in configuration, 'FALSE' otherwise.
 */
gboolean get_autohide_config(void);

/**
 * populate_dock_items:
 * @box: Pointer to the GTK 4 Box container widget (GtkWidget *) where items
 * live.
 *
 * Calls Guile Scheme initialization procedures to parse configured dock
 * launchers, builds the corresponding GTK 4 widgets (buttons, separators), and
 * appends them sequentially into the provided box layout container.
 */
void populate_dock_items(GtkWidget *box);

#endif // GUILE_BRIDGE_H
