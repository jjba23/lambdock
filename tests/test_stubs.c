// lambdock -- hyper-hackable desktop dock for Wayland
//
// SPDX-License-Identifier: MIT
// Copyright © Josep Bigorra <jjbigorra@gmail.com>

/* The test binaries do not compile main.c, which is where unregister_dock_state
 * lives in the real binary. Tests never register docks, so this double has
 * nothing to do -- and, crucially, it must not pull the registry into binaries
 * that link only a slice of the application sources. */

#include "../src/dock_window.h"
#include "../src/lambdock.h"

void unregister_dock_state(LambdockState *state) { (void)state; }
