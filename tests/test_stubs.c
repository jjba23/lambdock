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

#include "../src/dock_window.h"
#include "../src/lambdock.h"
#include <glib.h>

static GList *g_test_active_states = NULL;

GList *get_active_dock_states(void) { return g_test_active_states; }

void unregister_dock_state(LambdockState *state) {
  g_test_active_states = g_list_remove(g_test_active_states, state);
}
