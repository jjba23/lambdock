// lambdock

// Copyright © Josep Bigorra <jjbigorra@gmail.com>

// lambdock is free software : you can redistribute it and / or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.

// lambdock is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
// GNU General Public License for more details.

// You should have received a copy of the GNU General Public License
// along with lambdock. If not, see <https://www.gnu.org/licenses/>.

#ifndef LAMBDOCK_H
#define LAMBDOCK_H

#include <gtk/gtk.h>
#include <gtk4-layer-shell.h>
#include <libguile.h>
#include <math.h>

/**
 * ANIMATION_DURATION_US:
 * Duration of dock slide animations in microseconds (250,000 us = 250 ms = 0.25
 * seconds).
 */
#define ANIMATION_DURATION_US 250000

/* Forward declaration of low-level Wayland foreign toplevel handle struct */
struct zwlr_foreign_toplevel_handle_v1;

/* Forward declaration of central application state structure */
typedef struct LambdockState LambdockState;

/**
 * AppWindow:
 * @handle: Low-level Wayland protocol handle managing this specific window
 * instance.
 * @title: Dynamically updated window title string (e.g., "Mozilla Firefox").
 */
typedef struct AppWindow {
  struct zwlr_foreign_toplevel_handle_v1 *handle;
  char *title;
} AppWindow;

/**
 * DockItem:
 * @name: Human-readable display name of the application (e.g., "Terminal").
 * @icon: Icon identifier, theme name, or file system path.
 * @exec: Shell command line string executed when launching new instances.
 * @app_id: Desktop application identifier used for window matching (e.g.,
 * "org.gnome.Terminal").
 * @is_favorite: TRUE if pinned launcher configured from Scheme; FALSE if
 * dynamic running app.
 * @button: Pointer to the GTK 4 button widget rendered inside the dock layout.
 * @popover: Context menu GTK Popover widget attached to this dock button.
 * @windows: Pointer to GLib Doubly Linked List (GList*) storing active
 * AppWindow structs.
 * @state: Reference pointer back to the main LambdockState structure.
 */
typedef struct DockItem {
  char *name;
  char *icon;
  char *exec;
  char *app_id;
  gboolean is_favorite;
  GtkWidget *button;
  GtkWidget *popover;
  GList *windows;
  LambdockState *state;
} DockItem;

/**
 * LambdockState:
 * Global application state structure passed across callbacks and modules.
 */
struct LambdockState {
  GtkWidget *window;
  GtkWidget *box;
  GtkWidget *popover;
  gboolean auto_hide;
  gboolean show_when_clear;
  gboolean hover_animate;
  int icon_size;
  guint hide_timer_id;
  gboolean is_hidden;
  char *theme;

  GdkMonitor *monitor; /* NULL for default compositor placement */

  /* Position & Orientation */
  GtkLayerShellEdge edge;
  GtkOrientation orientation;

  /* Animation State */
  guint tick_cb_id;
  gint64 anim_start_time;
  int current_margin;
  int start_margin;
  int target_margin;
  int active_popovers;

  /* Unified Dock Items & Window Trackers */
  GList *dock_items;
  GList *window_trackers;
};

#endif // LAMBDOCK_H
