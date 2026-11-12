// lambdock -- hyper-hackable desktop dock for Wayland
//
// SPDX-License-Identifier: MIT
// Copyright © Josep Bigorra <jjbigorra@gmail.com>

#include "x11_rules.h"

/* Window types the desktop owns. Listing them would put an icon for the panel,
 * the desktop or a menu inside the dock. */
static const char *const DESKTOP_CHROME_TYPES[] = {
    "_NET_WM_WINDOW_TYPE_DESKTOP",  "_NET_WM_WINDOW_TYPE_DOCK",
    "_NET_WM_WINDOW_TYPE_TOOLBAR",  "_NET_WM_WINDOW_TYPE_MENU",
    "_NET_WM_WINDOW_TYPE_UTILITY",  "_NET_WM_WINDOW_TYPE_SPLASH",
    "_NET_WM_WINDOW_TYPE_NOTIFICATION", "_NET_WM_WINDOW_TYPE_TOOLTIP",
    "_NET_WM_WINDOW_TYPE_COMBO",    "_NET_WM_WINDOW_TYPE_DND",
};

gboolean lambdock_x11_type_is_desktop_chrome(const char *window_type) {
  if (!window_type || *window_type == '\0')
    return FALSE;

  for (gsize i = 0; i < G_N_ELEMENTS(DESKTOP_CHROME_TYPES); i++) {
    if (g_strcmp0(window_type, DESKTOP_CHROME_TYPES[i]) == 0)
      return TRUE;
  }

  return FALSE;
}

gboolean lambdock_x11_shows_window(gboolean desktop_chrome,
                                   gboolean skip_taskbar, gboolean own_window,
                                   gboolean has_wm_class) {
  return !desktop_chrome && !skip_taskbar && !own_window && has_wm_class;
}

LambdockX11WindowState lambdock_x11_window_state(gboolean hidden, gulong window,
                                                 gulong active_window) {
  LambdockX11WindowState state = {
      .minimized = hidden,
      .activated = active_window != 0 && window == active_window,
  };

  return state;
}

/**
 * clamp_axis:
 * @value: requested length.
 * @limit: length available on the monitor.
 *
 * Returns: @value bounded to 1..@limit, so a dock whose content or thickness
 * outgrew its monitor still gets a window that is on screen.
 */
static int clamp_axis(int value, int limit) {
  if (limit < 1)
    return 1;

  if (value > limit)
    return limit;

  return value < 1 ? 1 : value;
}

/** The reserved strip along @edge, in root coordinates. */
static LambdockX11Rect reserved_strip(const LambdockX11Placement *placement,
                                      GtkLayerShellEdge edge,
                                      LambdockX11Rect monitor, int main_px,
                                      int reserve_px) {
  switch (edge) {
  case GTK_LAYER_SHELL_EDGE_TOP:
    return (LambdockX11Rect){placement->x, monitor.y, main_px, reserve_px};
  case GTK_LAYER_SHELL_EDGE_LEFT:
    return (LambdockX11Rect){monitor.x, placement->y, reserve_px, main_px};
  case GTK_LAYER_SHELL_EDGE_RIGHT:
    return (LambdockX11Rect){monitor.x + monitor.width - reserve_px,
                             placement->y, reserve_px, main_px};
  case GTK_LAYER_SHELL_EDGE_BOTTOM:
  default:
    return (LambdockX11Rect){placement->x,
                             monitor.y + monitor.height - reserve_px, main_px,
                             reserve_px};
  }
}

LambdockX11Placement lambdock_x11_placement_compute(
    GtkLayerShellEdge edge, LambdockX11Rect root, LambdockX11Rect monitor,
    int desired_main_px, int window_cross_px, int reserve_px) {
  gboolean horizontal =
      (edge == GTK_LAYER_SHELL_EDGE_TOP || edge == GTK_LAYER_SHELL_EDGE_BOTTOM);
  gboolean at_start =
      (edge == GTK_LAYER_SHELL_EDGE_TOP || edge == GTK_LAYER_SHELL_EDGE_LEFT);

  int monitor_main = horizontal ? monitor.width : monitor.height;
  int monitor_cross = horizontal ? monitor.height : monitor.width;

  /* Nothing measured yet means "as wide as the monitor": collapsing to a
   * one-pixel bar on the first frame would show a dock that flickers in. */
  int main_px = (desired_main_px < 1) ? monitor_main
                                     : clamp_axis(desired_main_px, monitor_main);
  int cross_px = clamp_axis(window_cross_px, monitor_cross);
  int reserve = reserve_px < 1 ? 0 : clamp_axis(reserve_px, monitor_cross);

  int main_origin = (horizontal ? monitor.x : monitor.y) + (monitor_main - main_px) / 2;
  int cross_origin = at_start ? (horizontal ? monitor.y : monitor.x)
                              : (horizontal ? monitor.y + monitor.height
                                            : monitor.x + monitor.width) -
                                    cross_px;

  LambdockX11Placement placement = {
      .x = horizontal ? main_origin : cross_origin,
      .y = horizontal ? cross_origin : main_origin,
      .width = horizontal ? main_px : cross_px,
      .height = horizontal ? cross_px : main_px,
  };

  /* No reservation, no struts: a window may ask for its edge and nothing else. */
  if (reserve < 1)
    return placement;

  LambdockX11Rect reserved =
      reserved_strip(&placement, edge, monitor, main_px, reserve);

  /* The four distances are measured from the desktop's own edges, so that a
   * window manager can subtract the reservation from the work area. */
  switch (edge) {
  case GTK_LAYER_SHELL_EDGE_TOP:
    placement.strut_top = reserved.y + reserved.height - root.y;
    placement.strut_top_start_x = reserved.x;
    placement.strut_top_end_x = reserved.x + reserved.width - 1;
    break;
  case GTK_LAYER_SHELL_EDGE_LEFT:
    placement.strut_left = reserved.x + reserved.width - root.x;
    placement.strut_left_start_y = reserved.y;
    placement.strut_left_end_y = reserved.y + reserved.height - 1;
    break;
  case GTK_LAYER_SHELL_EDGE_RIGHT:
    placement.strut_right = root.x + root.width - reserved.x;
    placement.strut_right_start_y = reserved.y;
    placement.strut_right_end_y = reserved.y + reserved.height - 1;
    break;
  case GTK_LAYER_SHELL_EDGE_BOTTOM:
  default:
    placement.strut_bottom = root.y + root.height - reserved.y;
    placement.strut_bottom_start_x = reserved.x;
    placement.strut_bottom_end_x = reserved.x + reserved.width - 1;
    break;
  }

  return placement;
}
