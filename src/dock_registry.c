// lambdock -- hyper-hackable desktop dock for Wayland
//
// SPDX-License-Identifier: MIT
// Copyright © Josep Bigorra <jjbigorra@gmail.com>

#include "dock_registry.h"

/* The one and only owner of the active-dock list. Nothing outside this
 * translation unit may keep a pointer to it, which is what makes "who can
 * change the set of docks?" answerable. */
static GList *g_registered_docks = NULL;

void dock_registry_add(LambdockState *state) {
  if (!state || dock_registry_contains(state))
    return;

  g_registered_docks = g_list_append(g_registered_docks, state);
}

void dock_registry_remove(LambdockState *state) {
  if (!state)
    return;

  g_registered_docks = g_list_remove(g_registered_docks, state);
}

gboolean dock_registry_contains(const LambdockState *state) {
  if (!state)
    return FALSE;

  return g_list_find(g_registered_docks, (gpointer)state) != NULL;
}

guint dock_registry_count(void) {
  return g_list_length(g_registered_docks);
}

gboolean dock_registry_is_empty(void) { return g_registered_docks == NULL; }

GPtrArray *dock_registry_snapshot(void) {
  GPtrArray *states = g_ptr_array_new();

  for (GList *l = g_registered_docks; l != NULL; l = l->next)
    g_ptr_array_add(states, l->data);

  return states;
}

void dock_registry_foreach(void (*visit)(LambdockState *state, gpointer data),
                           gpointer user_data) {
  if (!visit)
    return;

  GPtrArray *states = dock_registry_snapshot();
  for (guint i = 0; i < states->len; i++)
    visit((LambdockState *)g_ptr_array_index(states, i), user_data);
  g_ptr_array_unref(states);
}

void dock_registry_clear(void) {
  g_list_free(g_registered_docks);
  g_registered_docks = NULL;
}
