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

#include "dock_window.h"
#include "guile_bridge.h"
#include "logger.h"
#include "ui/dock_item.h"
#include "ui/icon_resolver.h"
#include "ui/popover_menu.h"
#include "ui/theme_engine.h"

/* =========================================================================
 * ANIMATION ENGINE & EASING FUNCTIONS
 * ========================================================================= */

/**
 * ease_out_cubic:
 * Calculates a Cubic Ease-Out curve for smooth deceleration.
 * * Formula: f(t) = 1 - (1 - t)^3
 *
 * @param t Progress normalized between 0.0 (start) and 1.0 (end).
 * @return Eased interpolation value between 0.0 and 1.0.
 */
static double ease_out_cubic(double t) {
  double p = 1.0 - t;
  return 1.0 - (p * p * p);
}

/**
 * animate_margin_cb:
 * GTK Tick callback executed on every display frame update while animating.
 * * Smoothly interpolates `current_margin` towards `target_margin` using
 * elapsed frame-clock timestamps.
 *
 * @param widget The GTK window widget being animated.
 * @param frame_clock The monitor frame clock driving updates.
 * @param user_data Pointer to the active LambdockState structure.
 * @return G_SOURCE_CONTINUE to keep ticking, or G_SOURCE_REMOVE when finished.
 */
static gboolean animate_margin_cb(GtkWidget *widget, GdkFrameClock *frame_clock,
                                  gpointer user_data) {
  (void)widget; // Unused parameter
  LambdockState *state = (LambdockState *)user_data;

  // Retrieve current frame timestamp in microseconds
  gint64 now = gdk_frame_clock_get_frame_time(frame_clock);

  // Initialize animation start time on first frame tick
  if (state->anim_start_time == 0) {
    state->anim_start_time = now;
  }

  // Calculate normalized animation progress ratio [0.0, 1.0]
  double progress =
      (double)(now - state->anim_start_time) / (double)ANIMATION_DURATION_US;

  // --- Animation Complete Handler ---
  if (progress >= 1.0) {
    state->current_margin = state->target_margin;
    gtk_layer_set_margin(GTK_WINDOW(state->window), state->edge,
                         state->current_margin);
    state->tick_cb_id = 0;  // Reset callback ID
    return G_SOURCE_REMOVE; // Stop frame tick callback loop
  }

  // --- Intermediate Frame Interpolation ---
  double eased = ease_out_cubic(progress);
  state->current_margin =
      state->start_margin +
      (int)((state->target_margin - state->start_margin) * eased);

  // Apply new calculated margin to Wayland layer shell surface
  gtk_layer_set_margin(GTK_WINDOW(state->window), state->edge,
                       state->current_margin);

  return G_SOURCE_CONTINUE; // Request next frame tick
}

/**
 * start_margin_animation:
 * Initiates or updates a margin transition animation toward a target value.
 *
 * @param state Pointer to LambdockState.
 * @param target_margin Target layer shell margin (in pixels).
 */
static void start_margin_animation(LambdockState *state, int target_margin) {
  // Ignore if already positioned at target
  if (state->current_margin == target_margin)
    return;

  state->start_margin = state->current_margin;
  state->target_margin = target_margin;
  state->anim_start_time = 0; // Reset timer anchor

  // Register frame tick callback if not currently active
  if (state->tick_cb_id == 0) {
    state->tick_cb_id = gtk_widget_add_tick_callback(
        state->window, animate_margin_cb, state, NULL);
  }
}

/* =========================================================================
 * FISHEYE HOVER MAGNIFICATION ENGINE
 * ========================================================================= */

/**
 * reset_dock_items_scale:
 * Resets all dock item icons back to their default base pixel size.
 */
static void reset_dock_items_scale(LambdockState *state) {
  if (!state || !state->dock_items)
    return;

  int base_sz = (state->icon_size > 0) ? state->icon_size : 48;

  for (GList *l = state->dock_items; l != NULL; l = l->next) {
    DockItem *item = (DockItem *)l->data;
    if (!item || !item->button || !GTK_IS_BUTTON(item->button))
      continue;

    GtkWidget *child = gtk_button_get_child(GTK_BUTTON(item->button));
    if (child && GTK_IS_IMAGE(child)) {
      if (gtk_image_get_pixel_size(GTK_IMAGE(child)) != base_sz) {
        gtk_image_set_pixel_size(GTK_IMAGE(child), base_sz);
        gtk_widget_set_size_request(child, base_sz, base_sz);
      }
    }
  }
}

/**
 * update_dock_items_fisheye:
 * Computes fisheye magnification for dock icons based on current
 * cursor coordinates relative to dock item centers.
 */
static void update_dock_items_fisheye(LambdockState *state, double x,
                                      double y) {
  // Do not run hover magnification if disabled, hidden, or animating slide
  if (!state || !state->dock_items || !state->hover_animate ||
      state->is_hidden || state->tick_cb_id != 0 || state->current_margin != 0)
    return;

  int base_sz = (state->icon_size > 0) ? state->icon_size : 48;
  double base_size = (double)base_sz;
  const double max_scale = 1.45; // 45% max size magnification
  const double radius = 140.0;   // Influence radius in pixels

  for (GList *l = state->dock_items; l != NULL; l = l->next) {
    DockItem *item = (DockItem *)l->data;
    if (!item || !item->button || !GTK_IS_BUTTON(item->button))
      continue;

    graphene_rect_t bounds;
    if (gtk_widget_compute_bounds(item->button, state->window, &bounds)) {
      double center_x = bounds.origin.x + bounds.size.width / 2.0;
      double center_y = bounds.origin.y + bounds.size.height / 2.0;

      double dist = (state->orientation == GTK_ORIENTATION_HORIZONTAL)
                        ? fabs(x - center_x)
                        : fabs(y - center_y);

      double scale = 1.0;
      if (dist < radius) {
        // Cosine bell curve scaling from 1.0 (at radius edge) to max_scale (at
        // center)
        double factor = 0.5 * (1.0 + cos(M_PI * dist / radius));
        scale = 1.0 + (max_scale - 1.0) * factor;
      }

      int target_size = (int)(base_size * scale);
      GtkWidget *child = gtk_button_get_child(GTK_BUTTON(item->button));
      if (child && GTK_IS_IMAGE(child)) {
        if (gtk_image_get_pixel_size(GTK_IMAGE(child)) != target_size) {
          gtk_image_set_pixel_size(GTK_IMAGE(child), target_size);

          if (state->orientation == GTK_ORIENTATION_HORIZONTAL) {
            // Expand width along X axis; Y axis remains unconstrained
            gtk_widget_set_size_request(item->button, target_size, -1);
          } else {
            // Expand height along Y axis; X axis remains unconstrained
            gtk_widget_set_size_request(item->button, -1, target_size);
          }
        }
      }
    }
  }
}

/* =========================================================================
 * AUTO-HIDE & REVEAL LOGIC
 * ========================================================================= */

/**
 * reveal_dock:
 * Cancels pending hide timers and animates the dock back into full view (margin
 * = 0).
 *
 * @param state Pointer to LambdockState.
 */
void reveal_dock(LambdockState *state) {
  // Cancel any scheduled auto-hide timeout
  if (state->hide_timer_id > 0) {
    g_source_remove(state->hide_timer_id);
    state->hide_timer_id = 0;
  }

  // Trigger reveal animation if hidden or moving away from visible state
  if (state->is_hidden || state->target_margin != 0) {
    start_margin_animation(state, 0);
    state->is_hidden = FALSE;
  }
}

/**
 * on_hide_timeout:
 * Timer callback triggered after mouse-leave grace period expires.
 *
 * @param user_data Pointer to LambdockState.
 * @return G_SOURCE_REMOVE to unregister timer.
 */
static gboolean on_hide_timeout(gpointer user_data) {
  LambdockState *state = (LambdockState *)user_data;
  hide_dock(state);
  state->hide_timer_id = 0;
  return G_SOURCE_REMOVE;
}

/**
 * hide_dock:
 * Calculates window dimensions and slides the dock off-screen into hidden
 * margin. Keeps a 2-pixel strip visible on screen edge to receive mouse hover
 * events.
 *
 * @param state Pointer to LambdockState.
 */
void hide_dock(LambdockState *state) {
  // Do not hide if autohide is disabled or user has open popover menus
  if (!state->auto_hide || state->active_popovers > 0)
    return;

  // Do not hide if show-when-clear is active and no open windows exist
  if (state->show_when_clear && state->window_trackers == NULL)
    return;

  // Reset icon scaling to base size before calculating window dimensions
  if (state->hover_animate) {
    reset_dock_items_scale(state);
  }

  GtkOrientation dim_orient = (state->edge == GTK_LAYER_SHELL_EDGE_TOP ||
                               state->edge == GTK_LAYER_SHELL_EDGE_BOTTOM)
                                  ? GTK_ORIENTATION_VERTICAL
                                  : GTK_ORIENTATION_HORIZONTAL;

  int min_sz = 0, nat_sz = 0;
  if (state->box) {
    gtk_widget_measure(state->box, dim_orient, -1, &min_sz, &nat_sz, NULL,
                       NULL);
  }

  int window_dim = nat_sz;
  if (window_dim <= 0 && state->window) {
    window_dim = (dim_orient == GTK_ORIENTATION_VERTICAL)
                     ? gtk_widget_get_height(state->window)
                     : gtk_widget_get_width(state->window);
  }

  // Leave 2px strip exposed on screen edge to trigger hover re-entry
  int hide_margin = -(window_dim - 2);

  if (!state->is_hidden || state->target_margin != hide_margin) {
    start_margin_animation(state, hide_margin);
    state->is_hidden = TRUE;
  }
}

/* =========================================================================
 * EVENT CONTROLLERS & CALLBACKS
 * ========================================================================= */

/**
 * on_popover_closed:
 * Decrements active popover counter when a context menu is dismissed.
 * Re-schedules auto-hide if no open popovers remain.
 */
void on_popover_closed(GtkPopover *popover, gpointer user_data) {
  (void)popover;
  LambdockState *state = (LambdockState *)user_data;

  if (state->active_popovers > 0) {
    state->active_popovers--;
  }

  // If no menus remain open, schedule autohide after 400ms delay
  if (state->auto_hide && state->active_popovers == 0) {
    if (state->hide_timer_id != 0) {
      g_source_remove(state->hide_timer_id);
    }
    state->hide_timer_id = g_timeout_add(400, on_hide_timeout, state);
  }
}

/**
 * on_motion_enter:
 * Mouse motion controller event when pointer enters the dock region.
 */
static void on_motion_enter(GtkEventControllerMotion *controller, double x,
                            double y, gpointer user_data) {
  (void)controller;
  (void)x;
  (void)y;
  LambdockState *state = (LambdockState *)user_data;
  reveal_dock(state);
}

/**
 * on_motion_leave:
 * Mouse motion controller event when pointer leaves the dock surface.
 * Resets icon scaling and schedules an auto-hide check after interval.
 */
static void on_motion_leave(GtkEventControllerMotion *controller,
                            gpointer user_data) {
  (void)controller;
  LambdockState *state = (LambdockState *)user_data;

  if (state) {
    if (state->hover_animate) {
      reset_dock_items_scale(state);
    }
    if (state->auto_hide) {
      if (state->hide_timer_id != 0) {
        g_source_remove(state->hide_timer_id);
      }
      state->hide_timer_id = g_timeout_add(600, on_hide_timeout, state);
    }
  }
}

/**
 * on_motion_notify:
 * Mouse motion callback tracking cursor position across dock items for fisheye.
 */
static void on_motion_notify(GtkEventControllerMotion *controller, double x,
                             double y, gpointer user_data) {
  (void)controller;
  LambdockState *state = (LambdockState *)user_data;
  if (state && state->hover_animate && !state->is_hidden &&
      state->tick_cb_id == 0 && state->current_margin == 0) {
    update_dock_items_fisheye(state, x, y);
  }
}

/**
 * on_right_click:
 * Gesture controller callback for right-click context menu activation.
 */
void on_right_click(GtkGestureClick *gesture, gint n_press, gdouble x,
                    gdouble y, gpointer user_data) {
  (void)gesture;
  (void)n_press;
  LambdockState *state = (LambdockState *)user_data;

  if (!state->popover)
    return;

  // Set popover arrow positioning anchor at right-click coordinates
  GdkRectangle rect = {(int)x, (int)y, 1, 1};
  gtk_popover_set_pointing_to(GTK_POPOVER(state->popover), &rect);

  if (!gtk_widget_get_visible(state->popover)) {
    state->active_popovers++;
  }

  gtk_popover_popup(GTK_POPOVER(state->popover));
}

/* =========================================================================
 * LAYER SHELL & STATE INITIALIZATION
 * ========================================================================= */

/**
 * setup_layer_shell_window:
 * Converts standard GTK window to a Wayland Layer Shell surface and sets
 * anchors.
 *
 * @param state Pointer to LambdockState.
 */
void setup_layer_shell_window(LambdockState *state) {
  GtkWindow *window = GTK_WINDOW(state->window);

  // Initialize GTK Layer Shell integration for window
  gtk_layer_init_for_window(window);

  // Assign window to specific GdkMonitor output if specified
  if (state->monitor != NULL) {
    gtk_layer_set_monitor(window, state->monitor);
  }

  // Set desktop render layer (Top layer renders above normal application
  // windows)
  gtk_layer_set_layer(window, GTK_LAYER_SHELL_LAYER_TOP);

  // Anchor window to screen edge (Top, Bottom, Left, or Right)
  gtk_layer_set_anchor(window, state->edge, TRUE);

  // Exclusive zone reserves desktop space so windows do not overlap dock.
  // Set to 0 during auto-hide to allow windows to occupy full screen.
  if (state->auto_hide) {
    gtk_layer_set_exclusive_zone(window, 0);
  } else {
    gtk_layer_auto_exclusive_zone_enable(window);
  }
}

/**
 * setup_dock_motion_controller:
 * Attaches GtkEventControllerMotion to monitor cursor entry, leave, and motion.
 */
void setup_dock_motion_controller(LambdockState *state) {
  GtkEventController *motion = gtk_event_controller_motion_new();

  g_signal_connect(motion, "enter", G_CALLBACK(on_motion_enter), state);
  g_signal_connect(motion, "leave", G_CALLBACK(on_motion_leave), state);
  g_signal_connect(motion, "motion", G_CALLBACK(on_motion_notify), state);

  gtk_widget_add_controller(state->window, motion);
}

/**
 * schedule_initial_autohide:
 * Schedules initial hide timer on application launch if auto-hide is enabled.
 */
void schedule_initial_autohide(LambdockState *state) {
  if (state->auto_hide) {
    state->hide_timer_id = g_timeout_add(1000, on_hide_timeout, state);
  }
}

/* =========================================================================
 * MEMORY & DESTRUCTORS
 * ========================================================================= */

/**
 * free_lambdock_state:
 * Destructor for LambdockState structure. Performs deep memory freeing
 * for window tracker lists, dock item lists, and theme strings.
 *
 * @param state Pointer to LambdockState instance.
 */
void free_lambdock_state(LambdockState *state) {
  if (!state)
    return;

  // --- Free Wayland foreign-toplevel window tracker objects ---
  for (GList *l = state->window_trackers; l != NULL; l = l->next) {
    void *tracker_ptr = l->data;
    if (tracker_ptr) {
      // Offset pointers to string struct members (title and app_id)
      char **title_ptr = (char **)((char *)tracker_ptr + sizeof(void *) * 4);
      char **app_id_ptr = (char **)((char *)tracker_ptr + sizeof(void *) * 5);

      g_free(*title_ptr);
      g_free(*app_id_ptr);
      g_free(tracker_ptr);
    }
  }
  g_list_free(state->window_trackers);
  state->window_trackers = NULL;

  // --- Free dock launcher items ---
  for (GList *l = state->dock_items; l != NULL; l = l->next) {
    DockItem *item = (DockItem *)l->data;
    free_dock_item(item);
  }
  g_list_free(state->dock_items);
  state->dock_items = NULL;

  // --- Free state strings & main struct pointer ---
  g_free(state->theme);
  g_free(state);
}

/* =========================================================================
 * GTK APPLICATION ACTIONS (ABOUT & QUIT)
 * ========================================================================= */

/**
 * on_quit_action:
 * Action handler for menu 'Quit' option. Terminates GTK application loop.
 */
void on_quit_action(GSimpleAction *action, GVariant *parameter,
                    gpointer user_data) {
  (void)action;
  (void)parameter;
  GtkApplication *app = GTK_APPLICATION(user_data);
  g_application_quit(G_APPLICATION(app));
}

/**
 * on_about_action:
 * Action handler for menu 'About Lambdock' option. Constructs and displays
 * modal `GtkAboutDialog` with metadata, license, and logo assets.
 */
void on_about_action(GSimpleAction *action, GVariant *parameter,
                     gpointer user_data) {
  (void)action;
  (void)parameter;
  (void)user_data;

  GtkWidget *about = gtk_about_dialog_new();
  gtk_widget_add_css_class(about, "about-dialog");

  // Basic Details
  gtk_about_dialog_set_program_name(GTK_ABOUT_DIALOG(about), "lambdock");

#ifdef PACKAGE_VERSION
  gtk_about_dialog_set_version(GTK_ABOUT_DIALOG(about), PACKAGE_VERSION);
#endif

  gtk_about_dialog_set_comments(
      GTK_ABOUT_DIALOG(about),
      "A modern Wayland-native hackable desktop dock bar.\n\n"
      "Engineered with the performance of C and GTK4 Layer Shell for smooth "
      "auto-hide and animations, featuring dynamic Wayland foreign-toplevel "
      "window "
      "tracking, live CSS hot-reloading, and dynamic configuration powered by "
      "GNU "
      "Guile Scheme.");

  // Authors & Copyright
  const char *authors[] = {"Josep Bigorra (jjbigorra@gmail.com)", NULL};
  gtk_about_dialog_set_authors(GTK_ABOUT_DIALOG(about), authors);

  gtk_about_dialog_set_copyright(
      GTK_ABOUT_DIALOG(about),
      "Copyright © Josep Bigorra <jjbigorra@gmail.com>");

  // License (GPL-3.0)
  gtk_about_dialog_set_license_type(GTK_ABOUT_DIALOG(about),
                                    GTK_LICENSE_GPL_3_0);

  // Repository Web Links
  gtk_about_dialog_set_website(GTK_ABOUT_DIALOG(about),
                               "https://codeberg.org/jjba23/lambdock");
  gtk_about_dialog_set_website_label(GTK_ABOUT_DIALOG(about),
                                     "Find the project's source code here");

  // Logo Loading Logic from compiled GResource bundle
  const char *logo_resource =
      "/org/jointhefreeworld/lambdock/img/agnus-dei.png";
  GdkTexture *texture = gdk_texture_new_from_resource(logo_resource);

  if (texture) {
    gtk_about_dialog_set_logo(GTK_ABOUT_DIALOG(about), GDK_PAINTABLE(texture));
    g_object_unref(texture); // Drop local texture reference
  } else {
    LOG_C_WARN("About", "Failed to load logo from GResource: %s",
               logo_resource);
    gtk_about_dialog_set_logo_icon_name(GTK_ABOUT_DIALOG(about), "lambdock");
  }

  // Display dialog modally
  gtk_window_set_modal(GTK_WINDOW(about), TRUE);
  gtk_window_present(GTK_WINDOW(about));
}
