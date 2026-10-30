// lambdock -- hyper-hackable desktop dock for Wayland
//
// SPDX-License-Identifier: MIT
// Copyright © Josep Bigorra <jjbigorra@gmail.com>

#include "popover_menu.h"
#include "../accessibility.h"
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
 * MENU ROWS
 * ============================================================================
 * Every entry of the menu is the same shape: a symbolic icon, a left-aligned
 * label, and the whole row is the button. The icons are what make a menu of
 * five kinds of entry (windows, actions, views, folders, removals) scannable at
 * a glance; the label stays the only thing that names the action, so the menu
 * reads the same to a screen reader.
 */

/** Class of the popover child this file builds, which is how the navigation
 * below tells an item menu from a stack panel. */
#define POPOVER_MENU_CLASS "popover-menu"

static GtkWidget *menu_icon_new(const char *icon_name) {
  GtkWidget *image = gtk_image_new_from_icon_name(icon_name);
  gtk_image_set_pixel_size(GTK_IMAGE(image), POPOVER_MENU_ICON_SIZE);
  gtk_widget_set_valign(image, GTK_ALIGN_CENTER);
  gtk_widget_add_css_class(image, "menu-icon");

  return image;
}

/**
 * menu_row_new:
 * @icon_name: (nullable): symbolic icon for the row, or NULL for a bare label.
 * @label: the row's text; it is also its accessible name.
 *
 * Builds one row: a flat button holding the icon and a left-aligned label.
 */
static GtkWidget *menu_row_new(const char *icon_name, const char *label) {
  GtkWidget *button = gtk_button_new();
  gtk_widget_add_css_class(button, "flat");
  gtk_widget_add_css_class(button, "menu-row");

  GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
  gtk_widget_add_css_class(row, "menu-row-box");

  if (icon_name)
    gtk_box_append(GTK_BOX(row), menu_icon_new(icon_name));

  GtkWidget *text = gtk_label_new(label);
  gtk_label_set_xalign(GTK_LABEL(text), 0.0f);
  gtk_widget_set_hexpand(text, TRUE);
  gtk_widget_add_css_class(text, "menu-label");
  gtk_box_append(GTK_BOX(row), text);

  gtk_button_set_child(GTK_BUTTON(button), row);
  return button;
}

/** The row's action icon, or NULL for a row without one. */
static const char *app_action_icon(LambdockAppAction action) {
  switch (action) {
  case LAMBDOCK_APP_ACTION_NEW_WINDOW:
    return "window-new-symbolic";
  case LAMBDOCK_APP_ACTION_PRIVATE_WINDOW:
    return "view-conceal-symbolic";
  case LAMBDOCK_APP_ACTION_PLAY_PAUSE:
    return "media-playback-start-symbolic";
  case LAMBDOCK_APP_ACTION_PREVIOUS:
    return "media-skip-backward-symbolic";
  case LAMBDOCK_APP_ACTION_NEXT:
    return "media-skip-forward-symbolic";
  case LAMBDOCK_APP_ACTION_STOP:
    return "media-playback-stop-symbolic";
  default:
    return NULL;
  }
}

/* -------------------------------------------------------------------------
 * MENU KEYBOARD NAVIGATION
 * -------------------------------------------------------------------------
 * A context menu is expected to answer the arrow keys, and to have somewhere
 * sensible to start: the focus lands on the first row when the menu opens.
 *
 * The controller lives on the popover (which is also how Escape reaches it),
 * but it only acts when the popover is showing an *item menu*: a stack panel
 * reuses the same popover and has its own arrow handling in its view, so a
 * menu-level handler there would swallow the keys it needs.
 * ------------------------------------------------------------------------- */

/** Every sensitive, focusable button of the menu, in top-to-bottom order. */
static void menu_collect_rows(GtkWidget *widget, GPtrArray *out) {
  for (GtkWidget *child = gtk_widget_get_first_child(widget); child != NULL;
       child = gtk_widget_get_next_sibling(child)) {
    if (GTK_IS_BUTTON(child) && gtk_widget_get_sensitive(child) &&
        gtk_widget_get_focusable(child))
      g_ptr_array_add(out, child);

    menu_collect_rows(child, out);
  }
}

static GPtrArray *menu_rows(GtkWidget *popover) {
  GPtrArray *rows = g_ptr_array_new();

  GtkWidget *child = gtk_popover_get_child(GTK_POPOVER(popover));
  if (child && gtk_widget_has_css_class(child, POPOVER_MENU_CLASS))
    menu_collect_rows(child, rows);

  return rows;
}

/** Index of the row that holds the focus, or -1. */
static int menu_focused_row(GtkWidget *popover, GPtrArray *rows) {
  GtkRoot *root = gtk_widget_get_root(popover);
  GtkWidget *focus = (root && GTK_IS_WINDOW(root))
                         ? gtk_window_get_focus(GTK_WINDOW(root))
                         : NULL;
  if (!focus)
    return -1;

  for (guint i = 0; i < rows->len; i++) {
    GtkWidget *row = g_ptr_array_index(rows, i);
    for (GtkWidget *w = focus; w != NULL; w = gtk_widget_get_parent(w)) {
      if (w == row)
        return (int)i;
      if (w == popover)
        break;
    }
  }

  return -1;
}

static gboolean menu_focus_row(GtkWidget *popover, int index) {
  GPtrArray *rows = menu_rows(popover);
  gboolean focused = FALSE;

  if (rows->len > 0) {
    int target = CLAMP(index, 0, (int)rows->len - 1);
    gtk_widget_grab_focus(g_ptr_array_index(rows, target));
    focused = TRUE;
  }

  g_ptr_array_unref(rows);
  return focused;
}

/** Moves the focus by @delta rows from wherever it is. */
static gboolean menu_focus_step(GtkWidget *popover, int delta) {
  GPtrArray *rows = menu_rows(popover);
  if (rows->len == 0) {
    g_ptr_array_unref(rows);
    return FALSE;
  }

  int current = menu_focused_row(popover, rows);
  int next = (current < 0) ? (delta > 0 ? 0 : (int)rows->len - 1)
                           : CLAMP(current + delta, 0, (int)rows->len - 1);
  g_ptr_array_unref(rows);

  return menu_focus_row(popover, next);
}

static gboolean on_menu_key_pressed(GtkEventControllerKey *controller,
                                    guint keyval, guint keycode,
                                    GdkModifierType state, gpointer user_data) {
  (void)controller;
  (void)keycode;
  (void)state;

  GtkWidget *popover = GTK_WIDGET(user_data);

  switch (keyval) {
  case GDK_KEY_Down:
    return menu_focus_step(popover, 1);
  case GDK_KEY_Up:
    return menu_focus_step(popover, -1);
  case GDK_KEY_Home:
    return menu_focus_row(popover, 0);
  case GDK_KEY_End: {
    GPtrArray *rows = menu_rows(popover);
    gboolean focused =
        rows->len > 0 && menu_focus_row(popover, (int)rows->len - 1);
    g_ptr_array_unref(rows);
    return focused;
  }
  default:
    return FALSE;
  }
}

void lambdock_popover_menu_keys(GtkWidget *popover) {
  if (!popover || !GTK_IS_POPOVER(popover))
    return;

  /* Once per popover: a rebuild replaces the contents, not the widget. */
  if (g_object_get_data(G_OBJECT(popover), "lambdock-menu-keys"))
    return;

  GtkEventController *keys = gtk_event_controller_key_new();
  gtk_event_controller_set_propagation_phase(keys, GTK_PHASE_CAPTURE);
  g_signal_connect(keys, "key-pressed", G_CALLBACK(on_menu_key_pressed),
                   popover);
  gtk_widget_add_controller(popover, keys);
  g_object_set_data(G_OBJECT(popover), "lambdock-menu-keys",
                    GINT_TO_POINTER(1));
}

void lambdock_popover_menu_focus_first(GtkWidget *popover) {
  if (!popover || !GTK_IS_POPOVER(popover))
    return;

  /* Only an item menu has rows to land on; a stack panel focuses its first
   * entry itself, in lambdock_stack_show(). */
  menu_focus_row(popover, 0);
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

  /* The window may have closed (and the tracker freed its proxy) while this
   * menu was open: a stale handle here would be a use-after-free. */
  if (!data || !data->item || !dock_item_has_window(data->item, data->handle))
    return;

  // Request Wayland compositor to activate (focus) this specific window
  activate_toplevel_window(data->handle);

  // Close/dismiss the active GTK popover popup menu
  if (data->item->popover)
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

  /* Same stale-handle guard as on_popover_focus_window(). */
  if (!data || !data->item || !dock_item_has_window(data->item, data->handle))
    return;

  // Request Wayland compositor to close the foreign top-level window
  close_toplevel_window(data->handle);

  // Close/dismiss the active GTK popover popup menu
  if (data->item->popover)
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

  GtkWidget *button = menu_row_new(
      view == LAMBDOCK_STACK_VIEW_LIST ? "view-list-symbolic"
                                       : "view-grid-symbolic",
      label);
  gtk_widget_add_css_class(button, "view-mode");
  lambdock_accessible_update(button, label,
                             active ? _("The stack is shown this way.")
                                    : _("Shows the stack this way."));

  /* The active view is the one with a check at the end of its row, which is the
   * vocabulary the rest of the menu uses -- a bullet inside the label read like
   * a stray character. */
  if (active) {
    gtk_widget_add_css_class(button, "active");
    GtkWidget *row = gtk_button_get_child(GTK_BUTTON(button));
    if (row && GTK_IS_BOX(row))
      gtk_box_append(GTK_BOX(row), menu_icon_new("object-select-symbolic"));
  }

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
      GtkWidget *button = menu_row_new(app_action_icon(chosen),
                                       lambdock_app_action_label(chosen));

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

  GtkWidget *button =
      menu_row_new("edit-delete-symbolic", _("Remove from Dock"));
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
  /* This class is what tells the popover's key controller that what it holds is
   * an item menu and not a stack panel. */
  gtk_widget_add_css_class(vbox, POPOVER_MENU_CLASS);

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

  GtkWidget *btn_open =
      menu_row_new("user-trash-symbolic", _("Open Trash"));
  g_signal_connect(btn_open, "clicked", G_CALLBACK(on_popover_open_trash), item);
  gtk_box_append(GTK_BOX(vbox), btn_open);

  GtkWidget *btn_empty =
      menu_row_new("edit-delete-symbolic", _("Empty Trash"));
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

  GtkWidget *btn_open =
      menu_row_new("folder-open-symbolic", _("Open Folder"));
  g_signal_connect(btn_open, "clicked", G_CALLBACK(on_popover_open_folder),
                   item);
  gtk_box_append(GTK_BOX(vbox), btn_open);
}

/** Launchpad: reopen the overlay. */
static void menu_append_launchpad_section(GtkWidget *vbox, DockItem *item) {
  GtkWidget *btn_show =
      menu_row_new("view-app-grid-symbolic", _("Show Launchpad"));
  g_signal_connect(btn_show, "clicked",
                   G_CALLBACK(on_popover_show_launchpad), item);
  gtk_box_append(GTK_BOX(vbox), btn_show);
}

/**
 * One row: focus this window, or close it.
 *
 * The row carries the window's own state as classes, so the theme (and the
 * shared overlay stylesheet) can mark the window you are looking at and dim the
 * ones the compositor reported as minimized -- the two things a list of titles
 * otherwise cannot show. The close button is icon-only and therefore invisible
 * to a screen reader, so it is named after the window it closes.
 */
static void menu_append_window_row(GtkWidget *vbox, DockItem *item,
                                   AppWindow *win) {
  const char *win_title =
      (win->title && *win->title != '\0') ? win->title : _("Window");

  GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, POPOVER_ROW_SPACING);
  gtk_widget_add_css_class(row, "window-row");
  if (win->activated)
    gtk_widget_add_css_class(row, "current");
  if (win->minimized)
    gtk_widget_add_css_class(row, "minimized");

  /* A dot for the window that holds the focus; it is always present so the
   * titles of the rows line up whether or not one of them is current. */
  GtkWidget *marker =
      gtk_image_new_from_icon_name("media-record-symbolic");
  gtk_image_set_pixel_size(GTK_IMAGE(marker), 6);
  gtk_widget_set_valign(marker, GTK_ALIGN_CENTER);
  gtk_widget_add_css_class(marker, "window-current-marker");
  gtk_box_append(GTK_BOX(row), marker);

  GtkWidget *btn_focus = gtk_button_new_with_label(win_title);
  gtk_widget_add_css_class(btn_focus, "flat");
  gtk_widget_add_css_class(btn_focus, "window-title");
  gtk_widget_set_hexpand(btn_focus, TRUE);

  GtkWidget *child_label = gtk_button_get_child(GTK_BUTTON(btn_focus));
  if (GTK_IS_LABEL(child_label)) {
    gtk_label_set_ellipsize(GTK_LABEL(child_label), PANGO_ELLIPSIZE_END);
    gtk_label_set_max_width_chars(GTK_LABEL(child_label),
                                  POPOVER_TITLE_MAX_CHARS);
    gtk_label_set_xalign(GTK_LABEL(child_label), 0.0f);
  }
  lambdock_accessible_update(btn_focus, win_title,
                             _("Focus this window."));

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
  gtk_widget_add_css_class(btn_close, "window-close");
  gtk_widget_set_tooltip_text(btn_close, _("Close this window"));
  lambdock_accessible_update(btn_close, _("Close window"), win_title);

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

  GtkWidget *btn_launch =
      menu_row_new("window-new-symbolic", _("New Instance"));
  g_signal_connect_swapped(btn_launch, "clicked",
                           G_CALLBACK(on_popover_launch_clicked), item);
  gtk_box_append(GTK_BOX(vbox), btn_launch);
}

/** "Close All Windows", only for an item that has more than one. */
static void menu_append_close_all_row(GtkWidget *vbox, DockItem *item,
                                      int window_count) {
  if (window_count <= 1)
    return;

  GtkWidget *btn_close_all =
      menu_row_new("window-close-symbolic", _("Close All Windows"));
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
      GtkWidget *btn_launch =
          menu_row_new("system-run-symbolic", _("Launch Application"));
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

  /* Arrow-key navigation, installed once per popover; inert while the same
   * popover is showing a stack panel instead. */
  lambdock_popover_menu_keys(item->popover);

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
