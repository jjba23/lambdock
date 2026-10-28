// lambdock -- hyper-hackable desktop dock for Wayland
//
// SPDX-License-Identifier: MIT
// Copyright © Josep Bigorra <jjbigorra@gmail.com>

/* Unit tests for the dock registry: the only owner of the active dock list. */

#include "../src/dock_registry.h"

#include <glib.h>

static LambdockState *fake_state(void) { return g_new0(LambdockState, 1); }

static void reset_registry(void) {
  dock_registry_clear();
  g_assert_true(dock_registry_is_empty());
  g_assert_cmpuint(dock_registry_count(), ==, 0);
}

static void test_add_and_remove(void) {
  reset_registry();

  LambdockState *a = fake_state();
  LambdockState *b = fake_state();

  dock_registry_add(a);
  dock_registry_add(b);
  g_assert_cmpuint(dock_registry_count(), ==, 2);
  g_assert_true(dock_registry_contains(a));
  g_assert_true(dock_registry_contains(b));

  /* Registering the same dock twice is a no-op, so a repeated register cannot
   * make every broadcast act twice on one dock. */
  dock_registry_add(a);
  g_assert_cmpuint(dock_registry_count(), ==, 2);

  /* NULL is ignored rather than crashing. */
  dock_registry_add(NULL);
  g_assert_cmpuint(dock_registry_count(), ==, 2);

  dock_registry_remove(a);
  g_assert_cmpuint(dock_registry_count(), ==, 1);
  g_assert_false(dock_registry_contains(a));
  g_assert_true(dock_registry_contains(b));

  /* Removing something that was never registered (or already removed) is a
   * no-op: teardown paths call this unconditionally. */
  dock_registry_remove(a);
  dock_registry_remove(NULL);
  g_assert_cmpuint(dock_registry_count(), ==, 1);

  dock_registry_remove(b);
  g_assert_true(dock_registry_is_empty());

  g_free(a);
  g_free(b);
}

static void test_snapshot_order(void) {
  reset_registry();

  LambdockState *states[3] = {fake_state(), fake_state(), fake_state()};
  for (int i = 0; i < 3; i++)
    dock_registry_add(states[i]);

  GPtrArray *snapshot = dock_registry_snapshot();
  g_assert_cmpuint(snapshot->len, ==, 3);
  for (guint i = 0; i < 3; i++)
    g_assert_true(g_ptr_array_index(snapshot, i) == states[i]);
  g_ptr_array_unref(snapshot);

  for (int i = 0; i < 3; i++) {
    dock_registry_remove(states[i]);
    g_free(states[i]);
  }
}

typedef struct {
  LambdockState *target;
  guint visited;
} VisitContext;

static void visit_and_remove(LambdockState *state, gpointer data) {
  VisitContext *context = data;

  context->visited++;
  /* Removing from inside the walk must not break the iteration: the walk is
   * over a snapshot, not over the live list. */
  if (state == context->target)
    dock_registry_remove(state);
}

static void test_foreach_tolerates_removal(void) {
  reset_registry();

  LambdockState *states[3] = {fake_state(), fake_state(), fake_state()};
  for (int i = 0; i < 3; i++)
    dock_registry_add(states[i]);

  VisitContext context = {states[1], 0};
  dock_registry_foreach(visit_and_remove, &context);

  g_assert_cmpuint(context.visited, ==, 3);
  g_assert_cmpuint(dock_registry_count(), ==, 2);
  g_assert_false(dock_registry_contains(states[1]));

  /* A NULL visitor is a no-op instead of a crash. */
  dock_registry_foreach(NULL, NULL);
  g_assert_cmpuint(dock_registry_count(), ==, 2);

  dock_registry_clear();
  g_assert_true(dock_registry_is_empty());

  for (int i = 0; i < 3; i++)
    g_free(states[i]);
}

int main(int argc, char **argv) {
  g_test_init(&argc, &argv, NULL);

  g_test_add_func("/core/dock_registry/add_and_remove", test_add_and_remove);
  g_test_add_func("/core/dock_registry/snapshot_order", test_snapshot_order);
  g_test_add_func("/core/dock_registry/foreach_tolerates_removal",
                  test_foreach_tolerates_removal);

  return g_test_run();
}
