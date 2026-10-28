// lambdock -- hyper-hackable desktop dock for Wayland
//
// SPDX-License-Identifier: MIT
// Copyright © Josep Bigorra <jjbigorra@gmail.com>

#include "popover_menu.h"
#include "../logger.h"
#include "../toplevel_tracker.h"
#include "app_actions.h"
#include "desktop_entry.h"
#include "dock_dnd.h"
#include "dock_item.h"
#include "launchpad.h"
#include "stack.h"
#include "trash.h"
#include <glib/gi18n.h>
#include <locale.h>

/* ============================================================================
 * INTERNAL DATA STRUCTURES
 * ============================================================================
 */

/**
 * WindowActionData:
 * @item: Pointer to the associated DockItem (icon) on the dock bar.
 * @handle: Pointer to the specific Wayland top-level window handle.
 *
 * Helper struct used to pass context into signal callback handlers when a
 * button inside a specific window row (Focus or Close) is clicked in the
 * popover menu.
 */
typedef struct {
  DockItem *item;
  struct zwlr_foreign_toplevel_handle_v1 *handle;
} WindowActionData;

/* ============================================================================
 * MEMORY CLEANUP CLOSURE CALLBACKS
 * ============================================================================
 */

/**
 * free_action_data:
 * @data: Pointer to the dynamically allocated WindowActionData struct.
 * @closure: Pointer to the GTK GClosure (unused here, marked with void).
 *
 * `GClosureNotify` memory cleanup callback passed to `g_signal_connect_data()`.
 * GTK automatically invokes this function when the button widget is destroyed
 * or the signal is disconnected, preventing memory leaks for dynamically
 * created payload data.
 */
static void free_action_data(gpointer data, GClosure *closure) {
  (void)closure; // Suppress "unused parameter" compiler warning
  g_free(data);  // Free the dynamically allocated WindowActionData memory block
}

/* ============================================================================
 * ESCAPE DISMISSAL
 * ============================================================================
 */

static gboolean on_popover_key_pressed(GtkEventControllerKey *controller,
                                       guint keyval, guint keycode,
                                       GdkModifierType state,
                                       gpointer user_data) {
  (void)controller;
  (void)keycode;
  (void)state;

  if (keyval != GDK_KEY_Escape)
    return FALSE;

  GtkWidget *popover = GTK_WIDGET(user_data);
  if (popover && gtk_widget_get_visible(popover))
    gtk_popover_popdown(GTK_POPOVER(popover));

  return TRUE;
}

void lambdock_popover_close_on_escape(GtkWidget *popover) {
  if (!popover || !GTK_IS_POPOVER(popover))
    return;

  /* Called once per popover, at creation: a reload rebuilds menu contents but
   * reuses the popover widget. */
  if (g_object_get_data(G_OBJECT(popover), "lambdock-escape-close"))
    return;

  GtkEventController *keys = gtk_event_controller_key_new();
  gtk_event_controller_set_propagation_phase(keys, GTK_PHASE_CAPTURE);
  g_signal_connect(keys, "key-pressed", G_CALLBACK(on_popover_key_pressed),
                   popover);
  g_object_set_data(G_OBJECT(keys), "lambdock-escape-close",
                    GINT_TO_POINTER(1));
  gtk_widget_add_controller(popover, keys);
  g_object_set_data(G_OBJECT(popover), "lambdock-escape-close",
                    GINT_TO_POINTER(1));
}

/* ============================================================================
 * SIGNAL CALLBACK HANDLERS (POPOVER BUTTON ACTIONS)
 * ============================================================================
 */

/**
 * on_popover_focus_window:
 * @btn: GTK Button widget clicked (unused).
 * @user_data: Pointer to `WindowActionData` containing item and window handle.
 *
 * Callback triggered when clicking an individual window title button in the
 * popover. Brings the selected application window to focus and dismisses the
 * popover menu.
 */
static void on_popover_focus_window(GtkButton *btn, gpointer user_data) {
  (void)btn; // Mark unused parameter
  WindowActionData *data = (WindowActionData *)user_data;

  // Request Wayland compositor to activate (focus) this specific window
  if (data && data->handle)
    activate_toplevel_window(data->handle);

  // Close/dismiss the active GTK popover popup menu
  if (data && data->item && data->item->popover)
    gtk_popover_popdown(GTK_POPOVER(data->item->popover));
}

/**
 * on_popover_close_window:
 * @btn: GTK Button widget clicked (unused).
 * @user_data: Pointer to `WindowActionData` containing item and window handle.
 *
 * Callback triggered when clicking the close ("X") icon button next to a window
 * entry. Sends a close request for the window to the Wayland compositor and
 * dismisses the popover.
 */
static void on_popover_close_window(GtkButton *btn, gpointer user_data) {
  (void)btn; // Mark unused parameter
  WindowActionData *data = (WindowActionData *)user_data;

  // Request Wayland compositor to close the foreign top-level window
  if (data && data->handle)
    close_toplevel_window(data->handle);

  // Close/dismiss the active GTK popover popup menu
  if (data && data->item && data->item->popover)
    gtk_popover_popdown(GTK_POPOVER(data->item->popover));
}

/**
 * on_popover_close_all_windows:
 * @btn: GTK Button widget clicked (unused).
 * @user_data: Pointer to the associated `DockItem`.
 *
 * Callback triggered when clicking the "Close All Windows" option in the
 * popover. Iterates through all open windows attached to this dock item and
 * sends a close request for each.
 */
static void on_popover_close_all_windows(GtkButton *btn, gpointer user_data) {
  (void)btn; // Mark unused parameter
  DockItem *item = (DockItem *)user_data;
  if (!item)
    return; // Defensive guard check

  // Create a temporary linked list of window handles to avoid modifying
  // `item->windows` directly while iterating over it
  GList *handles = NULL;
  for (GList *l = item->windows; l != NULL; l = l->next) {
    AppWindow *win = (AppWindow *)l->data;
    if (win && win->handle)
      handles = g_list_append(handles, win->handle);
  }

  // Iterate over handle copy list and send close requests for every window
  // instance
  for (GList *l = handles; l != NULL; l = l->next) {
    close_toplevel_window((struct zwlr_foreign_toplevel_handle_v1 *)l->data);
  }
  g_list_free(handles); // Clean up temporary handle list pointer memory

  // Dismiss the GTK popover menu
  if (item->popover)
    gtk_popover_popdown(GTK_POPOVER(item->popover));
}

/**
 * on_popover_launch_clicked:
 * @item: Pointer to the associated `DockItem`.
 *
 * Callback triggered when clicking "Launch Application" or "New Instance" in
 * the popover. Spawns a new application process asynchronously and dismisses
 * the popover.
 */
static void on_popover_launch_clicked(DockItem *item) {
  if (item && item->exec && *item->exec != '\0') {
    GError *error = NULL;

    // Asynchronously execute command string without blocking main thread
    // execution
    if (!g_spawn_command_line_async(item->exec, &error)) {
      g_warning("[Lambdock] Failed to launch '%s': %s", item->exec,
                error->message);
      g_clear_error(&error); // Clean up GLib error structure allocation
    }
  }

  // Dismiss the GTK popover menu
  if (item && item->popover)
    gtk_popover_popdown(GTK_POPOVER(item->popover));
}

/* ============================================================================
 * STACK / LAUNCHPAD / REMOVAL MENU ENTRIES
 * ============================================================================
 */

/**
 * StackViewAction:
 * @item: The stack dock item whose view mode is being changed.
 * @view: The view mode to switch to.
 */
typedef struct {
  DockItem *item;
  LambdockStackView view;
} StackViewAction;

static void free_stack_view_action(gpointer data, GClosure *closure) {
  (void)closure;
  g_free(data);
}

static void on_popover_stack_view(GtkButton *btn, gpointer user_data) {
  (void)btn;
  StackViewAction *action = (StackViewAction *)user_data;
  if (!action || !action->item)
    return;

  lambdock_stack_set_view(action->item, action->view);

  /* Rebuild the menu so the active marker follows the choice. */
  populate_popover_menu(action->item);
}

static GtkWidget *stack_view_button(DockItem *item, LambdockStackView view,
                                    const char *label) {
  gboolean active = (lambdock_stack_view_from_string(item->stack_view) == view);

  char *text = g_strdup_printf("%s %s", active ? "●" : "○", label);
  GtkWidget *button = gtk_button_new_with_label(text);
  g_free(text);

  gtk_widget_add_css_class(button, "flat");
  if (active)
    gtk_widget_add_css_class(button, "stack-view-active");

  StackViewAction *action = g_new0(StackViewAction, 1);
  action->item = item;
  action->view = view;
  g_signal_connect_data(button, "clicked", G_CALLBACK(on_popover_stack_view),
                        action, free_stack_view_action, 0);

  return button;
}

static void on_popover_open_folder(GtkButton *btn, gpointer user_data) {
  (void)btn;
  DockItem *item = (DockItem *)user_data;
  if (!item)
    return;

  lambdock_open_path(item->stack_path);

  if (item->popover)
    gtk_popover_popdown(GTK_POPOVER(item->popover));
}

static void on_popover_show_launchpad(GtkButton *btn, gpointer user_data) {
  (void)btn;
  DockItem *item = (DockItem *)user_data;
  if (!item)
    return;

  if (item->popover)
    gtk_popover_popdown(GTK_POPOVER(item->popover));

  lambdock_launchpad_show(item->state);
}

/* ============================================================================
 * TRASH MENU ENTRIES
 * ============================================================================
 */

static void on_popover_open_trash(GtkButton *btn, gpointer user_data) {
  (void)btn;
  DockItem *item = (DockItem *)user_data;
  if (!item)
    return;

  if (!lambdock_trash_open())
    LOG_C_WARN("UI", "Could not open the trash");

  if (item->popover)
    gtk_popover_popdown(GTK_POPOVER(item->popover));
}

/**
 * EmptyTrashJob:
 * @button: The trash item's button, referenced for as long as the job lives, so
 * the item can be looked up when emptying finishes (a reload may have replaced
 * it in the meantime).
 */
typedef struct {
  GtkWidget *button;
} EmptyTrashJob;

static void free_empty_trash_job(gpointer data) {
  EmptyTrashJob *job = (EmptyTrashJob *)data;
  g_clear_object(&job->button);
  g_free(job);
}

/* Emptying can delete thousands of files, so it does not run on the main
 * thread: the dock would stop drawing (and auto-hiding) until it was done. */
static void empty_trash_in_thread(GTask *task, gpointer source_object,
                                  gpointer task_data,
                                  GCancellable *cancellable) {
  (void)source_object;
  (void)task_data;
  (void)cancellable;

  guint removed = lambdock_trash_empty();
  g_task_return_int(task, (gssize)removed);
}

static void on_empty_trash_finished(GObject *source_object,
                                    GAsyncResult *result,
                                    gpointer user_data) {
  (void)source_object;
  (void)user_data;

  GTask *task = G_TASK(result);
  GError *error = NULL;
  gssize removed = g_task_propagate_int(task, &error);

  if (error) {
    LOG_C_WARN("UI", "Emptying the trash failed: %s", error->message);
    g_clear_error(&error);
  } else {
    LOG_C_INFO("UI", "Emptied the trash (%" G_GSSIZE_FORMAT " items)",
               removed);
  }

  EmptyTrashJob *job = (EmptyTrashJob *)g_task_get_task_data(task);
  DockItem *item = job ? dock_item_from_widget(job->button) : NULL;
  if (item)
    dock_item_refresh_trash(item);
}

static void on_popover_empty_trash(GtkButton *btn, gpointer user_data) {
  (void)btn;
  DockItem *item = (DockItem *)user_data;
  if (!item)
    return;

  if (item->popover)
    gtk_popover_popdown(GTK_POPOVER(item->popover));

  EmptyTrashJob *job = g_new0(EmptyTrashJob, 1);
  job->button = item->button ? g_object_ref(item->button) : NULL;

  GTask *task = g_task_new(NULL, NULL, on_empty_trash_finished, NULL);
  g_task_set_task_data(task, job, free_empty_trash_job);
  g_task_run_in_thread(task, empty_trash_in_thread);
  g_object_unref(task);
}

/* ============================================================================
 * APPLICATION-SPECIFIC MENU ENTRIES
 * ============================================================================
 */

/**
 * AppActionData:
 * @item: The dock item the action belongs to.
 * @action: Which action this button carries out.
 * @music_bus: MPRIS bus name of the player to drive, or NULL for a launch.
 */
typedef struct {
  DockItem *item;
  LambdockAppAction action;
  char *music_bus;
} AppActionData;

static void free_app_action_data(gpointer data, GClosure *closure) {
  (void)closure;
  AppActionData *action = (AppActionData *)data;
  g_free(action->music_bus);
  g_free(action);
}

/**
 * on_popover_app_action:
 * @btn: The menu button (unused).
 * @user_data: The `AppActionData` of the entry.
 *
 * Runs one kind-specific action: a browser window command is spawned like any
 * other launch, and a playback control is sent to the player over MPRIS.
 */
static void on_popover_app_action(GtkButton *btn, gpointer user_data) {
  (void)btn;
  AppActionData *action = (AppActionData *)user_data;
  if (!action || !action->item)
    return;

  if (lambdock_app_action_is_music(action->action)) {
    lambdock_app_music_invoke(action->music_bus, action->action);
  } else {
    char *command =
        lambdock_app_action_command(action->item->exec, action->action);
    if (command) {
      GError *error = NULL;
      if (!g_spawn_command_line_async(command, &error)) {
        LOG_C_WARN("UI", "Failed to launch '%s': %s", command,
                   error->message);
        g_clear_error(&error);
      }
      g_free(command);
    }
  }

  if (action->item->popover)
    gtk_popover_popdown(GTK_POPOVER(action->item->popover));
}

/**
 * item_app_actions:
 * @item: An application item.
 * @kind_out: (out) (nullable): the kind of application @item is.
 * @music_bus_out: (out) (nullable): the running player's bus name, or NULL.
 *
 * The kind-specific actions the item offers. The desktop entry behind the item
 * supplies the categories that classify it, and the session bus says whether a
 * music player of that application is running.
 *
 * Returns: (transfer full): a GArray of LambdockAppAction (guint) values.
 */
static GArray *item_app_actions(DockItem *item, LambdockAppKind *kind_out,
                                char **music_bus_out) {
  const LambdockDesktopEntry *entry = lambdock_desktop_entry_lookup(
      (item->desktop_id && *item->desktop_id != '\0') ? item->desktop_id
                                                      : item->app_id);
  const char *categories = entry ? entry->categories : NULL;
  const char *app_id = (entry && entry->stem) ? entry->stem : item->app_id;
  const char *name = (item->name && *item->name != '\0')
                         ? item->name
                         : (entry ? entry->name : NULL);

  LambdockAppKind kind = lambdock_app_kind(app_id, name, item->exec, categories);
  if (kind_out)
    *kind_out = kind;

  char *music_bus = NULL;
  if (kind == LAMBDOCK_APP_KIND_MUSIC)
    music_bus = lambdock_app_music_player_find(app_id, name);

  GArray *actions =
      lambdock_app_actions(app_id, name, item->exec, categories, music_bus);
  if (music_bus_out)
    *music_bus_out = music_bus;
  else
    g_free(music_bus);

  return actions;
}

/**
 * append_action_buttons:
 * @vbox: The popover's vertical box, appended to.
 * @item: The application item the actions belong to.
 * @actions: Actions from item_app_actions().
 * @music_bus: (nullable): the player's bus name, for the music actions.
 *
 * Adds the section that belongs to the kind of application the item is beneath
 * a separator: a browser's new and private window commands, or a running music
 * player's playback controls. Nothing is added when the list is empty.
 */
static void append_action_buttons(GtkWidget *vbox, DockItem *item,
                                  GArray *actions, const char *music_bus) {
  if (actions->len > 0) {
    gtk_box_append(GTK_BOX(vbox),
                   gtk_separator_new(GTK_ORIENTATION_HORIZONTAL));

    for (guint i = 0; i < actions->len; i++) {
      LambdockAppAction chosen = (LambdockAppAction)g_array_index(actions,
                                                                  guint, i);
      GtkWidget *button =
          gtk_button_new_with_label(lambdock_app_action_label(chosen));
      gtk_widget_add_css_class(button, "flat");

      AppActionData *data = g_new0(AppActionData, 1);
      data->item = item;
      data->action = chosen;
      data->music_bus = music_bus ? g_strdup(music_bus) : NULL;
      g_signal_connect_data(button, "clicked", G_CALLBACK(on_popover_app_action),
                            data, free_app_action_data, 0);
      gtk_box_append(GTK_BOX(vbox), button);
    }
  }
}

static void on_popover_remove_item(GtkButton *btn, gpointer user_data) {
  (void)btn;
  DockItem *item = (DockItem *)user_data;

  /* The item, its popover and this very button are about to be destroyed, so
   * the actual removal happens from an idle callback. */
  schedule_remove_dock_item(item);
}

/* Appends the shared trailing entry that unpins an item from the dock. */
static void append_remove_entry(GtkWidget *vbox, DockItem *item) {
  if (!item->is_favorite)
    return;

  gtk_box_append(GTK_BOX(vbox), gtk_separator_new(GTK_ORIENTATION_HORIZONTAL));

  GtkWidget *button = gtk_button_new_with_label(_("Remove from Dock"));
  gtk_widget_add_css_class(button, "flat");
  gtk_widget_add_css_class(button, "destructive-action");
  g_signal_connect(button, "clicked", G_CALLBACK(on_popover_remove_item), item);
  gtk_box_append(GTK_BOX(vbox), button);
}

/* ============================================================================
 * POPOVER MENU UI GENERATOR & DYNAMIC REBUILDER
 * ============================================================================
 */

/* -------------------------------------------------------------------------
 * MENU SECTIONS
 * -------------------------------------------------------------------------
 * The context menu used to be one 200-line function with four early returns.
 * Each kind of item now has its own small section builder, and the entry point
 * below is only the running order.
 */

/** The popover's padded vertical container. */
static GtkWidget *popover_menu_vbox_new(void) {
  GtkWidget *vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, POPOVER_VBOX_SPACING);

  gtk_widget_set_margin_start(vbox, POPOVER_VBOX_MARGIN);
  gtk_widget_set_margin_end(vbox, POPOVER_VBOX_MARGIN);
  gtk_widget_set_margin_top(vbox, POPOVER_VBOX_MARGIN);
  gtk_widget_set_margin_bottom(vbox, POPOVER_VBOX_MARGIN);

  return vbox;
}

/** The item's name (or a sensible fallback) and the rule under it. */
static void menu_append_header(GtkWidget *vbox, const DockItem *item) {
  const char *title = lambdock_item_title(item, _("Application"));
  GtkWidget *label = gtk_label_new(title);

  gtk_widget_add_css_class(label, "title");
  gtk_box_append(GTK_BOX(vbox), label);
  gtk_box_append(GTK_BOX(vbox), gtk_separator_new(GTK_ORIENTATION_HORIZONTAL));
}

/** Trash: how much is in it, open it, empty it (no windows to list). */
static void menu_append_trash_section(GtkWidget *vbox, DockItem *item) {
  guint count = lambdock_trash_count();
  char *count_text = g_strdup_printf(
      ngettext("%u item in the trash", "%u items in the trash", count), count);

  GtkWidget *count_label = gtk_label_new(count_text);
  g_free(count_text);
  gtk_widget_add_css_class(count_label, "dim-label");
  gtk_box_append(GTK_BOX(vbox), count_label);

  GtkWidget *btn_open = gtk_button_new_with_label(_("Open Trash"));
  gtk_widget_add_css_class(btn_open, "flat");
  g_signal_connect(btn_open, "clicked", G_CALLBACK(on_popover_open_trash), item);
  gtk_box_append(GTK_BOX(vbox), btn_open);

  GtkWidget *btn_empty = gtk_button_new_with_label(_("Empty Trash"));
  gtk_widget_add_css_class(btn_empty, "flat");
  gtk_widget_add_css_class(btn_empty, "destructive-action");
  gtk_widget_set_sensitive(btn_empty, count > 0);
  g_signal_connect(btn_empty, "clicked", G_CALLBACK(on_popover_empty_trash),
                   item);
  gtk_box_append(GTK_BOX(vbox), btn_empty);
}

/** Stack: the view switch plus the folder action. */
static void menu_append_stack_section(GtkWidget *vbox, DockItem *item) {
  GtkWidget *views = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 4);

  gtk_box_append(GTK_BOX(views),
                 stack_view_button(item, LAMBDOCK_STACK_VIEW_GRID, _("Grid")));
  gtk_box_append(GTK_BOX(views),
                 stack_view_button(item, LAMBDOCK_STACK_VIEW_LIST, _("List")));
  gtk_box_append(GTK_BOX(vbox), views);

  GtkWidget *btn_open = gtk_button_new_with_label(_("Open Folder"));
  gtk_widget_add_css_class(btn_open, "flat");
  g_signal_connect(btn_open, "clicked", G_CALLBACK(on_popover_open_folder),
                   item);
  gtk_box_append(GTK_BOX(vbox), btn_open);
}

/** Launchpad: reopen the overlay. */
static void menu_append_launchpad_section(GtkWidget *vbox, DockItem *item) {
  GtkWidget *btn_show = gtk_button_new_with_label(_("Show Launchpad"));

  gtk_widget_add_css_class(btn_show, "flat");
  g_signal_connect(btn_show, "clicked",
                   G_CALLBACK(on_popover_show_launchpad), item);
  gtk_box_append(GTK_BOX(vbox), btn_show);
}

/** One row: focus this window, or close it. */
static void menu_append_window_row(GtkWidget *vbox, DockItem *item,
                                   AppWindow *win) {
  GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, POPOVER_ROW_SPACING);

  const char *win_title =
      (win->title && *win->title != '\0') ? win->title : _("Window");

  GtkWidget *btn_focus = gtk_button_new_with_label(win_title);
  gtk_widget_add_css_class(btn_focus, "flat");
  gtk_widget_set_hexpand(btn_focus, TRUE);

  GtkWidget *child_label = gtk_button_get_child(GTK_BUTTON(btn_focus));
  if (GTK_IS_LABEL(child_label)) {
    gtk_label_set_ellipsize(GTK_LABEL(child_label), PANGO_ELLIPSIZE_END);
    gtk_label_set_max_width_chars(GTK_LABEL(child_label),
                                  POPOVER_TITLE_MAX_CHARS);
  }

  /* `g_signal_connect_data` frees the action data with the button. */
  WindowActionData *focus_data = g_new0(WindowActionData, 1);
  focus_data->item = item;
  focus_data->handle = win->handle;
  g_signal_connect_data(btn_focus, "clicked",
                        G_CALLBACK(on_popover_focus_window), focus_data,
                        free_action_data, 0);
  gtk_box_append(GTK_BOX(row), btn_focus);

  GtkWidget *btn_close = gtk_button_new_from_icon_name("window-close-symbolic");
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

/** "New Instance" (or "New Window" for a browser, in its own section). */
static void menu_append_new_instance_row(GtkWidget *vbox, DockItem *item,
                                         LambdockAppKind app_kind) {
  if (!item->exec || *item->exec == '\0' ||
      app_kind == LAMBDOCK_APP_KIND_BROWSER)
    return;

  GtkWidget *btn_launch = gtk_button_new_with_label(_("New Instance"));
  gtk_widget_add_css_class(btn_launch, "flat");
  g_signal_connect_swapped(btn_launch, "clicked",
                           G_CALLBACK(on_popover_launch_clicked), item);
  gtk_box_append(GTK_BOX(vbox), btn_launch);
}

/** "Close All Windows", only for an item that has more than one. */
static void menu_append_close_all_row(GtkWidget *vbox, DockItem *item,
                                      int window_count) {
  if (window_count <= 1)
    return;

  GtkWidget *btn_close_all = gtk_button_new_with_label(_("Close All Windows"));
  gtk_widget_add_css_class(btn_close_all, "flat");
  g_signal_connect(btn_close_all, "clicked",
                   G_CALLBACK(on_popover_close_all_windows), item);
  gtk_box_append(GTK_BOX(vbox), btn_close_all);
}

/** The running application's windows, plus what to do about them. */
static void menu_append_window_section(GtkWidget *vbox, DockItem *item,
                                       LambdockAppKind app_kind) {
  if (item->windows == NULL) {
    /* Nothing is running: offer to start it. */
    if (item->exec && *item->exec != '\0') {
      GtkWidget *btn_launch = gtk_button_new_with_label(_("Launch Application"));
      gtk_widget_add_css_class(btn_launch, "flat");
      g_signal_connect_swapped(btn_launch, "clicked",
                               G_CALLBACK(on_popover_launch_clicked), item);
      gtk_box_append(GTK_BOX(vbox), btn_launch);
    }
    return;
  }

  int window_count = 0;
  for (GList *l = item->windows; l != NULL; l = l->next) {
    AppWindow *win = (AppWindow *)l->data;
    if (!win)
      continue;

    menu_append_window_row(vbox, item, win);
    window_count++;
  }

  gtk_box_append(GTK_BOX(vbox),
                 gtk_separator_new(GTK_ORIENTATION_HORIZONTAL));

  menu_append_new_instance_row(vbox, item, app_kind);
  menu_append_close_all_row(vbox, item, window_count);
}

/** Kind-specific actions: browser windows, player controls. */
static void menu_append_app_actions(GtkWidget *vbox, DockItem *item,
                                    GArray *app_actions, char *music_bus) {
  if (!app_actions)
    return;

  append_action_buttons(vbox, item, app_actions, music_bus);
}

/** Installs @vbox as the popover's child. */
static void menu_finish(DockItem *item, GtkWidget *vbox) {
  append_remove_entry(vbox, item);
  gtk_popover_set_child(GTK_POPOVER(item->popover), vbox);
}

/* -------------------------------------------------------------------------
 * THE MENU
 * -------------------------------------------------------------------------
 */

void populate_popover_menu(DockItem *item) {
  if (!item || !item->popover)
    return;

  /* Rebuild from scratch: the menu always describes the current state. */
  if (gtk_popover_get_child(GTK_POPOVER(item->popover)))
    gtk_popover_set_child(GTK_POPOVER(item->popover), NULL);

  GtkWidget *vbox = popover_menu_vbox_new();
  menu_append_header(vbox, item);

  if (item->kind == LAMBDOCK_ITEM_TRASH) {
    menu_append_trash_section(vbox, item);
    menu_finish(item, vbox);
    return;
  }

  if (item->kind == LAMBDOCK_ITEM_STACK) {
    menu_append_stack_section(vbox, item);
    menu_finish(item, vbox);
    return;
  }

  if (item->kind == LAMBDOCK_ITEM_LAUNCHPAD) {
    menu_append_launchpad_section(vbox, item);
    menu_finish(item, vbox);
    return;
  }

  /* Application items may have browser or music-player actions. They are
   * computed before the window list because a browser's "New Window" entry
   * replaces the generic "New Instance" one. */
  GArray *app_actions = NULL;
  LambdockAppKind app_kind = LAMBDOCK_APP_KIND_NONE;
  char *music_bus = NULL;

  if (item->kind == LAMBDOCK_ITEM_APP)
    app_actions = item_app_actions(item, &app_kind, &music_bus);

  menu_append_window_section(vbox, item, app_kind);
  menu_append_app_actions(vbox, item, app_actions, music_bus);

  g_clear_pointer(&app_actions, g_array_unref);
  g_free(music_bus);

  menu_finish(item, vbox);
}
