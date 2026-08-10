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

#include "dock_item.h" // Struct definitions for DockItem, LambdockState, AppWindow
#include "../guile_bridge.h"     // Scheme/Guile configuration interface
#include "../logger.h"           // Logging macro utilities (e.g., LOG_C_INFO)
#include "../toplevel_tracker.h" // Foreign window tracking via Wayland protocols
#include "icon_resolver.h" // Helper to load application icons from the system theme
#include "popover_menu.h" // Custom right-click and multi-window popover menus
#include "theme_engine.h" // CSS and theme engine utilities

/* ============================================================================
 * ALIGNMENT HELPERS
 * ============================================================================
 */

/**
 * apply_dock_item_alignment:
 * Applies edge-anchored alignment rules (valign/halign) to DockItem widgets,
 * ensuring non-animated items and separators remain anchored to the dock edge.
 */
void apply_dock_item_alignment(DockItem *item) {
  if (!item || !item->button || !item->state)
    return;

  LambdockState *state = item->state;
  int base_sz = (state->icon_size > 0) ? state->icon_size : DEFAULT_ICON_SIZE;

  // Set fixed base dimension on separators to prevent stretching
  if (GTK_IS_SEPARATOR(item->button)) {
    if (state->orientation == GTK_ORIENTATION_HORIZONTAL) {
      gtk_widget_set_size_request(item->button, -1, base_sz);
    } else {
      gtk_widget_set_size_request(item->button, base_sz, -1);
    }
  }

  GtkWidget *child = NULL;
  if (GTK_IS_BUTTON(item->button)) {
    child = gtk_button_get_child(GTK_BUTTON(item->button));
  }

  if (state->edge == GTK_LAYER_SHELL_EDGE_BOTTOM) {
    gtk_widget_set_valign(item->button, GTK_ALIGN_END);
    gtk_widget_set_halign(item->button, GTK_ALIGN_CENTER);
    if (child) {
      gtk_widget_set_valign(child, GTK_ALIGN_END);
      gtk_widget_set_halign(child, GTK_ALIGN_CENTER);
      if (GTK_IS_BOX(child)) {
        for (GtkWidget *w = gtk_widget_get_first_child(child); w != NULL;
             w = gtk_widget_get_next_sibling(w)) {
          gtk_widget_set_valign(w, GTK_ALIGN_END);
        }
      }
    }
  } else if (state->edge == GTK_LAYER_SHELL_EDGE_TOP) {
    gtk_widget_set_valign(item->button, GTK_ALIGN_START);
    gtk_widget_set_halign(item->button, GTK_ALIGN_CENTER);
    if (child) {
      gtk_widget_set_valign(child, GTK_ALIGN_START);
      gtk_widget_set_halign(child, GTK_ALIGN_CENTER);
      if (GTK_IS_BOX(child)) {
        for (GtkWidget *w = gtk_widget_get_first_child(child); w != NULL;
             w = gtk_widget_get_next_sibling(w)) {
          gtk_widget_set_valign(w, GTK_ALIGN_START);
        }
      }
    }
  } else if (state->edge == GTK_LAYER_SHELL_EDGE_LEFT) {
    gtk_widget_set_halign(item->button, GTK_ALIGN_START);
    gtk_widget_set_valign(item->button, GTK_ALIGN_CENTER);
    if (child) {
      gtk_widget_set_halign(child, GTK_ALIGN_START);
      gtk_widget_set_valign(child, GTK_ALIGN_CENTER);
      if (GTK_IS_BOX(child)) {
        for (GtkWidget *w = gtk_widget_get_first_child(child); w != NULL;
             w = gtk_widget_get_next_sibling(w)) {
          gtk_widget_set_halign(w, GTK_ALIGN_START);
        }
      }
    }
  } else if (state->edge == GTK_LAYER_SHELL_EDGE_RIGHT) {
    gtk_widget_set_halign(item->button, GTK_ALIGN_END);
    gtk_widget_set_valign(item->button, GTK_ALIGN_CENTER);
    if (child) {
      gtk_widget_set_halign(child, GTK_ALIGN_END);
      gtk_widget_set_valign(child, GTK_ALIGN_CENTER);
      if (GTK_IS_BOX(child)) {
        for (GtkWidget *w = gtk_widget_get_first_child(child); w != NULL;
             w = gtk_widget_get_next_sibling(w)) {
          gtk_widget_set_halign(w, GTK_ALIGN_END);
        }
      }
    }
  }
}

/* ============================================================================
 * EVENT CALLBACK HANDLERS (MOUSE CLICKS & INTERACTIONS)
 * ============================================================================
 */

/**
 * on_dock_item_left_click:
 * @btn: Pointer to the GtkButton widget that triggered the click.
 * @user_data: Generic pointer passed by GTK, pointing to our DockItem struct.
 *
 * Callback function triggered whenever the user left-clicks a dock icon.
 *
 * Behavior matrix:
 * 1. If 1 window is open  -> Focus/activate that window.
 * 2. If >1 window is open -> Open a popover menu listing all open windows.
 * 3. If 0 windows open    -> Execute the application launcher command
 * (`item->exec`).
 */
static void on_dock_item_left_click(GtkButton *btn, gpointer user_data) {
  // `(void)btn;` tells the compiler we intentionally aren't using the `btn`
  // parameter, preventing "unused variable" warnings during build.
  (void)btn;

  // Cast the generic `user_data` pointer back to our concrete `DockItem` struct
  // pointer.
  DockItem *item = (DockItem *)user_data;
  if (!item)
    return; // Defensive guard check: exit early if item is NULL

  // Case A: The application has active open windows
  if (item->windows != NULL) {
    // Check if there is exactly 1 window open for this app
    if (g_list_length(item->windows) == 1) {
      // Get the first (and only) window from the linked list
      AppWindow *win = (AppWindow *)item->windows->data;
      if (win && win->handle)
        // Request the Wayland compositor to bring this window to focus
        activate_toplevel_window(win->handle);
    } else {
      // More than 1 window open: populate and display context popover menu
      populate_popover_menu(item);

      // Keep track of active popover count in state so autohide knows not to
      // close the dock
      if (!gtk_widget_get_visible(item->popover))
        item->state->active_popovers++;

      // Reveal the GTK popover popup menu
      gtk_popover_popup(GTK_POPOVER(item->popover));
    }
  }
  // Case B: No open windows exist, but an executable launch string is provided
  else if (item->exec && *item->exec != '\0') {
    GError *error = NULL;

    // Asynchronously spawn a process to launch the app command without blocking
    // the main UI thread
    if (!g_spawn_command_line_async(item->exec, &error)) {
      // If spawning fails, log a system warning message
      g_warning("[Lambdock] Failed to launch '%s': %s", item->exec,
                error->message);
      g_clear_error(&error); // Clean up allocated GLib error structure
    }
  }
}

/**
 * on_dock_item_right_click:
 * @gesture: GTK gesture object that detected the click sequence.
 * @n_press: Number of press iterations (e.g., 1 for single click, 2 for
 * double).
 * @x: X coordinate of mouse click relative to the widget.
 * @y: Y coordinate of mouse click relative to the widget.
 * @user_data: Generic pointer passed by GTK, pointing to our DockItem struct.
 *
 * Callback function triggered when the user right-clicks a dock icon.
 * Always populates and opens the right-click context menu (popover).
 */
static void on_dock_item_right_click(GtkGestureClick *gesture, gint n_press,
                                     gdouble x, gdouble y, gpointer user_data) {
  // Mark unused parameter variables to avoid compiler warnings
  (void)gesture;
  (void)n_press;
  (void)x;
  (void)y;

  DockItem *item = (DockItem *)user_data;
  if (!item || !item->popover || !item->state)
    return; // Exit early if data structure references are invalid

  // Re-generate context menu options (e.g., Quit, Pin/Unpin, Window List)
  populate_popover_menu(item);

  // Increment active popover counter if it wasn't open yet
  if (!gtk_widget_get_visible(item->popover))
    item->state->active_popovers++;

  // Popup the GTK popover menu attached to this dock item button
  gtk_popover_popup(GTK_POPOVER(item->popover));
}

/* ============================================================================
 * DYNAMIC ITEM POLLING LOGIC & SCHEME EXCEPTION HANDLING
 * ============================================================================
 */

static SCM call_guile_proc_safe(void *data) {
  SCM fn = (SCM)data;
  return scm_call_0(fn);
}

static SCM handle_guile_proc_error(void *data, SCM key, SCM args) {
  (void)data;
  (void)key;
  (void)args;
  return SCM_BOOL_F;
}

static gboolean on_dynamic_item_poll(gpointer user_data) {
  DockItem *item = (DockItem *)user_data;
  if (!item || !scm_is_true(item->poll_fn) ||
      !scm_is_true(scm_procedure_p(item->poll_fn))) {
    return G_SOURCE_REMOVE;
  }

  // Safely execute Guile procedure catching any Scheme runtime exceptions
  SCM res =
      scm_internal_catch(SCM_BOOL_T, call_guile_proc_safe,
                         (void *)item->poll_fn, handle_guile_proc_error, NULL);

  if (scm_is_string(res)) {
    char *text = scm_to_utf8_string(res);
    if (text) {
      if (item->label && GTK_IS_LABEL(item->label)) {
        gtk_label_set_text(GTK_LABEL(item->label), text);
      }
      gtk_widget_set_tooltip_text(item->button, text);
      free(text);
    }
  }

  return G_SOURCE_CONTINUE;
}

/* ============================================================================
 * DOCK ITEM LIFECYCLE (CREATION & DESTRUCTION)
 * ============================================================================
 */

/**
 * create_dock_item:
 * @state: Pointer to the global Lambdock application state struct.
 * @name: Human-readable app name (e.g., "Firefox").
 * @icon: System icon theme name or image path (e.g., "firefox").
 * @exec: Terminal command line to launch the app (e.g., "firefox %u").
 * @app_id: Desktop file / Wayland application ID string.
 * @is_favorite: TRUE if pinned launcher; FALSE if dynamic running app
 * indicator.
 * @hover_animate: TRUE if hover fisheye magnification applies to this item.
 *
 * Allocates, initializes, and displays a new dock launcher item.
 *
 * Returns: Pointer to the newly created `DockItem` object.
 */
DockItem *create_dock_item(LambdockState *state, const char *name,
                           const char *icon, const char *exec,
                           const char *app_id, gboolean is_favorite,
                           gboolean hover_animate) {
  // `g_new0` allocates memory for 1 DockItem struct and fills memory with
  // zeros.
  DockItem *item = g_new0(DockItem, 1);

  // Store reference to global state and copy string arguments into heap memory
  item->state = state;
  item->name = g_strdup(name);
  item->icon = g_strdup(icon);
  item->exec = g_strdup(exec);
  // Fallback: Use icon name as app_id if explicit app_id wasn't provided
  item->app_id = g_strdup(app_id ? app_id : icon);
  item->is_favorite = is_favorite;
  item->hover_animate = hover_animate;

  // Retrieve default icon theme for current display and look up the desktop
  // icon image at configured size
  GdkDisplay *display = gdk_display_get_default();
  GtkIconTheme *icon_theme = gtk_icon_theme_get_for_display(display);
  int icon_sz =
      (state && state->icon_size > 0) ? state->icon_size : DEFAULT_ICON_SIZE;
  GtkWidget *image = create_image_for_icon(icon_theme, item->icon, icon_sz);

  // Allow image to overflow button boundaries when magnified
  gtk_widget_set_overflow(image, GTK_OVERFLOW_VISIBLE);

  // 1. Instantiate the button widget FIRST
  item->button = gtk_button_new();

  // Insert image widget inside button
  gtk_button_set_child(GTK_BUTTON(item->button), image);
  gtk_widget_add_css_class(item->button, "flat");

  // Enable overflow on button so magnified icons pop out without clipping
  gtk_widget_set_overflow(item->button, GTK_OVERFLOW_VISIBLE);

  // 2. Configure alignment on both the image and the valid button widget
  if (state) {
    apply_dock_item_alignment(item);
  }

  // Attach a mouse hover tooltip text showing the application name
  if (item->name && *item->name != '\0') {
    gtk_widget_set_tooltip_text(item->button, item->name);
  }

  // Connect left-click signal on the button to `on_dock_item_left_click`
  // callback
  g_signal_connect(item->button, "clicked", G_CALLBACK(on_dock_item_left_click),
                   item);

  // Initialize popover (popup menu) and attach it to the button widget
  item->popover = gtk_popover_new();
  gtk_popover_set_has_arrow(GTK_POPOVER(item->popover), FALSE);
  gtk_widget_set_parent(item->popover, item->button);

  // Listen for menu close events to decrement popover active state count
  g_signal_connect(item->popover, "closed", G_CALLBACK(on_popover_closed),
                   state);

  // Create a mouse click event controller specifically to intercept RIGHT-CLICK
  // (SECONDARY)
  GtkGesture *right_click = gtk_gesture_click_new();
  gtk_gesture_single_set_button(
      GTK_GESTURE_SINGLE(right_click),
      GDK_BUTTON_SECONDARY); // GDK_BUTTON_SECONDARY = Mouse Right Click
  g_signal_connect(right_click, "pressed", G_CALLBACK(on_dock_item_right_click),
                   item);
  // Attach gesture controller to the GTK button
  gtk_widget_add_controller(item->button, GTK_EVENT_CONTROLLER(right_click));

  // Append new button widget to the parent dock container layout box
  if (state && state->box)
    gtk_box_append(GTK_BOX(state->box), item->button);

  // Add dock item pointer to the global linked list of items
  if (state)
    state->dock_items = g_list_append(state->dock_items, item);

  return item;
}

/**
 * create_dynamic_dock_item:
 * Allocates and initializes a dynamic dock item widget with periodic text
 * polling.
 */
DockItem *create_dynamic_dock_item(LambdockState *state, const char *name,
                                   const char *icon, const char *exec,
                                   SCM poll_fn, guint interval_ms,
                                   gboolean hover_animate) {
  DockItem *item = g_new0(DockItem, 1);

  item->state = state;
  item->name = g_strdup(name);
  item->icon = g_strdup(icon);
  item->exec = g_strdup(exec);
  item->app_id = g_strdup(name);
  item->is_favorite = TRUE;
  item->poll_fn = poll_fn;
  item->hover_animate = hover_animate;

  // Protect Scheme procedure object from Guile Garbage Collector
  if (scm_is_true(poll_fn)) {
    scm_gc_protect_object(poll_fn);
  }

  item->button = gtk_button_new();

  GtkWidget *content_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);

  if (icon && *icon != '\0') {
    GdkDisplay *display = gdk_display_get_default();
    GtkIconTheme *icon_theme = gtk_icon_theme_get_for_display(display);
    int icon_sz =
        (state && state->icon_size > 0) ? state->icon_size : DEFAULT_ICON_SIZE;
    GtkWidget *image = create_image_for_icon(icon_theme, item->icon, icon_sz);
    gtk_widget_set_overflow(image, GTK_OVERFLOW_VISIBLE);
    gtk_box_append(GTK_BOX(content_box), image);
  }

  item->label = gtk_label_new(name);
  gtk_widget_add_css_class(item->label, "dynamic-item-label");
  gtk_box_append(GTK_BOX(content_box), item->label);

  gtk_button_set_child(GTK_BUTTON(item->button), content_box);
  gtk_widget_add_css_class(item->button, "flat");
  gtk_widget_add_css_class(item->button, "dynamic-item");
  gtk_widget_set_overflow(item->button, GTK_OVERFLOW_VISIBLE);

  if (state) {
    apply_dock_item_alignment(item);
  }

  if (item->name && *item->name != '\0') {
    gtk_widget_set_tooltip_text(item->button, item->name);
  }

  g_signal_connect(item->button, "clicked", G_CALLBACK(on_dock_item_left_click),
                   item);

  item->popover = gtk_popover_new();
  gtk_popover_set_has_arrow(GTK_POPOVER(item->popover), FALSE);
  gtk_widget_set_parent(item->popover, item->button);

  if (state) {
    g_signal_connect(item->popover, "closed", G_CALLBACK(on_popover_closed),
                     state);
  }

  GtkGesture *right_click = gtk_gesture_click_new();
  gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(right_click),
                                GDK_BUTTON_SECONDARY);
  g_signal_connect(right_click, "pressed", G_CALLBACK(on_dock_item_right_click),
                   item);
  gtk_widget_add_controller(item->button, GTK_EVENT_CONTROLLER(right_click));

  if (state && state->box)
    gtk_box_append(GTK_BOX(state->box), item->button);

  if (state)
    state->dock_items = g_list_append(state->dock_items, item);

  // Execute initial poll to populate label immediately upon creation
  on_dynamic_item_poll(item);

  // Register timer for dynamic interval polling
  if (interval_ms > 0 && scm_is_true(poll_fn)) {
    item->poll_timer_id =
        g_timeout_add(interval_ms, on_dynamic_item_poll, item);
  }

  return item;
}

/**
 * free_dock_item:
 * @item: Pointer to the DockItem to be destroyed and unallocated.
 *
 * Destroys GTK widgets and frees all dynamically allocated memory associated
 * with a dock item to prevent memory leaks.
 */
void free_dock_item(DockItem *item) {
  if (!item)
    return; // Exit if already NULL

  // Unregister active GLib timer callback
  if (item->poll_timer_id > 0) {
    g_source_remove(item->poll_timer_id);
    item->poll_timer_id = 0;
  }

  // Unprotect Scheme procedure object from GC
  if (scm_is_true(item->poll_fn)) {
    scm_gc_unprotect_object(item->poll_fn);
    item->poll_fn = SCM_BOOL_F;
  }

  // Detach popover menu widget from GTK hierarchy
  if (item->popover) {
    gtk_widget_unparent(item->popover);
    item->popover = NULL;
  }

  // Remove dock button widget from parent GTK box container
  if (item->button) {
    GtkWidget *parent = gtk_widget_get_parent(item->button);
    if (parent && GTK_IS_BOX(parent))
      gtk_box_remove(GTK_BOX(parent), item->button);
    item->button = NULL;
  }

  // Free linked list containing associated application window handles
  for (GList *l = item->windows; l != NULL; l = l->next) {
    AppWindow *win = (AppWindow *)l->data;
    g_free(win->title); // Free window title string
    g_free(win);        // Free AppWindow struct memory
  }
  g_list_free(item->windows); // Free GLib list nodes

  // Free heap-allocated strings
  g_free(item->name);
  g_free(item->icon);
  g_free(item->exec);
  g_free(item->app_id);

  // Free the outer DockItem struct memory itself
  g_free(item);
}

/**
 * clear_favorite_dock_items:
 * @state: Pointer to global application state.
 *
 * Clears and removes all user-pinned ("favorite") launchers from the dock
 * layout. Used when reloading configuration or resetting the UI.
 */
void clear_favorite_dock_items(LambdockState *state) {
  if (!state || !state->dock_items)
    return;

  // Unlink window trackers safely via the tracker API before destroying UI
  // items
  clear_window_tracker_dock_references(state);

  // Iterate over linked list and remove pinned favorite items
  GList *curr = state->dock_items;
  while (curr != NULL) {
    GList *next = curr->next; // Store next reference before potentially
                              // deleting current node
    DockItem *item = (DockItem *)curr->data;
    if (item && item->is_favorite) {
      // Remove element node from GLib linked list
      state->dock_items = g_list_delete_link(state->dock_items, curr);
      // Free memory & destroy UI widgets for this item
      free_dock_item(item);
    }
    curr = next;
  }
}

/* ============================================================================
 * WAYLAND WINDOW TRACKING INTEGRATION
 * ============================================================================
 */

/**
 * dock_item_add_window:
 * @item: Dock item to attach the window to.
 * @handle: Foreign Wayland window handle from compositor protocol events.
 *
 * Connects an open window instance to a dock item and updates its UI status.
 *
 * Returns: Pointer to created `AppWindow` wrapper struct.
 */
AppWindow *
dock_item_add_window(DockItem *item,
                     struct zwlr_foreign_toplevel_handle_v1 *handle) {
  if (!item || !handle)
    return NULL;

  // Allocate tracking wrapper for open window instance
  AppWindow *win = g_new0(AppWindow, 1);
  win->handle = handle;
  win->title = g_strdup(item->name ? item->name : item->app_id);

  // Append new window to item's list of open instances
  item->windows = g_list_append(item->windows, win);

  // Update indicator dots / CSS state on the dock button
  update_dock_item_status(item);
  return win;
}

/**
 * dock_item_remove_window:
 * @item: Dock item containing the target window.
 * @handle: Foreign Wayland window handle being closed.
 *
 * Removes a closed window handle association from a dock item.
 */
void dock_item_remove_window(DockItem *item,
                             struct zwlr_foreign_toplevel_handle_v1 *handle) {
  if (!item || !handle)
    return;

  // Search list for matching handle
  for (GList *l = item->windows; l != NULL; l = l->next) {
    AppWindow *win = (AppWindow *)l->data;
    if (win->handle == handle) {
      // Remove node from list and free allocation memory
      item->windows = g_list_remove(item->windows, win);
      g_free(win->title);
      g_free(win);
      break;
    }
  }

  // Refresh UI running indicators
  update_dock_item_status(item);
}

/**
 * update_dock_item_status:
 * @item: Dock item to inspect and refresh.
 *
 * Dynamically toggles CSS classes on the dock button widget based on whether
 * the app currently has active open windows.
 */
void update_dock_item_status(DockItem *item) {
  if (!item || !item->button)
    return;

  // If at least 1 window is open, add CSS class `.running-app` (renders active
  // dot/glow)
  if (item->windows != NULL) {
    if (!gtk_widget_has_css_class(item->button, "running-app"))
      gtk_widget_add_css_class(item->button, "running-app");
  } else {
    // If 0 open windows remain, remove CSS class `.running-app`
    if (gtk_widget_has_css_class(item->button, "running-app"))
      gtk_widget_remove_css_class(item->button, "running-app");
  }
}

/**
 * find_dock_item_by_app_id:
 * @state: Global Lambdock state.
 * @app_id: Application identifier or binary string to search for.
 *
 * Flexible lookup algorithm matching foreign Wayland window IDs to existing
 * dock items. Performs fallback substring fuzzy matching across App ID, icon
 * name, title, and binary commands.
 *
 * Returns: Matching `DockItem` pointer if found, or `NULL`.
 */
DockItem *find_dock_item_by_app_id(LambdockState *state, const char *app_id) {
  if (!state || !app_id || *app_id == '\0')
    return NULL;

  for (GList *l = state->dock_items; l != NULL; l = l->next) {
    DockItem *item = (DockItem *)l->data;

    // Check: Exact case-insensitive match against item->app_id
    if (item->app_id && g_ascii_strcasecmp(item->app_id, app_id) == 0)
      return item;

    // Check: Exact case-insensitive match against item->icon name
    if (item->icon && g_ascii_strcasecmp(item->icon, app_id) == 0)
      return item;

    // Check: Exact case-insensitive match against human-readable item->name
    if (item->name && g_ascii_strcasecmp(item->name, app_id) == 0)
      return item;

    // Check: Substring match against execution string command (e.g. "firefox
    // --new-window")
    if (item->exec && *item->exec != '\0') {
      char *lower_exec = g_ascii_strdown(item->exec, -1);
      char *lower_id = g_ascii_strdown(app_id, -1);

      // Look for bidirectional substring match
      gboolean match = (strstr(lower_exec, lower_id) != NULL) ||
                       (strstr(lower_id, lower_exec) != NULL);

      g_free(lower_exec); // Clean up temporary lowercase strings
      g_free(lower_id);

      if (match)
        return item;
    }
  }
  return NULL; // No match found
}

/* ============================================================================
 * LAYOUT SEPARATORS & RELOAD HELPERS
 * ============================================================================
 */

/**
 * create_separator_dock_item:
 * @state: Pointer to global Lambdock state.
 * @orientation: Orientation flag (GTK_ORIENTATION_VERTICAL or
 * GTK_ORIENTATION_HORIZONTAL).
 *
 * Creates a visual divider line widget inside the dock layout.
 *
 * Returns: Pointer to created separator `DockItem`.
 */
DockItem *create_separator_dock_item(LambdockState *state,
                                     GtkOrientation orientation) {
  DockItem *item = g_new0(DockItem, 1);
  item->state = state;
  item->is_favorite = TRUE;    // Mark as layout element so clear cycles
                               // preserve/handle as pinned structure
  item->hover_animate = FALSE; // Separators do not use hover animation

  // Instantiates standard GTK separator widget line
  item->button = gtk_separator_new(orientation);

  // Apply padding margins depending on vertical/horizontal orientation
  if (orientation == GTK_ORIENTATION_VERTICAL) {
    gtk_widget_set_margin_start(item->button, SEPARATOR_MARGIN_PX);
    gtk_widget_set_margin_end(item->button, SEPARATOR_MARGIN_PX);
  } else {
    gtk_widget_set_margin_top(item->button, SEPARATOR_MARGIN_PX);
    gtk_widget_set_margin_bottom(item->button, SEPARATOR_MARGIN_PX);
  }

  if (state) {
    apply_dock_item_alignment(item);
  }

  // Pack separator into UI container box and global dock items list
  if (state && state->box)
    gtk_box_append(GTK_BOX(state->box), item->button);
  if (state)
    state->dock_items = g_list_append(state->dock_items, item);

  return item;
}

/**
 * reload_dock_instance:
 * @state: Global Lambdock application state.
 *
 * Hot-reloads dock UI appearance, Scheme/Guile configuration rules, custom CSS
 * themes, autohide behavior, screen edge positioning, and rebuilds dock
 * launcher layout without restarting application.
 */
void reload_dock_instance(LambdockState *state) {
  if (!state)
    return;

  LOG_C_INFO("UI", "Hot-reloading UI layout and theme state...");

  // Reload active theme config and custom CSS
  g_free(state->theme);
  state->theme = get_theme_config();
  load_custom_css(state);

  // Cancel any pending auto-hide timer
  if (state->hide_timer_id != 0) {
    g_source_remove(state->hide_timer_id);
    state->hide_timer_id = 0;
  }

  // Hot-reload Position & Layer Shell Edge Anchoring
  GtkLayerShellEdge new_edge = get_position_config();
  if (new_edge != state->edge) {
    // Stop active tick animation callback if running
    if (state->tick_cb_id != 0) {
      gtk_widget_remove_tick_callback(state->window, state->tick_cb_id);
      state->tick_cb_id = 0;
    }

    // Reset margin on old edge and un-anchor it
    gtk_layer_set_margin(GTK_WINDOW(state->window), state->edge, 0);
    gtk_layer_set_anchor(GTK_WINDOW(state->window), state->edge, FALSE);

    // Set new edge anchor
    state->edge = new_edge;
    gtk_layer_set_anchor(GTK_WINDOW(state->window), state->edge, TRUE);

    // Recalculate orientation & update GTK Box direction
    state->orientation = (state->edge == GTK_LAYER_SHELL_EDGE_LEFT ||
                          state->edge == GTK_LAYER_SHELL_EDGE_RIGHT)
                             ? GTK_ORIENTATION_VERTICAL
                             : GTK_ORIENTATION_HORIZONTAL;
    gtk_orientable_set_orientation(GTK_ORIENTABLE(state->box),
                                   state->orientation);
    if (state->bg_capsule) {
      gtk_orientable_set_orientation(GTK_ORIENTABLE(state->bg_capsule),
                                     state->orientation);
    }

    // Reset margin and animation offsets
    state->current_margin = 0;
    state->target_margin = 0;
    state->start_margin = 0;
    state->is_hidden = FALSE;
  }

  // Hot-reload Auto-Hide, Hover Animation, Base Icon Size & Layer Shell
  // Exclusive Zone
  state->auto_hide = get_autohide_config();
  state->show_when_clear = get_show_when_clear_config();
  state->hover_animate = get_hover_animate_config();
  state->icon_size = get_icon_size_config();

  // Clear pinned items and repopulate dock launcher layout first
  clear_favorite_dock_items(state);
  populate_dock_items(state);

  // Re-sync Wayland foreign window handles back to newly populated launchers
  sync_window_trackers(state);

  // Update widget alignments for all dock items
  for (GList *l = state->dock_items; l != NULL; l = l->next) {
    DockItem *item = (DockItem *)l->data;
    if (!item || !item->button)
      continue;

    apply_dock_item_alignment(item);
  }

  update_dock_dimensions(state);

  if (state->auto_hide) {
    // Set exclusive zone to 0 so windows can occupy full screen space
    gtk_layer_set_exclusive_zone(GTK_WINDOW(state->window), 0);

    if (state->show_when_clear && state->window_trackers == NULL) {
      reveal_dock(state);
    } else {
      hide_dock(state);
    }
  } else {
    // Reserve desktop screen space dynamically when auto-hide is disabled
    gtk_layer_set_exclusive_zone(GTK_WINDOW(state->window),
                                 state->icon_size + CAPSULE_PADDING_PX);
    reveal_dock(state);
  }
}
