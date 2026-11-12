// lambdock -- hyper-hackable desktop dock for Wayland
//
// SPDX-License-Identifier: MIT
// Copyright © Josep Bigorra <jjbigorra@gmail.com>

/* The dock as an X11 window, asserted against the X server itself.
 *
 * Every other test checks a value the dock computed; this one checks what the
 * desktop would actually read: the window type that makes a window manager
 * treat the dock as a dock and not as an application, and the strut that makes
 * a maximized window stop at the dock's edge instead of under it. Both are
 * properties of the real window on the real (virtual) X server, so the test
 * fails if the dock stops publishing them, not if it computes different
 * numbers.
 */

#include "../src/dock_window.h"
#include "../src/lambdock.h"
#include "../src/x11_tracker.h"
#include <gtk/gtk.h>

#if defined(HAVE_X11)
#include <gdk/x11/gdkx.h>
#include <X11/Xatom.h>
#include <X11/Xlib.h>

/* ---- reading properties back off the server ---- */

static Atom atom_for(Display *display, const char *name) {
  return XInternAtom(display, name, False);
}

/** TRUE when @property of @window contains @value. */
static gboolean property_has_atom(Display *display, Window window,
                                  const char *property, const char *value) {
  Atom type = None;
  int format = 0;
  unsigned long n_items = 0;
  unsigned long bytes_after = 0;
  unsigned char *data = NULL;

  if (XGetWindowProperty(display, window, atom_for(display, property), 0, 32,
                         False, XA_ATOM, &type, &format, &n_items,
                         &bytes_after, &data) != Success ||
      !data)
    return FALSE;

  Atom wanted = atom_for(display, value);
  gboolean found = FALSE;
  for (unsigned long i = 0; i < n_items && !found; i++)
    found = ((Atom *)data)[i] == wanted;

  XFree(data);
  return found;
}

/** Reads a 32-bit cardinal list property into @values. */
static gboolean property_cardinals(Display *display, Window window,
                                   const char *property, long *values,
                                   unsigned long expected) {
  Atom type = None;
  int format = 0;
  unsigned long n_items = 0;
  unsigned long bytes_after = 0;
  unsigned char *data = NULL;

  if (XGetWindowProperty(display, window, atom_for(display, property), 0, 32,
                         False, XA_CARDINAL, &type, &format, &n_items,
                         &bytes_after, &data) != Success ||
      !data)
    return FALSE;

  gboolean ok = n_items >= expected;
  for (unsigned long i = 0; ok && i < expected; i++)
    values[i] = ((long *)data)[i];

  XFree(data);
  return ok;
}

/* ---- a dock, as the application builds one ---- */

/** The parts of a dock the placement path needs, in the order it needs them. */
static void build_dock(LambdockState *state) {
  state->window = gtk_window_new();
  gtk_window_set_default_size(GTK_WINDOW(state->window), 640, 96);

  state->overlay = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
  state->bg_capsule = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
  state->box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);

  gtk_box_append(GTK_BOX(state->overlay), state->bg_capsule);
  gtk_window_set_child(GTK_WINDOW(state->window), state->overlay);

  state->edge = GTK_LAYER_SHELL_EDGE_BOTTOM;
  state->orientation = GTK_ORIENTATION_HORIZONTAL;
  state->icon_size = DEFAULT_ICON_SIZE;
  state->capsule_padding = CAPSULE_MAIN_AXIS_PADDING_PX;

  /* Wayland only: on X11 this is the documented no-op, and the dock then places
   * itself through EWMH. It logs instead of warning there, because the dock is
   * not degraded -- it is docking another way. */
  setup_layer_shell_window(state);
}

/** Pumps the main loop until the window is realised and placed. */
static void settle(LambdockState *state) {
  for (int i = 0; i < 40; i++) {
    g_main_context_iteration(NULL, FALSE);
    update_dock_dimensions(state);
  }
}

/* ---- the tests ---- */

static void test_dock_window_is_an_x11_dock(void) {
  LambdockState state = {0};
  build_dock(&state);
  gtk_window_present(GTK_WINDOW(state.window));
  settle(&state);

  g_assert_cmpuint(state.x11_window, !=, 0);

  Display *display = XOpenDisplay(NULL);
  g_assert_nonnull(display);

  Window window = (Window)state.x11_window;

  /* The type is what makes a window manager place the dock like a panel
   * instead of tiling it like an application window. */
  g_assert_true(property_has_atom(display, window, "_NET_WM_WINDOW_TYPE",
                                  "_NET_WM_WINDOW_TYPE_DOCK"));

  /* Above other windows, on every desktop, out of taskbars and pagers. */
  g_assert_true(property_has_atom(display, window, "_NET_WM_STATE",
                                  "_NET_WM_STATE_ABOVE"));
  g_assert_true(property_has_atom(display, window, "_NET_WM_STATE",
                                  "_NET_WM_STATE_STICKY"));
  g_assert_true(property_has_atom(display, window, "_NET_WM_STATE",
                                  "_NET_WM_STATE_SKIP_TASKBAR"));

  XCloseDisplay(display);
  gtk_window_destroy(GTK_WINDOW(state.window));
}

static void test_dock_reserves_its_edge(void) {
  LambdockState state = {0};
  build_dock(&state);
  gtk_window_present(GTK_WINDOW(state.window));
  settle(&state);

  Display *display = XOpenDisplay(NULL);
  g_assert_nonnull(display);

  Window window = (Window)state.x11_window;
  long partial[12] = {0};

  /* _NET_WM_STRUT_PARTIAL: left, right, top, bottom, then the four ranges. */
  g_assert_true(
      property_cardinals(display, window, "_NET_WM_STRUT_PARTIAL", partial, 12));

  int capsule = dock_capsule_cross_dim(&state);
  g_assert_cmpint(partial[3], ==, capsule); /* bottom */
  g_assert_cmpint(partial[2], ==, 0);       /* top */
  g_assert_cmpint(partial[0], ==, 0);       /* left */
  g_assert_cmpint(partial[1], ==, 0);       /* right */

  /* The reservation spans the dock and nothing else: indices 10 and 11 are
   * `bottom_start_x` and `bottom_end_x`. */
  g_assert_cmpint(partial[10], ==, state.x11_rect_x);
  g_assert_cmpint(partial[11], ==, state.x11_rect_x + state.x11_rect_width - 1);

  /* A bottom dock sits on the bottom edge of the desktop, measured on the same
   * server the dock placed itself on. */
  int screen_height = DisplayHeight(display, DefaultScreen(display));
  g_assert_cmpint(state.x11_rect_y, ==, screen_height - state.x11_rect_height);

  /* The four-value hint says the same thing, for window managers that never
   * learned the partial one. */
  long struts[4] = {0};
  g_assert_true(property_cardinals(display, window, "_NET_WM_STRUT", struts, 4));
  g_assert_cmpint(struts[3], ==, capsule);

  XCloseDisplay(display);
  gtk_window_destroy(GTK_WINDOW(state.window));
}

static void test_placement_is_not_reapplied(void) {
  LambdockState state = {0};
  build_dock(&state);
  gtk_window_present(GTK_WINDOW(state.window));
  settle(&state);

  /* A placement that has not changed must not move the window again: every
   * move is a configure round trip, and this runs on every reload. */
  int x = state.x11_rect_x;
  int width = state.x11_rect_width;
  g_assert_true(state.x11_placed);

  update_dock_dimensions(&state);

  g_assert_cmpint(state.x11_rect_x, ==, x);
  g_assert_cmpint(state.x11_rect_width, ==, width);

  gtk_window_destroy(GTK_WINDOW(state.window));
}

#endif /* HAVE_X11 */

int main(int argc, char **argv) {
  g_test_init(&argc, &argv, NULL);

#if defined(HAVE_X11)
  if (!gtk_init_check()) {
    g_print("Skipping the X11 dock tests: no display server available\n");
    return 77;
  }

  if (!GDK_IS_X11_DISPLAY(gdk_display_get_default())) {
    g_print("Skipping the X11 dock tests: not an X11 session\n");
    return 77;
  }

  g_test_add_func("/x11/dock_window_type", test_dock_window_is_an_x11_dock);
  g_test_add_func("/x11/dock_reserves_its_edge", test_dock_reserves_its_edge);
  g_test_add_func("/x11/placement_is_not_reapplied",
                  test_placement_is_not_reapplied);
#else
  g_print("Skipping the X11 dock tests: built without X11 support\n");
  return 77;
#endif

  return g_test_run();
}
