#include "../src/lambdock.h"
#include "../src/ui/popover_menu.h"
#include <gtk/gtk.h>

static void test_popover_menu_construction(void) {
  DockItem item = {0};
  item.name = g_strdup("Firefox");
  item.exec = g_strdup("firefox");
  item.popover = gtk_popover_new();

  // 0 windows open -> Should build "Launch Application" option
  populate_popover_menu(&item);
  GtkWidget *vbox = gtk_popover_get_child(GTK_POPOVER(item.popover));
  g_assert_nonnull(vbox);
  g_assert_true(GTK_IS_BOX(vbox));

  // Attach 2 active windows -> Should build window list + "Close All Windows" +
  // "New Instance"
  AppWindow win1 = {.handle = (void *)0x1, .title = g_strdup("Tab 1")};
  AppWindow win2 = {.handle = (void *)0x2, .title = g_strdup("Tab 2")};
  item.windows = g_list_append(item.windows, &win1);
  item.windows = g_list_append(item.windows, &win2);

  populate_popover_menu(&item);
  vbox = gtk_popover_get_child(GTK_POPOVER(item.popover));
  g_assert_nonnull(vbox);

  g_free(win1.title);
  g_free(win2.title);
  g_list_free(item.windows);
  g_free(item.name);
  g_free(item.exec);
  g_object_ref_sink(item.popover);
  g_object_unref(item.popover);
}

int main(int argc, char **argv) {
  g_test_init(&argc, &argv, NULL);

  if (!gtk_init_check()) {
    g_print("Skipping popover menu tests: No display server available\n");
    return 77;
  }

  g_test_add_func("/ui/popover_menu/construction",
                  test_popover_menu_construction);

  return g_test_run();
}
