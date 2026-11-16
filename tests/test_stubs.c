// lambdock -- hyper-hackable desktop dock for Wayland
//
// SPDX-License-Identifier: MIT
// Copyright © Josep Bigorra <jjbigorra@gmail.com>

/* The test binaries do not compile main.c, which is where unregister_dock_state
 * lives in the real binary. Tests never register docks, so this double has
 * nothing to do -- and, crucially, it must not pull the registry into binaries
 * that link only a slice of the application sources. */

#include "../src/dock_window.h"
#include "../src/lambdock.h"
#include "../src/ui/dock_item.h"
#include "../src/toplevel_tracker.h"
#include "../src/x11_tracker.h"
#include "../src/ui/launchpad.h"
#include "../src/ui/shelf.h"
#include "../src/ui/stack.h"
#include "../src/guile_bridge.h"

void unregister_dock_state(LambdockState *state) { (void)state; }

/* Same shape, one layer up: the Rust archive reaches back into the C dock for
 * this call, and the linker resolves the whole archive slice a Rust module
 * shares, not only the function the test asked for. A weak double keeps every
 * binary that links a slice resolvable; the real definition in dock_window.c
 * wins wherever it is compiled in, and a test that never shows a dock never
 * calls this anyway. */
__attribute__((weak)) void update_dock_dimensions(LambdockState *state) {
  (void)state;
}

/* `rust/src/ui/shelf_follow.rs` asks the shelf frame where it projects a point.
 * A test binary that links a slice has no shelf frame at all, so the answers are
 * "no frame, no projection": the follow then draws its child flat, which is what
 * a dock without a glass shelf does. shelf.c wins wherever it is compiled in. */
__attribute__((weak)) gboolean shelf_frame_is_frame(gpointer widget) {
  (void)widget;
  return FALSE;
}

__attribute__((weak)) gboolean shelf_frame_is_projected(GtkWidget *frame) {
  (void)frame;
  return FALSE;
}

__attribute__((weak)) gboolean shelf_frame_project_point(GtkWidget *frame,
                                                         graphene_point_t *point) {
  (void)frame;
  (void)point;
  return FALSE;
}

__attribute__((weak)) GtkOrientation shelf_frame_cross_orientation(GtkWidget *frame) {
  (void)frame;
  return GTK_ORIENTATION_HORIZONTAL;
}

/* The two Rust modules that own the item record and its drag and drop still call
 * into dock_item.c (the widget builder), shelf.c (the reflection's indicator) and
 * the Scheme bridge (the layout overrides). A binary that links a slice has none
 * of those, and these doubles are the "nothing is there" answers: no item, no
 * reflection, no layout to persist. Each real definition wins wherever its file
 * is compiled in. */
__attribute__((weak)) char *dock_item_key(const DockItem *item) {
  (void)item;
  return NULL;
}

__attribute__((weak)) DockItem *dock_item_from_widget(GtkWidget *widget) {
  (void)widget;
  return NULL;
}

__attribute__((weak)) void free_dock_item(DockItem *item) { (void)item; }

__attribute__((weak)) void dock_item_refresh_trash(DockItem *item) { (void)item; }

__attribute__((weak)) void dock_item_update_accessible(DockItem *item) { (void)item; }

__attribute__((weak)) DockItem *create_dock_item(LambdockState *state,
                                                 const char *name,
                                                 const char *icon,
                                                 const char *exec,
                                                 const char *app_id,
                                                 gboolean is_favorite,
                                                 gboolean hover_animate) {
  (void)state;
  (void)name;
  (void)icon;
  (void)exec;
  (void)app_id;
  (void)is_favorite;
  (void)hover_animate;
  return NULL;
}

__attribute__((weak)) DockItem *create_stack_dock_item(LambdockState *state,
                                                       const char *name,
                                                       const char *icon,
                                                       const char *path,
                                                       const char *view,
                                                       gboolean hover_animate) {
  (void)state;
  (void)name;
  (void)icon;
  (void)path;
  (void)view;
  (void)hover_animate;
  return NULL;
}

__attribute__((weak)) void shelf_reflection_set_running(GtkWidget *reflection,
                                                        gboolean running) {
  (void)reflection;
  (void)running;
}

__attribute__((weak)) void reveal_dock(LambdockState *state) { (void)state; }

__attribute__((weak)) void dock_layout_add_item(const char *config_path,
                                                const char *kind,
                                                const char *name,
                                                const char *icon,
                                                const char *exec,
                                                const char *extra) {
  (void)config_path;
  (void)kind;
  (void)name;
  (void)icon;
  (void)exec;
  (void)extra;
}

__attribute__((weak)) void dock_layout_remove_item(const char *config_path,
                                                   const char *key) {
  (void)config_path;
  (void)key;
}

__attribute__((weak)) void dock_layout_set_order(const char *config_path, GList *keys) {
  (void)config_path;
  (void)keys;
}

/* A stack remembers its view mode through the layout overrides, and those live
 * in guile_bridge.c. A binary that links a slice has no bridge, and "nothing to
 * remember" is what this double says. */
__attribute__((weak)) void dock_layout_set_view(const char *config_path,
                                                const char *key,
                                                const char *view) {
  (void)config_path;
  (void)key;
  (void)view;
}

/* A dynamic item polls a Scheme procedure, and the procedure handles live in
 * guile_bridge.c. A binary that links a slice has no interpreter at all, so
 * "there is no poller" is the honest answer -- and the timer that would call one
 * is never installed. */
__attribute__((weak)) gboolean lambdock_poll_fn_is_callable(void *poll_fn) {
  (void)poll_fn;
  return FALSE;
}

/* The other half of the poller contract: the bridge protects a procedure from the
 * GC and releases it again when the item goes. A slice binary has no interpreter,
 * so the handle is null and nothing is protected. */
__attribute__((weak)) void *lambdock_poll_fn_protect(void *procedure) {
  (void)procedure;
  return NULL;
}

__attribute__((weak)) void lambdock_poll_fn_release(void *poll_fn) {
  (void)poll_fn;
}

/* Freeing the pinned launchers tells the trackers to forget the docks they point
 * at. A slice binary has no tracker compiled in, and the answer there is that
 * there is nothing to forget. */
__attribute__((weak)) void clear_window_tracker_dock_references(
    LambdockState *state) {
  (void)state;
}

/* A hot reload re-reads the settings and rebuilds the row, which is the bridge's
 * business, and re-anchors the surface, which is the window's. A slice binary has
 * neither: "nothing to read, nothing to build, no surface" is what these answer. */
__attribute__((weak)) void dock_read_settings(LambdockState *state) {
  (void)state;
}

__attribute__((weak)) void populate_dock_items_for(LambdockState *state) {
  (void)state;
}

__attribute__((weak)) void sync_window_trackers(LambdockState *state) {
  (void)state;
}

__attribute__((weak)) void dock_apply_exclusive_zone(LambdockState *state) {
  (void)state;
}

__attribute__((weak)) void apply_dock_mode_classes(LambdockState *state) {
  (void)state;
}

__attribute__((weak)) void refresh_dock_mode(LambdockState *state) {
  (void)state;
}

__attribute__((weak)) void dock_apply_motion_preference(LambdockState *state) {
  (void)state;
}

__attribute__((weak)) void hide_dock(LambdockState *state) {
  (void)state;
}

/* A window's own backend: the X11 half of the tracker is still x11_tracker.c, and
 * a binary that links a slice has no X11 tracker. "Nothing to start, nothing to
 * replay, no window to ask about" is what these answer. */
__attribute__((weak)) gboolean x11_tracker_start(GError **error) {
  (void)error;
  return FALSE;
}

__attribute__((weak)) void x11_tracker_replay(void) {}

__attribute__((weak)) void x11_tracker_activate(LambdockToplevel *window) {
  (void)window;
}

__attribute__((weak)) void x11_tracker_minimize(LambdockToplevel *window) {
  (void)window;
}

__attribute__((weak)) void x11_tracker_close(LambdockToplevel *window) {
  (void)window;
}

/* An item's menu releases the dock's popover slot when it closes, which is
 * dock_window.c's business. A slice binary has no dock window. */
__attribute__((weak)) void on_popover_closed(GtkPopover *popover,
                                             gpointer user_data) {
  (void)popover;
  (void)user_data;
}

__attribute__((weak)) gboolean lambdock_poll_fn_call(void *poll_fn,
                                                     LambdockReading *out) {
  (void)poll_fn;
  (void)out;
  return FALSE;
}

/* The item menu and the stack's view reach into the item builder, the tracker and
 * the launchpad for the actions they offer. A binary that links a slice has none of
 * them, and "no title, no window to focus, no view to switch" is what those doubles
 * answer; each real definition wins wherever its file is compiled in. */
__attribute__((weak)) const char *lambdock_item_title(const DockItem *item,
                                                      const char *fallback) {
  (void)item;
  return fallback;
}

__attribute__((weak)) void activate_toplevel_window(LambdockToplevel *window) {
  (void)window;
}

__attribute__((weak)) void close_toplevel_window(LambdockToplevel *window) {
  (void)window;
}

/* A left click on an item minimizes the focused window, or opens the item's menu
 * when several are running. Both live in files a slice binary does not compile:
 * the tracker owns the window, dock_item.c owns the menu. "Nothing to minimize,
 * nothing to show" is what these answer, and each real definition wins wherever
 * its file is compiled in. */
__attribute__((weak)) void minimize_toplevel_window(LambdockToplevel *window) {
  (void)window;
}

__attribute__((weak)) void lambdock_dock_item_popup_menu(DockItem *item) {
  (void)item;
}

__attribute__((weak)) void lambdock_stack_set_view(DockItem *item, LambdockStackView view) {
  (void)item;
  (void)view;
}

__attribute__((weak)) void lambdock_launchpad_show(LambdockState *state) {
  (void)state;
}

/* Escape closes the launchpad, and the Menu key opens the focused item's menu.
 * Neither file is compiled into a slice binary, and "nothing is open, nothing to
 * show" is what those binaries need to hear. */
__attribute__((weak)) gboolean lambdock_launchpad_is_open(void) { return FALSE; }

__attribute__((weak)) void lambdock_launchpad_hide(void) {}

__attribute__((weak)) void lambdock_dock_item_show_menu(DockItem *item) {
  (void)item;
}
