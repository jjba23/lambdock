// lambdock -- hyper-hackable desktop dock for Wayland
//
// SPDX-License-Identifier: MIT
// Copyright © Josep Bigorra <jjbigorra@gmail.com>

#include "dock_registry.h"

/* The registry's whole state. There is exactly one instance because "the set of
 * live docks" is process-wide, but naming it keeps the owner explicit: this
 * translation unit, and nothing else, can change the set of docks. */
typedef struct {
  GList *docks; /* LambdockState*, in registration order; not owned */
} DockRegistry;

/* Nothing outside this file may keep a pointer to the list, which is what makes
 * "who can change the set of docks?" answerable. */
static DockRegistry g_registry;

void dock_registry_add(LambdockState *state) {
  if (!state || dock_registry_contains(state))
    return;

  g_registry.docks = g_list_append(g_registry.docks, state);
}

void dock_registry_remove(LambdockState *state) {
  if (!state)
    return;

  g_registry.docks = g_list_remove(g_registry.docks, state);
}

gboolean dock_registry_contains(const LambdockState *state) {
  if (!state)
    return FALSE;

  return g_list_find(g_registry.docks, (gpointer)state) != NULL;
}

guint dock_registry_count(void) { return g_list_length(g_registry.docks); }

gboolean dock_registry_is_empty(void) { return g_registry.docks == NULL; }

GPtrArray *dock_registry_snapshot(void) {
  GPtrArray *states = g_ptr_array_sized_new(dock_registry_count());

  for (GList *node = g_registry.docks; node != NULL; node = node->next)
    g_ptr_array_add(states, node->data);

  return states;
}

void dock_registry_foreach(DockVisitor visit, gpointer user_data) {
  if (!visit)
    return;

  /* Walked by hand rather than with g_ptr_array_foreach(): the GLib callback
   * type is `void (*)(gpointer, gpointer)`, and hiding the cast would defeat
   * the point of DockVisitor making the element type explicit. */
  g_autoptr(GPtrArray) states = dock_registry_snapshot();
  for (guint i = 0; i < states->len; i++)
    visit(g_ptr_array_index(states, i), user_data);
}

void dock_registry_clear(void) {
  g_list_free(g_registry.docks);
  g_registry.docks = NULL;
}
