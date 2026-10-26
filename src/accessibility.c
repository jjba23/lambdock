// lambdock

// Copyright © Josep Bigorra <jjbigorra@gmail.com>

// lambdock is free software : you can redistribute it and / or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.

// lambdock is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY ; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
// GNU General Public License for more details.

// You should have received a copy of the GNU General Public License
// along with lambdock. If not, see <https://www.gnu.org/licenses/>.

#include "accessibility.h"

gboolean lambdock_system_animations_enabled(void) {
  GtkSettings *settings = gtk_settings_get_default();
  gboolean enabled = TRUE;

  if (settings)
    g_object_get(settings, "gtk-enable-animations", &enabled, NULL);

  return enabled;
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

guint lambdock_watch_system_motion(GCallback callback, gpointer data) {
  GtkSettings *settings = gtk_settings_get_default();
  if (!settings || !callback)
    return 0;

  return g_signal_connect(settings, "notify::gtk-enable-animations", callback,
                          data);
}
