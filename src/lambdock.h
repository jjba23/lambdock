#ifndef LAMBDOCK_H
#define LAMBDOCK_H

#include <gtk/gtk.h>
#include <gtk4-layer-shell.h>
#include <libguile.h>
#include <math.h>

#define ANIMATION_DURATION_US 250000 // 250ms

typedef struct {
  GtkWidget *window;
  gboolean auto_hide;
  guint hide_timer_id;
  gboolean is_hidden;

  /* Animation State */
  guint tick_cb_id;
  gint64 anim_start_time;
  int current_margin;
  int start_margin;
  int target_margin;
} LambdockState;

#endif // LAMBDOCK_H
