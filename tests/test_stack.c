// lambdock -- hyper-hackable desktop dock for Wayland
//
// SPDX-License-Identifier: MIT
// Copyright © Josep Bigorra <jjbigorra@gmail.com>

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
  g_assert_cmpint(lambdock_stack_view_from_string("LIST"), ==,
                  LAMBDOCK_STACK_VIEW_LIST);

  /* Anything unknown, including the removed fan view, is the default grid. */
  g_assert_cmpint(lambdock_stack_view_from_string(NULL), ==,
                  LAMBDOCK_STACK_VIEW_GRID);
  g_assert_cmpint(lambdock_stack_view_from_string(""), ==,
                  LAMBDOCK_STACK_VIEW_GRID);
  g_assert_cmpint(lambdock_stack_view_from_string("fan"), ==,
                  LAMBDOCK_STACK_VIEW_GRID);
  g_assert_cmpint(lambdock_stack_view_from_string("carousel"), ==,
                  LAMBDOCK_STACK_VIEW_GRID);

  /* Round trip. */
  for (int view = LAMBDOCK_STACK_VIEW_GRID; view <= LAMBDOCK_STACK_VIEW_LIST;
       view++) {
    const char *name = lambdock_stack_view_to_string((LambdockStackView)view);
    g_assert_cmpint(lambdock_stack_view_from_string(name), ==, view);
  }
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

  gtk_window_destroy(GTK_WINDOW(window));
  lambdock_stack_entries_free(entries);
  remove_mixed_entries(dir);
}

/* ============================================================================
 * BROWSING: PATHS, BREADCRUMBS AND TRUNCATION
 * ============================================================================
 */

static void test_child_and_parent_path(void) {
  char *child = lambdock_stack_child_path("/home/me/Downloads", "Shots");
  g_assert_cmpstr(child, ==, "/home/me/Downloads/Shots");
  g_free(child);

  g_assert_null(lambdock_stack_child_path(NULL, "x"));
  g_assert_null(lambdock_stack_child_path("/tmp", NULL));
  g_assert_null(lambdock_stack_child_path("", "x"));

  /* Inside the stack the panel walks up; at the stack's own folder there is
   * nowhere to go, and a path that is not inside it is not walked out of. */
  char *parent =
      lambdock_stack_parent_path("/home/me/Downloads/Shots/2024",
                                 "/home/me/Downloads");
  g_assert_cmpstr(parent, ==, "/home/me/Downloads/Shots");
  g_free(parent);

  parent = lambdock_stack_parent_path("/home/me/Downloads/Shots",
                                      "/home/me/Downloads");
  g_assert_cmpstr(parent, ==, "/home/me/Downloads");
  g_free(parent);

  g_assert_null(lambdock_stack_parent_path("/home/me/Downloads",
                                           "/home/me/Downloads"));
  g_assert_null(lambdock_stack_parent_path("/etc", "/home/me/Downloads"));
  g_assert_null(lambdock_stack_parent_path(NULL, "/home/me/Downloads"));
  g_assert_null(lambdock_stack_parent_path("/home/me/Downloads", NULL));

  /* Trailing slashes are the same place, not a different one. */
  parent = lambdock_stack_parent_path("/home/me/Downloads/Shots/",
                                      "/home/me/Downloads/");
  g_assert_cmpstr(parent, ==, "/home/me/Downloads");
  g_free(parent);
}

static void test_breadcrumb(void) {
  char *crumb =
      lambdock_stack_breadcrumb("/home/me/Downloads", "/home/me/Downloads",
                                "Downloads");
  g_assert_cmpstr(crumb, ==, "Downloads");
  g_free(crumb);

  crumb = lambdock_stack_breadcrumb("/home/me/Downloads/Shots/2024",
                                    "/home/me/Downloads", "Downloads");
  g_assert_cmpstr(crumb, ==, "Downloads / Shots / 2024");
  g_free(crumb);

  /* Without a label the root is named after its folder. */
  crumb = lambdock_stack_breadcrumb("/home/me/Downloads/Shots",
                                    "/home/me/Downloads", NULL);
  g_assert_cmpstr(crumb, ==, "Downloads / Shots");
  g_free(crumb);

  /* A path outside the stack (or no stack at all) still gets a title. */
  crumb = lambdock_stack_breadcrumb("/etc", "/home/me/Downloads", "Downloads");
  g_assert_cmpstr(crumb, ==, "Downloads");
  g_free(crumb);

  crumb = lambdock_stack_breadcrumb(NULL, NULL, NULL);
  g_assert_cmpstr(crumb, ==, "Stack");
  g_free(crumb);
}

static void test_list_directory_truncated(void) {
  char *dir = g_dir_make_tmp("lambdock-stack-trunc-XXXXXX", NULL);
  g_assert_nonnull(dir);

  for (int i = 0; i < 5; i++) {
    char *name = g_strdup_printf("%s/file-%d.txt", dir, i);
    g_assert_true(g_file_set_contents(name, "x", -1, NULL));
    g_free(name);
  }
  /* Hidden files do not count towards the cap, exactly as they do not show. */
  char *hidden = g_build_filename(dir, ".hidden", NULL);
  g_assert_true(g_file_set_contents(hidden, "x", -1, NULL));
  g_free(hidden);

  gboolean truncated = FALSE;
  GPtrArray *all =
      lambdock_stack_list_directory_limited(dir, 10, &truncated);
  g_assert_cmpuint(all->len, ==, 5);
  g_assert_false(truncated);
  lambdock_stack_entries_free(all);

  truncated = FALSE;
  GPtrArray *limited =
      lambdock_stack_list_directory_limited(dir, 3, &truncated);
  g_assert_cmpuint(limited->len, ==, 3);
  g_assert_true(truncated);
  lambdock_stack_entries_free(limited);

  /* Exactly at the cap is not truncated. */
  truncated = TRUE;
  GPtrArray *exact =
      lambdock_stack_list_directory_limited(dir, 5, &truncated);
  g_assert_cmpuint(exact->len, ==, 5);
  g_assert_false(truncated);
  lambdock_stack_entries_free(exact);

  /* 0 means the built-in cap; the answer is reported all the same. */
  truncated = TRUE;
  GPtrArray *fallback =
      lambdock_stack_list_directory_limited(dir, 0, &truncated);
  g_assert_cmpuint(fallback->len, ==, 5);
  g_assert_false(truncated);
  lambdock_stack_entries_free(fallback);

  GDir *d = g_dir_open(dir, 0, NULL);
  const char *name;
  while ((name = g_dir_read_name(d)) != NULL) {
    char *child = g_build_filename(dir, name, NULL);
    g_unlink(child);
    g_free(child);
  }
  g_dir_close(d);
  g_rmdir(dir);
  g_free(dir);
}

/* ============================================================================
 * THE PANEL
 * ============================================================================
 */

static DockItem *panel_item(const char *path, const char *view) {
  DockItem *item = g_new0(DockItem, 1);
  item->kind = LAMBDOCK_ITEM_STACK;
  item->name = g_strdup("Downloads");
  item->stack_path = g_strdup(path);
  item->stack_view = g_strdup(view ? view : "grid");
  return item;
}

static void free_panel_item(DockItem *item) {
  g_free(item->name);
  g_free(item->stack_path);
  g_free(item->stack_view);
  g_free(item);
}

/** The widget of the panel whose class is CLASS, or NULL. */
static GtkWidget *panel_find_class(GtkWidget *panel, const char *klass) {
  if (gtk_widget_has_css_class(panel, klass))
    return panel;

  for (GtkWidget *child = gtk_widget_get_first_child(panel); child != NULL;
       child = gtk_widget_get_next_sibling(child)) {
    GtkWidget *found = panel_find_class(child, klass);
    if (found)
      return found;
  }

  return NULL;
}

static gboolean panel_has_label(GtkWidget *panel, const char *text) {
  if (GTK_IS_LABEL(panel) &&
      g_strcmp0(gtk_label_get_text(GTK_LABEL(panel)), text) == 0)
    return TRUE;

  for (GtkWidget *child = gtk_widget_get_first_child(panel); child != NULL;
       child = gtk_widget_get_next_sibling(child)) {
    if (panel_has_label(child, text))
      return TRUE;
  }

  return false;
}

static void test_panel_header_and_navigation(void) {
  char *root = g_dir_make_tmp("lambdock-stack-panel-XXXXXX", NULL);
  g_assert_nonnull(root);

  char *sub = g_build_filename(root, "Shots", NULL);
  g_assert_cmpint(g_mkdir(sub, 0755), ==, 0);
  char *inner = g_build_filename(sub, "2024", NULL);
  g_assert_cmpint(g_mkdir(inner, 0755), ==, 0);

  DockItem *item = panel_item(root, "grid");

  /* At the stack's own folder: the breadcrumb is its label, there is no back
   * button, and the list/grid toggle offers the other view. */
  GtkWidget *panel = lambdock_stack_panel_new(item, root);
  g_assert_nonnull(panel);
  g_assert_true(gtk_widget_has_css_class(panel, "stack-popover"));
  g_assert_true(panel_has_label(panel, "Downloads"));
  g_assert_nonnull(panel_find_class(panel, "stack-count"));
  g_assert_null(panel_find_class(panel, "stack-back"));

  GtkWidget *toggle = panel_find_class(panel, "stack-view-toggle");
  g_assert_nonnull(toggle);
  GtkWidget *toggle_icon = gtk_button_get_child(GTK_BUTTON(toggle));
  g_assert_true(GTK_IS_IMAGE(toggle_icon));
  g_assert_cmpstr(gtk_image_get_icon_name(GTK_IMAGE(toggle_icon)), ==,
                  "view-list-symbolic");
  g_assert_nonnull(panel_find_class(panel, "stack-open"));

  g_object_ref_sink(panel);
  g_object_unref(panel);

  /* Inside a folder: a breadcrumb, a back button, and the toggle now offers the
   * grid (the item is on the list view). */
  g_free(item->stack_view);
  item->stack_view = g_strdup("list");

  panel = lambdock_stack_panel_new(item, inner);
  g_assert_true(panel_has_label(panel, "Downloads / Shots / 2024"));
  g_assert_nonnull(panel_find_class(panel, "stack-back"));

  toggle = panel_find_class(panel, "stack-view-toggle");
  toggle_icon = gtk_button_get_child(GTK_BUTTON(toggle));
  g_assert_cmpstr(gtk_image_get_icon_name(GTK_IMAGE(toggle_icon)), ==,
                  "view-grid-symbolic");

  /* The panel lists the contents of the folder it was given: 2024 is empty, so
   * it says so instead of showing a stale list. */
  g_assert_true(panel_has_label(panel, "This folder is empty"));

  g_object_ref_sink(panel);
  g_object_unref(panel);

  /* One level up, that folder is an entry again. */
  panel = lambdock_stack_panel_new(item, sub);
  g_assert_true(panel_has_label(panel, "Downloads / Shots"));
  g_assert_true(panel_has_label(panel, "2024"));

  g_object_ref_sink(panel);
  g_object_unref(panel);

  free_panel_item(item);
  g_free(inner);
  g_free(sub);
  g_rmdir(g_build_filename(root, "Shots", "2024", NULL));
  g_rmdir(g_build_filename(root, "Shots", NULL));
  g_rmdir(root);
  g_free(root);
}

/* Clicking a folder inside the panel walks into it instead of handing it to the
 * file manager, and clicking the back button walks out again. */
static void test_panel_walks_into_folders(void) {
  char *root = g_dir_make_tmp("lambdock-stack-walk-XXXXXX", NULL);
  g_assert_nonnull(root);
  char *sub = g_build_filename(root, "Shots", NULL);
  g_assert_cmpint(g_mkdir(sub, 0755), ==, 0);

  GtkWidget *window = gtk_window_new();
  GtkWidget *anchor = gtk_button_new_with_label("anchor");
  gtk_window_set_child(GTK_WINDOW(window), anchor);

  DockItem *item = panel_item(root, "grid");
  item->popover = gtk_popover_new();
  GtkWidget *popover = g_object_ref_sink(item->popover);
  gtk_widget_set_parent(item->popover, anchor);

  GtkWidget *panel = lambdock_stack_panel_new(item, root);
  gtk_popover_set_child(GTK_POPOVER(item->popover), panel);
  g_object_set_data_full(G_OBJECT(item->popover), "lambdock-stack-path",
                         g_strdup(root), g_free);

  /* The folder entry: clicking it rebuilds the panel one level down. */
  GtkWidget *folder_button = NULL;
  GtkWidget *view = g_object_get_data(G_OBJECT(panel), "lambdock-stack-view");
  g_assert_nonnull(view);
  GtkWidget *flow = scrolled_content(view);
  g_assert_true(GTK_IS_FLOW_BOX(flow));
  GtkFlowBoxChild *first =
      gtk_flow_box_get_child_at_index(GTK_FLOW_BOX(flow), 0);
  g_assert_nonnull(first);
  folder_button = gtk_flow_box_child_get_child(first);
  g_assert_nonnull(folder_button);

  g_signal_emit_by_name(folder_button, "clicked");

  GtkWidget *rebuilt = gtk_popover_get_child(GTK_POPOVER(item->popover));
  g_assert_nonnull(rebuilt);
  g_assert_true(panel_has_label(rebuilt, "Downloads / Shots"));
  g_assert_nonnull(panel_find_class(rebuilt, "stack-back"));

  /* ...and the back button returns to the stack's own folder. */
  GtkWidget *back = panel_find_class(rebuilt, "stack-back");
  g_signal_emit_by_name(back, "clicked");

  GtkWidget *back_at_root = gtk_popover_get_child(GTK_POPOVER(item->popover));
  g_assert_nonnull(back_at_root);
  g_assert_true(panel_has_label(back_at_root, "Downloads"));
  g_assert_null(panel_find_class(back_at_root, "stack-back"));

  gtk_widget_unparent(item->popover);
  g_object_unref(popover); /* the test's own ref: frees the popover */
  free_panel_item(item);
  gtk_window_destroy(GTK_WINDOW(window));

  g_rmdir(sub);
  g_rmdir(root);
  g_free(sub);
  g_free(root);
}

int main(int argc, char **argv) {
  g_test_init(&argc, &argv, NULL);

  g_test_add_func("/ui/stack/expand_path", test_expand_path);
  g_test_add_func("/ui/stack/list_directory", test_list_directory);
  g_test_add_func("/ui/stack/view_mode_names", test_view_mode_names);

  if (!gtk_init_check()) {
    g_print("Skipping stack widget tests: No display server available\n");
    return g_test_run();
  }

  g_test_add_func("/ui/stack/view_widgets", test_view_widgets);
  g_test_add_func("/ui/stack/view_thumbnails", test_view_thumbnails);
  g_test_add_func("/ui/stack/thumbnail_for_path", test_thumbnail_for_path);
  g_test_add_func("/ui/stack/view_accessibility", test_view_accessibility);
  g_test_add_func("/ui/stack/view_keyboard", test_view_keyboard);
  g_test_add_func("/ui/stack/child_and_parent_path",
                  test_child_and_parent_path);
  g_test_add_func("/ui/stack/breadcrumb", test_breadcrumb);
  g_test_add_func("/ui/stack/list_directory_truncated",
                  test_list_directory_truncated);
  g_test_add_func("/ui/stack/panel_header",
                  test_panel_header_and_navigation);
  g_test_add_func("/ui/stack/panel_walks_into_folders",
                  test_panel_walks_into_folders);

  return g_test_run();
}
