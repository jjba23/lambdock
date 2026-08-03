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
 * get_autohide_config:
 *
 * Calls Guile Scheme function 'get-dock-auto-hide?' to read user auto-hide
 * preferences.
 *
 * Returns: TRUE if auto-hide is enabled in Scheme configuration; FALSE
 * otherwise.
 */
gboolean get_autohide_config(void);

/**
 * get_position_config:
 *
 * Calls Guile Scheme function 'get-dock-position' to retrieve screen attachment
 * edge.
 *
 * Returns: GtkLayerShellEdge enum representing screen edge placement.
 */
GtkLayerShellEdge get_position_config(void);

/**
 * populate_dock_items:
 * @state: Pointer to main LambdockState structure.
 *
 * Evaluates configured dock launchers from embedded Guile Scheme engine and
 * populates dock items into the GTK box container and tracking lists.
 */
void populate_dock_items(LambdockState *state);

#endif // GUILE_BRIDGE_H
