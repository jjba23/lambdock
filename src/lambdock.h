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
 *
 * Represents an individual open window on the desktop tracked via Wayland
 * protocols. Multiple AppWindow structures can belong to a single unified
 * DockItem.
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
 *
 * Core data structure representing a unified launcher and running window
 * manager item.
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
 * @window: Main GTK 4 Application Window instance.
 * @box: Central layout container box housing dock buttons and separators.
 * @popover: Root context popover menu for right-clicking open background areas.
 * @auto_hide: Configured preference indicating whether auto-hide behavior is
 * enabled.
 * @hide_timer_id: GLib timeout event source ID managing auto-hide countdown
 * delays.
 * @is_hidden: Boolean tracking whether the dock is currently off-screen.
 * @edge: Wayland Layer Shell screen attachment edge (TOP, BOTTOM, LEFT, RIGHT).
 * @orientation: GTK layout direction (HORIZONTAL for top/bottom, VERTICAL for
 * sides).
 * @tick_cb_id: GTK widget frame clock animation callback identifier.
 * @anim_start_time: Monotonic microsecond timestamp anchoring animation
 * progress.
 * @current_margin: Current animated edge offset margin in pixels.
 * @start_margin: Starting pixel offset when an animation slide begins.
 * @target_margin: Final desired pixel offset when an animation slide completes.
 * @active_popovers: Counter tracking currently open context menus (suspends
 * auto-hide).
 * @dock_items: GLib Doubly Linked List (GList*) containing all active DockItem
 * structures.
 *
 * Global application state structure passed across callbacks and modules.
 */
struct LambdockState {
  GtkWidget *window;
  GtkWidget *box;
  GtkWidget *popover;
  gboolean auto_hide;
  guint hide_timer_id;
  gboolean is_hidden;
  char *theme;

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

  /* Unified Dock Items */
  GList *dock_items;
};

#endif // LAMBDOCK_H
