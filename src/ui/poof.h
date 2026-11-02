// lambdock -- hyper-hackable desktop dock for Wayland
//
// SPDX-License-Identifier: MIT
// Copyright © Josep Bigorra <jjbigorra@gmail.com>

#ifndef POOF_H
#define POOF_H

#include "../lambdock.h"

/* =========================================================================
 * THE POOF
 * =========================================================================
 * Dragging an icon out of the dock used to make it vanish. On the real 10.9
 * dock it burst into a puff of smoke, and Dock.app ships that animation as
 * `poof.png`: five 128x128 frames, one icon's worth of smoke.
 *
 * This is the widget side of it. The burst itself is arithmetic in
 * `core/poof.h`; here it is a frame-clock animation painted into a widget the
 * size of the icon it replaces.
 */

/**
 * lambdock_poof_take_slot:
 * @item: The item that is about to leave the dock.
 *
 * Puts a poof widget in the item's place, sized to the icon it replaces, and
 * starts the burst. Call it immediately before removing @item: the puff then
 * stays where the icon was, keeps the dock's width for the length of the
 * animation, removes itself when it is done and re-measures the dock so the
 * remaining icons close the gap.
 *
 * Returns: TRUE when a burst was started. FALSE when there is nothing to
 * animate -- an unrealised dock, or an item that is not in the dock's box -- in
 * which case the caller should just remove the item.
 */
gboolean lambdock_poof_take_slot(DockItem *item);

#endif // POOF_H
