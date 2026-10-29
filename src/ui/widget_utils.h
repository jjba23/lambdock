// lambdock -- hyper-hackable desktop dock for Wayland
//
// SPDX-License-Identifier: MIT
// Copyright © Josep Bigorra <jjbigorra@gmail.com>

#ifndef LAMBDOCK_UI_WIDGET_UTILS_H
#define LAMBDOCK_UI_WIDGET_UTILS_H

#include "../lambdock.h"

/* =========================================================================
 * CSS CLASS HELPERS
 * =========================================================================
 * Say 'this dock is a taskbar' once: add or remove one class, or a whole set of
 * mutually exclusive ones, from a boolean.
 *
 * For state-driven styling only -- a widget that is always a tile can just call
 * gtk_widget_add_css_class() at construction.
 */

/**
 * lambdock_set_css_class:
 * @widget: (nullable): the widget to tag; NULL is ignored.
 * @name: the CSS class.
 * @enabled: TRUE to add the class, FALSE to remove it.
 *
 * Adds or removes one CSS class, touching the widget only when the class is not
 * already in the state asked for. GTK already de-duplicates classes, so this is
 * not about memory: it is about the *decision* being one line at the call site,
 * which is what lets callers say "the dock is a taskbar" instead of writing an
 * add/remove/has_class dance everywhere.
 */
void lambdock_set_css_class(GtkWidget *widget, const char *name,
                            gboolean enabled);

/**
 * lambdock_toggle_css_classes:
 * @widget: (nullable): the widget to tag; NULL is ignored.
 * @names: the CSS classes.
 * @n_names: how many entries @names has.
 * @enabled: TRUE to add them all, FALSE to remove them all.
 *
 * The combinator form of lambdock_set_css_class() for sets of mutually exclusive
 * classes (the dock's `shelf-*` edges).
 */
void lambdock_toggle_css_classes(GtkWidget *widget, const char *const *names,
                                 gsize n_names, gboolean enabled);

#endif // LAMBDOCK_UI_WIDGET_UTILS_H
