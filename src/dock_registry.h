// lambdock -- hyper-hackable desktop dock for Wayland
//
// SPDX-License-Identifier: MIT
// Copyright © Josep Bigorra <jjbigorra@gmail.com>

#ifndef LAMBDOCK_DOCK_REGISTRY_H
#define LAMBDOCK_DOCK_REGISTRY_H

#include "lambdock.h"

/*
 * =========================================================================
 * THE DOCK REGISTRY
 * =========================================================================
 * lambdock can show one dock per monitor per settings file, so several
 * `LambdockState` instances live at once. Everything that has to act on "all
 * the docks" -- the toplevel tracker broadcasting a window title, the
 * preferences window redrawing, a monitor hotplug -- used to reach into a
 * process-wide `GList` and walk it directly, which meant any caller could
 * mutate the list mid-iteration and dangling states were a use-after-free
 * waiting to happen.
 *
 * This module is the only owner of that list. Callers get a snapshot (an array
 * of borrowed pointers) instead, so destroying a dock inside the loop cannot
 * corrupt the iteration. The registry itself is not thread safe: like the rest
 * of the GTK code it is only touched from the main thread.
 */

/**
 * dock_registry_add:
 * Registers @state as an active dock. Adding the same state twice is a no-op,
 * so a double register cannot produce duplicate broadcasts.
 */
void dock_registry_add(LambdockState *state);

/**
 * dock_registry_remove:
 * Removes @state if it is registered. Removing a state that is not registered,
 * or NULL, is a no-op, so teardown paths can call it unconditionally.
 */
void dock_registry_remove(LambdockState *state);

/**
 * dock_registry_contains:
 * Returns: TRUE when @state is the registered dock.
 */
gboolean dock_registry_contains(const LambdockState *state);

/**
 * dock_registry_count / dock_registry_is_empty:
 * Returns: how many docks are registered right now.
 */
guint dock_registry_count(void);
gboolean dock_registry_is_empty(void);

/**
 * dock_registry_snapshot:
 * Returns: (transfer container) (element-type LambdockState*): a new array of
 * borrowed states, in registration order. The caller owns the array (unref it
 * with g_ptr_array_unref()) but not the states: a state that is destroyed while
 * the caller walks the snapshot is its own responsibility, exactly as with any
 * other borrowed pointer. Iterating a snapshot is nevertheless safe against the
 * registry changing underneath the loop.
 */
GPtrArray *dock_registry_snapshot(void);

/**
 * dock_registry_foreach:
 * @visit: called once per registered dock, in registration order.
 * @user_data: passed through to @visit.
 *
 * Walks a snapshot, so @visit may register or remove docks (including its own
 * @state) without disturbing the walk.
 */
void dock_registry_foreach(void (*visit)(LambdockState *state, gpointer data),
                           gpointer user_data);

/**
 * dock_registry_clear:
 * Forgets every registered dock without touching the states themselves. Used
 * by the shutdown path and by tests.
 */
void dock_registry_clear(void);

#endif // LAMBDOCK_DOCK_REGISTRY_H
