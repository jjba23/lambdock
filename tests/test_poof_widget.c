// lambdock -- hyper-hackable desktop dock for Wayland
//
// SPDX-License-Identifier: MIT
// Copyright © Josep Bigorra <jjbigorra@gmail.com>

#include "../src/ui/poof.h"

/* The burst's shape is tested as arithmetic in tests/test_poof.c. What is left
 * to prove here is the widget's own life: it takes the removed icon's place in
 * the dock's box, it keeps that footprint while it plays, and it takes itself
 * out again when it is done -- which is the part that could leak or crash. */

/* Spins the context until @predicate is true, up to about a second. */
static gboolean wait_for(gboolean (*predicate)(gpointer), gpointer data) {
  for (int i = 0; i < 1000; i++) {
    if (predicate(data))
      return TRUE;
    g_main_context_iteration(NULL, FALSE);
    g_usleep(1000);
  }
  return predicate(data);
}

static gboolean box_has_one_child(gpointer data) {
  GtkWidget *first = gtk_widget_get_first_child(GTK_WIDGET(data));
  return first != NULL && gtk_widget_get_next_sibling(first) == NULL;
}

static gboolean box_has_no_poof(gpointer data) {
  for (GtkWidget *child = gtk_widget_get_first_child(GTK_WIDGET(data));
       child != NULL; child = gtk_widget_get_next_sibling(child)) {
    if (gtk_widget_has_css_class(child, "dock-poof"))
      return FALSE;
  }
  return TRUE;
}

/** Spins the context until @widget has an allocation. */
static void wait_allocated(GtkWidget *widget) {
  for (int i = 0; i < 400 && gtk_widget_get_width(widget) <= 0; i++) {
    g_main_context_iteration(NULL, FALSE);
    g_usleep(1000);
  }
}

/** Finds the poof widget in a box by its class. */
static GtkWidget *find_poof(GtkWidget *box) {
  for (GtkWidget *child = gtk_widget_get_first_child(box); child != NULL;
       child = gtk_widget_get_next_sibling(child)) {
    if (gtk_widget_has_css_class(child, "dock-poof"))
      return child;
  }
  return NULL;
}

static void test_poof_takes_the_slot_and_leaves_it(void) {
  GtkWidget *window = gtk_window_new();
  GtkWidget *box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
  GtkWidget *icon = gtk_label_new("icon");
  gtk_widget_set_size_request(icon, 48, 48);
  gtk_box_append(GTK_BOX(box), icon);
  gtk_window_set_child(GTK_WINDOW(window), box);
  gtk_window_set_default_size(GTK_WINDOW(window), 240, 120);
  gtk_window_present(GTK_WINDOW(window));

  for (int i = 0; i < 400 && gtk_widget_get_width(icon) <= 0; i++) {
    g_main_context_iteration(NULL, FALSE);
    g_usleep(1000);
  }

  if (gtk_widget_get_width(icon) <= 0) {
    g_test_skip("window was never allocated");
    gtk_window_destroy(GTK_WINDOW(window));
    return;
  }

  /* The dock's box, and an item standing in for the icon being removed. */
  LambdockState state = {0};
  state.window = window;
  state.box = box;

  DockItem item = {0};
  item.state = &state;
  item.button = icon;

  g_assert_true(lambdock_poof_take_slot(&item));

  /* It sits exactly where the icon was: same size, same position in the box. */
  GtkWidget *poof = find_poof(box);
  g_assert_nonnull(poof);
  wait_allocated(poof);
  g_assert_cmpint(gtk_widget_get_width(poof), ==, 48);
  g_assert_cmpint(gtk_widget_get_height(poof), >, 0);
  g_assert_true(gtk_widget_get_next_sibling(icon) == poof);

  /* ... and it paints something while it plays. The burst is empty on the frame
   * the clock starts it, so wait for the first frame that actually has a puff
   * instead of for a wall-clock delay: a busy builder may not have delivered one
   * yet. */
  GskRenderNode *node = NULL;
  for (int i = 0; i < 600 && node == NULL; i++) {
    g_main_context_iteration(NULL, FALSE);
    g_usleep(1000);

    if (!find_poof(box))
      break;

    GtkSnapshot *snapshot = gtk_snapshot_new();
    gtk_widget_snapshot_child(box, poof, snapshot);
    node = gtk_snapshot_free_to_node(snapshot);
  }
  g_assert_nonnull(node);

  graphene_rect_t bounds;
  gsk_render_node_get_bounds(node, &bounds);
  g_assert_cmpfloat(bounds.size.width, >, 0.0);
  g_assert_cmpfloat(bounds.size.height, >, 0.0);
  /* An icon's worth of smoke, however tall the box is: the puffs are laid out
   * in the icon's square and end at its edge -- nowhere near the 1156px box the
   * poof sits in. */
  g_assert_cmpfloat(bounds.size.height, <=, 52.0);
  g_assert_cmpfloat(bounds.size.width, <=, 52.0);
  gsk_render_node_unref(node);

  /* Removing the item (as the drag-out path does) leaves the puff behind, and
   * the puff clears itself out when the animation is over. */
  gtk_box_remove(GTK_BOX(box), icon);
  g_assert_true(box_has_one_child(box));
  g_assert_nonnull(find_poof(box));

  g_assert_true(wait_for(box_has_no_poof, box));
  g_assert_null(find_poof(box));
  /* Nothing else was ever in this box: the icon was removed and the smoke
   * cleared itself, so the dock's box is empty again. */
  g_assert_null(gtk_widget_get_first_child(box));

  gtk_window_destroy(GTK_WINDOW(window));
}

/* An item that is not in the dock's box, or a dock that is not on screen, has
 * nothing to animate: the caller is told so and simply removes the item. */
static void test_no_slot_no_burst(void) {
  LambdockState state = {0};
  GtkWidget *box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
  GtkWidget *icon = gtk_label_new("icon");
  gtk_box_append(GTK_BOX(box), icon);
  state.box = box;

  DockItem item = {0};
  item.state = &state;
  item.button = icon;

  /* No state at all. */
  g_assert_false(lambdock_poof_take_slot(NULL));

  /* No dock window: the item is not in a dock. */
  g_assert_false(lambdock_poof_take_slot(&item));
  g_assert_null(find_poof(box));

  /* A dock whose box exists but the item is elsewhere. */
  GtkWidget *other = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
  GtkWidget *stray = gtk_label_new("stray");
  gtk_box_append(GTK_BOX(other), stray);
  item.button = stray;
  g_assert_false(lambdock_poof_take_slot(&item));

  g_object_ref_sink(box);
  g_object_unref(box);
  g_object_ref_sink(other);
  g_object_unref(other);
}

int main(int argc, char **argv) {
  g_test_init(&argc, &argv, NULL);

  /* This test animates and renders real frames. The default Vulkan renderer
   * reports a swapchain warning on some compositors, and the test harness treats
   * warnings as fatal, so render with cairo: the burst's pixels are identical and
   * the test is about the widget's life, not about the GPU. */
  g_setenv("GSK_RENDERER", "cairo", TRUE);

  /* Both cases below build widgets, so there is nothing to run on a machine
   * with no display server. The burst's own arithmetic is in test_poof. */
  if (!gtk_init_check()) {
    g_print("Skipping poof widget tests: No display server\n");
    return g_test_run();
  }

  g_test_add_func("/ui/poof/no-slot", test_no_slot_no_burst);
  g_test_add_func("/ui/poof/takes-slot", test_poof_takes_the_slot_and_leaves_it);

  return g_test_run();
}
