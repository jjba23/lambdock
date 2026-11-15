// lambdock -- hyper-hackable desktop dock for Wayland
//
// SPDX-License-Identifier: MIT
// Copyright © Josep Bigorra <jjbigorra@gmail.com>

#include "poll_budget.h"

double lambdock_polls_per_second(const guint *intervals, gsize count) {
  if (!intervals)
    return 0.0;

  double total = 0.0;
  for (gsize i = 0; i < count; i++) {
    if (intervals[i] > 0)
      total += 1000.0 / (double)intervals[i];
  }

  return total;
}

gboolean lambdock_poll_budget_exceeded(const guint *intervals, gsize count,
                                       double budget) {
  return lambdock_polls_per_second(intervals, count) > budget;
}
