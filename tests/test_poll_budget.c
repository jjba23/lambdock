// lambdock -- hyper-hackable desktop dock for Wayland
//
// SPDX-License-Identifier: MIT
// Copyright © Josep Bigorra <jjbigorra@gmail.com>

#include "../src/core/poll_budget.h"

#include <glib.h>

/* The poller budget is arithmetic: intervals in, polls per second out, and one
 * boolean for "this configuration asks for more than the dock allows". No GTK,
 * no display, no Guile. */

static void test_rate(void) {
  guint one_hz[] = {1000};
  g_assert_cmpfloat(lambdock_polls_per_second(one_hz, 1), ==, 1.0);

  guint half_hz[] = {2000};
  g_assert_cmpfloat(lambdock_polls_per_second(half_hz, 1), ==, 0.5);

  /* Four one-second pollers schedule four calls a second between them. */
  guint four[] = {1000, 1000, 1000, 1000};
  g_assert_cmpfloat(lambdock_polls_per_second(four, 4), ==, 4.0);

  /* Mixed intervals add up. */
  guint mixed[] = {1000, 2000, 500};
  g_assert_cmpfloat(lambdock_polls_per_second(mixed, 3), ==, 3.5);
}

static void test_empty_and_zero(void) {
  /* No items, or no array at all, schedule nothing. */
  g_assert_cmpfloat(lambdock_polls_per_second(NULL, 4), ==, 0.0);
  g_assert_cmpfloat(lambdock_polls_per_second(NULL, 0), ==, 0.0);

  /* A zero interval means "no timer": counting it as an infinite rate would
   * make every dock with a disabled poller look over budget. */
  guint zero[] = {0, 1000};
  g_assert_cmpfloat(lambdock_polls_per_second(zero, 2), ==, 1.0);
}

static void test_budget(void) {
  guint at_budget[] = {100, 200, 250}; /* 10 + 5 + 4 = 19/s */
  g_assert_true(lambdock_poll_budget_exceeded(at_budget, 3, 10.0));

  /* Exactly at the budget is allowed: the rule is about exceeding it. */
  guint exact[] = {100, 100, 100, 100, 100}; /* 50/s */
  g_assert_false(lambdock_poll_budget_exceeded(exact, 5, 50.0));

  guint under[] = {2000, 2000}; /* 1/s */
  g_assert_false(lambdock_poll_budget_exceeded(under, 2, 12.0));
}

int main(int argc, char **argv) {
  g_test_init(&argc, &argv, NULL);

  g_test_add_func("/poll_budget/rate", test_rate);
  g_test_add_func("/poll_budget/empty-and-zero", test_empty_and_zero);
  g_test_add_func("/poll_budget/budget", test_budget);

  return g_test_run();
}
