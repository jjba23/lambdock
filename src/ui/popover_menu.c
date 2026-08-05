#include "popover_menu.h"
#include "../toplevel_tracker.h"

typedef struct {
  DockItem *item;
  struct zwlr_foreign_toplevel_handle_v1 *handle;
} WindowActionData;

static void free_action_data(gpointer data, GClosure *closure) {
  (void)closure;
  g_free(data);
}

static void on_popover_focus_window(GtkButton *btn, gpointer user_data) {
  (void)btn;
  WindowActionData *data = (WindowActionData *)user_data;
  if (data && data->handle)
    activate_toplevel_window(data->handle);
  if (data && data->item && data->item->popover)
    gtk_popover_popdown(GTK_POPOVER(data->item->popover));
}

static void on_popover_close_window(GtkButton *btn, gpointer user_data) {
  (void)btn;
  WindowActionData *data = (WindowActionData *)user_data;
  if (data && data->handle)
    close_toplevel_window(data->handle);
  if (data && data->item && data->item->popover)
    gtk_popover_popdown(GTK_POPOVER(data->item->popover));
}

static void on_popover_close_all_windows(GtkButton *btn, gpointer user_data) {
  (void)btn;
  DockItem *item = (DockItem *)user_data;
  if (!item)
    return;

  GList *handles = NULL;
  for (GList *l = item->windows; l != NULL; l = l->next) {
    AppWindow *win = (AppWindow *)l->data;
    handles = g_list_append(handles, win->handle);
  }
  for (GList *l = handles; l != NULL; l = l->next) {
    close_toplevel_window((struct zwlr_foreign_toplevel_handle_v1 *)l->data);
  }
  g_list_free(handles);

  if (item->popover)
    gtk_popover_popdown(GTK_POPOVER(item->popover));
}

static void on_popover_launch_clicked(DockItem *item) {
  if (item && item->exec && *item->exec != '\0') {
    GError *error = NULL;
    if (!g_spawn_command_line_async(item->exec, &error)) {
      g_warning("[Lambdock] Failed to launch '%s': %s", item->exec,
                error->message);
      g_clear_error(&error);
    }
  }
  if (item && item->popover)
    gtk_popover_popdown(GTK_POPOVER(item->popover));
}

void populate_popover_menu(DockItem *item) {
  if (!item || !item->popover)
    return;

  GtkWidget *old_child = gtk_popover_get_child(GTK_POPOVER(item->popover));
  if (old_child)
    gtk_popover_set_child(GTK_POPOVER(item->popover), NULL);

  GtkWidget *vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
  gtk_widget_set_margin_start(vbox, 8);
  gtk_widget_set_margin_end(vbox, 8);
  gtk_widget_set_margin_top(vbox, 8);
  gtk_widget_set_margin_bottom(vbox, 8);

  const char *title = (item->name && *item->name != '\0')       ? item->name
                      : (item->app_id && *item->app_id != '\0') ? item->app_id
                                                                : "Application";
  GtkWidget *label = gtk_label_new(title);
  gtk_widget_add_css_class(label, "title");
  gtk_box_append(GTK_BOX(vbox), label);
  gtk_box_append(GTK_BOX(vbox), gtk_separator_new(GTK_ORIENTATION_HORIZONTAL));

  if (item->windows != NULL) {
    int win_count = g_list_length(item->windows);
    for (GList *l = item->windows; l != NULL; l = l->next) {
      AppWindow *win = (AppWindow *)l->data;
      GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);

      const char *win_title =
          (win->title && *win->title != '\0') ? win->title : "Window";
      GtkWidget *btn_focus = gtk_button_new_with_label(win_title);
      gtk_widget_add_css_class(btn_focus, "flat");
      gtk_widget_set_hexpand(btn_focus, TRUE);

      GtkWidget *child_label = gtk_button_get_child(GTK_BUTTON(btn_focus));
      if (GTK_IS_LABEL(child_label)) {
        gtk_label_set_ellipsize(GTK_LABEL(child_label), PANGO_ELLIPSIZE_END);
        gtk_label_set_max_width_chars(GTK_LABEL(child_label), 30);
      }

      WindowActionData *action_data = g_new0(WindowActionData, 1);
      action_data->item = item;
      action_data->handle = win->handle;
      g_signal_connect_data(btn_focus, "clicked",
                            G_CALLBACK(on_popover_focus_window), action_data,
                            free_action_data, 0);
      gtk_box_append(GTK_BOX(row), btn_focus);

      GtkWidget *btn_close =
          gtk_button_new_from_icon_name("window-close-symbolic");
      gtk_widget_add_css_class(btn_close, "flat");

      WindowActionData *close_data = g_new0(WindowActionData, 1);
      close_data->item = item;
      close_data->handle = win->handle;
      g_signal_connect_data(btn_close, "clicked",
                            G_CALLBACK(on_popover_close_window), close_data,
                            free_action_data, 0);
      gtk_box_append(GTK_BOX(row), btn_close);
      gtk_box_append(GTK_BOX(vbox), row);
    }

    gtk_box_append(GTK_BOX(vbox),
                   gtk_separator_new(GTK_ORIENTATION_HORIZONTAL));

    if (item->exec && *item->exec != '\0') {
      GtkWidget *btn_launch = gtk_button_new_with_label("New Instance");
      gtk_widget_add_css_class(btn_launch, "flat");
      g_signal_connect_swapped(btn_launch, "clicked",
                               G_CALLBACK(on_popover_launch_clicked), item);
      gtk_box_append(GTK_BOX(vbox), btn_launch);
    }

    if (win_count > 1) {
      GtkWidget *btn_close_all = gtk_button_new_with_label("Close All Windows");
      gtk_widget_add_css_class(btn_close_all, "flat");
      g_signal_connect(btn_close_all, "clicked",
                       G_CALLBACK(on_popover_close_all_windows), item);
      gtk_box_append(GTK_BOX(vbox), btn_close_all);
    }
  } else if (item->exec && *item->exec != '\0') {
    GtkWidget *btn_launch = gtk_button_new_with_label("Launch Application");
    gtk_widget_add_css_class(btn_launch, "flat");
    g_signal_connect_swapped(btn_launch, "clicked",
                             G_CALLBACK(on_popover_launch_clicked), item);
    gtk_box_append(GTK_BOX(vbox), btn_launch);
  }

  gtk_popover_set_child(GTK_POPOVER(item->popover), vbox);
}
