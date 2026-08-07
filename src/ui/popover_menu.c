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

#include "popover_menu.h"
#include "../toplevel_tracker.h"

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
 * POPOVER MENU UI GENERATOR & DYNAMIC REBUILDER
 * ============================================================================
 */

/**
 * populate_popover_menu:
 * @item: Pointer to target `DockItem` whose menu needs to be populated/built.
 *
 * Dynamically constructs and attaches the GTK menu layout inside
 * `item->popover`.
 *
 * Menu layout structure built:
 * ┌──────────────────────────────────────────────┐
 * │ App Name Header Label                        │
 * │ ───────────────────────────────────────────  │
 * │ [ Window Title 1... ]   [ X ]                │  <-- (If open windows exist)
 * │ [ Window Title 2... ]   [ X ]                │
 * │ ───────────────────────────────────────────  │
 * │ New Instance                                 │  <-- (If exec string exists)
 * │ Close All Windows                            │  <-- (If > 1 window exists)
 * └──────────────────────────────────────────────┘
 */
void populate_popover_menu(DockItem *item) {
  if (!item || !item->popover)
    return; // Defensive guard: exit if references are invalid

  // Clear existing popover child widget before rebuilding layout contents
  GtkWidget *old_child = gtk_popover_get_child(GTK_POPOVER(item->popover));
  if (old_child)
    gtk_popover_set_child(GTK_POPOVER(item->popover), NULL);

  // Create main vertical container box with configured element spacing and
  // padding margins
  GtkWidget *vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, POPOVER_VBOX_SPACING);
  gtk_widget_set_margin_start(vbox, POPOVER_VBOX_MARGIN);
  gtk_widget_set_margin_end(vbox, POPOVER_VBOX_MARGIN);
  gtk_widget_set_margin_top(vbox, POPOVER_VBOX_MARGIN);
  gtk_widget_set_margin_bottom(vbox, POPOVER_VBOX_MARGIN);

  // --------------------------------------------------------------------------
  // App Header Title & Separator Line
  // --------------------------------------------------------------------------
  const char *title = (item->name && *item->name != '\0')       ? item->name
                      : (item->app_id && *item->app_id != '\0') ? item->app_id
                                                                : "Application";
  GtkWidget *label = gtk_label_new(title);
  gtk_widget_add_css_class(label,
                           "title"); // Apply CSS class `.title` for styling
  gtk_box_append(GTK_BOX(vbox), label);
  gtk_box_append(GTK_BOX(vbox), gtk_separator_new(GTK_ORIENTATION_HORIZONTAL));

  // --------------------------------------------------------------------------
  // Open Application Window List Construction
  // --------------------------------------------------------------------------
  if (item->windows != NULL) {
    int win_count = g_list_length(item->windows);

    // Loop through each active open window attached to this dock launcher
    for (GList *l = item->windows; l != NULL; l = l->next) {
      AppWindow *win = (AppWindow *)l->data;

      // Horizontal row container: [ Focus Button | Close Button ]
      GtkWidget *row =
          gtk_box_new(GTK_ORIENTATION_HORIZONTAL, POPOVER_ROW_SPACING);

      const char *win_title =
          (win->title && *win->title != '\0') ? win->title : "Window";

      // Create focus/activate button with window title label
      GtkWidget *btn_focus = gtk_button_new_with_label(win_title);
      gtk_widget_add_css_class(btn_focus, "flat");
      gtk_widget_set_hexpand(btn_focus,
                             TRUE); // Allow button to expand horizontally

      // Format title text: Ellipsize long window titles with "..." at end
      GtkWidget *child_label = gtk_button_get_child(GTK_BUTTON(btn_focus));
      if (GTK_IS_LABEL(child_label)) {
        gtk_label_set_ellipsize(GTK_LABEL(child_label), PANGO_ELLIPSIZE_END);
        gtk_label_set_max_width_chars(GTK_LABEL(child_label),
                                      POPOVER_TITLE_MAX_CHARS);
      }

      // Prepare action data and attach click signal for focusing window
      WindowActionData *action_data = g_new0(WindowActionData, 1);
      action_data->item = item;
      action_data->handle = win->handle;

      // `g_signal_connect_data` attaches callback with `free_action_data`
      // automatically freeing `action_data` when the button widget is destroyed
      g_signal_connect_data(btn_focus, "clicked",
                            G_CALLBACK(on_popover_focus_window), action_data,
                            free_action_data, 0);
      gtk_box_append(GTK_BOX(row), btn_focus);

      // Create window close ("X") icon button
      GtkWidget *btn_close =
          gtk_button_new_from_icon_name("window-close-symbolic");
      gtk_widget_add_css_class(btn_close, "flat");

      // Prepare action data and attach click signal for closing window
      WindowActionData *close_data = g_new0(WindowActionData, 1);
      close_data->item = item;
      close_data->handle = win->handle;
      g_signal_connect_data(btn_close, "clicked",
                            G_CALLBACK(on_popover_close_window), close_data,
                            free_action_data, 0);
      gtk_box_append(GTK_BOX(row), btn_close);

      // Append completed window item row into main popover vertical box
      gtk_box_append(GTK_BOX(vbox), row);
    }

    // Horizontal separator line below open window entries
    gtk_box_append(GTK_BOX(vbox),
                   gtk_separator_new(GTK_ORIENTATION_HORIZONTAL));

    // ------------------------------------------------------------------------
    // Option: "New Instance" (When app is already running)
    // ------------------------------------------------------------------------
    if (item->exec && *item->exec != '\0') {
      GtkWidget *btn_launch = gtk_button_new_with_label("New Instance");
      gtk_widget_add_css_class(btn_launch, "flat");

      // `g_signal_connect_swapped` automatically passes `item` as the primary
      // first parameter to `on_popover_launch_clicked` instead of the button
      // pointer
      g_signal_connect_swapped(btn_launch, "clicked",
                               G_CALLBACK(on_popover_launch_clicked), item);
      gtk_box_append(GTK_BOX(vbox), btn_launch);
    }

    // ------------------------------------------------------------------------
    // Option: "Close All Windows" (Only shown if > 1 window open)
    // ------------------------------------------------------------------------
    if (win_count > 1) {
      GtkWidget *btn_close_all = gtk_button_new_with_label("Close All Windows");
      gtk_widget_add_css_class(btn_close_all, "flat");
      g_signal_connect(btn_close_all, "clicked",
                       G_CALLBACK(on_popover_close_all_windows), item);
      gtk_box_append(GTK_BOX(vbox), btn_close_all);
    }
  }
  // --------------------------------------------------------------------------
  // Option: "Launch Application" (When no windows are open yet)
  // --------------------------------------------------------------------------
  else if (item->exec && *item->exec != '\0') {
    GtkWidget *btn_launch = gtk_button_new_with_label("Launch Application");
    gtk_widget_add_css_class(btn_launch, "flat");
    g_signal_connect_swapped(btn_launch, "clicked",
                             G_CALLBACK(on_popover_launch_clicked), item);
    gtk_box_append(GTK_BOX(vbox), btn_launch);
  }

  // Set built vertical box layout as active child container of the GTK popover
  gtk_popover_set_child(GTK_POPOVER(item->popover), vbox);
}
