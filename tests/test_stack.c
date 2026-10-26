// lambdock

// Copyright © Josep Bigorra <jjbigorra@gmail.com>

// lambdock is free software : you can redistribute it and / or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.

// lambdock is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY ; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
// GNU General Public License for more details.

// You should have received a copy of the GNU General Public License
// along with lambdock. If not, see <https://www.gnu.org/licenses/>.

#include "../src/ui/stack.h"
#include <gdk-pixbuf/gdk-pixbuf.h>
#include <glib/gstdio.h>
#include <gtk/gtk.h>
#include <math.h>

/* ============================================================================
 * PATH HANDLING
 * ============================================================================ */

static void test_expand_path(void) {
  char *path = lambdock_stack_expand_path("~");
  g_assert_cmpstr(path, ==, g_get_home_dir());
  g_free(path);

  path = lambdock_stack_expand_path("~/Downloads");
  char *expected = g_build_filename(g_get_home_dir(), "Downloads", NULL);
  g_assert_cmpstr(path, ==, expected);
  g_free(expected);
  g_free(path);

  /* Absolute and relative paths are passed through untouched. */
  path = lambdock_stack_expand_path("/usr/share");
  g_assert_cmpstr(path, ==, "/usr/share");
  g_free(path);

  g_assert_null(lambdock_stack_expand_path(NULL));
  g_assert_null(lambdock_stack_expand_path(""));
}

/* ============================================================================
 * DIRECTORY LISTING
 * ============================================================================ */

static void test_list_directory(void) {
  char *dir = g_dir_make_tmp("lambdock-stack-XXXXXX", NULL);
  g_assert_nonnull(dir);

  char *sub = g_build_filename(dir, "zdir", NULL);
  g_assert_cmpint(g_mkdir_with_parents(sub, 0755), ==, 0);
  g_free(sub);

  char *file_b = g_build_filename(dir, "beta.txt", NULL);
  g_assert_true(g_file_set_contents(file_b, "b", -1, NULL));
  g_free(file_b);

  char *file_a = g_build_filename(dir, "Alpha.txt", NULL);
  g_assert_true(g_file_set_contents(file_a, "a", -1, NULL));
  g_free(file_a);

  char *hidden = g_build_filename(dir, ".hidden", NULL);
  g_assert_true(g_file_set_contents(hidden, "h", -1, NULL));
  g_free(hidden);

  GPtrArray *entries = lambdock_stack_list_directory(dir);
  g_assert_nonnull(entries);
  g_assert_cmpuint(entries->len, ==, 3);

  /* Folders first, then names in locale order, and dot files never show up. */
  LambdockStackEntry *entry = g_ptr_array_index(entries, 0);
  g_assert_true(entry->is_dir);
  g_assert_cmpstr(entry->name, ==, "zdir");
  g_assert_cmpstr(((LambdockStackEntry *)g_ptr_array_index(entries, 1))->name,
                  ==, "Alpha.txt");
  g_assert_cmpstr(((LambdockStackEntry *)g_ptr_array_index(entries, 2))->name,
                  ==, "beta.txt");

  /* The entry paths are absolute and usable. */
  g_assert_true(((LambdockStackEntry *)g_ptr_array_index(entries, 1))->path[0] ==
                '/');

  lambdock_stack_entries_free(entries);

  /* A missing directory is an empty list, not a crash (it does log a warning,
   * which the test framework would otherwise treat as fatal). */
  g_test_expect_message(NULL, G_LOG_LEVEL_WARNING,
                        "*not a readable directory*");
  entries = lambdock_stack_list_directory("/nonexistent/lambdock/stack");
  g_test_assert_expected_messages();
  g_assert_nonnull(entries);
  g_assert_cmpuint(entries->len, ==, 0);
  lambdock_stack_entries_free(entries);
  lambdock_stack_entries_free(NULL);

  char *paths[3];
  paths[0] = g_build_filename(dir, "beta.txt", NULL);
  paths[1] = g_build_filename(dir, "Alpha.txt", NULL);
  paths[2] = g_build_filename(dir, ".hidden", NULL);
  for (int i = 0; i < 3; i++) {
    g_assert_cmpint(g_remove(paths[i]), ==, 0);
    g_free(paths[i]);
  }
  char *sub2 = g_build_filename(dir, "zdir", NULL);
  g_assert_cmpint(g_rmdir(sub2), ==, 0);
  g_free(sub2);
  g_assert_cmpint(g_rmdir(dir), ==, 0);
  g_free(dir);
}

/* ============================================================================
 * THUMBNAILS
 * ============================================================================ */

static void test_thumbnail_for_path(void) {
  char *dir = g_dir_make_tmp("lambdock-thumb-XXXXXX", NULL);
  g_assert_nonnull(dir);

  /* A wide image is scaled down and centre-cropped, not squashed: the result is
   * square whatever the source aspect ratio was. */
  char *png = g_build_filename(dir, "wide.png", NULL);
  GdkPixbuf *pixbuf = gdk_pixbuf_new(GDK_COLORSPACE_RGB, FALSE, 8, 80, 40);
  g_assert_nonnull(pixbuf);
  gdk_pixbuf_fill(pixbuf, 0x3366ccff);
  GError *error = NULL;
  g_assert_true(gdk_pixbuf_save(pixbuf, png, "png", &error, NULL));
  g_clear_error(&error);
  g_object_unref(pixbuf);

  GdkPaintable *thumb = lambdock_stack_thumbnail_for_path(png, 64);
  g_assert_nonnull(thumb);
  g_assert_cmpint(gdk_paintable_get_intrinsic_width(thumb), ==, 64);
  g_assert_cmpint(gdk_paintable_get_intrinsic_height(thumb), ==, 64);

  /* The pixels survive the trip through GdkPixbuf and GdkTexture in the right
   * order: a red-ish source must not come out blue. */
  GdkTextureDownloader *downloader =
      gdk_texture_downloader_new(GDK_TEXTURE(thumb));
  gdk_texture_downloader_set_format(downloader, GDK_MEMORY_R8G8B8A8);
  gsize stride = 0;
  GBytes *pixels = gdk_texture_downloader_download_bytes(downloader, &stride);
  gsize length = 0;
  const guchar *data = g_bytes_get_data(pixels, &length);
  g_assert_cmpuint(length, >=, 64u * 64u * 4u);
  g_assert_cmpuint(stride, >=, 64u * 4u);
  g_assert_cmpint(data[0], ==, 0x33);
  g_assert_cmpint(data[1], ==, 0x66);
  g_assert_cmpint(data[2], ==, 0xcc);
  g_assert_cmpint(data[3], ==, 0xff);
  g_bytes_unref(pixels);
  gdk_texture_downloader_free(downloader);
  g_object_unref(thumb);

  /* A transparent image keeps its alpha through the memory texture. */
  char *rgba_png = g_build_filename(dir, "alpha.png", NULL);
  GdkPixbuf *rgba = gdk_pixbuf_new(GDK_COLORSPACE_RGB, TRUE, 8, 40, 80);
  g_assert_nonnull(rgba);
  gdk_pixbuf_fill(rgba, 0x3366cc80);
  g_assert_true(gdk_pixbuf_save(rgba, rgba_png, "png", &error, NULL));
  g_clear_error(&error);
  g_object_unref(rgba);

  GdkPaintable *alpha_thumb = lambdock_stack_thumbnail_for_path(rgba_png, 64);
  g_assert_nonnull(alpha_thumb);
  g_assert_cmpint(gdk_paintable_get_intrinsic_width(alpha_thumb), ==, 64);
  g_assert_cmpint(gdk_paintable_get_intrinsic_height(alpha_thumb), ==, 64);

  downloader = gdk_texture_downloader_new(GDK_TEXTURE(alpha_thumb));
  gdk_texture_downloader_set_format(downloader, GDK_MEMORY_R8G8B8A8);
  pixels = gdk_texture_downloader_download_bytes(downloader, &stride);
  data = g_bytes_get_data(pixels, &length);
  g_assert_cmpint(data[3], ==, 0x80);
  g_bytes_unref(pixels);
  gdk_texture_downloader_free(downloader);
  g_object_unref(alpha_thumb);

  /* Anything that is not a readable image falls back to the icon path. */
  char *txt = g_build_filename(dir, "notes.txt", NULL);
  g_assert_true(g_file_set_contents(txt, "not an image", -1, NULL));
  g_assert_null(lambdock_stack_thumbnail_for_path(txt, 64));
  g_assert_null(lambdock_stack_thumbnail_for_path(dir, 64));
  g_assert_null(
      lambdock_stack_thumbnail_for_path("/nonexistent/lambdock/thumb.png", 64));
  g_assert_null(lambdock_stack_thumbnail_for_path(NULL, 64));
  g_assert_null(lambdock_stack_thumbnail_for_path(png, 0));

  g_assert_cmpint(g_remove(txt), ==, 0);
  g_assert_cmpint(g_remove(png), ==, 0);
  g_assert_cmpint(g_remove(rgba_png), ==, 0);
  g_assert_cmpint(g_rmdir(dir), ==, 0);
  g_free(txt);
  g_free(png);
  g_free(rgba_png);
  g_free(dir);
}

/* ============================================================================
 * VIEW MODES
 * ============================================================================ */

static void test_view_mode_names(void) {
  g_assert_cmpint(lambdock_stack_view_from_string("grid"), ==,
                  LAMBDOCK_STACK_VIEW_GRID);
  g_assert_cmpint(lambdock_stack_view_from_string("list"), ==,
                  LAMBDOCK_STACK_VIEW_LIST);
  g_assert_cmpint(lambdock_stack_view_from_string("FAN"), ==,
                  LAMBDOCK_STACK_VIEW_FAN);

  /* Anything unknown is the default grid. */
  g_assert_cmpint(lambdock_stack_view_from_string(NULL), ==,
                  LAMBDOCK_STACK_VIEW_GRID);
  g_assert_cmpint(lambdock_stack_view_from_string(""), ==,
                  LAMBDOCK_STACK_VIEW_GRID);
  g_assert_cmpint(lambdock_stack_view_from_string("carousel"), ==,
                  LAMBDOCK_STACK_VIEW_GRID);

  /* Round trip. */
  for (int view = LAMBDOCK_STACK_VIEW_GRID; view <= LAMBDOCK_STACK_VIEW_FAN;
       view++) {
    const char *name = lambdock_stack_view_to_string((LambdockStackView)view);
    g_assert_cmpint(lambdock_stack_view_from_string(name), ==, view);
  }
}

/* ============================================================================
 * FAN HIT TESTING
 * ============================================================================ */

static void test_fan_contains(void) {
  /* A flat card: plain rectangle test. */
  g_assert_true(
      lambdock_stack_fan_contains(0, 0, 0, 0, 0, 100, 100));
  g_assert_true(
      lambdock_stack_fan_contains(49, -49, 0, 0, 0, 100, 100));
  g_assert_false(
      lambdock_stack_fan_contains(51, 0, 0, 0, 0, 100, 100));

  /* Centred away from the origin. */
  g_assert_true(
      lambdock_stack_fan_contains(110, 205, 100, 200, 0, 100, 100));

  /* Rotating the card by 90 degrees swaps which side the point falls on: a
   * point below the centre is now inside the (now tall) card, one to the right
   * is outside. */
  g_assert_true(
      lambdock_stack_fan_contains(100, 290, 100, 200, 90, 200, 40));
  g_assert_false(
      lambdock_stack_fan_contains(190, 200, 100, 200, 90, 200, 40));
}

/* ============================================================================
 * VIEW WIDGETS
 * ============================================================================ */

static void test_view_widgets(void) {
  GPtrArray *entries = g_ptr_array_new_with_free_func(
      (GDestroyNotify)lambdock_stack_entry_free);
  for (int i = 0; i < 5; i++) {
    LambdockStackEntry *entry = g_new0(LambdockStackEntry, 1);
    entry->name = g_strdup_printf("file-%d.txt", i);
    entry->path = g_strdup("/tmp/file.txt");
    entry->is_dir = FALSE;
    g_ptr_array_add(entries, entry);
  }

  /* Empty folder: a friendly label instead of an empty popover. */
  GtkWidget *empty = lambdock_stack_view_new(NULL, LAMBDOCK_STACK_VIEW_GRID, 48);
  g_assert_true(GTK_IS_LABEL(empty));
  g_object_ref_sink(empty);
  g_object_unref(empty);

  GtkWidget *grid = lambdock_stack_view_new(entries, LAMBDOCK_STACK_VIEW_GRID,
                                            48);
  g_assert_true(GTK_IS_SCROLLED_WINDOW(grid));
  /* Grid and list are panels with a real minimum size, not thin strips. */
  g_assert_cmpint(gtk_scrolled_window_get_min_content_width(
                      GTK_SCROLLED_WINDOW(grid)),
                  >=, 500);
  g_assert_cmpint(gtk_scrolled_window_get_min_content_height(
                      GTK_SCROLLED_WINDOW(grid)),
                  >=, 350);
  g_object_ref_sink(grid);
  g_object_unref(grid);

  GtkWidget *list = lambdock_stack_view_new(entries, LAMBDOCK_STACK_VIEW_LIST,
                                            48);
  g_assert_true(GTK_IS_SCROLLED_WINDOW(list));
  g_assert_cmpint(gtk_scrolled_window_get_min_content_width(
                      GTK_SCROLLED_WINDOW(list)),
                  >=, 500);
  g_assert_cmpint(gtk_scrolled_window_get_min_content_height(
                      GTK_SCROLLED_WINDOW(list)),
                  >=, 350);
  g_object_ref_sink(list);
  g_object_unref(list);

  /* The fan paints itself: it must report a sensible size for its cards. */
  GtkWidget *fan = lambdock_stack_view_new(entries, LAMBDOCK_STACK_VIEW_FAN,
                                           48);
  g_assert_nonnull(fan);
  /* The fan is wrapped in a scroller so a wide one stays usable. */
  g_assert_true(GTK_IS_SCROLLED_WINDOW(fan));
  GtkWidget *fan_cards = gtk_scrolled_window_get_child(GTK_SCROLLED_WINDOW(fan));
  g_assert_nonnull(fan_cards);

  int minimum = 0;
  int natural = 0;
  gtk_widget_measure(fan_cards, GTK_ORIENTATION_HORIZONTAL, -1, &minimum,
                     &natural, NULL, NULL);
  g_assert_cmpint(natural, >, 0);
  gtk_widget_measure(fan_cards, GTK_ORIENTATION_VERTICAL, -1, &minimum,
                     &natural, NULL, NULL);
  g_assert_cmpint(natural, >, 100);
  g_object_ref_sink(fan);
  g_object_unref(fan);

  lambdock_stack_entries_free(entries);
}

/* The first image in a grid/list view is the entry icon, before its async
 * thumbnail replaces it. */
static GtkWidget *find_first_image(GtkWidget *widget) {
  if (!widget)
    return NULL;
  if (GTK_IS_IMAGE(widget))
    return widget;

  for (GtkWidget *child = gtk_widget_get_first_child(widget); child;
       child = gtk_widget_get_next_sibling(child)) {
    GtkWidget *found = find_first_image(child);
    if (found)
      return found;
  }
  return NULL;
}

static gboolean wait_for_image_texture(GtkImage *image) {
  for (int attempt = 0; attempt < 300; attempt++) {
    GdkPaintable *paintable = gtk_image_get_paintable(image);
    if (paintable && GDK_IS_TEXTURE(paintable))
      return TRUE;

    while (g_main_context_iteration(NULL, FALSE)) {
    }
    g_usleep(10 * 1000);
  }
  return FALSE;
}

static void test_view_thumbnails(void) {
  char *dir = g_dir_make_tmp("lambdock-view-thumb-XXXXXX", NULL);
  g_assert_nonnull(dir);

  char *png = g_build_filename(dir, "photo.png", NULL);
  GdkPixbuf *pixbuf = gdk_pixbuf_new(GDK_COLORSPACE_RGB, FALSE, 8, 64, 64);
  g_assert_nonnull(pixbuf);
  gdk_pixbuf_fill(pixbuf, 0x3366ccff);
  g_assert_true(gdk_pixbuf_save(pixbuf, png, "png", NULL, NULL));
  g_object_unref(pixbuf);

  GPtrArray *entries = lambdock_stack_list_directory(dir);
  g_assert_cmpuint(entries->len, ==, 1);

  GtkWidget *grid =
      lambdock_stack_view_new(entries, LAMBDOCK_STACK_VIEW_GRID, 48);
  GtkWidget *image = find_first_image(grid);
  g_assert_nonnull(image);
  g_assert_true(GTK_IS_IMAGE(image));
  g_assert_true(wait_for_image_texture(GTK_IMAGE(image)));
  g_object_ref_sink(grid);
  g_object_unref(grid);

  GtkWidget *list =
      lambdock_stack_view_new(entries, LAMBDOCK_STACK_VIEW_LIST, 48);
  image = find_first_image(list);
  g_assert_nonnull(image);
  g_assert_true(GTK_IS_IMAGE(image));
  g_assert_true(wait_for_image_texture(GTK_IMAGE(image)));
  g_object_ref_sink(list);
  g_object_unref(list);

  lambdock_stack_entries_free(entries);
  g_assert_cmpint(g_remove(png), ==, 0);
  g_assert_cmpint(g_rmdir(dir), ==, 0);
  g_free(png);
  g_free(dir);
}

/* ============================================================================
 * ACCESSIBILITY & KEYBOARD
 * ============================================================================ */

/* Emits the key signal of a widget's key controller, the same way GTK would
 * dispatch a real key press. */
static gboolean send_key(GtkWidget *widget, guint keyval) {
  GListModel *controllers = gtk_widget_observe_controllers(widget);
  gboolean handled = FALSE;

  for (guint i = 0; i < g_list_model_get_n_items(controllers) && !handled; i++) {
    GtkEventController *controller = g_list_model_get_item(controllers, i);
    if (GTK_IS_EVENT_CONTROLLER_KEY(controller)) {
      g_signal_emit_by_name(controller, "key-pressed", keyval, 0,
                            (GdkModifierType)0, &handled);
    }
    g_object_unref(controller);
  }

  g_object_unref(controllers);
  return handled;
}

/* The child of a scroller, looking through the viewport GTK adds for widgets
 * that are not themselves GtkScrollable. */
static GtkWidget *scrolled_content(GtkWidget *scrolled) {
  if (!scrolled || !GTK_IS_SCROLLED_WINDOW(scrolled))
    return NULL;

  GtkWidget *child =
      gtk_scrolled_window_get_child(GTK_SCROLLED_WINDOW(scrolled));
  if (child && GTK_IS_VIEWPORT(child))
    return gtk_viewport_get_child(GTK_VIEWPORT(child));
  return child;
}

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

/* A folder with a subfolder and two files: enough to check the sorted order,
 * the folder/file wording and the keyboard walk. */
static GPtrArray *make_mixed_entries(char **out_dir) {
  char *dir = g_dir_make_tmp("lambdock-stack-a11y-XXXXXX", NULL);
  g_assert_nonnull(dir);

  char *sub = g_build_filename(dir, "subdir", NULL);
  g_assert_cmpint(g_mkdir_with_parents(sub, 0755), ==, 0);
  g_free(sub);

  char *alpha = g_build_filename(dir, "alpha.txt", NULL);
  g_assert_true(g_file_set_contents(alpha, "a", -1, NULL));
  g_free(alpha);

  char *beta = g_build_filename(dir, "beta.txt", NULL);
  g_assert_true(g_file_set_contents(beta, "b", -1, NULL));
  g_free(beta);

  GPtrArray *entries = lambdock_stack_list_directory(dir);
  g_assert_cmpuint(entries->len, ==, 3);

  if (out_dir)
    *out_dir = dir;
  else
    g_free(dir);
  return entries;
}

static void remove_mixed_entries(const char *dir) {
  char *sub = g_build_filename(dir, "subdir", NULL);
  char *alpha = g_build_filename(dir, "alpha.txt", NULL);
  char *beta = g_build_filename(dir, "beta.txt", NULL);
  g_assert_cmpint(g_remove(alpha), ==, 0);
  g_assert_cmpint(g_remove(beta), ==, 0);
  g_assert_cmpint(g_rmdir(sub), ==, 0);
  g_assert_cmpint(g_rmdir(dir), ==, 0);
  g_free(alpha);
  g_free(beta);
  g_free(sub);
}

static void test_view_accessibility(void) {
  char *dir = NULL;
  GPtrArray *entries = make_mixed_entries(&dir);
  const char *names[3] = {"subdir", "alpha.txt", "beta.txt"};

  /* Grid: every cell is a button a screen reader can name. */
  GtkWidget *grid =
      lambdock_stack_view_new(entries, LAMBDOCK_STACK_VIEW_GRID, 48);
  GtkWidget *flow = scrolled_content(grid);
  g_assert_true(GTK_IS_FLOW_BOX(flow));
  assert_accessible_label(flow, "Folder contents");
  assert_accessible_description(
      flow, "Use the arrow keys to choose an entry and Enter to open it.");

  for (guint i = 0; i < 3; i++) {
    GtkFlowBoxChild *child =
        gtk_flow_box_get_child_at_index(GTK_FLOW_BOX(flow), i);
    GtkWidget *button = gtk_flow_box_child_get_child(child);
    assert_accessible_label(button, names[i]);
    assert_accessible_description(
        button, i == 0 ? "Folder. Press Enter to open it."
                       : "File. Press Enter to open it.");
  }
  g_object_ref_sink(grid);
  g_object_unref(grid);

  /* List: the same names, as rows. */
  GtkWidget *list =
      lambdock_stack_view_new(entries, LAMBDOCK_STACK_VIEW_LIST, 48);
  GtkWidget *box = scrolled_content(list);
  g_assert_true(GTK_IS_BOX(box));

  GtkWidget *row = gtk_widget_get_first_child(box);
  for (guint i = 0; i < 3; i++) {
    g_assert_nonnull(row);
    assert_accessible_label(row, names[i]);
    row = gtk_widget_get_next_sibling(row);
  }
  g_object_ref_sink(list);
  g_object_unref(list);

  /* Fan: a single painted widget, so it announces itself as a group and says
   * how to use it. Its cards are announced one by one as they are selected. */
  GtkWidget *fan =
      lambdock_stack_view_new(entries, LAMBDOCK_STACK_VIEW_FAN, 48);
  GtkWidget *fan_cards = scrolled_content(fan);
  assert_accessible_label(fan_cards, "Fan view");
  assert_accessible_description(
      fan_cards,
      "Fan of 3 items. Left and right choose one, Enter opens it.");
  g_assert_true(gtk_widget_get_focusable(fan_cards));
  g_object_ref_sink(fan);
  g_object_unref(fan);

  lambdock_stack_entries_free(entries);
  remove_mixed_entries(dir);
}

static void test_view_keyboard(void) {
  char *dir = NULL;
  GPtrArray *entries = make_mixed_entries(&dir);
  GtkWidget *window = gtk_window_new();

  /* Grid: arrows move between cells, Home/End jump to the ends. */
  GtkWidget *grid =
      lambdock_stack_view_new(entries, LAMBDOCK_STACK_VIEW_GRID, 48);
  gtk_window_set_child(GTK_WINDOW(window), grid);

  GtkWidget *flow = scrolled_content(grid);
  GtkFlowBoxChild *first_child =
      gtk_flow_box_get_child_at_index(GTK_FLOW_BOX(flow), 0);
  GtkFlowBoxChild *second_child =
      gtk_flow_box_get_child_at_index(GTK_FLOW_BOX(flow), 1);
  GtkWidget *first = gtk_flow_box_child_get_child(first_child);
  GtkWidget *second = gtk_flow_box_child_get_child(second_child);
  GtkWidget *last = gtk_flow_box_child_get_child(
      gtk_flow_box_get_child_at_index(GTK_FLOW_BOX(flow), 2));

  gtk_widget_grab_focus(first);
  g_assert_true(lambdock_stack_view_focus_step(flow, 1, 0));
  g_assert_true(gtk_window_get_focus(GTK_WINDOW(window)) == second);

  g_assert_true(send_key(flow, GDK_KEY_Right));
  g_assert_true(gtk_window_get_focus(GTK_WINDOW(window)) == last);
  g_assert_true(send_key(flow, GDK_KEY_End));
  g_assert_true(gtk_window_get_focus(GTK_WINDOW(window)) == last);
  g_assert_true(send_key(flow, GDK_KEY_Home));
  g_assert_true(gtk_window_get_focus(GTK_WINDOW(window)) == first);

  gtk_window_set_child(GTK_WINDOW(window), NULL);
  g_object_ref_sink(grid);
  g_object_unref(grid);

  /* List: Up/Down (and Left/Right) walk the rows. */
  GtkWidget *list =
      lambdock_stack_view_new(entries, LAMBDOCK_STACK_VIEW_LIST, 48);
  gtk_window_set_child(GTK_WINDOW(window), list);

  GtkWidget *box = scrolled_content(list);
  GtkWidget *row_first = gtk_widget_get_first_child(box);
  GtkWidget *row_last = gtk_widget_get_last_child(box);

  gtk_widget_grab_focus(row_first);
  g_assert_true(send_key(box, GDK_KEY_Down));
  g_assert_true(gtk_window_get_focus(GTK_WINDOW(window)) ==
                gtk_widget_get_next_sibling(row_first));
  g_assert_true(send_key(box, GDK_KEY_End));
  g_assert_true(gtk_window_get_focus(GTK_WINDOW(window)) == row_last);

  gtk_window_set_child(GTK_WINDOW(window), NULL);
  g_object_ref_sink(list);
  g_object_unref(list);

  /* Fan: the painted cards take the arrow keys, and the caption/tooltip follows
   * the selection so sighted keyboard users see the same thing a screen reader
   * hears. */
  GtkWidget *fan =
      lambdock_stack_view_new(entries, LAMBDOCK_STACK_VIEW_FAN, 48);
  gtk_window_set_child(GTK_WINDOW(window), fan);

  GtkWidget *cards = scrolled_content(fan);
  g_assert_true(gtk_widget_grab_focus(cards));

  /* Home makes the starting card explicit whether or not focusing the fan
   * already selected one. */
  g_assert_true(send_key(cards, GDK_KEY_Home));
  g_assert_cmpstr(gtk_widget_get_tooltip_text(cards), ==, "subdir");

  g_assert_true(send_key(cards, GDK_KEY_Right));
  g_assert_cmpstr(gtk_widget_get_tooltip_text(cards), ==, "alpha.txt");

  g_assert_true(send_key(cards, GDK_KEY_End));
  g_assert_cmpstr(gtk_widget_get_tooltip_text(cards), ==, "beta.txt");

  g_assert_true(send_key(cards, GDK_KEY_Home));
  g_assert_cmpstr(gtk_widget_get_tooltip_text(cards), ==, "subdir");

  /* At the first card Left stays put but is still the fan's key. */
  g_assert_true(send_key(cards, GDK_KEY_Left));
  g_assert_cmpstr(gtk_widget_get_tooltip_text(cards), ==, "subdir");

  gtk_window_destroy(GTK_WINDOW(window));
  lambdock_stack_entries_free(entries);
  remove_mixed_entries(dir);
}

int main(int argc, char **argv) {
  g_test_init(&argc, &argv, NULL);

  g_test_add_func("/ui/stack/expand_path", test_expand_path);
  g_test_add_func("/ui/stack/list_directory", test_list_directory);
  g_test_add_func("/ui/stack/view_mode_names", test_view_mode_names);
  g_test_add_func("/ui/stack/fan_contains", test_fan_contains);

  if (!gtk_init_check()) {
    g_print("Skipping stack widget tests: No display server available\n");
    return g_test_run();
  }

  g_test_add_func("/ui/stack/view_widgets", test_view_widgets);
  g_test_add_func("/ui/stack/view_thumbnails", test_view_thumbnails);
  g_test_add_func("/ui/stack/thumbnail_for_path", test_thumbnail_for_path);
  g_test_add_func("/ui/stack/view_accessibility", test_view_accessibility);
  g_test_add_func("/ui/stack/view_keyboard", test_view_keyboard);

  return g_test_run();
}
