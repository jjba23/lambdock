// lambdock

// Copyright © Josep Bigorra <jjbigorra@gmail.com>

// lambdock is free software : you can redistribute it and / or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.

// lambdock is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
// GNU General Public License for more details.

// You should have received a copy of the GNU General Public License
// along with lambdock. If not, see <https://www.gnu.org/licenses/>.

#include "../src/lambdock.h"
#include "../src/ui/stack.h"
#include "../src/ui/popover_menu.h"
#include "../src/ui/trash.h"
#include <gtk/gtk.h>
#include <string.h>

/* Collects every user-visible label in the popover, in creation order. Row
 * containers contribute their focus-button label; the close buttons are icon
 * only and are inspected separately where relevant. */
static GPtrArray *menu_labels(DockItem *item) {
  GPtrArray *out = g_ptr_array_new_with_free_func(g_free);
  GtkWidget *box = gtk_popover_get_child(GTK_POPOVER(item->popover));
  if (!box)
    return out;

  for (GtkWidget *w = gtk_widget_get_first_child(box); w != NULL;
       w = gtk_widget_get_next_sibling(w)) {
    if (GTK_IS_LABEL(w)) {
      g_ptr_array_add(out, g_strdup(gtk_label_get_text(GTK_LABEL(w))));
    } else if (GTK_IS_BUTTON(w)) {
      GtkWidget *child = gtk_button_get_child(GTK_BUTTON(w));
      if (child && GTK_IS_LABEL(child))
        g_ptr_array_add(out, g_strdup(gtk_label_get_text(GTK_LABEL(child))));
    } else if (GTK_IS_BOX(w)) {
      for (GtkWidget *c = gtk_widget_get_first_child(w); c != NULL;
           c = gtk_widget_get_next_sibling(c)) {
        if (GTK_IS_BUTTON(c)) {
          GtkWidget *child = gtk_button_get_child(GTK_BUTTON(c));
          if (child && GTK_IS_LABEL(child))
            g_ptr_array_add(out, g_strdup(gtk_label_get_text(GTK_LABEL(child))));
        }
      }
    }
  }
  return out;
}

static guint menu_child_count(DockItem *item) {
  GtkWidget *box = gtk_popover_get_child(GTK_POPOVER(item->popover));
  guint n = 0;
  for (GtkWidget *w = gtk_widget_get_first_child(box); w != NULL;
       w = gtk_widget_get_next_sibling(w))
    n++;
  return n;
}

static DockItem *new_item(const char *name, const char *app_id,
                          const char *exec) {
  DockItem *item = g_new0(DockItem, 1);
  item->name = g_strdup(name);
  item->app_id = g_strdup(app_id);
  item->exec = g_strdup(exec);
  item->popover = gtk_popover_new();
  return item;
}

static void free_item(DockItem *item) {
  for (GList *l = item->windows; l != NULL; l = l->next) {
    AppWindow *w = (AppWindow *)l->data;
    g_free(w->title);
    g_free(w);
  }
  g_list_free(item->windows);
  g_free(item->name);
  g_free(item->app_id);
  g_free(item->exec);
  g_object_ref_sink(item->popover);
  g_object_unref(item->popover);
  g_free(item);
}

static AppWindow *add_window(DockItem *item, const char *title) {
  AppWindow *win = g_new0(AppWindow, 1);
  win->handle = (void *)item->windows;
  win->title = g_strdup(title);
  item->windows = g_list_append(item->windows, win);
  return win;
}

static void test_popover_menu_guards(void) {
  populate_popover_menu(NULL);

  DockItem no_popover = {0};
  no_popover.name = g_strdup("X");
  populate_popover_menu(&no_popover);
  g_free(no_popover.name);
}

static void test_popover_menu_no_windows_with_exec(void) {
  DockItem *item = new_item("Firefox", "firefox", "firefox %u");

  populate_popover_menu(item);

  g_assert_cmpint(menu_child_count(item), ==, 3);
  GPtrArray *labels = menu_labels(item);
  g_assert_cmpint(labels->len, ==, 2);
  g_assert_cmpstr(g_ptr_array_index(labels, 0), ==, "Firefox");
  g_assert_cmpstr(g_ptr_array_index(labels, 1), ==, "Launch Application");
  g_ptr_array_unref(labels);

  free_item(item);
}

static void test_popover_menu_no_windows_no_exec(void) {
  DockItem *item = new_item("Firefox", "firefox", NULL);

  populate_popover_menu(item);

  /* Header + separator only: no launch option without an exec command. */
  g_assert_cmpint(menu_child_count(item), ==, 2);
  GPtrArray *labels = menu_labels(item);
  g_assert_cmpint(labels->len, ==, 1);
  g_assert_cmpstr(g_ptr_array_index(labels, 0), ==, "Firefox");
  g_ptr_array_unref(labels);

  free_item(item);
}

static void test_popover_menu_single_window(void) {
  DockItem *item = new_item("Firefox", "firefox", "firefox %u");
  add_window(item, "Mozilla Firefox");

  populate_popover_menu(item);

  /* header, sep, row, sep, New Instance */
  g_assert_cmpint(menu_child_count(item), ==, 5);
  GPtrArray *labels = menu_labels(item);
  g_assert_cmpint(labels->len, ==, 3);
  g_assert_cmpstr(g_ptr_array_index(labels, 1), ==, "Mozilla Firefox");
  g_assert_cmpstr(g_ptr_array_index(labels, 2), ==, "New Instance");
  g_ptr_array_unref(labels);

  free_item(item);
}

static void test_popover_menu_multi_window(void) {
  DockItem *item = new_item("Firefox", "firefox", "firefox %u");
  add_window(item, "Tab 1");
  add_window(item, "Tab 2");

  populate_popover_menu(item);

  /* header, sep, row, row, sep, New Instance, Close All Windows */
  g_assert_cmpint(menu_child_count(item), ==, 7);
  GPtrArray *labels = menu_labels(item);
  g_assert_cmpint(labels->len, ==, 5);
  g_assert_cmpstr(g_ptr_array_index(labels, 0), ==, "Firefox");
  g_assert_cmpstr(g_ptr_array_index(labels, 1), ==, "Tab 1");
  g_assert_cmpstr(g_ptr_array_index(labels, 2), ==, "Tab 2");
  g_assert_cmpstr(g_ptr_array_index(labels, 3), ==, "New Instance");
  g_assert_cmpstr(g_ptr_array_index(labels, 4), ==, "Close All Windows");
  g_ptr_array_unref(labels);

  free_item(item);
}

static void test_popover_menu_window_close_button(void) {
  DockItem *item = new_item("Firefox", "firefox", "firefox");

  /* A NULL title falls back to the generic "Window" label. */
  add_window(item, NULL);

  populate_popover_menu(item);

  GtkWidget *box = gtk_popover_get_child(GTK_POPOVER(item->popover));
  GtkWidget *row = NULL;
  for (GtkWidget *w = gtk_widget_get_first_child(box); w != NULL;
       w = gtk_widget_get_next_sibling(w)) {
    if (GTK_IS_BOX(w)) {
      row = w;
      break;
    }
  }
  g_assert_nonnull(row);

  GtkWidget *focus = gtk_widget_get_first_child(row);
  GtkWidget *close = gtk_widget_get_next_sibling(focus);
  g_assert_true(GTK_IS_BUTTON(focus));
  g_assert_true(GTK_IS_BUTTON(close));

  GtkWidget *focus_label = gtk_button_get_child(GTK_BUTTON(focus));
  g_assert_true(GTK_IS_LABEL(focus_label));
  g_assert_cmpstr(gtk_label_get_text(GTK_LABEL(focus_label)), ==, "Window");
  g_assert_cmpint(gtk_label_get_ellipsize(GTK_LABEL(focus_label)), ==,
                  PANGO_ELLIPSIZE_END);
  g_assert_cmpint(gtk_label_get_max_width_chars(GTK_LABEL(focus_label)), ==,
                  POPOVER_TITLE_MAX_CHARS);

  GtkWidget *close_child = gtk_button_get_child(GTK_BUTTON(close));
  g_assert_true(GTK_IS_IMAGE(close_child));
  g_assert_cmpstr(gtk_image_get_icon_name(GTK_IMAGE(close_child)), ==,
                  "window-close-symbolic");

  free_item(item);
}

static void test_popover_menu_header_fallback(void) {
  /* Missing name falls back to the app_id. */
  DockItem *item = new_item(NULL, "org.example.App", "run");
  populate_popover_menu(item);
  GPtrArray *labels = menu_labels(item);
  g_assert_cmpstr(g_ptr_array_index(labels, 0), ==, "org.example.App");
  g_ptr_array_unref(labels);
  free_item(item);

  /* Missing name and app_id fall back to the generic title. */
  item = new_item(NULL, NULL, "run");
  populate_popover_menu(item);
  labels = menu_labels(item);
  g_assert_cmpstr(g_ptr_array_index(labels, 0), ==, "Application");
  g_ptr_array_unref(labels);
  free_item(item);
}

static void test_popover_menu_rebuild(void) {
  DockItem *item = new_item("Firefox", "firefox", "firefox");
  add_window(item, "Only");

  populate_popover_menu(item);
  g_assert_cmpint(menu_child_count(item), ==, 5);

  /* Rebuilding with more windows must fully replace the previous layout, not
   * append to it. */
  add_window(item, "Second");
  populate_popover_menu(item);
  g_assert_cmpint(menu_child_count(item), ==, 7);

  /* Rebuilding again with no windows shrinks back down. */
  for (GList *l = item->windows; l != NULL; l = l->next)
    g_free(((AppWindow *)l->data)->title);
  g_list_free_full(item->windows, g_free);
  item->windows = NULL;

  populate_popover_menu(item);
  g_assert_cmpint(menu_child_count(item), ==, 3);

  free_item(item);
}


/* Collects the text of every label anywhere in the menu, in order. */
static void collect_labels(GtkWidget *widget, GPtrArray *out) {
  for (GtkWidget *child = gtk_widget_get_first_child(widget); child != NULL;
       child = gtk_widget_get_next_sibling(child)) {
    if (GTK_IS_LABEL(child))
      g_ptr_array_add(out, g_strdup(gtk_label_get_text(GTK_LABEL(child))));
    collect_labels(child, out);
  }
}

static gboolean labels_contain(GPtrArray *labels, const char *needle) {
  for (guint i = 0; i < labels->len; i++) {
    if (g_strcmp0(g_ptr_array_index(labels, i), needle) == 0)
      return TRUE;
  }
  return FALSE;
}

static void test_popover_menu_stack(void) {
  DockItem *item = new_item("Downloads", "Downloads", NULL);
  item->kind = LAMBDOCK_ITEM_STACK;
  item->is_favorite = TRUE;
  item->stack_path = g_strdup("/tmp");
  item->stack_view = g_strdup("list");

  populate_popover_menu(item);

  GPtrArray *labels = g_ptr_array_new_with_free_func(g_free);
  collect_labels(gtk_popover_get_child(GTK_POPOVER(item->popover)), labels);

  /* Header, the three view modes (with the active one marked), the folder
   * action and "Remove from Dock". */
  g_assert_true(labels_contain(labels, "Downloads"));
  /* The active view mode is marked with a filled circle, the others with an
   * empty one (the item is on the list view). */
  g_assert_true(labels_contain(labels, "\xe2\x97\x8b Grid"));
  g_assert_true(labels_contain(labels, "\xe2\x97\x8f List"));
  g_assert_true(labels_contain(labels, "\xe2\x97\x8b Fan"));
  g_assert_true(labels_contain(labels, "Open Folder"));
  g_assert_true(labels_contain(labels, "Remove from Dock"));

  g_ptr_array_unref(labels);
  g_free(item->stack_path);
  g_free(item->stack_view);
  item->stack_path = NULL;
  item->stack_view = NULL;
  free_item(item);
}

static void test_popover_menu_launchpad(void) {
  DockItem *item = new_item("Launchpad", "Launchpad", NULL);
  item->kind = LAMBDOCK_ITEM_LAUNCHPAD;
  item->is_favorite = TRUE;

  populate_popover_menu(item);

  GPtrArray *labels = g_ptr_array_new_with_free_func(g_free);
  collect_labels(gtk_popover_get_child(GTK_POPOVER(item->popover)), labels);

  g_assert_true(labels_contain(labels, "Show Launchpad"));
  g_assert_true(labels_contain(labels, "Remove from Dock"));

  g_ptr_array_unref(labels);
  free_item(item);
}

/* Finds the button whose label is TEXT anywhere in the menu. */
static GtkWidget *find_menu_button(GtkWidget *widget, const char *text) {
  for (GtkWidget *child = gtk_widget_get_first_child(widget); child != NULL;
       child = gtk_widget_get_next_sibling(child)) {
    if (GTK_IS_BUTTON(child)) {
      GtkWidget *label = gtk_button_get_child(GTK_BUTTON(child));
      if (label && GTK_IS_LABEL(label) &&
          g_strcmp0(gtk_label_get_text(GTK_LABEL(label)), text) == 0)
        return child;
    }

    GtkWidget *found = find_menu_button(child, text);
    if (found)
      return found;
  }
  return NULL;
}

static void test_popover_menu_trash(void) {
  /* The trash menu reports the count, offers opening and emptying, and only
   * offers emptying while there is something to empty. */
  guint count = lambdock_trash_count();

  DockItem *item = new_item("Trash", "trash", NULL);
  item->kind = LAMBDOCK_ITEM_TRASH;
  item->is_favorite = TRUE;

  populate_popover_menu(item);

  GtkWidget *content = gtk_popover_get_child(GTK_POPOVER(item->popover));
  GPtrArray *labels = g_ptr_array_new_with_free_func(g_free);
  collect_labels(content, labels);

  gboolean has_count = FALSE;
  for (guint i = 0; i < labels->len; i++) {
    const char *text = g_ptr_array_index(labels, i);
    if (text && strstr(text, " in the trash"))
      has_count = TRUE;
  }

  g_assert_true(has_count);
  g_assert_true(labels_contain(labels, "Open Trash"));
  g_assert_true(labels_contain(labels, "Empty Trash"));
  g_assert_true(labels_contain(labels, "Remove from Dock"));

  GtkWidget *empty = find_menu_button(content, "Empty Trash");
  g_assert_nonnull(empty);
  g_assert_cmpint(gtk_widget_get_sensitive(empty), ==, count > 0);

  g_ptr_array_unref(labels);
  free_item(item);
}

static void test_popover_menu_remove_entry(void) {
  /* Pinned items offer "Remove from Dock"; window-tracked (non-favourite) ones
   * do not, because the dock did not create them. */
  DockItem *pinned = new_item("Firefox", "firefox", "firefox");
  pinned->is_favorite = TRUE;
  populate_popover_menu(pinned);

  GPtrArray *labels = g_ptr_array_new_with_free_func(g_free);
  collect_labels(gtk_popover_get_child(GTK_POPOVER(pinned->popover)), labels);
  g_assert_true(labels_contain(labels, "Remove from Dock"));
  g_ptr_array_unref(labels);
  free_item(pinned);

  DockItem *tracked = new_item("Firefox", "firefox", "firefox");
  populate_popover_menu(tracked);

  labels = g_ptr_array_new_with_free_func(g_free);
  collect_labels(gtk_popover_get_child(GTK_POPOVER(tracked->popover)), labels);
  g_assert_false(labels_contain(labels, "Remove from Dock"));
  g_ptr_array_unref(labels);
  free_item(tracked);
}

int main(int argc, char **argv) {
  g_test_init(&argc, &argv, NULL);

  if (!gtk_init_check()) {
    g_print("Skipping popover menu tests: No display server available\n");
    return 77;
  }

  g_test_add_func("/ui/popover_menu/guards", test_popover_menu_guards);
  g_test_add_func("/ui/popover_menu/no_windows_with_exec",
                  test_popover_menu_no_windows_with_exec);
  g_test_add_func("/ui/popover_menu/no_windows_no_exec",
                  test_popover_menu_no_windows_no_exec);
  g_test_add_func("/ui/popover_menu/single_window",
                  test_popover_menu_single_window);
  g_test_add_func("/ui/popover_menu/multi_window",
                  test_popover_menu_multi_window);
  g_test_add_func("/ui/popover_menu/window_close_button",
                  test_popover_menu_window_close_button);
  g_test_add_func("/ui/popover_menu/header_fallback",
                  test_popover_menu_header_fallback);
  g_test_add_func("/ui/popover_menu/rebuild", test_popover_menu_rebuild);
  g_test_add_func("/ui/popover_menu/stack", test_popover_menu_stack);
  g_test_add_func("/ui/popover_menu/launchpad", test_popover_menu_launchpad);
  g_test_add_func("/ui/popover_menu/trash", test_popover_menu_trash);
  g_test_add_func("/ui/popover_menu/remove_entry",
                  test_popover_menu_remove_entry);

  return g_test_run();
}
