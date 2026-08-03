// lambdock

// Copyright © Josep Bigorra <jjbigorra@gmail.com>

// lambdock is free software : you can redistribute it and / or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.

// lambdock is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.See the
// GNU General Public License for more details.

// You should have received a copy of the GNU General Public License
// along with lambdock.If not, see <https://www.gnu.org/licenses/>.

#ifndef LAMBDOCK_H
#define LAMBDOCK_H

#include <gtk/gtk.h>
#include <gtk4-layer-shell.h>
#include <libguile.h>
#include <math.h>

#define ANIMATION_DURATION_US 250000 // 250ms

struct zwlr_foreign_toplevel_handle_v1;

typedef struct LambdockState LambdockState;

/**
 * DockItem:
 * Unified structure representing a dock launcher and running application
 * instance.
 */
typedef struct DockItem {
  char *name;
  char *icon;
  char *exec;
  char *app_id;
  gboolean is_favorite;
  GtkWidget *button;
  GtkWidget *popover;
  struct zwlr_foreign_toplevel_handle_v1 *handle;
  LambdockState *state;
} DockItem;

struct LambdockState {
  GtkWidget *window;
  GtkWidget *box; /* Main GTK box layout container */
  GtkWidget *popover;
  gboolean auto_hide;
  guint hide_timer_id;
  gboolean is_hidden;

  /* Position & Orientation */
  GtkLayerShellEdge edge;
  GtkOrientation orientation;

  /* Animation State */
  guint tick_cb_id;
  gint64 anim_start_time;
  int current_margin;
  int start_margin;
  int target_margin;
  int active_popovers;

  /* Unified Dock Items */
  GList *dock_items; /* List of DockItem pointers */
};

#endif // LAMBDOCK_H
