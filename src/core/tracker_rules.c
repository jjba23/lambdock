// lambdock -- hyper-hackable desktop dock for Wayland
//
// SPDX-License-Identifier: MIT
// Copyright © Josep Bigorra <jjbigorra@gmail.com>

#include "tracker_rules.h"

/* Ways the compositor spells "this is the dock itself". */
static const char *const OWN_APP_IDS[] = {"lambdock",
                                          "org.jointhefreeworld.lambdock"};

gboolean lambdock_tracker_ignores_app_id(const char *app_id) {
  if (!app_id || *app_id == '\0')
    return TRUE;

  for (gsize i = 0; i < G_N_ELEMENTS(OWN_APP_IDS); i++) {
    if (g_ascii_strcasecmp(app_id, OWN_APP_IDS[i]) == 0)
      return TRUE;
  }

  return FALSE;
}

gboolean lambdock_tracker_item_should_be_dropped(gboolean has_windows,
                                                 gboolean is_favorite) {
  return !has_windows && !is_favorite;
}

gboolean lambdock_tracker_should_hide(gboolean auto_hide,
                                      gboolean show_when_clear,
                                      gboolean has_tracked_windows) {
  return auto_hide && show_when_clear && has_tracked_windows;
}

gboolean lambdock_tracker_should_reveal(gboolean show_when_clear,
                                        gboolean has_tracked_windows) {
  return show_when_clear && !has_tracked_windows;
}
