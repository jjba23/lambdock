// lambdock -- hyper-hackable desktop dock for Wayland
//
// SPDX-License-Identifier: MIT
// Copyright © Josep Bigorra <jjbigorra@gmail.com>

#include "accessibility.h"

/* Cached "gtk-enable-animations": this is read once per animation frame, and a
 * GtkSettings property lookup by name is avoidable work on the one path that
 * has a frame budget. lambdock_watch_system_motion() refreshes it. */
static gboolean system_animations_enabled = TRUE;
static gboolean system_animations_queried = FALSE;
static gboolean system_animations_watched = FALSE;

gboolean lambdock_system_animations_enabled(void) {
  if (system_animations_queried)
    return system_animations_enabled;

  GtkSettings *settings = gtk_settings_get_default();
  if (settings)
    g_object_get(settings, "gtk-enable-animations", &system_animations_enabled,
                 NULL);

  system_animations_queried = TRUE;
  return system_animations_enabled;
}

gboolean lambdock_reduce_motion(const LambdockState *state) {
  if (state && state->reduce_motion)
    return TRUE;

  return !lambdock_system_animations_enabled();
}

gboolean lambdock_animations_enabled(const LambdockState *state) {
  return !lambdock_reduce_motion(state);
}

void lambdock_accessible_update(GtkWidget *widget, const char *label,
                                const char *description) {
  if (!widget || !GTK_IS_ACCESSIBLE(widget))
    return;

  /* Keep the last pushed pair on the widget: this runs on every title/state
   * event for the item that matched, and re-pushing identical properties is
   * pure AT-SPI traffic that can make a screen reader stutter. */
  const char *last_label =
      g_object_get_data(G_OBJECT(widget), "lambdock-a11y-label");
  const char *last_description =
      g_object_get_data(G_OBJECT(widget), "lambdock-a11y-description");

  if (g_strcmp0(last_label, label) == 0 &&
      g_strcmp0(last_description, description) == 0)
    return;

  g_object_set_data_full(G_OBJECT(widget), "lambdock-a11y-label",
                         g_strdup(label), g_free);
  g_object_set_data_full(G_OBJECT(widget), "lambdock-a11y-description",
                         g_strdup(description), g_free);

  GtkAccessible *accessible = GTK_ACCESSIBLE(widget);

  if (description) {
    gtk_accessible_update_property(
        accessible, GTK_ACCESSIBLE_PROPERTY_LABEL, label,
        GTK_ACCESSIBLE_PROPERTY_DESCRIPTION, description, -1);
  } else {
    gtk_accessible_update_property(accessible, GTK_ACCESSIBLE_PROPERTY_LABEL,
                                   label, -1);
  }
}

void lambdock_accessible_announce(GtkWidget *widget, const char *message,
                                  gboolean assertive) {
  if (!widget || !message || *message == '\0')
    return;

#if GTK_CHECK_VERSION(4, 14, 0)
  if (!GTK_IS_ACCESSIBLE(widget))
    return;

  gtk_accessible_announce(
      GTK_ACCESSIBLE(widget), message,
      assertive ? GTK_ACCESSIBLE_ANNOUNCEMENT_PRIORITY_HIGH
                : GTK_ACCESSIBLE_ANNOUNCEMENT_PRIORITY_LOW);
#else
  (void)widget;
  (void)assertive;
#endif
}

/** Drops the cached "gtk-enable-animations" so the next read is fresh. */
static void on_system_animations_changed(GtkSettings *settings,
                                         GParamSpec *pspec, gpointer data) {
  (void)settings;
  (void)pspec;
  (void)data;

  system_animations_queried = FALSE;
}

guint lambdock_watch_system_motion(GCallback callback, gpointer data) {
  GtkSettings *settings = gtk_settings_get_default();
  if (!settings)
    return 0;

  /* One cache-invalidating handler per settings object, for the lifetime of the
   * process: the dock reads the value every animation frame, so it must not go
   * stale when the desktop toggles animations. */
  if (!system_animations_watched) {
    system_animations_watched = TRUE;
    g_signal_connect(settings, "notify::gtk-enable-animations",
                     G_CALLBACK(on_system_animations_changed), NULL);
  }

  if (!callback)
    return 0;

  return g_signal_connect(settings, "notify::gtk-enable-animations", callback,
                          data);
}
