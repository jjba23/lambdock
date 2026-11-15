// lambdock -- hyper-hackable desktop dock for Wayland
//
// SPDX-License-Identifier: MIT
// Copyright © Josep Bigorra <jjbigorra@gmail.com>

#ifndef LAMBDOCK_CORE_ITEM_TEXT_H
#define LAMBDOCK_CORE_ITEM_TEXT_H

#include <glib.h>

/*
 * =========================================================================
 * ITEM TEXT (PURE) -- what a dock item says about itself
 * =========================================================================
 * Labels, screen-reader descriptions, tooltips and gauge readouts, as functions
 * of values: no widgets, no display, no Guile. `dock_item.c` reads the fields and
 * calls in here, so the wording of every item kind can be unit tested (and
 * translated) without constructing a dock.
 *
 * Only strings, counts and numbers go in: nothing here knows what a DockItem is,
 * which is what keeps it free of GTK.
 */

/**
 * LambdockItemText:
 * @label: (nullable) (transfer full): the name a screen reader announces.
 * @description: (nullable) (transfer full): the item's state or instructions.
 *
 * Two strings that always belong together, so the per-kind functions can return
 * them as one value instead of writing through two out-parameters.
 */
typedef struct LambdockItemText {
  char *label;
  char *description;
} LambdockItemText;

/**
 * lambdock_item_text_clear:
 * @text: (inout): text produced by any lambdock_item_text_*() function.
 *
 * Frees both strings and zeroes the struct, so an early return cannot leak it.
 */
void lambdock_item_text_clear(LambdockItemText *text);

/**
 * lambdock_item_display_name:
 * @name: (nullable): the configured name.
 * @fallback: (nullable): what to use when @name is missing or empty.
 *
 * The "use the name, but not an empty one" rule that every kind repeats.
 *
 * Returns: (transfer none) (nullable): @name if it has content, else @fallback.
 */
const char *lambdock_item_display_name(const char *name, const char *fallback);

/**
 * lambdock_item_text_app:
 * @name: (nullable): the application's configured name.
 * @window_count: how many windows the application currently has open.
 *
 * Returns: the label and description of an application item.
 */
LambdockItemText lambdock_item_text_app(const char *name, guint window_count);

/**
 * lambdock_item_text_dynamic:
 * @name: (nullable): the widget's configured name.
 * @reading: (nullable): the latest value it polled, as shown in its tooltip.
 *
 * Returns: the label and description of a polling (clock, CPU, meter) item.
 */
LambdockItemText lambdock_item_text_dynamic(const char *name,
                                           const char *reading);

/**
 * lambdock_item_text_stack:
 * @name: (nullable): the folder stack's name.
 *
 * Returns: the label and description of a folder stack item.
 */
LambdockItemText lambdock_item_text_stack(const char *name);

/**
 * lambdock_item_text_launchpad:
 * @name: (nullable): the grid's configured name.
 *
 * Returns: the label and description of the application grid item.
 */
LambdockItemText lambdock_item_text_launchpad(const char *name);

/**
 * lambdock_item_text_trash:
 * @name: (nullable): the trash item's configured name.
 * @item_count: how many items the desktop trash holds.
 *
 * Returns: the label and description of the trash item.
 */
LambdockItemText lambdock_item_text_trash(const char *name, guint item_count);

/**
 * lambdock_item_trash_tooltip:
 * @name: (nullable): the trash item's configured name.
 * @item_count: how many items the desktop trash holds.
 *
 * Returns: (transfer full): the item's tooltip -- its name, plus the count while
 * the trash is not empty.
 */
char *lambdock_item_trash_tooltip(const char *name, guint item_count);

/**
 * lambdock_meter_reading_fraction:
 * @reading: the number a Scheme poller returned.
 * @maximum: the reading that fills the gauge (0 or less means 100).
 *
 * A poller can return NaN, infinity or a value well outside its range; the gauge
 * takes a 0..1 fraction and must never be handed a non-finite one.
 *
 * Returns: @reading / @maximum, clamped to [0, 1].
 */
double lambdock_meter_reading_fraction(double reading, double maximum);

/**
 * lambdock_meter_reading_text:
 * @reading: the number a Scheme poller returned.
 * @unit: (nullable): suffix appended to the number ("%", "°C").
 *
 * Returns: (transfer full): the number as the gauge writes it ("42%"), with
 * non-finite readings shown as 0.
 */
char *lambdock_meter_reading_text(double reading, const char *unit);

/**
 * lambdock_window_title_text:
 * @show_titles: whether the dock wants window titles at all.
 * @has_windows: whether the item has a running window.
 * @window_title: (nullable): that window's title, as the compositor reported it.
 * @name: (nullable): the item's configured name.
 * @app_id: (nullable): the item's application id.
 *
 * Taskbar mode writes the running window's title inside the icon and falls back
 * to the application when the window has not named itself yet.
 *
 * Returns: (transfer none) (nullable): the text to show, or NULL to hide the
 * label.
 */
const char *lambdock_window_title_text(gboolean show_titles, gboolean has_windows,
                                       const char *window_title,
                                       const char *name, const char *app_id);

/*
 * The "why is this item here" inspector answers four questions per item: which
 * launcher created it, which desktop entry it resolved, which icon it found and
 * which poller drives it. The wording is here, as values, so the answers can be
 * tested without opening a menu.
 */

/**
 * lambdock_inspector_launcher_text:
 * @config_path: (nullable): the settings file that created the item.
 * @item_key: (nullable): the item's stable key ("app:Firefox").
 *
 * Returns: (transfer full): "key, from path" -- or "key (no settings file: the
 *   fallback dock)" for a dock created without one.
 */
char *lambdock_inspector_launcher_text(const char *config_path,
                                       const char *item_key);

/**
 * lambdock_inspector_icon_text:
 * @icon: (nullable): the icon name or path the item asked for.
 * @found_in_theme: whether the icon theme (or the file system, for a path)
 *   resolved it.
 *
 * Returns: (transfer full): the icon name and where it was found.
 */
char *lambdock_inspector_icon_text(const char *icon, gboolean found_in_theme);

/**
 * lambdock_inspector_poller_text:
 * @poll_name: (nullable): the Scheme procedure that drives the item.
 * @interval_ms: how often that procedure is called.
 *
 * Returns: (transfer full): the poller and its interval, or a sentence saying
 *   the item does not poll.
 */
char *lambdock_inspector_poller_text(const char *poll_name, guint interval_ms);

#endif // LAMBDOCK_CORE_ITEM_TEXT_H
