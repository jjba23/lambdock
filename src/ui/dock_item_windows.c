// lambdock -- hyper-hackable desktop dock for Wayland
//
// SPDX-License-Identifier: MIT
// Copyright (c) Josep Bigorra <jjbigorra@gmail.com>

/* ============================================================================
 * THE DOCK ITEM'S WINDOWS
 * ============================================================================
 *
 * What an item knows about the windows the compositor reported for it: which
 * handles it tracks, how many, whether one holds the focus, whether they are all
 * minimized, and which item a reported app id belongs to.
 *
 * This used to be the second half of ui/dock_item.c, so a file about building an
 * icon also owned the window model. The two meet at the DockItem value and
 * nowhere else; they are two files now, and this one keeps an item's windows up
 * to date.
 * ========================================================================= */

#include "dock_item_windows.h"

#include "../app_window.h"
#include "../core/item_text.h"
#include "../logger.h"
#include "../toplevel_tracker.h"
#include "dock_item.h"
#include "desktop_entry.h"
#include "shelf.h"
#include "widget_utils.h"
#include <string.h>

/**
 * clear_tracked_windows:
 * Drops every tracked AppWindow and handle, and removes the items that existed
 * only because a window was tracked.
 *
 * Used when the Wayland foreign-toplevel manager goes away: its handles become
 * invalid, so no dock may keep pointers to them.
 */
void clear_tracked_windows(LambdockState *state) {
  if (!state)
    return;

  /* Drop every AppWindow and all tracked handles, then remove the non-favorite
   * items that existed only because a window was tracked. Used when the Wayland
   * foreign-toplevel manager goes away and its handles become invalid, so no
   * dock keeps dangling pointers. */
  for (GList *l = state->dock_items; l != NULL; l = l->next) {
    DockItem *item = (DockItem *)l->data;
    if (!item)
      continue;

    app_windows_free(item->windows);
    item->windows = NULL;
    update_dock_item_status(item);
  }

  GList *curr = state->dock_items;
  while (curr != NULL) {
    GList *next = curr->next;
    DockItem *item = (DockItem *)curr->data;
    if (item && !item->is_favorite) {
      state->dock_items = g_list_delete_link(state->dock_items, curr);
      free_dock_item(item);
    }
    curr = next;
  }

  g_list_free(state->window_trackers);
  state->window_trackers = NULL;

  if (state->show_when_clear)
    reveal_dock(state);
}


/* ============================================================================
 * WAYLAND WINDOW TRACKING INTEGRATION
 * ============================================================================
 */

AppWindow *
dock_item_add_window(DockItem *item,
                     struct zwlr_foreign_toplevel_handle_v1 *handle) {
  if (!item || !handle)
    return NULL;

  /* Idempotency guard: a handle must map to exactly one AppWindow. Callers
   * such as sync_window_trackers() (invoked on every reload) and repeated
   * toplevel app_id events can re-add the same handle; without this check they
   * would duplicate entries, leak AppWindow structs and inflate window counts. */
  for (GList *l = item->windows; l != NULL; l = l->next) {
    AppWindow *existing = (AppWindow *)l->data;
    if (existing && existing->handle == handle)
      return existing;
  }

  AppWindow *win = app_window_new(handle, item->name ? item->name : item->app_id);

  item->windows = g_list_append(item->windows, win);

  update_dock_item_status(item);
  return win;
}

void dock_item_remove_window(DockItem *item,
                             struct zwlr_foreign_toplevel_handle_v1 *handle) {
  if (!item || !handle)
    return;

  for (GList *l = item->windows; l != NULL; l = l->next) {
    AppWindow *win = (AppWindow *)l->data;
    if (app_window_matches(win, handle)) {
      item->windows = g_list_remove(item->windows, win);
      app_window_free(win);
      break;
    }
  }

  update_dock_item_status(item);
}

gboolean dock_item_has_window(
    const DockItem *item,
    const struct zwlr_foreign_toplevel_handle_v1 *handle) {
  if (!item || !handle)
    return FALSE;

  for (const GList *node = item->windows; node != NULL; node = node->next) {
    if (app_window_matches(node->data, handle))
      return TRUE;
  }

  return FALSE;
}

/** Toggles the `running-app` class and the shelf's running indicator together. */
/** TRUE when the item has windows and every one of them is minimized. */
static gboolean all_windows_minimized(const DockItem *item) {
  if (!item->windows)
    return FALSE;

  for (const GList *node = item->windows; node != NULL; node = node->next) {
    const AppWindow *window = node->data;
    if (!window || !window->minimized)
      return FALSE;
  }

  return TRUE;
}

/** TRUE when one of the item's windows holds the keyboard focus. */
static gboolean any_window_focused(const DockItem *item) {
  for (const GList *node = item->windows; node != NULL; node = node->next) {
    const AppWindow *window = node->data;
    if (window && window->activated)
      return TRUE;
  }

  return FALSE;
}

static void apply_running_state(DockItem *item) {
  gboolean running = (item->windows != NULL);

  lambdock_set_css_class(item->button, "running-app", running);

  /* Two states every theme can style independently:
   *  - `window-focused`: the app's window is the one being looked at, which is
   *    the depressed task button in a taskbar-style theme;
   *  - `window-minimized`: all of its windows are tucked away (overlays.css
   *    dims the icon for every theme). */
  lambdock_set_css_class(item->button, "window-focused",
                         any_window_focused(item));
  lambdock_set_css_class(item->button, "window-minimized",
                         all_windows_minimized(item));

  /* The glass shelf draws its own running indicator near the outer edge. */
  if (item->reflection)
    shelf_reflection_set_running(item->reflection, running);
}

/**
 * apply_window_title_label:
 * Shows the running window's title inside the button in taskbar mode, and hides
 * the label otherwise. App items only: a dynamic item's label carries its
 * reading, and separators have none.
 */
static void apply_window_title_label(DockItem *item) {
  if (!item->window_title_label || !item->label)
    return;

  AppWindow *window = item->windows ? (AppWindow *)item->windows->data : NULL;
  const char *text = lambdock_window_title_text(
      item->state && item->state->show_window_titles, item->windows != NULL,
      window ? window->title : NULL, item->name, item->app_id);

  if (text)
    gtk_label_set_text(GTK_LABEL(item->label), text);
  gtk_widget_set_visible(item->label, text != NULL);
}

void update_dock_item_status(DockItem *item) {
  if (!item || !item->button)
    return;

  apply_running_state(item);

  /* The running state and the title are both part of what a screen reader
   * announces, so refresh the label before anything else can return early. */
  dock_item_update_accessible(item);
  apply_window_title_label(item);
}

void dock_item_refresh_window_state(DockItem *item) {
  if (!item || !item->button)
    return;

  /* Only the CSS classes: titles and accessible text are handled by the tracker's
   * title visitor, and re-deriving them here would defeat the memoisation. */
  apply_running_state(item);
}

/**
 * exec_matches_app_id:
 * Allocation-free, case-insensitive token match of APP_ID inside EXEC, used as
 * a last-resort heuristic when the exact app_id/icon/name comparisons fail.
 * Requires whole-token (word boundary) matches and a minimum length so short
 * ids cannot match unrelated executables, and it no longer tests the reverse
 * direction (exec contained in app_id), which produced false positives.
 */
static gboolean exec_matches_app_id(const char *exec, const char *app_id) {
  if (!exec || *exec == '\0' || !app_id)
    return FALSE;

  size_t idlen = strlen(app_id);
  if (idlen < 3)
    return FALSE;

  for (const char *p = exec; *p != '\0'; p++) {
    if (p != exec && g_ascii_isalnum((guchar)p[-1]))
      continue; /* not at a token boundary */

    if (g_ascii_strncasecmp(p, app_id, idlen) != 0)
      continue;

    char right = p[idlen];
    if (right == '\0' || !g_ascii_isalnum((guchar)right))
      return TRUE;
  }
  return FALSE;
}

/* A predicate over one dock item, plus the value it needs. */
typedef gboolean (*DockItemPredicate)(const DockItem *item, gconstpointer data);

/** First item of @state matching @predicate, walking the row in order. */
static DockItem *find_item(const LambdockState *state,
                           DockItemPredicate predicate, gconstpointer data) {
  for (GList *l = state->dock_items; l != NULL; l = l->next) {
    DockItem *item = (DockItem *)l->data;
    if (item && predicate(item, data))
      return item;
  }

  return NULL;
}

/**
 * item_matches_app_id:
 * Whether @item is known by @app_id: the spellings a launcher can be written
 * with, plus the exec heuristic.
 */
static gboolean item_matches_app_id(const DockItem *item, const char *app_id) {
  if (item->app_id && g_ascii_strcasecmp(item->app_id, app_id) == 0)
    return TRUE;

  if (item->icon && g_ascii_strcasecmp(item->icon, app_id) == 0)
    return TRUE;

  if (item->name && g_ascii_strcasecmp(item->name, app_id) == 0)
    return TRUE;

  if (item->desktop_id && g_ascii_strcasecmp(item->desktop_id, app_id) == 0)
    return TRUE;

  return exec_matches_app_id(item->exec, app_id);
}

/**
 * item_matches_entry:
 * Whether @item resolves to the same desktop entry as @entry.
 *
 * This is the precise match: it keeps a pinned launcher and the window it opens
 * from showing up as two icons when their ids are spelled differently (a pinned
 * "IntelliJ" whose exec is `idea`, and a window reporting app id
 * `jetbrains-idea`, both land on jetbrains-idea-....desktop).
 */
static gboolean item_matches_entry(const DockItem *item,
                                   const LambdockDesktopEntry *entry) {
  if (!entry)
    return FALSE;

  if (item->desktop_id && entry->stem &&
      g_ascii_strcasecmp(item->desktop_id, entry->stem) == 0)
    return TRUE;

  if (item->app_id &&
      ((entry->stem && g_ascii_strcasecmp(item->app_id, entry->stem) == 0) ||
       (entry->id && g_ascii_strcasecmp(item->app_id, entry->id) == 0)))
    return TRUE;

  if (item->icon && entry->icon &&
      g_ascii_strcasecmp(item->icon, entry->icon) == 0)
    return TRUE;

  return item->exec && entry->exec &&
         exec_matches_app_id(entry->exec, item->exec);
}

/* Adapters: the finder passes the value as gconstpointer. */
static gboolean matches_app_id(const DockItem *item, gconstpointer data) {
  return item_matches_app_id(item, (const char *)data);
}

static gboolean matches_entry(const DockItem *item, gconstpointer data) {
  return item_matches_entry(item, (const LambdockDesktopEntry *)data);
}

DockItem *find_dock_item_by_app_id(LambdockState *state, const char *app_id) {
  if (!state || !app_id || *app_id == '\0')
    return NULL;

  DockItem *match = find_item(state, matches_app_id, app_id);
  if (match)
    return match;

  /* Nothing matched the raw id. The desktop entry behind it knows the names the
   * application is actually installed under, so use those for a second pass: a
   * pinned "Files" item (icon "nautilus") tracks a window whose app id is
   * "org.gnome.Nautilus", and a pinned "Terminal" tracks "org.gnome.Terminal". */
  return find_item(state, matches_entry,
                   lambdock_desktop_entry_lookup(app_id));
}

