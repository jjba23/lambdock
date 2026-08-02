// lambdock

// Copyright © Josep Bigorra <jjbigorra@gmail.com>

// lambdock is free software : you can redistribute it and / or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.

// lambdock is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.See the
// GNU General Public License for more details.

// You should have received a copy of the GNU General Public License
// along with lambdock.If not, see <https://www.gnu.org/licenses/>.

/* Standard preprocessor directive preventing duplicate header inclusions */
#pragma once

/* Include dock window header to access the LambdockState struct declaration */
#include "dock_window.h"

/* =========================================================================
 * WAYLAND WINDOW TRACKER INTERFACE
 * ========================================================================= */

/**
 * init_toplevel_tracker:
 * @state: Pointer to the core application state (LambdockState).
 *
 * Connects to the Wayland display server registry and registers listeners for
 * foreign toplevel window protocols. This enables Lambdock to dynamically
 * track open applications, display running app icons, and focus or close
 * windows.
 */
void init_toplevel_tracker(LambdockState *state);
