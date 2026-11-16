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

/* Same shape, one layer up: the Rust archive reaches back into the C dock for
 * this call, and the linker resolves the whole archive slice a Rust module
 * shares, not only the function the test asked for. A weak double keeps every
 * binary that links a slice resolvable; the real definition in dock_window.c
 * wins wherever it is compiled in, and a test that never shows a dock never
 * calls this anyway. */
__attribute__((weak)) void update_dock_dimensions(LambdockState *state) {
  (void)state;
}
