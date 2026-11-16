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

__attribute__((weak)) void lambdock_stack_set_view(DockItem *item, LambdockStackView view) {
  (void)item;
  (void)view;
}

/* A stack cell built in Rust walks into a folder by asking the panel above it to
 * rebuild -- and the panel is still stack.c. A binary that links the archive
 * without stack.c gets this: a click on a folder that changes nothing. The real
 * definition wins wherever stack.c is compiled in. */
__attribute__((weak)) void lambdock_stack_popover_rebuild(DockItem *item,
                                                          const char *path) {
  (void)item;
  (void)path;
}

__attribute__((weak)) void lambdock_launchpad_show(LambdockState *state) {
  (void)state;
}
