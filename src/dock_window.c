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
#include "ui_helpers.h"

static double ease_out_cubic(double t) {
  double p = 1.0 - t;
  return 1.0 - (p * p * p);
}

static gboolean animate_margin_cb(GtkWidget *widget, GdkFrameClock *frame_clock,
                                  gpointer user_data) {
  (void)widget;
  LambdockState *state = (LambdockState *)user_data;

  gint64 now = gdk_frame_clock_get_frame_time(frame_clock);

  if (state->anim_start_time == 0) {
    state->anim_start_time = now;
  }

  double progress =
      (double)(now - state->anim_start_time) / (double)ANIMATION_DURATION_US;

  if (progress >= 1.0) {
    state->current_margin = state->target_margin;
    gtk_layer_set_margin(GTK_WINDOW(state->window), state->edge,
                         state->current_margin);
    state->tick_cb_id = 0;
    return G_SOURCE_REMOVE;
  }

  double eased = ease_out_cubic(progress);
  state->current_margin =
      state->start_margin +
      (int)((state->target_margin - state->start_margin) * eased);

  gtk_layer_set_margin(GTK_WINDOW(state->window), state->edge,
                       state->current_margin);

  return G_SOURCE_CONTINUE;
}

static void start_margin_animation(LambdockState *state, int target_margin) {
  if (state->current_margin == target_margin)
    return;

  state->start_margin = state->current_margin;
  state->target_margin = target_margin;
  state->anim_start_time = 0;

  if (state->tick_cb_id == 0) {
    state->tick_cb_id = gtk_widget_add_tick_callback(
        state->window, animate_margin_cb, state, NULL);
  }
}

void reveal_dock(LambdockState *state) {
  if (state->hide_timer_id > 0) {
    g_source_remove(state->hide_timer_id);
    state->hide_timer_id = 0;
  }

  if (state->is_hidden || state->target_margin != 0) {
    start_margin_animation(state, 0);
    state->is_hidden = FALSE;
  }
}

static gboolean on_hide_timeout(gpointer user_data) {
  LambdockState *state = (LambdockState *)user_data;
  hide_dock(state);
  state->hide_timer_id = 0;
  return G_SOURCE_REMOVE;
}

void hide_dock(LambdockState *state) {
  if (!state->auto_hide || state->active_popovers > 0)
    return;

  int window_dim = (state->edge == GTK_LAYER_SHELL_EDGE_TOP ||
                    state->edge == GTK_LAYER_SHELL_EDGE_BOTTOM)
                       ? gtk_widget_get_height(state->window)
                       : gtk_widget_get_width(state->window);

  int hide_margin = -(window_dim - 2);

  if (!state->is_hidden || state->target_margin != hide_margin) {
    start_margin_animation(state, hide_margin);
    state->is_hidden = TRUE;
  }
}

void on_popover_closed(GtkPopover *popover, gpointer user_data) {
  (void)popover;
  LambdockState *state = (LambdockState *)user_data;

  if (state->active_popovers > 0) {
    state->active_popovers--;
  }

  if (state->auto_hide && state->active_popovers == 0) {
    if (state->hide_timer_id != 0) {
      g_source_remove(state->hide_timer_id);
    }
    state->hide_timer_id = g_timeout_add(400, on_hide_timeout, state);
  }
}

static void on_motion_enter(GtkEventControllerMotion *controller, double x,
                            double y, gpointer user_data) {
  (void)controller;
  (void)x;
  (void)y;
  LambdockState *state = (LambdockState *)user_data;
  reveal_dock(state);
}

static void on_motion_leave(GtkEventControllerMotion *controller,
                            gpointer user_data) {
  (void)controller;
  LambdockState *state = (LambdockState *)user_data;

  if (state->auto_hide) {
    if (state->hide_timer_id != 0) {
      g_source_remove(state->hide_timer_id);
    }
    state->hide_timer_id = g_timeout_add(720, on_hide_timeout, state);
  }
}

void on_right_click(GtkGestureClick *gesture, gint n_press, gdouble x,
                    gdouble y, gpointer user_data) {
  (void)gesture;
  (void)n_press;
  LambdockState *state = (LambdockState *)user_data;

  if (!state->popover)
    return;

  GdkRectangle rect = {(int)x, (int)y, 1, 1};
  gtk_popover_set_pointing_to(GTK_POPOVER(state->popover), &rect);

  if (!gtk_widget_get_visible(state->popover)) {
    state->active_popovers++;
  }

  gtk_popover_popup(GTK_POPOVER(state->popover));
}

void setup_layer_shell_window(LambdockState *state) {
  GtkWindow *window = GTK_WINDOW(state->window);

  gtk_layer_init_for_window(window);

  if (state->monitor != NULL) {
    gtk_layer_set_monitor(window, state->monitor);
  }

  gtk_layer_set_layer(window, GTK_LAYER_SHELL_LAYER_TOP);
  gtk_layer_set_anchor(window, state->edge, TRUE);

  if (state->auto_hide) {
    gtk_layer_set_exclusive_zone(window, 0);
  } else {
    gtk_layer_auto_exclusive_zone_enable(window);
  }
}

void setup_dock_motion_controller(LambdockState *state) {
  GtkEventController *motion = gtk_event_controller_motion_new();

  g_signal_connect(motion, "enter", G_CALLBACK(on_motion_enter), state);
  g_signal_connect(motion, "leave", G_CALLBACK(on_motion_leave), state);

  gtk_widget_add_controller(state->window, motion);
}

void schedule_initial_autohide(LambdockState *state) {
  if (state->auto_hide) {
    state->hide_timer_id = g_timeout_add(1000, on_hide_timeout, state);
  }
}

void free_lambdock_state(LambdockState *state) {
  if (!state)
    return;

  // Free active window tracker allocations
  for (GList *l = state->window_trackers; l != NULL; l = l->next) {
    void *tracker_ptr = l->data;
    if (tracker_ptr) {
      char **title_ptr = (char **)((char *)tracker_ptr + sizeof(void *) * 4);
      char **app_id_ptr = (char **)((char *)tracker_ptr + sizeof(void *) * 5);
      g_free(*title_ptr);
      g_free(*app_id_ptr);
      g_free(tracker_ptr);
    }
  }
  g_list_free(state->window_trackers);
  state->window_trackers = NULL;

  for (GList *l = state->dock_items; l != NULL; l = l->next) {
    DockItem *item = (DockItem *)l->data;
    free_dock_item(item);
  }
  g_list_free(state->dock_items);
  state->dock_items = NULL;

  g_free(state->theme);
  g_free(state);
}

void on_quit_action(GSimpleAction *action, GVariant *parameter,
                    gpointer user_data) {
  (void)action;
  (void)parameter;
  GtkApplication *app = GTK_APPLICATION(user_data);
  g_application_quit(G_APPLICATION(app));
}

void on_about_action(GSimpleAction *action, GVariant *parameter,
                     gpointer user_data) {
  (void)action;
  (void)parameter;
  (void)user_data;

  GtkWidget *about = gtk_about_dialog_new();

  gtk_widget_add_css_class(about, "about-dialog");

  gtk_about_dialog_set_program_name(GTK_ABOUT_DIALOG(about), "Lambdock");
  gtk_about_dialog_set_comments(
      GTK_ABOUT_DIALOG(about),
      "A lightweight, customizable Wayland dock driven by Guile Scheme.");
  gtk_about_dialog_set_copyright(GTK_ABOUT_DIALOG(about),
                                 "Copyright \xc2\xa9 Josep Bigorra");
  gtk_about_dialog_set_license_type(GTK_ABOUT_DIALOG(about),
                                    GTK_LICENSE_GPL_3_0);

  gtk_about_dialog_set_website(GTK_ABOUT_DIALOG(about),
                               "https://codeberg.org/jjba23g/lambdock");
  gtk_about_dialog_set_website_label(GTK_ABOUT_DIALOG(about),
                                     "Codeberg Repository");

  const char *logo_path = "assets/lambdock.png";
  if (g_file_test(logo_path, G_FILE_TEST_EXISTS)) {
    GFile *logo_file = g_file_new_for_path(logo_path);
    GdkTexture *texture = gdk_texture_new_from_file(logo_file, NULL);
    if (texture) {
      gtk_about_dialog_set_logo(GTK_ABOUT_DIALOG(about),
                                GDK_PAINTABLE(texture));
      g_object_unref(texture);
    }
    g_object_unref(logo_file);
  } else {
    gtk_about_dialog_set_logo_icon_name(GTK_ABOUT_DIALOG(about), "lambdock");
  }

  gtk_window_set_modal(GTK_WINDOW(about), TRUE);
  gtk_window_present(GTK_WINDOW(about));
}
