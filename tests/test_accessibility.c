// lambdock -- hyper-hackable desktop dock for Wayland
//
// SPDX-License-Identifier: MIT
// Copyright © Josep Bigorra <jjbigorra@gmail.com>

#include "../src/accessibility.h"
#include "../src/dock_window.h"
#include "../src/lambdock.h"
#include "../src/ui/dock_item.h"
#include "../src/ui/launchpad.h"
#include "../src/ui/stack.h"
#include <glib/gstdio.h>
#include <gtk/gtk.h>

/* ============================================================================
 * FIXTURES
 * ============================================================================
 * A dock without a compositor: a window (so focus has a root to live in), the
 * item box, and the dock state the item builders expect.
 */

typedef struct {
  LambdockState state;
  GtkWidget *window;
} DockFixture;

static DockFixture *fixture_new(GtkOrientation orientation) {
  DockFixture *fixture = g_new0(DockFixture, 1);

  fixture->state.orientation = orientation;
  fixture->state.edge = orientation == GTK_ORIENTATION_HORIZONTAL
                            ? GTK_LAYER_SHELL_EDGE_BOTTOM
                            : GTK_LAYER_SHELL_EDGE_LEFT;
  fixture->state.icon_size = DEFAULT_ICON_SIZE;
  fixture->state.window = gtk_window_new();
  fixture->window = fixture->state.window;
  fixture->state.box = gtk_box_new(orientation, 0);
  gtk_window_set_child(GTK_WINDOW(fixture->state.window), fixture->state.box);

  return fixture;
}

static void fixture_free(DockFixture *fixture) {
  clear_favorite_dock_items(&fixture->state);
  gtk_window_destroy(GTK_WINDOW(fixture->window));
  g_free(fixture);
}

/* Emits the key signal the dock's key controller listens to and returns whether
 * the handler used it. */
static gboolean send_key(GtkWidget *root, guint keyval, GdkModifierType state) {
  GListModel *controllers = gtk_widget_observe_controllers(root);
  gboolean handled = FALSE;

  for (guint i = 0; i < g_list_model_get_n_items(controllers) && !handled; i++) {
    GtkEventController *controller = g_list_model_get_item(controllers, i);
    if (GTK_IS_EVENT_CONTROLLER_KEY(controller)) {
      g_signal_emit_by_name(controller, "key-pressed", keyval, 0, state,
                            &handled);
    }
    g_object_unref(controller);
  }

  g_object_unref(controllers);
  return handled;
}

/* gtk_test_accessible_check_property() returns NULL when the widget exposes the
 * expected value, and a message when it does not. */
static void assert_accessible_label(GtkWidget *widget, const char *label) {
  char *message = gtk_test_accessible_check_property(
      GTK_ACCESSIBLE(widget), GTK_ACCESSIBLE_PROPERTY_LABEL, label);
  g_assert_null(message);
  g_free(message);
}

static void assert_accessible_description(GtkWidget *widget,
                                          const char *description) {
  char *message = gtk_test_accessible_check_property(
      GTK_ACCESSIBLE(widget), GTK_ACCESSIBLE_PROPERTY_DESCRIPTION, description);
  g_assert_null(message);
  g_free(message);
}

/* ============================================================================
 * MOTION
 * ============================================================================ */

static void test_reduce_motion(void) {
  LambdockState state = {0};
  gboolean system_enabled = lambdock_system_animations_enabled();

  /* With nothing configured the desktop's own setting decides. */
  g_assert_cmpint(lambdock_reduce_motion(NULL), ==, !system_enabled);
  g_assert_cmpint(lambdock_animations_enabled(NULL), ==, system_enabled);
  g_assert_cmpint(lambdock_reduce_motion(&state), ==, !system_enabled);

  /* The dock's own switch always wins, or only ever adds to the desktop's. */
  state.reduce_motion = TRUE;
  g_assert_true(lambdock_reduce_motion(&state));
  g_assert_false(lambdock_animations_enabled(&state));

  state.reduce_motion = FALSE;
  g_assert_cmpint(lambdock_reduce_motion(&state), ==, !system_enabled);
  g_assert_cmpint(lambdock_animations_enabled(&state), ==, system_enabled);
}

/* hide_dock()/reveal_dock() move a layer-shell margin. Turning the fixture into
 * a real layer surface (where the compositor has the protocol) lets the slide
 * tests exercise the shipping path instead of the no-layer fallback. */
static gboolean fixture_enable_layer_shell(GtkWidget *window) {
  if (!lambdock_layer_shell_available())
    return FALSE;

  gtk_layer_init_for_window(GTK_WINDOW(window));
  return gtk_layer_is_layer_window(GTK_WINDOW(window));
}

/* The dock still hides and reveals with motion reduced; it just does it in one
 * step instead of starting a frame-clock animation. */
static void test_reduced_motion_skips_slides(void) {
  DockFixture *fixture = fixture_new(GTK_ORIENTATION_HORIZONTAL);
  LambdockState *state = &fixture->state;

  if (!fixture_enable_layer_shell(fixture->window)) {
    g_test_skip("no layer shell on this display: the slide needs a surface");
    fixture_free(fixture);
    return;
  }

  state->auto_hide = TRUE;
  state->show_when_clear = FALSE; /* Otherwise hide_dock() keeps it visible. */
  state->reduce_motion = TRUE;

  hide_dock(state);
  g_assert_true(state->is_hidden);
  g_assert_cmpint(state->current_margin, ==, state->target_margin);
  g_assert_cmpuint(state->tick_cb_id, ==, 0);

  reveal_dock(state);
  g_assert_false(state->is_hidden);
  g_assert_cmpint(state->current_margin, ==, 0);
  g_assert_cmpint(state->target_margin, ==, 0);
  g_assert_cmpuint(state->tick_cb_id, ==, 0);

  /* The re-entrant preference update is safe to run on a settled dock. */
  dock_apply_motion_preference(state);
  g_assert_cmpuint(state->tick_cb_id, ==, 0);

  fixture_free(fixture);
}

/* Without layer shell (X11, GNOME) there is no margin to slide, so the dock must
 * not claim to be hidden while it is still on screen -- that would freeze the
 * fisheye and the hover animations on a dock the user can see. */
static void test_hide_requires_layer_surface(void) {
  DockFixture *fixture = fixture_new(GTK_ORIENTATION_HORIZONTAL);
  LambdockState *state = &fixture->state;

  /* Deliberately NOT a layer surface. */
  g_assert_false(gtk_layer_is_layer_window(GTK_WINDOW(fixture->window)));

  state->auto_hide = TRUE;
  state->show_when_clear = FALSE;

  hide_dock(state);
  g_assert_false(state->is_hidden);
  g_assert_cmpuint(state->tick_cb_id, ==, 0);
  g_assert_cmpint(state->target_margin, ==, 0);

  fixture_free(fixture);
}

/* ============================================================================
 * SCREEN READER LABELS
 * ============================================================================ */

static void test_app_item_labels(void) {
  DockFixture *fixture = fixture_new(GTK_ORIENTATION_HORIZONTAL);

  DockItem *item = create_dock_item(
      &fixture->state, "Terminal", "utilities-terminal", "true",
      "org.gnome.Terminal", TRUE, TRUE);
  g_assert_nonnull(item);

  assert_accessible_label(item->button, "Terminal");
  assert_accessible_description(item->button, "Not running");

  /* The running state is announced, and counts windows. */
  AppWindow *first =
      dock_item_add_window(item, (struct zwlr_foreign_toplevel_handle_v1 *)0x1);
  g_assert_nonnull(first);
  assert_accessible_description(item->button, "Running, 1 window");

  dock_item_add_window(item, (struct zwlr_foreign_toplevel_handle_v1 *)0x2);
  assert_accessible_description(item->button, "Running, 2 windows");

  /* Closing the windows goes back to the idle wording. */
  dock_item_remove_window(item, (struct zwlr_foreign_toplevel_handle_v1 *)0x1);
  dock_item_remove_window(item, (struct zwlr_foreign_toplevel_handle_v1 *)0x2);
  g_assert_null(item->windows);
  assert_accessible_description(item->button, "Not running");

  fixture_free(fixture);
}

static void test_stack_and_launchpad_labels(void) {
  DockFixture *fixture = fixture_new(GTK_ORIENTATION_HORIZONTAL);

  DockItem *stack = create_stack_dock_item(&fixture->state, "Projects", "folder",
                                           "/tmp", "grid", TRUE);
  g_assert_nonnull(stack);
  assert_accessible_label(stack->button, "Projects, folder stack");
  assert_accessible_description(stack->button, "Opens the folder's contents.");

  DockItem *launchpad = create_launchpad_dock_item(
      &fixture->state, "Applications", NULL, TRUE);
  g_assert_nonnull(launchpad);
  assert_accessible_label(launchpad->button, "Applications");
  assert_accessible_description(
      launchpad->button, "Opens the application grid. Escape closes it.");

  fixture_free(fixture);
}

static void test_trash_item_labels(void) {
  DockFixture *fixture = fixture_new(GTK_ORIENTATION_HORIZONTAL);

  DockItem *trash =
      create_trash_dock_item(&fixture->state, "Trash", NULL, TRUE);
  g_assert_nonnull(trash);
  /* The test environment points XDG_DATA_HOME at a fresh directory, so the
   * trash is empty and the item says so. */
  g_assert_cmpuint(trash->trash_count, ==, 0);
  assert_accessible_label(trash->button, "Trash");
  assert_accessible_description(trash->button,
                                "The trash is empty. Opens the trash.");

  fixture_free(fixture);
}

static void test_announce_guards(void) {
  /* Announcing without a widget or message is a no-op, not a crash. */
  lambdock_accessible_announce(NULL, "hello", FALSE);
  lambdock_accessible_announce(NULL, NULL, TRUE);

  GtkWidget *button = gtk_button_new_with_label("one");
  lambdock_accessible_announce(button, NULL, FALSE);
  lambdock_accessible_announce(button, "", FALSE);
  lambdock_accessible_announce(button, "Selection moved", TRUE);
  g_object_ref_sink(button);
  g_object_unref(button);
}

/* ============================================================================
 * DOCK KEYBOARD NAVIGATION
 * ============================================================================ */

static void test_dock_keyboard_steps(void) {
  DockFixture *fixture = fixture_new(GTK_ORIENTATION_HORIZONTAL);

  DockItem *first = create_dock_item(&fixture->state, "One", "folder", "true",
                                     "one", TRUE, FALSE);
  DockItem *second = create_dock_item(&fixture->state, "Two", "folder", "true",
                                      "two", TRUE, FALSE);
  DockItem *third = create_dock_item(&fixture->state, "Three", "folder", "true",
                                     "three", TRUE, FALSE);

  lambdock_dock_attach_key_navigation(fixture->state.window, &fixture->state);
  /* The controller is installed once; asking again must not stack another. */
  lambdock_dock_attach_key_navigation(fixture->state.window, &fixture->state);

  g_assert_cmpint(lambdock_dock_item_count(&fixture->state), ==, 3);
  g_assert_null(lambdock_dock_item_focused(&fixture->state));

  /* Right with nothing focused starts at the first icon. */
  g_assert_true(send_key(fixture->state.window, GDK_KEY_Right, 0));
  g_assert_true(lambdock_dock_item_focused(&fixture->state) == first);

  g_assert_true(send_key(fixture->state.window, GDK_KEY_Right, 0));
  g_assert_true(lambdock_dock_item_focused(&fixture->state) == second);

  g_assert_true(send_key(fixture->state.window, GDK_KEY_End, 0));
  g_assert_true(lambdock_dock_item_focused(&fixture->state) == third);

  /* At the end the key is not consumed, so a parent could still act on it. */
  g_assert_false(send_key(fixture->state.window, GDK_KEY_Right, 0));

  g_assert_true(send_key(fixture->state.window, GDK_KEY_Left, 0));
  g_assert_true(lambdock_dock_item_focused(&fixture->state) == second);

  g_assert_true(send_key(fixture->state.window, GDK_KEY_Home, 0));
  g_assert_true(lambdock_dock_item_focused(&fixture->state) == first);

  /* Up/Down are the dock's cross axis and untouched on a horizontal dock. */
  g_assert_false(send_key(fixture->state.window, GDK_KEY_Down, 0));
  g_assert_true(lambdock_dock_item_focused(&fixture->state) == first);

  fixture_free(fixture);
}

static void test_vertical_dock_keyboard_steps(void) {
  DockFixture *fixture = fixture_new(GTK_ORIENTATION_VERTICAL);

  DockItem *first = create_dock_item(&fixture->state, "One", "folder", "true",
                                     "one", TRUE, FALSE);
  DockItem *second = create_dock_item(&fixture->state, "Two", "folder", "true",
                                      "two", TRUE, FALSE);

  lambdock_dock_attach_key_navigation(fixture->state.window, &fixture->state);

  /* A side dock is walked with Up/Down, not Left/Right. */
  g_assert_false(send_key(fixture->state.window, GDK_KEY_Right, 0));
  g_assert_true(send_key(fixture->state.window, GDK_KEY_Down, 0));
  g_assert_true(lambdock_dock_item_focused(&fixture->state) == first);
  g_assert_true(send_key(fixture->state.window, GDK_KEY_Down, 0));
  g_assert_true(lambdock_dock_item_focused(&fixture->state) == second);
  g_assert_true(send_key(fixture->state.window, GDK_KEY_Up, 0));
  g_assert_true(lambdock_dock_item_focused(&fixture->state) == first);

  fixture_free(fixture);
}

static void test_separators_are_skipped(void) {
  DockFixture *fixture = fixture_new(GTK_ORIENTATION_HORIZONTAL);

  DockItem *first = create_dock_item(&fixture->state, "One", "folder", "true",
                                     "one", TRUE, FALSE);
  DockItem *separator = create_separator_dock_item(
      &fixture->state, GTK_ORIENTATION_VERTICAL);
  DockItem *second = create_dock_item(&fixture->state, "Two", "folder", "true",
                                      "two", TRUE, FALSE);
  g_assert_nonnull(separator);

  g_assert_cmpint(lambdock_dock_item_count(&fixture->state), ==, 2);
  g_assert_cmpint(lambdock_dock_item_index_of(&fixture->state, first), ==, 0);
  g_assert_cmpint(lambdock_dock_item_index_of(&fixture->state, second), ==, 1);
  g_assert_cmpint(lambdock_dock_item_index_of(&fixture->state, separator), ==,
                  -1);

  lambdock_dock_attach_key_navigation(fixture->state.window, &fixture->state);
  gtk_widget_grab_focus(first->button);
  g_assert_true(send_key(fixture->state.window, GDK_KEY_Right, 0));
  g_assert_true(lambdock_dock_item_focused(&fixture->state) == second);

  fixture_free(fixture);
}

static void test_keyboard_reorder(void) {
  DockFixture *fixture = fixture_new(GTK_ORIENTATION_HORIZONTAL);

  DockItem *first = create_dock_item(&fixture->state, "One", "folder", "true",
                                     "one", TRUE, FALSE);
  DockItem *second = create_dock_item(&fixture->state, "Two", "folder", "true",
                                      "two", TRUE, FALSE);
  DockItem *third = create_dock_item(&fixture->state, "Three", "folder", "true",
                                     "three", TRUE, FALSE);

  lambdock_dock_attach_key_navigation(fixture->state.window, &fixture->state);
  gtk_widget_grab_focus(first->button);

  /* Ctrl+Right is the keyboard's drag: it swaps the focused icon with the next
   * one, in the item list, the widget box and (with a config path) the stored
   * layout. */
  g_assert_true(send_key(fixture->state.window, GDK_KEY_Right,
                         GDK_CONTROL_MASK));
  g_assert_true(fixture->state.dock_items->data == second);
  g_assert_true(fixture->state.dock_items->next->data == first);
  g_assert_true(gtk_widget_get_first_child(fixture->state.box) ==
                second->container);
  g_assert_true(lambdock_dock_item_focused(&fixture->state) == first);

  g_assert_true(send_key(fixture->state.window, GDK_KEY_Left, GDK_CONTROL_MASK));
  g_assert_true(fixture->state.dock_items->data == first);
  g_assert_true(gtk_widget_get_first_child(fixture->state.box) ==
                first->container);

  /* Nothing before the first item to swap with. */
  g_assert_false(send_key(fixture->state.window, GDK_KEY_Left, GDK_CONTROL_MASK));
  g_assert_true(fixture->state.dock_items->data == first);

  g_assert_nonnull(third);
  fixture_free(fixture);
}

static void test_keyboard_context_menu(void) {
  DockFixture *fixture = fixture_new(GTK_ORIENTATION_HORIZONTAL);

  DockItem *item = create_dock_item(&fixture->state, "Terminal",
                                    "utilities-terminal", "true",
                                    "org.gnome.Terminal", TRUE, FALSE);
  lambdock_dock_attach_key_navigation(fixture->state.window, &fixture->state);
  gtk_widget_grab_focus(item->button);

  /* Menu is the keyboard's right click. */
  g_assert_true(send_key(fixture->state.window, GDK_KEY_Menu, 0));
  g_assert_true(gtk_widget_get_visible(item->popover));

  /* Escape closes it and is consumed. */
  g_assert_true(send_key(fixture->state.window, GDK_KEY_Escape, 0));
  g_assert_false(gtk_widget_get_visible(item->popover));
  g_assert_false(send_key(fixture->state.window, GDK_KEY_Escape, 0));

  /* Shift+F10 does the same; F10 alone does not. */
  g_assert_false(send_key(fixture->state.window, GDK_KEY_F10, 0));
  g_assert_true(send_key(fixture->state.window, GDK_KEY_F10, GDK_SHIFT_MASK));
  g_assert_true(gtk_widget_get_visible(item->popover));

  fixture_free(fixture);
}

/* ============================================================================
 * LAUNCHPAD
 * ============================================================================
 * The window itself needs a real application to present, so the grid is built
 * directly and filled the way launchpad_refill() fills it: same widget, same key
 * controller, same classes.
 */

static GtkWidget *launchpad_tile(const char *name) {
  GtkWidget *tile = gtk_button_new_with_label(name);
  gtk_widget_add_css_class(tile, "launchpad-tile");
  return tile;
}

static void test_launchpad_grid(void) {
  g_assert_false(lambdock_launchpad_is_open());
  /* Hiding a launchpad that was never shown is a no-op. */
  lambdock_launchpad_hide();

  GtkWidget *window = gtk_window_new();
  GtkWidget *flow = lambdock_launchpad_grid_new(NULL);
  gtk_window_set_child(GTK_WINDOW(window), flow);

  assert_accessible_label(flow, "Applications");
  assert_accessible_description(
      flow, "Application grid. Use the arrow keys to choose an application "
            "and Enter to open it.");

  GtkWidget *first = launchpad_tile("Files");
  GtkWidget *second = launchpad_tile("Terminal");
  GtkWidget *third = launchpad_tile("Browser");
  gtk_flow_box_append(GTK_FLOW_BOX(flow), first);
  gtk_flow_box_append(GTK_FLOW_BOX(flow), second);
  gtk_flow_box_append(GTK_FLOW_BOX(flow), third);

  gtk_widget_grab_focus(first);

  g_assert_true(send_key(flow, GDK_KEY_Right, 0));
  g_assert_true(gtk_window_get_focus(GTK_WINDOW(window)) == second);

  g_assert_true(send_key(flow, GDK_KEY_End, 0));
  g_assert_true(gtk_window_get_focus(GTK_WINDOW(window)) == third);

  /* Down moves by a row, clamped at the end of the grid. */
  g_assert_true(send_key(flow, GDK_KEY_Down, 0));
  g_assert_true(gtk_window_get_focus(GTK_WINDOW(window)) == third);

  g_assert_true(send_key(flow, GDK_KEY_Home, 0));
  g_assert_true(gtk_window_get_focus(GTK_WINDOW(window)) == first);

  g_assert_true(send_key(flow, GDK_KEY_Left, 0));
  g_assert_true(gtk_window_get_focus(GTK_WINDOW(window)) == first);

  gtk_window_destroy(GTK_WINDOW(window));
}

/* ============================================================================
 * STACK VIEW KEYBOARD HELPERS
 * ============================================================================ */

static GtkWidget *flat_view_with_buttons(GtkBox **out_box, int count) {
  GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
  for (int i = 0; i < count; i++) {
    char *label = g_strdup_printf("Entry %d", i);
    gtk_box_append(GTK_BOX(box), gtk_button_new_with_label(label));
    g_free(label);
  }

  if (out_box)
    *out_box = GTK_BOX(box);
  return box;
}

static void test_stack_view_keyboard(void) {
  GtkWidget *window = gtk_window_new();

  GtkBox *box = NULL;
  GtkWidget *view = flat_view_with_buttons(&box, 3);
  gtk_window_set_child(GTK_WINDOW(window), view);

  g_assert_cmpint(lambdock_stack_view_focus_step(view, 0, 1), ==, TRUE);
  g_assert_true(gtk_window_get_focus(GTK_WINDOW(window)) ==
                gtk_widget_get_first_child(view));

  /* Down the list walks one row; at the end the focus stays. */
  g_assert_true(lambdock_stack_view_focus_step(view, 0, 1));
  g_assert_true(
      gtk_window_get_focus(GTK_WINDOW(window)) ==
      gtk_widget_get_next_sibling(gtk_widget_get_first_child(view)));

  g_assert_true(lambdock_stack_view_focus_step(view, 0, 1));
  g_assert_true(gtk_window_get_focus(GTK_WINDOW(window)) ==
                gtk_widget_get_last_child(view));

  g_assert_true(lambdock_stack_view_focus_step(view, 0, 1));
  g_assert_true(gtk_window_get_focus(GTK_WINDOW(window)) ==
                gtk_widget_get_last_child(view));

  g_assert_true(lambdock_stack_view_focus_edge(view, FALSE));
  g_assert_true(gtk_window_get_focus(GTK_WINDOW(window)) ==
                gtk_widget_get_first_child(view));

  g_assert_true(lambdock_stack_view_focus_edge(view, TRUE));
  g_assert_true(gtk_window_get_focus(GTK_WINDOW(window)) ==
                gtk_widget_get_last_child(view));

  /* An empty view has nowhere to go. */
  GtkWidget *empty = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
  g_assert_false(lambdock_stack_view_focus_step(empty, 0, 1));
  g_assert_false(lambdock_stack_view_focus_edge(empty, TRUE));
  g_object_ref_sink(empty);
  g_object_unref(empty);

  g_assert_false(lambdock_stack_view_focus_step(NULL, 0, 1));

  gtk_window_destroy(GTK_WINDOW(window));
}

int main(int argc, char **argv) {
  g_test_init(&argc, &argv, NULL);

  /* The trash count and the GUI preference files must not touch the real home
   * directory during tests. */
  char *data_dir = g_dir_make_tmp("lambdock-a11y-data-XXXXXX", NULL);
  g_assert_nonnull(data_dir);
  g_setenv("XDG_DATA_HOME", data_dir, TRUE);

  g_test_add_func("/accessibility/reduce_motion", test_reduce_motion);

  if (!gtk_init_check()) {
    g_print("Skipping accessibility widget tests: No display server "
            "available\n");
    return g_test_run();
  }

  g_test_add_func("/accessibility/reduced_motion_slides",
                  test_reduced_motion_skips_slides);
  g_test_add_func("/accessibility/hide_requires_layer_surface",
                  test_hide_requires_layer_surface);
  g_test_add_func("/accessibility/labels/app_item", test_app_item_labels);
  g_test_add_func("/accessibility/labels/stack_and_launchpad",
                  test_stack_and_launchpad_labels);
  g_test_add_func("/accessibility/labels/trash", test_trash_item_labels);
  g_test_add_func("/accessibility/announce_guards", test_announce_guards);
  g_test_add_func("/accessibility/keyboard/steps",
                  test_dock_keyboard_steps);
  g_test_add_func("/accessibility/keyboard/vertical_dock",
                  test_vertical_dock_keyboard_steps);
  g_test_add_func("/accessibility/keyboard/separators",
                  test_separators_are_skipped);
  g_test_add_func("/accessibility/keyboard/reorder", test_keyboard_reorder);
  g_test_add_func("/accessibility/keyboard/context_menu",
                  test_keyboard_context_menu);
  g_test_add_func("/accessibility/launchpad_grid", test_launchpad_grid);
  g_test_add_func("/accessibility/stack_view_keyboard",
                  test_stack_view_keyboard);

  int result = g_test_run();
  g_free(data_dir);
  return result;
}
