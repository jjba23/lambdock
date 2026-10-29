// lambdock -- hyper-hackable desktop dock for Wayland
//
// SPDX-License-Identifier: MIT
// Copyright © Josep Bigorra <jjbigorra@gmail.com>

#include "app_window.h"

AppWindow *app_window_new(struct zwlr_foreign_toplevel_handle_v1 *handle,
                          const char *title) {
  AppWindow *win = g_new0(AppWindow, 1);

  win->handle = handle;
  win->title = g_strdup(title);

  return win;
}

void app_window_set_title(AppWindow *win, const char *title) {
  if (!win || !title)
    return;

  /* Titles arrive on every compositor title event, often with the same text:
   * re-allocating and re-pushing it would wake the label, the accessible name
   * and the title tooltip for nothing. */
  if (g_strcmp0(win->title, title) == 0)
    return;

  g_free(win->title);
  win->title = g_strdup(title);
}

void app_window_set_state(AppWindow *win, gboolean minimized,
                          gboolean activated) {
  if (!win)
    return;

  win->minimized = minimized;
  win->activated = activated;
}

gboolean app_window_matches(const AppWindow *win,
                            const struct zwlr_foreign_toplevel_handle_v1 *handle) {
  return win && win->handle == handle;
}

void app_window_free(AppWindow *win) {
  if (!win)
    return;

  g_free(win->title);
  g_free(win);
}

void app_windows_free(GList *windows) {
  for (GList *l = windows; l != NULL; l = l->next)
    app_window_free((AppWindow *)l->data);

  g_list_free(windows);
}
