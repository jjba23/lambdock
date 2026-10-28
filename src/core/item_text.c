// lambdock -- hyper-hackable desktop dock for Wayland
//
// SPDX-License-Identifier: MIT
// Copyright © Josep Bigorra <jjbigorra@gmail.com>

#include "item_text.h"

#include <glib/gi18n.h>
#include <math.h>

/*
 * =========================================================================
 * ITEM TEXT (PURE)
 * =========================================================================
 * Wording only: no widgets, no GTK, no globals. Everything a user reads about an
 * item is decided here from plain values, which is also what makes it
 * testable -- including the plural forms.
 */

void lambdock_item_text_clear(LambdockItemText *text) {
  if (!text)
    return;

  g_clear_pointer(&text->label, g_free);
  g_clear_pointer(&text->description, g_free);
}

const char *lambdock_item_display_name(const char *name, const char *fallback) {
  return (name && *name != '\0') ? name : fallback;
}

/**
 * finite_or:
 * A reading that is not a number is a broken reading, not a large one, so it
 * falls back instead of being clamped to a huge value.
 */
static double finite_or(double value, double fallback) {
  return isfinite(value) ? value : fallback;
}

/* What a screen reader should say about an app item besides its name. */
static char *running_text(guint window_count) {
  if (window_count == 0)
    return g_strdup(_("Not running"));

  return g_strdup_printf(
      ngettext("Running, %u window", "Running, %u windows", window_count),
      window_count);
}

LambdockItemText lambdock_item_text_app(const char *name,
                                        guint window_count) {
  const char *title = lambdock_item_display_name(name, NULL);

  return (LambdockItemText){
      .label = g_strdup(title ? title : _("Application")),
      .description = running_text(window_count),
  };
}

LambdockItemText lambdock_item_text_dynamic(const char *name,
                                            const char *reading) {
  const char *title = lambdock_item_display_name(name, NULL);
  char *label = NULL;

  if (reading && *reading != '\0') {
    /* Meter items already put "name: reading" in the tooltip, and repeating the
     * name would read as "CPU: CPU: 42%". */
    const char *prefix = title ? title : "";
    label = g_str_has_prefix(reading, prefix)
                ? g_strdup(reading)
                : g_strdup_printf("%s: %s", title ? title : _("Reading"),
                                  reading);
  } else {
    label = g_strdup(title ? title : _("Reading"));
  }

  return (LambdockItemText){
      .label = label,
      .description = g_strdup(_("Live reading from the dock.")),
  };
}

LambdockItemText lambdock_item_text_stack(const char *name) {
  const char *title = lambdock_item_display_name(name, _("Stack"));

  return (LambdockItemText){
      .label = g_strdup_printf(_("%s, folder stack"), title),
      .description = g_strdup(_("Opens the folder's contents.")),
  };
}

LambdockItemText lambdock_item_text_launchpad(const char *name) {
  const char *title = lambdock_item_display_name(name, _("Applications"));

  return (LambdockItemText){
      .label = g_strdup(title),
      .description = g_strdup(_("Opens the application grid. Escape closes it.")),
  };
}

LambdockItemText lambdock_item_text_trash(const char *name, guint item_count) {
  const char *title = lambdock_item_display_name(name, _("Trash"));

  if (item_count == 0) {
    return (LambdockItemText){
        .label = g_strdup(title),
        .description = g_strdup(_("The trash is empty. Opens the trash.")),
    };
  }

  g_autofree char *count = g_strdup_printf(
      ngettext("%u item in the trash", "%u items in the trash", item_count),
      item_count);

  return (LambdockItemText){
      .label = g_strdup(title),
      .description = g_strdup_printf(_("%s. Opens the trash."), count),
  };
}

char *lambdock_item_trash_tooltip(const char *name, guint item_count) {
  const char *title = lambdock_item_display_name(name, _("Trash"));

  if (item_count == 0)
    return g_strdup(title);

  g_autofree char *items =
      g_strdup_printf(ngettext("%u item", "%u items", item_count), item_count);

  return g_strdup_printf("%s — %s", title, items);
}

double lambdock_meter_reading_fraction(double reading, double maximum) {
  double full_scale = (maximum > 0.0) ? maximum : 100.0;
  double raw = (full_scale > 0.0) ? reading / full_scale : 0.0;

  return CLAMP(finite_or(raw, 0.0), 0.0, 1.0);
}

char *lambdock_meter_reading_text(double reading, const char *unit) {
  /* The gauge writes whole numbers, so a wild reading would otherwise print as
   * 20 digits and break the icon's layout. */
  double shown = CLAMP(finite_or(reading, 0.0), -1e9, 1e9);

  return g_strdup_printf("%.0f%s", shown, unit ? unit : "");
}

const char *lambdock_window_title_text(gboolean show_titles,
                                       gboolean has_windows,
                                       const char *window_title,
                                       const char *name, const char *app_id) {
  if (!show_titles || !has_windows)
    return NULL;

  if (window_title && *window_title != '\0')
    return window_title;

  return name ? name : (app_id ? app_id : "");
}
