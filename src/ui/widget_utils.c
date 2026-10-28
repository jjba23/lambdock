// lambdock -- hyper-hackable desktop dock for Wayland
//
// SPDX-License-Identifier: MIT
// Copyright © Josep Bigorra <jjbigorra@gmail.com>

#include "widget_utils.h"

void lambdock_set_css_class(GtkWidget *widget, const char *name,
                            gboolean enabled) {
  if (!widget || !name)
    return;

  if (enabled) {
    if (!gtk_widget_has_css_class(widget, name))
      gtk_widget_add_css_class(widget, name);
  } else if (gtk_widget_has_css_class(widget, name)) {
    gtk_widget_remove_css_class(widget, name);
  }
}

void lambdock_toggle_css_classes(GtkWidget *widget, const char *const *names,
                                 gsize n_names, gboolean enabled) {
  if (!names)
    return;

  for (gsize i = 0; i < n_names; i++)
    lambdock_set_css_class(widget, names[i], enabled);
}
