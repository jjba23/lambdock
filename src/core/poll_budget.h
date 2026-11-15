// lambdock -- hyper-hackable desktop dock for Wayland
//
// SPDX-License-Identifier: MIT
// Copyright © Josep Bigorra <jjbigorra@gmail.com>

#ifndef LAMBDOCK_POLL_BUDGET_H
#define LAMBDOCK_POLL_BUDGET_H

#include <glib.h>

/*
 * =========================================================================
 * POLLER BUDGET (PURE)
 * =========================================================================
 * Every dynamic item is a Scheme thunk called on the GTK main thread. The
 * budget is the number of polls per second a dock may schedule in total; when a
 * configuration asks for more, the dock says so instead of letting a fourth
 * CPU widget quietly eat a frame. It is a rule, not a measurement: the cost of
 * one poller is the poller's own business, but how often a dock may call one is
 * the dock's.
 */

/** Total scheduled polls per second one dock may ask for. */
#define LAMBDOCK_POLL_BUDGET_PER_SECOND 12.0

/**
 * lambdock_polls_per_second:
 * @intervals: (nullable): poll periods in milliseconds.
 * @count: how many entries @intervals holds.
 *
 * The rate a list of pollers schedules: a 1000 ms interval is one poll per
 * second, a 2000 ms one half of that. A zero or negative interval schedules
 * nothing (the item has no timer) and is ignored.
 *
 * Returns: the total polls per second.
 */
double lambdock_polls_per_second(const guint *intervals, gsize count);

/**
 * lambdock_poll_budget_exceeded:
 * @intervals: (nullable): poll periods in milliseconds.
 * @count: how many entries @intervals holds.
 * @budget: the allowed polls per second.
 *
 * Returns: TRUE when the schedule is strictly above @budget.
 */
gboolean lambdock_poll_budget_exceeded(const guint *intervals, gsize count,
                                       double budget);

#endif // LAMBDOCK_POLL_BUDGET_H
