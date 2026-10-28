// lambdock -- hyper-hackable desktop dock for Wayland
//
// SPDX-License-Identifier: MIT
// Copyright © Josep Bigorra <jjbigorra@gmail.com>

#include "../src/core/item_text.h"

#include <glib.h>
#include <math.h>

/* The strings a dock item shows, tested as values: no display, no widgets, no
 * Guile. These are the rules that used to be spread over five `describe_*`
 * functions writing through two out-parameters each. */

/* ============================================================================
 * DISPLAY NAME
 * ============================================================================ */

static void test_display_name(void) {
  g_assert_cmpstr(lambdock_item_display_name("Terminal", "fallback"), ==,
                  "Terminal");

  /* An empty name is no name: that is what makes "Terminal" show instead of a
   * blank icon label. */
  g_assert_cmpstr(lambdock_item_display_name("", "fallback"), ==, "fallback");
  g_assert_null(lambdock_item_display_name(NULL, NULL));
  g_assert_cmpstr(lambdock_item_display_name(NULL, "fallback"), ==, "fallback");
}

/* ============================================================================
 * ACCESSIBLE TEXT PER KIND
 * ============================================================================ */

static void test_text_app(void) {
  LambdockItemText text = lambdock_item_text_app("Terminal", 0);
  g_assert_cmpstr(text.label, ==, "Terminal");
  g_assert_cmpstr(text.description, ==, "Not running");
  lambdock_item_text_clear(&text);
  g_assert_null(text.label);
  g_assert_null(text.description);

  /* Singular and plural window counts. */
  text = lambdock_item_text_app("Terminal", 1);
  g_assert_cmpstr(text.description, ==, "Running, 1 window");
  lambdock_item_text_clear(&text);

  text = lambdock_item_text_app("Terminal", 3);
  g_assert_cmpstr(text.description, ==, "Running, 3 windows");
  lambdock_item_text_clear(&text);

  /* An unnamed application still says what it is. */
  text = lambdock_item_text_app(NULL, 0);
  g_assert_cmpstr(text.label, ==, "Application");
  lambdock_item_text_clear(&text);

  text = lambdock_item_text_app("", 0);
  g_assert_cmpstr(text.label, ==, "Application");
  lambdock_item_text_clear(&text);

  /* Clearing NULL is tolerated. */
  lambdock_item_text_clear(NULL);
}

static void test_text_dynamic(void) {
  /* A reading that does not already carry the name gets it prepended. */
  LambdockItemText text = lambdock_item_text_dynamic("CPU", "42%");
  g_assert_cmpstr(text.label, ==, "CPU: 42%");
  g_assert_cmpstr(text.description, ==, "Live reading from the dock.");
  lambdock_item_text_clear(&text);

  /* Meter items already write "name: reading", so it is not repeated. */
  text = lambdock_item_text_dynamic("CPU", "CPU: 42%");
  g_assert_cmpstr(text.label, ==, "CPU: 42%");
  lambdock_item_text_clear(&text);

  /* No reading yet: the name alone. */
  text = lambdock_item_text_dynamic("CPU", NULL);
  g_assert_cmpstr(text.label, ==, "CPU");
  lambdock_item_text_clear(&text);

  text = lambdock_item_text_dynamic("CPU", "");
  g_assert_cmpstr(text.label, ==, "CPU");
  lambdock_item_text_clear(&text);

  /* Nameless with a reading: the reading is the label. */
  text = lambdock_item_text_dynamic(NULL, "42%");
  g_assert_cmpstr(text.label, ==, "42%");
  lambdock_item_text_clear(&text);

  /* Nameless and no reading: a generic label rather than an empty one. */
  text = lambdock_item_text_dynamic(NULL, NULL);
  g_assert_cmpstr(text.label, ==, "Reading");
  lambdock_item_text_clear(&text);
}

static void test_text_stack_and_launchpad(void) {
  LambdockItemText text = lambdock_item_text_stack("Downloads");
  g_assert_cmpstr(text.label, ==, "Downloads, folder stack");
  g_assert_cmpstr(text.description, ==, "Opens the folder's contents.");
  lambdock_item_text_clear(&text);

  text = lambdock_item_text_stack(NULL);
  g_assert_cmpstr(text.label, ==, "Stack, folder stack");
  lambdock_item_text_clear(&text);

  text = lambdock_item_text_launchpad("Applications");
  g_assert_cmpstr(text.label, ==, "Applications");
  g_assert_nonnull(strstr(text.description, "Escape"));
  lambdock_item_text_clear(&text);

  text = lambdock_item_text_launchpad(NULL);
  g_assert_cmpstr(text.label, ==, "Applications");
  lambdock_item_text_clear(&text);
}

static void test_text_trash(void) {
  LambdockItemText text = lambdock_item_text_trash("Trash", 0);
  g_assert_cmpstr(text.label, ==, "Trash");
  g_assert_cmpstr(text.description, ==,
                  "The trash is empty. Opens the trash.");
  lambdock_item_text_clear(&text);

  text = lambdock_item_text_trash("Trash", 1);
  g_assert_cmpstr(text.description, ==, "1 item in the trash. Opens the trash.");
  lambdock_item_text_clear(&text);

  text = lambdock_item_text_trash("Trash", 4);
  g_assert_cmpstr(text.description, ==,
                  "4 items in the trash. Opens the trash.");
  lambdock_item_text_clear(&text);

  text = lambdock_item_text_trash(NULL, 2);
  g_assert_cmpstr(text.label, ==, "Trash");
  lambdock_item_text_clear(&text);
}

static void test_trash_tooltip(void) {
  g_autofree char *empty = lambdock_item_trash_tooltip("Trash", 0);
  g_assert_cmpstr(empty, ==, "Trash");

  g_autofree char *one = lambdock_item_trash_tooltip("Trash", 1);
  g_assert_cmpstr(one, ==, "Trash — 1 item");

  g_autofree char *many = lambdock_item_trash_tooltip("Trash", 7);
  g_assert_cmpstr(many, ==, "Trash — 7 items");

  g_autofree char *unnamed = lambdock_item_trash_tooltip(NULL, 0);
  g_assert_cmpstr(unnamed, ==, "Trash");
}

/* ============================================================================
 * GAUGE READINGS
 * ============================================================================ */

static void test_meter_fraction(void) {
  g_assert_cmpfloat(lambdock_meter_reading_fraction(50.0, 100.0), ==, 0.5);
  g_assert_cmpfloat(lambdock_meter_reading_fraction(0.0, 100.0), ==, 0.0);
  g_assert_cmpfloat(lambdock_meter_reading_fraction(100.0, 100.0), ==, 1.0);

  /* Out-of-range readings fill or empty the ring instead of wrapping it. */
  g_assert_cmpfloat(lambdock_meter_reading_fraction(250.0, 100.0), ==, 1.0);
  g_assert_cmpfloat(lambdock_meter_reading_fraction(-40.0, 100.0), ==, 0.0);

  /* A gauge with no configured maximum is a percentage gauge. */
  g_assert_cmpfloat(lambdock_meter_reading_fraction(25.0, 0.0), ==, 0.25);
  g_assert_cmpfloat(lambdock_meter_reading_fraction(25.0, -10.0), ==, 0.25);

  /* Broken readings from a Scheme poller must never reach the gauge. */
  g_assert_cmpfloat(lambdock_meter_reading_fraction(NAN, 100.0), ==, 0.0);
  g_assert_cmpfloat(lambdock_meter_reading_fraction(INFINITY, 100.0), ==, 0.0);
}

static void test_meter_reading_text(void) {
  g_autofree char *percent = lambdock_meter_reading_text(42.4, "%");
  g_assert_cmpstr(percent, ==, "42%");

  /* Rounded to whole numbers, which is all the icon has room for. */
  g_autofree char *rounded = lambdock_meter_reading_text(42.6, "%");
  g_assert_cmpstr(rounded, ==, "43%");

  g_autofree char *celsius = lambdock_meter_reading_text(54.0, "°C");
  g_assert_cmpstr(celsius, ==, "54°C");

  g_autofree char *no_unit = lambdock_meter_reading_text(7.0, NULL);
  g_assert_cmpstr(no_unit, ==, "7");

  /* NaN prints as 0, not as "nan". */
  g_autofree char *nan = lambdock_meter_reading_text(NAN, "%");
  g_assert_cmpstr(nan, ==, "0%");

  /* Absurd readings are clamped before they are printed. */
  g_autofree char *huge = lambdock_meter_reading_text(1e300, "");
  g_assert_cmpstr(huge, ==, "1000000000");
}

/* ============================================================================
 * WINDOW TITLES (TASKBAR MODE)
 * ============================================================================ */

static void test_window_title_text(void) {
  /* Titles off, or no window: the label stays hidden. */
  g_assert_null(lambdock_window_title_text(FALSE, TRUE, "Inbox", "Mail", "mail"));
  g_assert_null(lambdock_window_title_text(TRUE, FALSE, "Inbox", "Mail", "mail"));

  /* The running window names itself: show that. */
  g_assert_cmpstr(lambdock_window_title_text(TRUE, TRUE, "Inbox", "Mail", "mail"),
                  ==, "Inbox");

  /* A window with no title yet falls back to the application. */
  g_assert_cmpstr(lambdock_window_title_text(TRUE, TRUE, NULL, "Mail", "mail"),
                  ==, "Mail");
  g_assert_cmpstr(lambdock_window_title_text(TRUE, TRUE, "", "Mail", "mail"), ==,
                  "Mail");
  g_assert_cmpstr(lambdock_window_title_text(TRUE, TRUE, NULL, NULL, "mail"), ==,
                  "mail");

  /* Nothing at all to say still hides nothing: an empty title is a title. */
  g_assert_cmpstr(lambdock_window_title_text(TRUE, TRUE, NULL, NULL, NULL), ==,
                  "");
}

int main(int argc, char **argv) {
  g_test_init(&argc, &argv, NULL);

  g_test_add_func("/core/item_text/display_name", test_display_name);
  g_test_add_func("/core/item_text/app", test_text_app);
  g_test_add_func("/core/item_text/dynamic", test_text_dynamic);
  g_test_add_func("/core/item_text/stack_and_launchpad",
                  test_text_stack_and_launchpad);
  g_test_add_func("/core/item_text/trash", test_text_trash);
  g_test_add_func("/core/item_text/trash_tooltip", test_trash_tooltip);
  g_test_add_func("/core/item_text/meter_fraction", test_meter_fraction);
  g_test_add_func("/core/item_text/meter_reading_text",
                  test_meter_reading_text);
  g_test_add_func("/core/item_text/window_title_text", test_window_title_text);

  return g_test_run();
}
