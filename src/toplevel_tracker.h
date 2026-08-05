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

#ifndef TOPLEVEL_TRACKER_H
#define TOPLEVEL_TRACKER_H

#include "dock_window.h"

/**
 * init_toplevel_tracker:
 * @state: Pointer to LambdockState context.
 *
 * Connects to low-level Wayland display server registry and binds listeners to
 * track open application window instances dynamically.
 */
void init_toplevel_tracker(LambdockState *state);

/**
 * activate_toplevel_window:
 * @handle: Pointer to target Wayland foreign toplevel handle struct.
 *
 * Requests compositor window activation to bring target window to user focus.
 */
void activate_toplevel_window(struct zwlr_foreign_toplevel_handle_v1 *handle);

/**
 * close_toplevel_window:
 * @handle: Pointer to target Wayland foreign toplevel handle struct.
 *
 * Requests graceful window closure via Wayland foreign toplevel protocols.
 */
void close_toplevel_window(struct zwlr_foreign_toplevel_handle_v1 *handle);

/**
 * sync_window_trackers:
 * @state: Pointer to LambdockState context.
 *
 * Re-binds active Wayland window trackers to current dock items after
 * configuration reloading or reset.
 */
void sync_window_trackers(LambdockState *state);

void clear_window_tracker_dock_references(LambdockState *state);

#endif // TOPLEVEL_TRACKER_H
