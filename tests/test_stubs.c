// lambdock -- hyper-hackable desktop dock for Wayland
//
// SPDX-License-Identifier: MIT
// Copyright © Josep Bigorra <jjbigorra@gmail.com>

#include "../src/dock_window.h"
#include "../src/lambdock.h"
#include <glib.h>

static GList *g_test_active_states = NULL;

GList *get_active_dock_states(void) { return g_test_active_states; }

void unregister_dock_state(LambdockState *state) {
  g_test_active_states = g_list_remove(g_test_active_states, state);
}
