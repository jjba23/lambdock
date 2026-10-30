// lambdock -- hyper-hackable desktop dock for Wayland
//
// SPDX-License-Identifier: MIT
// Copyright © Josep Bigorra <jjbigorra@gmail.com>

#include "../src/ui/desktop_entry.h"
#include "../src/ui/launchpad.h"
#include <glib.h>
#include <gtk/gtk.h>
#include <libguile.h>
#include <string.h>

/* ============================================================================
 * WIDGET HELPERS
 * ============================================================================
 * A tile is a flat button that parents its copy menu, so the menu is found by
 * walking the tile and the rows by walking the popover: same widgets, same
 * classes the launchpad itself builds.
 */

static GtkWidget *tile_popover(GtkWidget *tile) {
  for (GtkWidget *child = gtk_widget_get_first_child(tile); child != NULL;
       child = gtk_widget_get_next_sibling(child)) {
    if (GTK_IS_POPOVER(child))
      return child;
  }
  return NULL;
}

/* The right click the tile listens to, borrowed from it (NULL when there is
 * none). A GtkButton has a click gesture of its own for the primary button, so
 * the one that matters here is the secondary one. */
static GtkGesture *tile_right_click(GtkWidget *tile) {
  GListModel *controllers = gtk_widget_observe_controllers(tile);
  GtkGesture *found = NULL;

  for (guint i = 0; i < g_list_model_get_n_items(controllers) && !found; i++) {
    GtkEventController *controller = g_list_model_get_item(controllers, i);
    if (GTK_IS_GESTURE_CLICK(controller) &&
        gtk_gesture_single_get_button(GTK_GESTURE_SINGLE(controller)) ==
            GDK_BUTTON_SECONDARY)
      found = GTK_GESTURE(controller);
    g_object_unref(controller);
  }

  g_object_unref(controllers);
  return found;
}

/* The copy rows of a menu, in the order they are shown. */
static GPtrArray *menu_rows(GtkWidget *menu) {
  GPtrArray *rows = g_ptr_array_new();
  GtkWidget *box = gtk_popover_get_child(GTK_POPOVER(menu));

  for (GtkWidget *child = gtk_widget_get_first_child(box); child != NULL;
       child = gtk_widget_get_next_sibling(child)) {
    if (gtk_widget_has_css_class(child, "copy-row"))
      g_ptr_array_add(rows, child);
  }

  return rows;
}

/* A row shows where the copy comes from and the command it runs. */
static const char *row_source(GtkWidget *row) {
  GtkWidget *box = gtk_button_get_child(GTK_BUTTON(row));
  return gtk_label_get_text(GTK_LABEL(gtk_widget_get_first_child(box)));
}

static const char *row_command(GtkWidget *row) {
  GtkWidget *box = gtk_button_get_child(GTK_BUTTON(row));
  GtkWidget *detail =
      gtk_widget_get_next_sibling(gtk_widget_get_first_child(box));
  return gtk_label_get_text(GTK_LABEL(detail));
}

/* ============================================================================
 * FIXTURES
 * ============================================================================
 * The scan is what fills an entry's copies, and tests/test_desktop_entry.c
 * covers that. The tiles are built here from an entry the scan could have
 * produced: same fields, same desktop file path, with the other copy attached.
 */

static LambdockDesktopEntry *entry_from(const char *path, const char *name,
                                        const char *exec) {
  char *data = g_strdup_printf("[Desktop Entry]\n"
                               "Type=Application\n"
                               "Name=%s\n"
                               "Exec=%s\n"
                               "Icon=application-x-executable\n",
                               name, exec);
  char *id = g_path_get_basename(path);
  LambdockDesktopEntry *entry = lambdock_desktop_entry_new_from_data(data, id);

  g_assert_nonnull(entry);
  g_free(id);
  g_free(data);

  /* Where the desktop file lives is what the menu names the copy after. */
  entry->path = g_strdup(path);
  return entry;
}

/* Hands COPY to KEEPER as its other installed copy, the way the scan does. */
static void attach_copy(LambdockDesktopEntry *keeper,
                        LambdockDesktopEntry *copy) {
  if (!keeper->alternates)
    keeper->alternates = g_ptr_array_new_with_free_func(
        (GDestroyNotify)lambdock_desktop_entry_free);

  g_ptr_array_add(keeper->alternates, copy);
}

/* ============================================================================
 * THE COPIES MENU
 * ============================================================================
 * A grid shows one tile per application, so the copies a scan collapses are
 * only reachable from that tile. The paths below are a user's own desktop file
 * and a Flatpak export of the same program, which is the pair a launcher has to
 * tell apart.
 */

static void test_tile_copies(const char *data_dir) {
  char *own_path =
      g_build_filename(data_dir, "applications", "libreoffice-writer.desktop",
                       NULL);
  char *export_path = g_build_filename(
      data_dir, "flatpak", "exports", "share", "applications",
      "org.libreoffice.LibreOffice.writer.desktop", NULL);
  char *solo_path =
      g_build_filename(data_dir, "applications", "solo.desktop", NULL);

  LambdockDesktopEntry *entry = entry_from(
      own_path, "LibreOffice Writer", "libreoffice --writer %U");
  LambdockDesktopEntry *copy = entry_from(
      export_path, "LibreOffice Writer",
      "/usr/bin/flatpak run org.libreoffice.LibreOffice --writer %U");
  attach_copy(entry, copy);

  LambdockDesktopEntry *solo = entry_from(solo_path, "Solo", "solo --start");

  /* A window, like the launchpad's: a popover needs a toplevel to open in, and
   * the tiles are what it owns. */
  GtkWidget *window = gtk_window_new();
  GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
  gtk_window_set_child(GTK_WINDOW(window), box);

  GtkWidget *tile = lambdock_launchpad_tile_new(entry, 64);
  g_assert_nonnull(tile);
  g_assert_true(gtk_widget_has_css_class(tile, "launchpad-tile"));
  gtk_box_append(GTK_BOX(box), tile);

  /* The tooltip says the other copy is there, so the menu is discoverable
   * without a right click that would otherwise do nothing. */
  const char *tooltip = gtk_widget_get_tooltip_text(tile);
  g_assert_nonnull(tooltip);
  g_assert_nonnull(strstr(tooltip, "other installed copies"));

  GtkWidget *menu = tile_popover(tile);
  g_assert_nonnull(menu);

  /* The tile's own copy comes first, then the one it was collapsed with, each
   * named after where it comes from and showing the command it runs. */
  GPtrArray *rows = menu_rows(menu);
  g_assert_cmpuint(rows->len, ==, 2);

  g_assert_cmpstr(row_source(g_ptr_array_index(rows, 0)), ==, "Your files");
  g_assert_cmpstr(row_command(g_ptr_array_index(rows, 0)), ==, entry->exec);
  g_assert_cmpstr(row_source(g_ptr_array_index(rows, 1)), ==, "Flatpak");
  g_assert_cmpstr(row_command(g_ptr_array_index(rows, 1)), ==, copy->exec);

  /* Each row carries its own command: clicking the second one starts the
   * Flatpak copy, not the one the tile itself would start. */
  g_assert_cmpstr(row_command(g_ptr_array_index(rows, 0)), !=,
                  row_command(g_ptr_array_index(rows, 1)));

  /* The right mouse button is what opens the menu, and pressing it on the tile
   * shows that menu. */
  g_assert_false(gtk_widget_get_visible(menu));

  GtkGesture *right_click = tile_right_click(tile);
  g_assert_nonnull(right_click);
  g_signal_emit_by_name(right_click, "pressed", 1, 5.0, 5.0);
  g_assert_true(gtk_widget_get_visible(menu));

  g_ptr_array_unref(rows);

  /* An application that is installed once has no copies to offer: no menu, and
   * nothing for a right click to open. */
  g_assert_null(solo->alternates);

  GtkWidget *solo_tile = lambdock_launchpad_tile_new(solo, 64);
  g_assert_nonnull(solo_tile);
  g_assert_null(tile_popover(solo_tile));
  g_assert_null(tile_right_click(solo_tile));
  gtk_box_append(GTK_BOX(box), solo_tile);

  /* Hiding a launchpad that is not open, with no menu open either, is a
   * no-op. */
  g_assert_false(lambdock_launchpad_is_open());
  lambdock_launchpad_hide();

  /* A tile lets go of its menu before it is destroyed, the way the grid does
   * when it is rebuilt; the closed one is still parented here. */
  gtk_widget_unparent(menu);
  gtk_window_destroy(GTK_WINDOW(window));

  lambdock_desktop_entry_free(solo);
  /* This one owns the Flatpak copy as its alternate. */
  lambdock_desktop_entry_free(entry);

  g_free(solo_path);
  g_free(export_path);
  g_free(own_path);
}

/* ============================================================================
 * PURE HELPERS
 * ============================================================================
 * The search filter and the column arithmetic are data-in/data-out, so they are
 * tested here without a display: the same cases the grid hits, at the sizes that
 * used to be guesses.
 */

static void test_entry_matches(void) {
  LambdockDesktopEntry *entry =
      entry_from("/tmp/firefox.desktop", "Firefox", "firefox %u");

  /* Nothing typed means everything is shown. */
  g_assert_true(lambdock_launchpad_entry_matches(entry, NULL));
  g_assert_true(lambdock_launchpad_entry_matches(entry, ""));

  /* Both the name and the command line are searched, case-insensitively. */
  g_assert_true(lambdock_launchpad_entry_matches(entry, "fire"));
  g_assert_true(lambdock_launchpad_entry_matches(entry, "FIREFOX"));

  g_assert_false(lambdock_launchpad_entry_matches(entry, "chromium"));
  g_assert_false(lambdock_launchpad_entry_matches(NULL, "firefox"));

  lambdock_desktop_entry_free(entry);

  /* A word that is only in the command finds the entry too (field codes like
   * "%u" are stripped when the desktop file is parsed, so the flags are what a
   * command search can match on). */
  entry = entry_from("/tmp/writer.desktop", "Writer",
                     "libreoffice --writer --norestore");
  g_assert_true(lambdock_launchpad_entry_matches(entry, "--norestore"));
  g_assert_true(lambdock_launchpad_entry_matches(entry, "writer"));
  g_assert_false(lambdock_launchpad_entry_matches(entry, "--calc"));

  lambdock_desktop_entry_free(entry);
}

static void test_columns(void) {
  /* As many as fit: a 1280 px window holds 1280 - 112 px of tiles, and a tile
   * of a 64 px icon is 150 px wide. */
  g_assert_cmpint(lambdock_launchpad_columns_for(1168, 64, 0), ==, 7);

  /* Narrow screens clamp to the minimum, wide ones to the maximum, so a grid
   * never becomes one column or a wall of thirty. */
  g_assert_cmpint(lambdock_launchpad_columns_for(300, 64, 0), ==,
                  LAUNCHPAD_AUTO_COLUMNS_MIN);
  g_assert_cmpint(lambdock_launchpad_columns_for(5000, 64, 0), ==,
                  LAUNCHPAD_AUTO_COLUMNS_MAX);

  /* No measurable width yet (first allocation): a usable default. */
  g_assert_cmpint(lambdock_launchpad_columns_for(0, 64, 0), ==,
                  LAUNCHPAD_FALLBACK_COLUMNS);

  /* A configured count is an upper bound, honoured when it fits... */
  g_assert_cmpint(lambdock_launchpad_columns_for(1168, 64, 4), ==, 4);

  /* ...and ignored when the screen cannot hold it. */
  g_assert_cmpint(lambdock_launchpad_columns_for(400, 64, 14), ==,
                  LAUNCHPAD_COLUMNS_MIN);

  /* A bigger icon means wider tiles, so fewer of them fit. */
  g_assert_cmpint(lambdock_launchpad_columns_for(1168, 160, 0),
                  <, lambdock_launchpad_columns_for(1168, 32, 0));
}

/* A GtkSearchEntry is composite: focusing it moves the focus to its inner text
 * widget, so "the keyboard is in the search field" means "the focused widget is
 * the entry or one of its children". */
static gboolean focus_is_within(GtkWidget *ancestor) {
  GtkRoot *root = gtk_widget_get_root(ancestor);
  GtkWidget *focus = root ? gtk_root_get_focus(root) : NULL;

  for (GtkWidget *widget = focus; widget != NULL;
       widget = gtk_widget_get_parent(widget)) {
    if (widget == ancestor)
      return TRUE;
  }

  return FALSE;
}

/* ============================================================================
 * FOCUS
 * ============================================================================
 */

/* Presents and hides happen asynchronously: run the loop until the surface is
 * in the state expected, so the assertions test the settled state, not a race. */
static void spin_until_mapped(GtkWidget *widget, gboolean wanted) {
  for (int i = 0; i < 200 && gtk_widget_get_mapped(widget) != wanted; i++)
    g_main_context_iteration(NULL, FALSE);
}

/* Opening the launchpad has to leave the keyboard in the search field: no click
 * first. The window's focus widget is what the compositor's exclusive keyboard
 * goes to, so that is what this asserts. */
static void test_search_focus(void) {
  GtkApplication *app = gtk_application_new("dev.lambdock.test.launchpad",
                                            G_APPLICATION_NON_UNIQUE);
  g_assert_nonnull(app);
  /* GTK refuses to add windows before the application has started up. */
  g_assert_true(g_application_register(G_APPLICATION(app), NULL, NULL));

  LambdockState state = {0};
  state.window = gtk_application_window_new(app);

  lambdock_launchpad_show(&state);

  GtkWidget *search = lambdock_launchpad_search_entry();
  g_assert_nonnull(search);
  g_assert_true(GTK_IS_SEARCH_ENTRY(search));

  GtkRoot *root = gtk_widget_get_root(search);
  g_assert_true(GTK_IS_WINDOW(root));
  spin_until_mapped(GTK_WIDGET(root), TRUE);
  g_assert_true(focus_is_within(search));

  /* A second open reuses the window and must focus the field again. */
  lambdock_launchpad_hide();
  spin_until_mapped(GTK_WIDGET(root), FALSE);
  lambdock_launchpad_show(&state);
  spin_until_mapped(GTK_WIDGET(root), TRUE);
  g_assert_true(focus_is_within(search));

  lambdock_launchpad_shutdown();
  gtk_window_destroy(GTK_WINDOW(state.window));
  g_object_unref(app);
}

/* ============================================================================
 * THE HEADER
 * ============================================================================
 */

/** TRUE when @widget (or any descendant) is a label showing @text. */
static gboolean has_label_text(GtkWidget *widget, const char *text) {
  if (GTK_IS_LABEL(widget) &&
      g_strcmp0(gtk_label_get_text(GTK_LABEL(widget)), text) == 0)
    return TRUE;

  for (GtkWidget *child = gtk_widget_get_first_child(widget); child != NULL;
       child = gtk_widget_get_next_sibling(child)) {
    if (has_label_text(child, text))
      return TRUE;
  }

  return FALSE;
}

/* The header is the close button and nothing else, and the button goes on the
 * side the setting asks for -- the left end unless it says "right". */
static void test_header(void) {
  GtkWidget *close = NULL;
  GtkWidget *header = lambdock_launchpad_header_new(&close);
  g_assert_nonnull(header);
  g_assert_true(GTK_IS_BOX(header));
  g_assert_nonnull(close);
  g_assert_true(GTK_IS_BUTTON(close));

  /* No visible "Applications" title: the grid is the content, and the search
   * field says what it searches. */
  g_assert_false(has_label_text(header, "Applications"));

  /* The close button is the first child by default. */
  g_assert_true(gtk_widget_get_first_child(header) == close);

  /* ...and the last one when the dock asks for the right side. */
  lambdock_launchpad_header_place_close(header, close, TRUE);
  GtkWidget *last = gtk_widget_get_last_child(header);
  g_assert_true(last == close);

  /* Moving it back is the same call with the other side. */
  lambdock_launchpad_header_place_close(header, close, FALSE);
  g_assert_true(gtk_widget_get_first_child(header) == close);

  /* A half-built header (no button, or a widget that is not a box) is ignored
   * rather than fatal. */
  lambdock_launchpad_header_place_close(NULL, close, TRUE);
  lambdock_launchpad_header_place_close(header, NULL, TRUE);
  lambdock_launchpad_header_place_close(gtk_label_new("not a header"), close,
                                        TRUE);
  g_assert_true(gtk_widget_get_first_child(header) == close);

  g_object_ref_sink(header);
  g_object_unref(header);
}

/* ============================================================================
 * RUNNER
 * ============================================================================
 */

static void inner_test_launchpad(void *data, int argc, char **argv) {
  (void)data;

  g_test_init(&argc, &argv, NULL);

  g_test_add_func("/launchpad/entry_matches", test_entry_matches);
  g_test_add_func("/launchpad/columns", test_columns);
  g_test_add_func("/launchpad/header", test_header);

  /* The user's own applications directory is what makes a copy "Your files",
   * and the preference files must not touch the real home directory. The system
   * data dirs stay untouched: the tiles resolve their icons through them. */
  char *data_dir = g_dir_make_tmp("lambdock-launchpad-xdg-XXXXXX", NULL);
  g_assert_nonnull(data_dir);

  char *apps = g_build_filename(data_dir, "applications", NULL);
  g_assert_cmpint(g_mkdir_with_parents(apps, 0755), ==, 0);
  g_free(apps);

  g_setenv("XDG_DATA_HOME", data_dir, TRUE);
  g_setenv("HOME", data_dir, TRUE);
  /* Keep the launchpad's own application scan away from the host's
   * applications: this suite is about focus and layout, not about whichever
   * icons the machine happens to have (a missing icon is a warning, and
   * warnings are fatal here). */
  g_setenv("XDG_DATA_DIRS", data_dir, TRUE);

  if (!gtk_init_check()) {
    g_print("Skipping launchpad widget tests: No display server available\n");
    g_test_run();
    g_free(data_dir);
    return;
  }

  /* The launchpad resolves its tile size through the Guile bridge, so the real
   * modules have to be loaded, exactly as at startup. */
  scm_c_eval_string("(set! %load-path (cons \"scheme\" %load-path))");
  scm_c_use_module("lambdock core");

  char *config_path = g_build_filename(data_dir, "settings.scm", NULL);
  g_assert_true(
      g_file_set_contents(config_path, "(define dock-theme 'vanilla)\n", -1,
                          NULL));
  SCM load_fn = scm_c_public_ref("lambdock core", "load-dock-config!");
  scm_call_1(load_fn, scm_from_utf8_string(config_path));

  g_test_add_data_func("/launchpad/tile_copies", data_dir,
                       (GTestDataFunc)test_tile_copies);
  g_test_add_func("/launchpad/search_focus", test_search_focus);

  g_test_run();

  g_free(config_path);
  g_free(data_dir);
}

int main(int argc, char **argv) {
  scm_boot_guile(argc, argv, inner_test_launchpad, NULL);
  return 0;
}

