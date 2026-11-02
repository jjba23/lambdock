// lambdock -- hyper-hackable desktop dock for Wayland
//
// SPDX-License-Identifier: MIT
// Copyright © Josep Bigorra <jjbigorra@gmail.com>

#include "poof.h"

#include "../core/poof.h"
#include "../dock_window.h"

/* ============================================================================
 * LambdockPoof — the burst of smoke an icon leaves behind
 * ============================================================================
 *
 * A widget the size of the icon it replaces, animated by the frame clock: each
 * frame resolves the puffs in core/poof.c for the current progress and paints
 * them as radial gradients. When the animation is over the widget takes itself
 * out of the dock's box, and the dock measures itself again so the remaining
 * icons close the gap.
 */

typedef struct _LambdockPoof {
  GtkWidget parent_instance;

  LambdockState *state; /* Dock to re-measure when the burst is over */
  LambdockPoofParticle particles[LAMBDOCK_POOF_PARTICLES];
  guint tick_id;
  guint64 start_us;
  gboolean started;
  double progress;
} LambdockPoof;

typedef struct _LambdockPoofClass {
  GtkWidgetClass parent_class;
} LambdockPoofClass;

GType lambdock_poof_get_type(void);
G_DEFINE_TYPE(LambdockPoof, lambdock_poof, GTK_TYPE_WIDGET)

#define LAMBDOCK_TYPE_POOF (lambdock_poof_get_type())
#define LAMBDOCK_POOF(obj)                                                    \
  (G_TYPE_CHECK_INSTANCE_CAST((obj), LAMBDOCK_TYPE_POOF, LambdockPoof))

/**
 * poof_paint_puff:
 * Draws one puff: light in the middle, greyer at its edge, fading to nothing.
 *
 * The two-tone fill is what lets a single puff read on a pale platinum dock and
 * on a smoked one; a flat grey circle would disappear into one or the other.
 */
static void poof_paint_puff(GtkSnapshot *snapshot, double cx, double cy,
                            double radius, double alpha) {
  if (radius <= 0.0 || alpha <= 0.0)
    return;

  graphene_rect_t bounds =
      GRAPHENE_RECT_INIT((float)(cx - radius), (float)(cy - radius),
                         (float)(radius * 2.0), (float)(radius * 2.0));
  graphene_point_t center;
  graphene_point_init(&center, (float)cx, (float)cy);

  GskColorStop stops[3] = {
      {(float)0.0, {0.97f, 0.97f, 0.99f, (float)alpha}},
      {(float)0.50, {0.80f, 0.81f, 0.85f, (float)(0.72 * alpha)}},
      {(float)1.0, {0.32f, 0.33f, 0.36f, 0.0f}},
  };

  gtk_snapshot_append_radial_gradient(snapshot, &bounds, &center, (float)radius,
                                      (float)radius, 0.0f, 1.0f, stops, 3);
}

static void lambdock_poof_snapshot(GtkWidget *widget, GtkSnapshot *snapshot) {
  LambdockPoof *self = LAMBDOCK_POOF(widget);

  double w = (double)gtk_widget_get_width(widget);
  double h = (double)gtk_widget_get_height(widget);
  double size = MIN(w, h);
  if (size <= 0.0 || self->progress <= 0.0)
    return;

  double t = MIN(self->progress, 1.0);
  double fade = lambdock_poof_progress_alpha(t);
  if (fade <= 0.0)
    return;

  /* The puffs are laid out in the icon's own square, centred in whatever the
   * widget was allocated: the smoke must never stretch into a band just because
   * the dock's box is taller than the icon. */
  double ox = (w - size) / 2.0;
  double oy = (h - size) / 2.0;

  /* The dense middle first, then the puffs that leave it. */
  LambdockPoofPuff core;
  lambdock_poof_core_at(t, &core);
  poof_paint_puff(snapshot, ox + size / 2.0, oy + size / 2.0,
                  core.radius * size, core.alpha * fade);

  LambdockPoofPuff puffs[LAMBDOCK_POOF_PARTICLES];
  lambdock_poof_particles_at(self->particles, LAMBDOCK_POOF_PARTICLES, t, puffs);

  for (gsize i = 0; i < LAMBDOCK_POOF_PARTICLES; i++) {
    if (puffs[i].alpha <= 0.004)
      continue;
    poof_paint_puff(snapshot, ox + puffs[i].x * size, oy + puffs[i].y * size,
                    puffs[i].radius * size, puffs[i].alpha * fade);
  }
}

/* Takes the finished widget out of the dock on an idle: a widget must not
 * disappear from inside its own tick callback while GTK is still iterating. */
static gboolean poof_remove_idle(gpointer user_data) {
  GtkWidget *widget = GTK_WIDGET(user_data);
  LambdockPoof *self = LAMBDOCK_POOF(widget);
  LambdockState *state = self->state;
  self->state = NULL;

  GtkWidget *parent = gtk_widget_get_parent(widget);
  if (parent && GTK_IS_BOX(parent))
    gtk_box_remove(GTK_BOX(parent), widget);

  /* The gap closes now, not while the smoke was still there. */
  if (state && state->window && GTK_IS_WINDOW(state->window))
    update_dock_dimensions(state);

  return G_SOURCE_REMOVE;
}

static gboolean on_poof_tick(GtkWidget *widget, GdkFrameClock *clock,
                             gpointer user_data) {
  (void)user_data;
  LambdockPoof *self = LAMBDOCK_POOF(widget);

  gint64 now = gdk_frame_clock_get_frame_time(clock);
  if (!self->started) {
    self->started = TRUE;
    self->start_us = (guint64)now;
  }

  double elapsed_ms = (double)(now - (gint64)self->start_us) / 1000.0;
  self->progress = elapsed_ms / LAMBDOCK_POOF_DURATION_MS;

  if (self->progress >= 1.0) {
    self->tick_id = 0;
    g_idle_add_full(G_PRIORITY_DEFAULT_IDLE, poof_remove_idle,
                    g_object_ref(widget), g_object_unref);
    return G_SOURCE_REMOVE;
  }

  gtk_widget_queue_draw(widget);
  return G_SOURCE_CONTINUE;
}

static void lambdock_poof_dispose(GObject *object) {
  LambdockPoof *self = LAMBDOCK_POOF(object);

  /* The dock may be going away underneath the burst. */
  self->state = NULL;

  if (self->tick_id != 0) {
    gtk_widget_remove_tick_callback(GTK_WIDGET(self), self->tick_id);
    self->tick_id = 0;
  }

  G_OBJECT_CLASS(lambdock_poof_parent_class)->dispose(object);
}

static void lambdock_poof_class_init(LambdockPoofClass *klass) {
  GObjectClass *object_class = G_OBJECT_CLASS(klass);
  GtkWidgetClass *widget_class = GTK_WIDGET_CLASS(klass);

  object_class->dispose = lambdock_poof_dispose;
  widget_class->snapshot = lambdock_poof_snapshot;
}

static void lambdock_poof_init(LambdockPoof *self) {
  self->state = NULL;
  self->tick_id = 0;
  self->start_us = 0;
  self->started = FALSE;
  self->progress = 0.0;

  gtk_widget_set_can_focus(GTK_WIDGET(self), FALSE);
  gtk_widget_set_can_target(GTK_WIDGET(self), FALSE);
  gtk_widget_add_css_class(GTK_WIDGET(self), "dock-poof");
}

gboolean lambdock_poof_take_slot(DockItem *item) {
  if (!item)
    return FALSE;

  LambdockState *state = item->state;
  if (!state || !state->window || !state->box || !GTK_IS_BOX(state->box))
    return FALSE;

  GtkWidget *slot = item->container ? item->container : item->cell;
  if (!slot)
    slot = item->button;
  if (!slot || gtk_widget_get_parent(slot) != state->box)
    return FALSE;

  /* An unsized slot means the dock is not on screen; there is nothing to see,
   * so the item is removed without an animation. */
  int width = gtk_widget_get_width(slot);
  int height = gtk_widget_get_height(slot);
  if (width <= 0 || height <= 0)
    return FALSE;

  LambdockPoof *poof = g_object_new(LAMBDOCK_TYPE_POOF, NULL);
  poof->state = state;
  lambdock_poof_particles_init(poof->particles, LAMBDOCK_POOF_PARTICLES,
                               (guint32)g_get_monotonic_time() ^
                                   ((guint32)width << 16) ^ (guint32)height);

  GtkWidget *widget = GTK_WIDGET(poof);
  /* The same footprint as the icon, so the dock does not reflow under the
   * smoke, and the same alignment, so it sits on the shelf like its neighbours. */
  gtk_widget_set_size_request(widget, width, height);
  /* Follow the icon's alignment, but never fill: a poof stretched over the
   * dock's whole height would be a grey band, not a puff. */
  GtkAlign valign = gtk_widget_get_valign(slot);
  GtkAlign halign = gtk_widget_get_halign(slot);
  gtk_widget_set_valign(widget, valign == GTK_ALIGN_FILL ? GTK_ALIGN_CENTER
                                                         : valign);
  gtk_widget_set_halign(widget, halign == GTK_ALIGN_FILL ? GTK_ALIGN_CENTER
                                                         : halign);

  /* Right after the slot, which the caller removes next: the puff ends up at
   * exactly the index the icon had, and the dock's layout does not jump. */
  gtk_box_insert_child_after(GTK_BOX(state->box), widget, slot);

  poof->tick_id = gtk_widget_add_tick_callback(widget, on_poof_tick, NULL, NULL);

  return TRUE;
}
