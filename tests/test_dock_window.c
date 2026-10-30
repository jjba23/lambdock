// lambdock -- hyper-hackable desktop dock for Wayland
//
// SPDX-License-Identifier: MIT
// Copyright © Josep Bigorra <jjbigorra@gmail.com>

#include "../src/dock_window.h"
#include "../src/lambdock.h"
#include "../src/ui/dock_item.h"
#include <gtk/gtk.h>

#define MAX_CROSS_DIM(size) \
  ((int)((size) * CROSS_DIM_SCALE_FACTOR) + CROSS_DIM_PADDING_PX)

/* ============================================================================
 * MONITOR VALIDATION
 * ============================================================================ */

static void test_is_monitor_valid(void) {
  g_assert_false(is_monitor_valid(NULL));

  GdkDisplay *display = gdk_display_get_default();
  GListModel *monitors = gdk_display_get_monitors(display);
  guint n = g_list_model_get_n_items(monitors);

  if (n > 0) {
    GdkMonitor *mon = g_list_model_get_item(monitors, 0);
    g_assert_true(is_monitor_valid(mon));
    g_object_unref(mon);
  }

  /* A pointer that is not part of the model is invalid. */
  g_assert_false(is_monitor_valid((GdkMonitor *)0xdeadbeef));
}

/* ============================================================================
 * DIMENSION UPDATES
 * ============================================================================ */

static void test_update_dock_dimensions_guards(void) {
  update_dock_dimensions(NULL);

  LambdockState no_widgets = {0};
  update_dock_dimensions(&no_widgets);

  LambdockState only_overlay = {0};
  only_overlay.overlay = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
  update_dock_dimensions(&only_overlay);
  g_object_ref_sink(only_overlay.overlay);
  g_object_unref(only_overlay.overlay);
}

static void test_update_dock_dimensions_horizontal(void) {
  LambdockState state = {0};
  state.overlay = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
  state.bg_capsule = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
  state.box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
  state.orientation = GTK_ORIENTATION_HORIZONTAL;
  state.edge = GTK_LAYER_SHELL_EDGE_BOTTOM;
  state.icon_size = 48;
  state.capsule_padding = CAPSULE_MAIN_AXIS_PADDING_PX;

  update_dock_dimensions(&state);

  int w = 0, h = 0;
  gtk_widget_get_size_request(state.overlay, &w, &h);
  g_assert_cmpint(h, ==, MAX_CROSS_DIM(48));
  g_assert_cmpint(w, ==, -1);

  gtk_widget_get_size_request(state.bg_capsule, &w, &h);
  g_assert_cmpint(h, ==, 48 + CAPSULE_PADDING_PX);
  g_assert_cmpint(w, ==, -1);

  g_assert_cmpint(gtk_widget_get_margin_start(state.box), ==,
                  CAPSULE_MAIN_AXIS_PADDING_PX);
  g_assert_cmpint(gtk_widget_get_margin_end(state.box), ==,
                  CAPSULE_MAIN_AXIS_PADDING_PX);

  g_object_ref_sink(state.overlay);
  g_object_unref(state.overlay);
  g_object_ref_sink(state.bg_capsule);
  g_object_unref(state.bg_capsule);
  g_object_ref_sink(state.box);
  g_object_unref(state.box);
}

static void test_capsule_padding_setting(void) {
  LambdockState state = {0};
  state.overlay = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
  state.bg_capsule = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
  state.box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
  state.orientation = GTK_ORIENTATION_HORIZONTAL;
  state.edge = GTK_LAYER_SHELL_EDGE_BOTTOM;
  state.icon_size = 48;

  /* `dock-capsule-padding' moves the icons in from the capsule's ends. */
  state.capsule_padding = 8;
  update_dock_dimensions(&state);
  g_assert_cmpint(gtk_widget_get_margin_start(state.box), ==, 8);
  g_assert_cmpint(gtk_widget_get_margin_end(state.box), ==, 8);

  /* Zero is a real value: icons flush with the ends of the capsule. */
  state.capsule_padding = 0;
  update_dock_dimensions(&state);
  g_assert_cmpint(gtk_widget_get_margin_start(state.box), ==, 0);
  g_assert_cmpint(gtk_widget_get_margin_end(state.box), ==, 0);

  /* The capsule's cross dimension does not depend on it. */
  g_assert_cmpint(dock_capsule_cross_dim(&state), ==,
                  48 + CAPSULE_PADDING_PX);

  g_object_ref_sink(state.overlay);
  g_object_unref(state.overlay);
  g_object_ref_sink(state.bg_capsule);
  g_object_unref(state.bg_capsule);
  g_object_ref_sink(state.box);
  g_object_unref(state.box);
}

static void test_update_dock_dimensions_vertical(void) {
  LambdockState state = {0};
  state.overlay = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
  state.bg_capsule = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
  state.box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
  state.orientation = GTK_ORIENTATION_VERTICAL;
  state.edge = GTK_LAYER_SHELL_EDGE_LEFT;
  state.icon_size = 40;
  state.capsule_padding = CAPSULE_MAIN_AXIS_PADDING_PX;

  update_dock_dimensions(&state);

  int w = 0, h = 0;
  gtk_widget_get_size_request(state.overlay, &w, &h);
  g_assert_cmpint(w, ==, MAX_CROSS_DIM(40));
  g_assert_cmpint(h, ==, -1);

  gtk_widget_get_size_request(state.bg_capsule, &w, &h);
  g_assert_cmpint(w, ==, 40 + CAPSULE_PADDING_PX);
  g_assert_cmpint(h, ==, -1);

  g_assert_cmpint(gtk_widget_get_margin_top(state.box), ==,
                  CAPSULE_MAIN_AXIS_PADDING_PX);
  g_assert_cmpint(gtk_widget_get_margin_bottom(state.box), ==,
                  CAPSULE_MAIN_AXIS_PADDING_PX);

  g_object_ref_sink(state.overlay);
  g_object_unref(state.overlay);
  g_object_ref_sink(state.bg_capsule);
  g_object_unref(state.bg_capsule);
  g_object_ref_sink(state.box);
  g_object_unref(state.box);
}

static void test_update_dock_dimensions_stale_monitor(void) {
  /* A stale monitor pointer must be detected and replaced by the default
   * monitor without touching a freed object. */
  LambdockState state = {0};
  state.overlay = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
  state.bg_capsule = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
  state.box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
  state.orientation = GTK_ORIENTATION_HORIZONTAL;
  state.icon_size = 48;
  state.capsule_padding = CAPSULE_MAIN_AXIS_PADDING_PX;
  state.monitor = (GdkMonitor *)0xdeadbeef;

  update_dock_dimensions(&state);

  g_assert_cmpint(gtk_widget_get_margin_start(state.box), ==,
                  CAPSULE_MAIN_AXIS_PADDING_PX);

  g_object_ref_sink(state.overlay);
  g_object_unref(state.overlay);
  g_object_ref_sink(state.bg_capsule);
  g_object_unref(state.bg_capsule);
  g_object_ref_sink(state.box);
  g_object_unref(state.box);
}

/* ============================================================================
 * TASKBAR MODE
 * ============================================================================ */

static void test_apply_dock_mode_classes(void) {
  LambdockState state = {0};
  state.bg_capsule = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);

  apply_dock_mode_classes(&state);
  g_assert_false(gtk_widget_has_css_class(state.bg_capsule, "taskbar"));

  state.taskbar = TRUE;
  apply_dock_mode_classes(&state);
  g_assert_true(gtk_widget_has_css_class(state.bg_capsule, "taskbar"));

  /* Idempotent when already applied. */
  apply_dock_mode_classes(&state);
  g_assert_true(gtk_widget_has_css_class(state.bg_capsule, "taskbar"));

  state.taskbar = FALSE;
  apply_dock_mode_classes(&state);
  g_assert_false(gtk_widget_has_css_class(state.bg_capsule, "taskbar"));

  /* NULL guards. */
  apply_dock_mode_classes(NULL);

  g_object_ref_sink(state.bg_capsule);
  g_object_unref(state.bg_capsule);
}

static void test_apply_dock_mode_classes_shelf(void) {
  LambdockState state = {0};
  state.bg_capsule = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
  state.window = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
  state.edge = GTK_LAYER_SHELL_EDGE_BOTTOM;
  state.shelf = lambdock_shelf_params_default();

  /* Plain capsule: no shelf classes anywhere. */
  apply_dock_mode_classes(&state);
  g_assert_false(gtk_widget_has_css_class(state.bg_capsule, "shelf-mode"));
  g_assert_false(gtk_widget_has_css_class(state.bg_capsule, "shelf-bottom"));
  g_assert_false(gtk_widget_has_css_class(state.window, "shelf-mode"));

  /* Any non-default shelf parameter tags both widgets plus the docked edge. */
  state.shelf.tilt_deg = 14.0;
  apply_dock_mode_classes(&state);
  g_assert_true(gtk_widget_has_css_class(state.bg_capsule, "shelf-mode"));
  g_assert_true(gtk_widget_has_css_class(state.window, "shelf-mode"));
  g_assert_true(gtk_widget_has_css_class(state.bg_capsule, "shelf-bottom"));
  g_assert_true(gtk_widget_has_css_class(state.window, "shelf-bottom"));
  g_assert_false(gtk_widget_has_css_class(state.bg_capsule, "shelf-top"));

  /* Switching the edge replaces the edge class instead of accumulating it. */
  state.edge = GTK_LAYER_SHELL_EDGE_TOP;
  apply_dock_mode_classes(&state);
  g_assert_true(gtk_widget_has_css_class(state.bg_capsule, "shelf-top"));
  g_assert_false(gtk_widget_has_css_class(state.bg_capsule, "shelf-bottom"));

  state.edge = GTK_LAYER_SHELL_EDGE_LEFT;
  apply_dock_mode_classes(&state);
  g_assert_true(gtk_widget_has_css_class(state.bg_capsule, "shelf-left"));
  g_assert_false(gtk_widget_has_css_class(state.bg_capsule, "shelf-top"));

  /* Reflection-only shelf parameters count as active too. */
  state.shelf = lambdock_shelf_params_default();
  state.edge = GTK_LAYER_SHELL_EDGE_BOTTOM;
  state.shelf.reflection_ratio = 0.5;
  apply_dock_mode_classes(&state);
  g_assert_true(gtk_widget_has_css_class(state.bg_capsule, "shelf-mode"));

  /* Going back to the defaults removes every shelf class again. */
  state.shelf = lambdock_shelf_params_default();
  apply_dock_mode_classes(&state);
  g_assert_false(gtk_widget_has_css_class(state.bg_capsule, "shelf-mode"));
  g_assert_false(gtk_widget_has_css_class(state.bg_capsule, "shelf-bottom"));
  g_assert_false(gtk_widget_has_css_class(state.window, "shelf-mode"));

  g_object_ref_sink(state.bg_capsule);
  g_object_unref(state.bg_capsule);
  g_object_ref_sink(state.window);
  g_object_unref(state.window);
}

static void test_update_dock_dimensions_shelf(void) {
  LambdockState state = {0};
  state.overlay = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
  state.bg_capsule = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
  state.box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
  state.orientation = GTK_ORIENTATION_HORIZONTAL;
  state.edge = GTK_LAYER_SHELL_EDGE_BOTTOM;
  state.icon_size = 48;
  state.capsule_padding = CAPSULE_MAIN_AXIS_PADDING_PX;
  state.shelf = lambdock_shelf_params_default();
  state.shelf.tilt_deg = 14.0;
  state.shelf.margin_px = -20;
  state.shelf.front_px = 24;
  state.shelf.reflection_ratio = 0.25; /* band = 12px, inside the 24px front */

  update_dock_dimensions(&state);

  int w = 0, h = 0;
  gtk_widget_get_size_request(state.overlay, &w, &h);
  g_assert_cmpint(h, ==, dock_cross_axis_dim(&state));
  g_assert_cmpint(h, >, MAX_CROSS_DIM(48)); /* reflection band is reserved */

  gtk_widget_get_size_request(state.bg_capsule, &w, &h);
  g_assert_cmpint(h, ==, dock_capsule_cross_dim(&state));

  /* The mirrors live *inside* the front glass: the icons stop `front` pixels
   * from the screen edge and the reflection widget fills the band below them,
   * so the offset the layout has to apply is front - band. That is what puts the
   * icons' centre on the slab's top edge, as in the OS X dock. */
  int band = (int)round(48 * 0.25);
  g_assert_cmpint(gtk_widget_get_margin_bottom(state.box), ==, 24 - band);

  /* A top dock mirrors that margin and keeps the same dimensions. */
  state.edge = GTK_LAYER_SHELL_EDGE_TOP;
  update_dock_dimensions(&state);
  g_assert_cmpint(gtk_widget_get_margin_top(state.box), ==, 24 - band);
  g_assert_cmpint(gtk_widget_get_margin_bottom(state.box), ==, 0);

  /* When the mirror is taller than the front glass, the glass grows to hold it
   * and there is nothing left to offset: the band occupies all of it. */
  state.edge = GTK_LAYER_SHELL_EDGE_BOTTOM;
  state.shelf.reflection_ratio = 0.75; /* band = 36px > front 24px */
  update_dock_dimensions(&state);
  g_assert_cmpint(gtk_widget_get_margin_bottom(state.box), ==, 0);

  /* Back to the defaults restores the classic floating capsule dimensions. */
  state.shelf = lambdock_shelf_params_default();
  state.edge = GTK_LAYER_SHELL_EDGE_BOTTOM;
  update_dock_dimensions(&state);
  gtk_widget_get_size_request(state.bg_capsule, &w, &h);
  g_assert_cmpint(h, ==, 48 + CAPSULE_PADDING_PX);
  gtk_widget_get_size_request(state.overlay, &w, &h);
  g_assert_cmpint(h, ==, MAX_CROSS_DIM(48));
  g_assert_cmpint(gtk_widget_get_margin_bottom(state.box), ==, 0);

  g_object_ref_sink(state.overlay);
  g_object_unref(state.overlay);
  g_object_ref_sink(state.bg_capsule);
  g_object_unref(state.bg_capsule);
  g_object_ref_sink(state.box);
  g_object_unref(state.box);
}

static void test_update_dock_dimensions_taskbar(void) {  LambdockState state = {0};
  state.overlay = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
  state.bg_capsule = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
  state.box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
  state.scrolled_window = gtk_scrolled_window_new();
  state.orientation = GTK_ORIENTATION_HORIZONTAL;
  state.edge = GTK_LAYER_SHELL_EDGE_BOTTOM;
  state.icon_size = 48;
  state.taskbar = TRUE;

  update_dock_dimensions(&state);

  g_assert_cmpint(gtk_widget_get_halign(state.bg_capsule), ==, GTK_ALIGN_FILL);
  g_assert_cmpint(gtk_widget_get_halign(state.box), ==, GTK_ALIGN_START);
  g_assert_cmpint(gtk_widget_get_halign(state.scrolled_window), ==,
                  GTK_ALIGN_FILL);
  g_assert_true(gtk_widget_get_hexpand(state.scrolled_window));

  /* Switching back to dock mode recenters and stops expanding. */
  state.taskbar = FALSE;
  update_dock_dimensions(&state);

  g_assert_cmpint(gtk_widget_get_halign(state.bg_capsule), ==,
                  GTK_ALIGN_CENTER);
  g_assert_cmpint(gtk_widget_get_halign(state.box), ==, GTK_ALIGN_CENTER);
  g_assert_cmpint(gtk_widget_get_halign(state.scrolled_window), ==,
                  GTK_ALIGN_CENTER);
  g_assert_false(gtk_widget_get_hexpand(state.scrolled_window));

  g_object_ref_sink(state.overlay);
  g_object_unref(state.overlay);
  g_object_ref_sink(state.bg_capsule);
  g_object_unref(state.bg_capsule);
  g_object_ref_sink(state.box);
  g_object_unref(state.box);
  g_object_ref_sink(state.scrolled_window);
  g_object_unref(state.scrolled_window);
}

/* ============================================================================
 * POPOVER COUNTER / AUTO-HIDE
 * ============================================================================ */

static void test_on_popover_closed(void) {
  LambdockState state = {0};
  state.auto_hide = TRUE;
  state.active_popovers = 2;

  on_popover_closed(NULL, &state);
  g_assert_cmpint(state.active_popovers, ==, 1);
  /* A popover is still open: no hide timer yet. */
  g_assert_cmpint(state.hide_timer_id, ==, 0);

  on_popover_closed(NULL, &state);
  g_assert_cmpint(state.active_popovers, ==, 0);
  /* Last popover closed -> schedule an auto-hide. */
  g_assert_cmpint(state.hide_timer_id, !=, 0);
  g_source_remove(state.hide_timer_id);

  /* A spurious close when nothing is open must not drive the counter
   * negative; with auto-hide on it still (re)schedules a hide. */
  LambdockState idle = {0};
  idle.auto_hide = TRUE;
  on_popover_closed(NULL, &idle);
  g_assert_cmpint(idle.active_popovers, ==, 0);
  g_assert_cmpint(idle.hide_timer_id, !=, 0);
  g_source_remove(idle.hide_timer_id);

  /* With auto-hide disabled, closing a popover schedules nothing. */
  LambdockState no_auto = {0};
  no_auto.auto_hide = FALSE;
  no_auto.active_popovers = 1;
  on_popover_closed(NULL, &no_auto);
  g_assert_cmpint(no_auto.active_popovers, ==, 0);
  g_assert_cmpint(no_auto.hide_timer_id, ==, 0);
}

static void test_hide_dock_guards(void) {
  /* Auto-hide disabled: nothing happens. */
  LambdockState s1 = {0};
  hide_dock(&s1);
  g_assert_false(s1.is_hidden);

  /* An open popover blocks hiding. */
  LambdockState s2 = {0};
  s2.auto_hide = TRUE;
  s2.active_popovers = 1;
  hide_dock(&s2);
  g_assert_false(s2.is_hidden);

  /* show_when_clear with no tracked windows keeps the dock visible. */
  LambdockState s3 = {0};
  s3.auto_hide = TRUE;
  s3.show_when_clear = TRUE;
  s3.window_trackers = NULL;
  hide_dock(&s3);
  g_assert_false(s3.is_hidden);
}

static gboolean dummy_timeout_cb(gpointer data) {
  (void)data;
  return G_SOURCE_REMOVE;
}

static void test_reveal_dock_cancels_timer(void) {
  LambdockState state = {0};
  state.hide_timer_id = g_timeout_add(60000, dummy_timeout_cb, NULL);

  reveal_dock(&state);
  g_assert_cmpint(state.hide_timer_id, ==, 0);
  g_assert_false(state.is_hidden);
}

/* A tick callback only has to exist: the animation it belongs to never runs in
 * a test, and the point is the state the reveal leaves behind. */
static gboolean dummy_tick_cb(GtkWidget *widget, GdkFrameClock *clock,
                              gpointer data) {
  (void)widget;
  (void)clock;
  (void)data;
  return G_SOURCE_REMOVE;
}

/* A reveal can arrive while a hide is already on its way: the dock starts
 * sliding away and the pointer lands on the trigger strip, or the window
 * tracker asks to hide while the pointer is right at the edge.
 *
 * The reveal used to be dropped in that case. The dock was still at margin 0 --
 * the hide had been *requested* but had not had a frame yet -- so "already
 * where it is wanted" looked true, the reveal returned without doing anything,
 * and the hide went on. The dock then sat off screen believing it was visible,
 * and could not be brought back by a pointer that stayed on the strip: the state
 * said it was already out, so nothing revealed it, and the only thing that
 * would have (a crossing) needs the pointer to leave and come back. That is a
 * dock that hides but does not reveal. */
static void test_reveal_turns_a_pending_hide_around(void) {
  GtkWidget *window = gtk_window_new();

  LambdockState state = {0};
  state.window = window;
  state.is_hidden = TRUE;
  state.current_margin = 0;   /* the hide is requested, not ticked yet */
  state.target_margin = -144; /* ... and it is heading off screen */
  state.tick_cb_id =
      gtk_widget_add_tick_callback(window, dummy_tick_cb, NULL, NULL);

  reveal_dock(&state);

  g_assert_false(state.is_hidden);
  g_assert_cmpint(state.current_margin, ==, 0);
  /* The target is what the dock is *going* to be: leaving it below zero would
   * mean the reveal asked for nothing and the slide away continues. */
  g_assert_cmpint(state.target_margin, ==, 0);

  if (state.tick_cb_id != 0)
    gtk_widget_remove_tick_callback(window, state.tick_cb_id);
  gtk_window_destroy(GTK_WINDOW(window));
}

/* The mirror image: a hide that is already sliding has to be turned around by a
 * reveal too, and has to slide back from where the dock actually is rather than
 * jumping. */
static void test_reveal_mid_slide_starts_from_where_the_dock_is(void) {
  GtkWidget *window = gtk_window_new();

  LambdockState state = {0};
  state.window = window;
  state.is_hidden = TRUE;
  state.current_margin = -96;
  state.target_margin = -144;
  state.tick_cb_id =
      gtk_widget_add_tick_callback(window, dummy_tick_cb, NULL, NULL);

  reveal_dock(&state);

  g_assert_false(state.is_hidden);
  g_assert_cmpint(state.target_margin, ==, 0);
  /* The slide back starts from where the dock is, not from 0: anything else
   * snaps the dock into place before it moves. */
  g_assert_cmpint(state.current_margin, ==, -96);

  if (state.tick_cb_id != 0)
    gtk_widget_remove_tick_callback(window, state.tick_cb_id);
  gtk_window_destroy(GTK_WINDOW(window));
}

/* ============================================================================
 * AUTO-HIDE: CROSSINGS THAT ARE ECHOES OF THE DOCK ITSELF
 * ============================================================================ */

/* The dock slides under a pointer that is not moving, and every slide ends with
 * the compositor reporting crossings that follow from the surface moving rather
 * than from the pointer. Acting on those is what bounced the dock at the screen
 * edge, so a settling dock's crossings have to be ignored -- but only for a
 * moment: once it has been still for a while, a leave is the user leaving. */
static void test_dock_is_settling(void) {
  LambdockState state = {0};

  g_assert_false(dock_is_settling(&state));
  g_assert_false(dock_is_settling(NULL));

  /* A slide in progress is always settling. */
  state.tick_cb_id = 1;
  g_assert_true(dock_is_settling(&state));
  state.tick_cb_id = 0;

  /* So is one that has just stopped... */
  state.margin_anim_end_us = g_get_monotonic_time();
  g_assert_true(dock_is_settling(&state));

  /* ...but not one that stopped a while ago, or the dock would never hide. */
  state.margin_anim_end_us = g_get_monotonic_time() - 5 * 1000 * 1000;
  g_assert_false(dock_is_settling(&state));
}

/* ============================================================================
 * TEARDOWN
 * ============================================================================ */

static void test_destroy_dock_instance_reentrancy(void) {
  /* When a teardown is already in progress, the call must be a no-op so the
   * state is not freed twice. */
  LambdockState *state = g_new0(LambdockState, 1);
  state->is_destroying = TRUE;
  destroy_dock_instance(state);
  g_free(state);

  destroy_dock_instance(NULL);
}

static void test_destroy_dock_instance_cleans_up(void) {
  LambdockState *state = g_new0(LambdockState, 1);

  /* window == NULL keeps us off the Wayland layer-shell code paths. */
  state->config_path = g_strdup("/tmp/lambdock-test.scm");
  state->active_theme_class = g_strdup("theme-vanilla");
  state->theme = strdup("vanilla"); /* freed with free() by the destructor */
  state->show_when_clear = FALSE;

  DockItem *item = g_new0(DockItem, 1);
  item->state = state;
  item->poll_fn = NULL;
  item->button = gtk_button_new();
  state->dock_items = g_list_append(state->dock_items, item);

  state->window_trackers = g_list_append(state->window_trackers, (void *)0x1);

  state->popover = gtk_popover_new();

  /* Nothing to assert afterwards (state is freed), but it must not leak or
   * crash. */
  destroy_dock_instance(state);
}

int main(int argc, char **argv) {
  g_test_init(&argc, &argv, NULL);

  if (!gtk_init_check()) {
    g_print("Skipping dock window tests: No display server available\n");
    return 77;
  }

  g_test_add_func("/ui/dock_window/monitor_valid", test_is_monitor_valid);
  g_test_add_func("/ui/dock_window/dimensions_guards",
                  test_update_dock_dimensions_guards);
  g_test_add_func("/ui/dock_window/capsule_padding",
                  test_capsule_padding_setting);
  g_test_add_func("/ui/dock_window/dimensions_horizontal",
                  test_update_dock_dimensions_horizontal);
  g_test_add_func("/ui/dock_window/dimensions_vertical",
                  test_update_dock_dimensions_vertical);
  g_test_add_func("/ui/dock_window/dimensions_stale_monitor",
                  test_update_dock_dimensions_stale_monitor);
  g_test_add_func("/ui/dock_window/apply_dock_mode_classes",
                  test_apply_dock_mode_classes);
  g_test_add_func("/ui/dock_window/apply_dock_mode_classes_shelf",
                  test_apply_dock_mode_classes_shelf);
  g_test_add_func("/ui/dock_window/dimensions_shelf",
                  test_update_dock_dimensions_shelf);
  g_test_add_func("/ui/dock_window/dimensions_taskbar",
                  test_update_dock_dimensions_taskbar);
  g_test_add_func("/ui/dock_window/popover_closed", test_on_popover_closed);
  g_test_add_func("/ui/dock_window/hide_guards", test_hide_dock_guards);
  g_test_add_func("/ui/dock_window/reveal_cancels_timer",
                  test_reveal_dock_cancels_timer);
  g_test_add_func("/ui/dock_window/reveal_turns_a_pending_hide_around",
                  test_reveal_turns_a_pending_hide_around);
  g_test_add_func("/ui/dock_window/reveal_mid_slide",
                  test_reveal_mid_slide_starts_from_where_the_dock_is);
  g_test_add_func("/ui/dock_window/dock_is_settling", test_dock_is_settling);
  g_test_add_func("/ui/dock_window/destroy_reentrancy",
                  test_destroy_dock_instance_reentrancy);
  g_test_add_func("/ui/dock_window/destroy_cleans_up",
                  test_destroy_dock_instance_cleans_up);

  return g_test_run();
}
