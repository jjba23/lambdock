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

/* --- UI Layout & Dimension Constants --- */
#define DEFAULT_ICON_SIZE 48
#define CAPSULE_PADDING_PX 24
#define CAPSULE_MAIN_AXIS_PADDING_PX 12
#define DOCK_BOX_SPACING_PX 8
#define SEPARATOR_MARGIN_PX 4

/* Cross-axis headroom calculation for fisheye magnification */
#define CROSS_DIM_SCALE_FACTOR 1.65
#define CROSS_DIM_PADDING_PX 36
#define CROSS_DIM_TOLERANCE_PX 20

/* Fisheye Hover Magnification */
#define FISHEYE_MAX_SCALE 1.45
#define FISHEYE_RADIUS_PX 140.0

/* Auto-hide & Timer Constants */
#define AUTOHIDE_TRIGGER_STRIP_PX 2
#define AUTOHIDE_INITIAL_DELAY_MS 1000
#define AUTOHIDE_LEAVE_DELAY_MS 600
#define AUTOHIDE_POPOVER_DELAY_MS 400
#define CONFIG_RELOAD_DEBOUNCE_MS 150

/* Popover Menu Layout Constants */
#define POPOVER_VBOX_SPACING 4
#define POPOVER_VBOX_MARGIN 8
#define POPOVER_ROW_SPACING 6
#define POPOVER_TITLE_MAX_CHARS 30

/* Resource Parsing Constants */
#define RESOURCE_URI_PREFIX "resource://"
#define RESOURCE_URI_PREFIX_LEN 11

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
 * @poll_fn: Guile procedure called periodically to fetch updated item text.
 * @poll_timer_id: GLib timeout source ID managing polling intervals.
 * @label: GtkLabel rendering text output for dynamic items.
 * @hover_animate: TRUE if hover fisheye magnification applies to this item.
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
  SCM poll_fn;
  guint poll_timer_id;
  GtkWidget *label;
  gboolean hover_animate;
} DockItem;

/**
 * LambdockState:
 * Global application state structure passed across callbacks and modules.
 */
struct LambdockState {
  GtkWidget *window;
  GtkWidget *overlay;         /* Main overlay container */
  GtkWidget *bg_capsule;      /* Capsule background box */
  GtkWidget *scrolled_window; /* Scroll viewport container for dock items */
  GtkWidget *box;             /* Icon container box */
  GtkWidget *popover;
  GtkSizeGroup *size_group;
  gboolean auto_hide;
  gboolean show_when_clear;
  gboolean hover_animate;
  int icon_size;
  guint hide_timer_id;
  gboolean is_hidden;
  char *theme;
  char *config_path; /* Absolute path to settings file for this dock instance */
  gboolean is_destroying; /* Re-entrancy guard flag */

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

  /* Per instance CSS providers */
  GtkCssProvider *base_css_provider;
  GtkCssProvider *user_css_provider;
};

#endif // LAMBDOCK_H
