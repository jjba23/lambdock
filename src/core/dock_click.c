// lambdock -- hyper-hackable desktop dock for Wayland
//
// SPDX-License-Identifier: MIT
// Copyright © Josep Bigorra <jjbigorra@gmail.com>

#include "dock_click.h"

LambdockClickAction lambdock_click_action_for(guint window_count,
                                              gboolean window_minimized,
                                              gboolean window_active,
                                              const char *launcher_command) {
  if (window_count == 0) {
    return (launcher_command && *launcher_command != '\0')
               ? LAMBDOCK_CLICK_LAUNCH
               : LAMBDOCK_CLICK_NOTHING;
  }

  if (window_count > 1)
    return LAMBDOCK_CLICK_MENU;

  /* Exactly one window: the click toggles it. An active window tucks away; a
   * minimized one -- or one that is merely behind another app -- comes forward.
   * A minimized window is never reported as active, but checking both keeps the
   * rule correct for compositors that lag behind on focus events. */
  if (window_active && !window_minimized)
    return LAMBDOCK_CLICK_MINIMIZE;

  return LAMBDOCK_CLICK_ACTIVATE;
}
