// lambdock -- hyper-hackable desktop dock for Wayland
//
// SPDX-License-Identifier: MIT
// Copyright © Josep Bigorra <jjbigorra@gmail.com>

#ifndef LAMBDOCK_H
#define LAMBDOCK_H

#include <gtk/gtk.h>
#include <gtk4-layer-shell.h>
#include <libguile.h>
#include <math.h>

/* =========================================================================
 * THE CENTRAL HEADER
 * =========================================================================
 * The shared vocabulary: LambdockState, DockItem, AppWindow, shelf parameters
 * and every layout constant and default.
 *
 * Types only -- no behaviour lives here. Each module publishes its own header
 * for that, so including this file never drags in a module you did not ask for.
 */

/**
 * ANIMATION_DURATION_US:
 * Duration of dock slide animations in microseconds (250,000 us = 250 ms = 0.25
 * seconds).
 */
#define ANIMATION_DURATION_US 250000

/* --- UI Layout & Dimension Constants --- */
#define DEFAULT_ICON_SIZE 48
#define CAPSULE_PADDING_PX 24
/* Padding inside the capsule along the dock's axis, between its ends and the
 * first/last icon. This is only the default: `dock-capsule-padding' overrides it
 * (see LambdockState.capsule_padding). */
#define CAPSULE_MAIN_AXIS_PADDING_PX 12
#define CAPSULE_MAIN_AXIS_PADDING_MAX_PX 96
/* Gap between dock items, in px. The default is the Mac OS X dock's: with its
 * 50px icons and ~15px gaps the spacing is about 0.3x the icon size, and the
 * themes' own button padding/margins make up the rest. */
#define DOCK_ITEM_SPACING_PX 4
#define SEPARATOR_MARGIN_PX 4

/* Cross-axis headroom calculation for fisheye magnification */
#define CROSS_DIM_SCALE_FACTOR 1.65
#define CROSS_DIM_PADDING_PX 36
#define CROSS_DIM_TOLERANCE_PX 20

/* Fisheye Hover Magnification */
#define FISHEYE_MAX_SCALE 1.45
#define FISHEYE_RADIUS_PX 140.0
/* Layout granularity of the magnified sizes, in px. The icon itself is painted
 * through a continuous transform, so this only decides how often the row has to
 * be re-measured while the pointer moves. */
#define FISHEYE_LAYOUT_STEP_PX 2

/* --- Glass shelf / reflection parameters (see LambdockShelfParams) --- */
/* Number of alpha-faded bands used to draw the reflection falloff. Each band is
 * a clipped copy of the same icon, drawn inside one blur node; 16 steps keep the
 * ramp smooth (the blur softens the steps anyway) while costing a fraction of
 * the draws that 40 did on every repaint of the row. */
#define SHELF_REFLECTION_BANDS 16
/* Hard limits used to keep user supplied parameters sane. */
#define SHELF_TILT_MAX_DEG 45.0
#define SHELF_PERSPECTIVE_MIN 50.0
#define SHELF_PERSPECTIVE_MAX 5000.0
#define SHELF_EXTRA_MAX_PX 200
#define SHELF_REFLECTION_RATIO_MAX 2.0
#define SHELF_REFLECTION_BLUR_MAX 0.25
/* How far the keystone (flare) may lean the dock's silhouette, per side. */
#define SHELF_FLARE_MAX_PX 80
/* Built-in defaults: the classic flat capsule. */
#define SHELF_DEFAULT_TILT 0.0
#define SHELF_DEFAULT_PERSPECTIVE 400.0
#define SHELF_DEFAULT_MARGIN_PX CAPSULE_PADDING_PX
#define SHELF_DEFAULT_FRONT_PX 0
#define SHELF_DEFAULT_REFLECTION 0.0
#define SHELF_DEFAULT_REFLECTION_INTENSITY 0.55
#define SHELF_DEFAULT_REFLECTION_BLUR 0.05
#define SHELF_DEFAULT_FLARE_PX 0

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

/* Max characters shown for a running-window title label on taskbar buttons */
#define WINDOW_TITLE_MAX_CHARS 28

/* Resource Parsing Constants */
#define RESOURCE_URI_PREFIX "resource://"
#define RESOURCE_URI_PREFIX_LEN 11

/* Drag and drop. Dock icons are dragged as a plain string with this prefix, so
 * the dock can tell its own drags apart from text and file drops. */
#define DOCK_DND_ITEM_PREFIX "lambdock-item:"

/* Forward declaration of low-level Wayland foreign toplevel handle struct */
struct zwlr_foreign_toplevel_handle_v1;

/* Forward declaration of central application state structure */
typedef struct LambdockState LambdockState;

/**
 * LambdockItemKind:
 * @LAMBDOCK_ITEM_APP: A pinned application launcher (exec command line).
 * @LAMBDOCK_ITEM_DYNAMIC: A polling text item (clock, CPU, battery...).
 * @LAMBDOCK_ITEM_SEPARATOR: A visual divider.
 * @LAMBDOCK_ITEM_STACK: A folder whose contents open in a popover.
 * @LAMBDOCK_ITEM_LAUNCHPAD: Opens the full screen application grid.
 * @LAMBDOCK_ITEM_TRASH: The desktop trash: shows whether it has anything in it,
 * opens it and empties it.
 *
 * What a dock icon actually is. The kind decides the click behaviour and which
 * extra fields of DockItem are meaningful.
 */
typedef enum {
  LAMBDOCK_ITEM_APP = 0,
  LAMBDOCK_ITEM_DYNAMIC,
  LAMBDOCK_ITEM_SEPARATOR,
  LAMBDOCK_ITEM_STACK,
  LAMBDOCK_ITEM_LAUNCHPAD,
  LAMBDOCK_ITEM_TRASH
} LambdockItemKind;

/**
 * AppWindow:
 * @handle: Low-level Wayland protocol handle managing this specific window
 * instance.
 * @title: Dynamically updated window title string (e.g., "Mozilla Firefox").
 * @minimized: whether the compositor last reported this window as minimized.
 * @activated: whether it last reported this window as focused.
 *
 * The two state flags are a mirror of the compositor's `state` event (see
 * toplevel_tracker.c). A dock click reads them to decide between minimizing and
 * restoring, which is what makes "minimize to dock" work.
 */
typedef struct AppWindow {
  struct zwlr_foreign_toplevel_handle_v1 *handle;
  char *title;
  gboolean minimized;
  gboolean activated;
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
 * @cell: The icon-sized cell that holds the item's content. The theme's button
 * padding sits outside it, so a label, a gauge and a separator are exactly as
 * tall as an icon.
 * @container: Wrapper box holding @button and, if enabled, @reflection.
 * @reflection: Mirrored glass-shelf reflection (shown when reflections are on).
 * @popover: Context menu GTK Popover widget attached to this dock button.
 * @windows: Pointer to GLib Doubly Linked List (GList*) storing active
 * AppWindow structs.
 * @state: Reference pointer back to the main LambdockState structure.
 * @poll_fn: Guile procedure called periodically to fetch updated item text.
 * @poll_timer_id: GLib timeout source ID managing polling intervals.
 * @label: GtkLabel rendering text output for dynamic items.
 * @hover_animate: TRUE if hover fisheye magnification applies to this item.
 * @kind: What the item is (app, dynamic, separator, stack, launchpad).
 * @stack_path: Folder shown by a stack item (NULL otherwise).
 * @stack_view: Stack view mode name ("grid" or "list").
 * @dragging: TRUE while the user drags this item (guards the drop handlers).
 * @meter: Circular gauge drawn instead of text for meter dynamic items.
 * @meter_unit: Suffix of the gauge's reading ("%" or "°C").
 * @meter_max: Value that fills the gauge (0..100 readings use 100).
 * @icon_image: The item's icon widget, kept so an item whose icon changes with
 * its state (the trash) can swap its artwork.
 * @trash_timer_id: GLib timeout re-counting the trash of a trash item.
 * @trash_count: Items counted in the trash at the last refresh.
 * @trash_full: TRUE while that count was above zero.
 */
typedef struct DockItem {
  char *name;
  char *icon;
  char *exec;
  char *app_id;
  gboolean is_favorite;
  GtkWidget *button;
  GtkWidget *cell;       /* Icon-sized content cell inside @button */
  GtkWidget *container;  /* Wrapper box: [button] + [reflection] */
  GtkWidget *reflection; /* Mirrored glass-shelf icon reflection */
  GtkWidget *separator_follow; /* Separators only: the shelf-following body */
  GtkWidget *popover;
  GList *windows;
  LambdockState *state;
  SCM poll_fn;
  guint poll_timer_id;
  GtkWidget *label;
  gboolean window_title_label; /* TRUE when label shows a window title */
  gboolean hover_animate;
  LambdockItemKind kind;
  char *stack_path; /* Folder of a stack item */
  char *stack_view; /* "grid" / "list" */
  gboolean dragging;
  GtkWidget *meter;     /* Gauge shown for meter dynamic items */
  char *meter_unit;     /* Reading suffix: "%" or "°C" */
  double meter_max;     /* Reading that fills the gauge */
  char *desktop_id;     /* Desktop entry this item resolves to (canonical id) */
  GtkWidget *icon_image; /* The item's GtkImage, owned by the widget tree */
  guint trash_timer_id;  /* Refresh timer of a trash item */
  guint trash_count;     /* Items in the trash at the last refresh */
  gboolean trash_full;   /* TRUE while the trash has anything in it */
} DockItem;

/**
 * LambdockShelfParams:
 * Geometry of the dock's glass shelf.
 *
 * There are no dock modes: the dock always runs the same widgets and these
 * numbers describe its shape. The defaults reproduce the classic flat capsule.
 *
 * @tilt_deg: Shelf tilt away from the viewer, in degrees (0 = flat).
 * @perspective: Projection distance in pixels (smaller = stronger 3D).
 * @margin_px: Glass on the inner side of the icons (away from the screen edge).
 * Negative values let the icons overhang the shelf, like the OS X 3D dock.
 * @front_px: Glass on the outer side of the icons (the front face). The
 * reflection band lives here when it is taller than this.
 * @reflection_ratio: Reflection band height / icon size (0 = no reflections).
 * @reflection_intensity: Alpha of the reflection where it touches the icon.
 * @reflection_blur_ratio: Gaussian blur radius / icon size.
 * @flare_px: How much wider the dock is at the screen edge than at the inner
 * edge, in pixels per side. 0 keeps the slab rectangular; a positive value
 * gives it the OS X dock's keystone silhouette (wider where it meets the
 * screen), which is what makes the Mavericks slab look like a 3D shelf.
 */
typedef struct LambdockShelfParams {
  double tilt_deg;
  double perspective;
  int margin_px;
  int front_px;
  double reflection_ratio;
  double reflection_intensity;
  double reflection_blur_ratio;
  int flare_px;
} LambdockShelfParams;

/**
 * LambdockState:
 * Global application state structure passed across callbacks and modules.
 */
struct LambdockState {
  GtkWidget *window;
  GtkWidget *overlay;         /* Main overlay container */
  GtkWidget *bg_shelf;        /* 3D shelf wrapper around bg_capsule */
  GtkWidget *bg_capsule;      /* Capsule background box */
  GtkWidget *scrolled_window; /* Scroll viewport container for dock items */
  GtkWidget *box;             /* Icon container box */
  GtkWidget *popover;
  GtkSizeGroup *size_group;
  gboolean auto_hide;
  gboolean show_when_clear;
  gboolean track_windows; /* <-- Track open Wayland foreign windows */
  gboolean hover_animate;
  gboolean reduce_motion;       /* Disable slide/fisheye animations (accessibility) */
  gboolean taskbar;             /* Full-width bar instead of floating dock */
  gboolean show_window_titles;  /* Show window titles on running app items */
  LambdockShelfParams shelf;    /* Glass shelf / reflection geometry */
  int icon_size;
  int item_spacing;      /* Gap between dock items, in px */
  int capsule_padding;   /* Room between the capsule's ends and the icons, px */
  guint hide_timer_id;
  gboolean is_hidden;
  char *theme;
  char *active_theme_class; /* Tracked CSS theme class e.g. "theme-unity" */
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
  gint64 margin_anim_end_us; /* When the last slide stopped (0: never). */
  int active_popovers;

  /* Fisheye Hover State. Pointer events arrive faster than the display
   * refreshes, so the newest position is kept here and applied once per frame
   * by `pointer_tick_id`; `applied_*` is what the icons are scaled to now. */
  guint pointer_tick_id;
  double pointer_x;
  double pointer_y;
  double applied_pointer_x;
  double applied_pointer_y;
  gboolean pointer_applied;

  /* Auto-hide: whether the pointer has moved since it last entered the dock. A
   * leave that arrives without any movement is an echo of the dock sliding under
   * a pointer that never moved, not the user leaving. */
  gboolean pointer_moved_since_enter;

  /* Unified Dock Items & Window Trackers */
  GList *dock_items;
  GList *window_trackers;

  /* Per instance CSS providers */
  GtkCssProvider *base_css_provider;
  GtkCssProvider *taskbar_css_provider; /* Loaded in taskbar / window titles */
  GtkCssProvider *shelf_css_provider;   /* Loaded when the shelf is active */
  GtkCssProvider *user_css_provider;
};

/**
 * lambdock_shelf_params_default:
 * Returns the built-in shelf parameters, i.e. the plain flat dock.
 */
LambdockShelfParams lambdock_shelf_params_default(void);

/**
 * lambdock_shelf_params_effective:
 * @state: Dock state (may be NULL).
 *
 * Returns the shelf parameters to use for @state, falling back to the built-in
 * defaults for NULL or zero-initialised states (partially built docks, tests).
 */
LambdockShelfParams lambdock_shelf_params_effective(const LambdockState *state);

/**
 * lambdock_shelf_active:
 * @state: Dock state (may be NULL).
 *
 * Returns TRUE when the shelf geometry differs from the plain flat capsule,
 * which is what switches on the shelf CSS classes and the shelf modifier
 * stylesheet. With the default parameters this is FALSE, so the classic dock is
 * completely unaffected.
 */
gboolean lambdock_shelf_active(const LambdockState *state);

#endif // LAMBDOCK_H
