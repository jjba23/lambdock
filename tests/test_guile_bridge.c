// lambdock -- hyper-hackable desktop dock for Wayland
//
// SPDX-License-Identifier: MIT
// Copyright © Josep Bigorra <jjbigorra@gmail.com>

#include "../src/guile_bridge.h"
#include "../src/lambdock.h"
#include "../src/ui/dock_item.h"
#include "../src/ui/launchpad.h"
#include "../src/ui/clock.h"
#include "../src/ui/meter.h"
#include <glib.h>
#include <gtk/gtk.h>
#include <libguile.h>
#include <string.h>

/* A fake (lambdock core) module whose answers are keyed off the config-path
 * argument, so we can exercise every conversion/branch of the C bridge. */
static const char *FAKE_MODULES =
    "(define-module (lambdock core)\n"
    "  #:export (get-dock-auto-hide?\n"
    "            get-dock-show-when-clear?\n"
    "            get-dock-hover-animate?\n"
    "            get-dock-track-windows?\n"
    "            get-dock-enable-repl?\n"
    "            get-dock-icon-size\n"
    "            get-dock-theme\n"
    "            get-dock-monitor\n"
    "            get-dock-position\n"
    "            get-dock-exclude-apps\n"
    "            get-dock-launchpad-icon-size\n"
    "            get-dock-launchpad-columns\n"
    "            get-dock-launchpad-blur?\n"
    "            get-dock-items\n"
    "            dock-item-type dock-item-name dock-item-icon dock-item-exec\n"
    "            dock-item-poll-fn dock-item-poll-interval\n"
    "            dock-item-hover-animate?\n"
    "            dock-item-path dock-item-view-mode\n"
    "            dock-item-meter? dock-item-clock? dock-item-unit\n"
    "            dock-item-max\n"
    "            dock-layout-add! dock-layout-remove!\n"
    "            dock-layout-set-order! dock-layout-set-view!\n"
    "            get-layout-calls clear-layout-calls!\n"
    "            load-dock-config! init-dock! get-init-count get-loaded))\n"
    "(define (arg-or a) (if (pair? a) (car a) #f))\n"
    "(define (get-dock-auto-hide? . a) (equal? (arg-or a) \"auto\"))\n"
    "(define (get-dock-show-when-clear? . a) (equal? (arg-or a) \"show\"))\n"
    "(define (get-dock-hover-animate? . a) (equal? (arg-or a) \"hover\"))\n"
    "(define (get-dock-track-windows? . a) (equal? (arg-or a) \"track\"))\n"
    "(define (get-dock-enable-repl? . a) (equal? (arg-or a) \"repl\"))\n"
    "(define (get-dock-icon-size . a)\n"
    "  (cond ((equal? (arg-or a) \"big\") 96)\n"
    "        ((equal? (arg-or a) \"zero\") 0)\n"
    "        ((equal? (arg-or a) \"junk\") \"not-a-number\")\n"
    "        (else 48)))\n"
    "(define (get-dock-theme . a) (if (equal? (arg-or a) \"sym\") 'unity "
    "\"vanilla\"))\n"
    "(define (get-dock-monitor . a) (if (equal? (arg-or a) \"hdmi\") 'HDMI-1 "
    "\"default\"))\n"
    "(define (get-dock-position . a)\n"
    "  (cond ((equal? (arg-or a) \"top\") 'top)\n"
    "        ((equal? (arg-or a) \"left\") 'left)\n"
    "        ((equal? (arg-or a) \"right\") 'right)\n"
    "        ((equal? (arg-or a) \"weird\") 'diagonal)\n"
    "        (else 'bottom)))\n"
    "(define (get-dock-exclude-apps . a)\n"
    "  (list 'bad-app \"TRACKER-DAEMON\" 42 \"\" 'empty-sym))\n"
    "(define (get-dock-launchpad-icon-size . a)\n"
    "  (cond ((not (pair? a)) \"not-configured\")\n"
    "        ((equal? (arg-or a) \"launch-big\") 120)\n"
    "        ((equal? (arg-or a) \"launch-tiny\") 2)\n"
    "        ((equal? (arg-or a) \"junk\") \"not-a-number\")\n"
    "        (else 64)))\n"
    "(define (get-dock-launchpad-columns . a)\n"
    "  (cond ((equal? (arg-or a) \"cols\") 5)\n"
    "        ((equal? (arg-or a) \"cols-low\") 1)\n"
    "        ((equal? (arg-or a) \"cols-zero\") 0)\n"
    "        ((equal? (arg-or a) \"junk\") \"not-a-number\")\n"
    "        (else 0)))\n"
    "(define (get-dock-launchpad-blur? . a)\n"
    "  (equal? (arg-or a) \"frost\"))\n"
    "(define (get-dock-items . a)\n"
    "  (list (vector 'app \"Good\" \"utilities-terminal\" \"good\" #f 0 #t)\n"
    "        (vector 'app \"bad-app\" \"utilities-terminal\" \"bad\" #f 0 #t)\n"
    "        (vector 'dynamic \"Dyn\" \"appointment-new\" \"dyn\"\n"
    "                (lambda () \"TICK\") 111 #f)\n"
    "        (vector 'stack \"Downloads\" \"folder\" \"\" #f 0 #t\n"
    "                \"/tmp/downloads\" 'list)\n"
    "        (vector 'launchpad \"Launchpad\" \"view-app-grid-symbolic\" \"\"\n"
    "                #f 0 #t \"\" 'grid)\n"
    "        (vector 'dynamic \"Gauge\" \"\" \"gauge\"\n"
    "                (lambda () 42) 111 #f \"\" 'grid #t \"%\" 100)\n"
    "        (vector 'dynamic \"Clock\" \"\" \"clock\"\n"
    "                (lambda () \"Thu Oct 29 13:07\") 111 #f \"\" 'grid #f #f\n"
    "                \"\" 100 #t)\n"
    "        (vector 'separator \"\" \"\" \"\" #f 0 #f)\n"
    "        (vector 'trash \"Trash\" \"user-trash\" \"\" #f 0 #t)))\n"
    "(define (dock-item-type v) (vector-ref v 0))\n"
    "(define (dock-item-name v) (vector-ref v 1))\n"
    "(define (dock-item-icon v) (vector-ref v 2))\n"
    "(define (dock-item-exec v) (vector-ref v 3))\n"
    "(define (dock-item-poll-fn v) (vector-ref v 4))\n"
    "(define (dock-item-poll-interval v) (vector-ref v 5))\n"
    "(define (dock-item-hover-animate? v) (vector-ref v 6))\n"
    "(define (dock-item-path v) (if (> (vector-length v) 7) (vector-ref v 7) \"\"))\n"
    "(define (dock-item-view-mode v)\n"
    "  (if (> (vector-length v) 8) (vector-ref v 8) 'grid))\n"
    "(define (dock-item-meter? v)\n"
    "  (if (> (vector-length v) 9) (vector-ref v 9) #f))\n"
    "(define (dock-item-unit v)\n"
    "  (if (> (vector-length v) 10) (vector-ref v 10) \"\"))\n"
    "(define (dock-item-max v)\n"
    "  (if (> (vector-length v) 11) (vector-ref v 11) 100))\n"
    "(define (dock-item-clock? v)\n"
    "  (if (> (vector-length v) 12) (vector-ref v 12) #f))\n"
    "(define %layout-calls '())\n"
    "(define (dock-layout-add! path kind name icon arg view)\n"
    "  (set! %layout-calls\n"
    "        (cons (list 'add path kind name icon arg view) %layout-calls)))\n"
    "(define (dock-layout-remove! path key)\n"
    "  (set! %layout-calls (cons (list 'remove path key) %layout-calls)))\n"
    "(define (dock-layout-set-order! path keys)\n"
    "  (set! %layout-calls (cons (list 'order path keys) %layout-calls)))\n"
    "(define (dock-layout-set-view! path key view)\n"
    "  (set! %layout-calls (cons (list 'view path key view) %layout-calls)))\n"
    "(define (get-layout-calls) (reverse %layout-calls))\n"
    "(define (clear-layout-calls!) (set! %layout-calls '()))\n"
    "(define %init-count 0)\n"
    "(define (init-dock!) (set! %init-count (+ %init-count 1)))\n"
    "(define (get-init-count) %init-count)\n"
    "(define %loaded #f)\n"
    "(define (load-dock-config! p) (set! %loaded p))\n"
    "(define (get-loaded) %loaded)\n"
    "(define-module (lambdock loader) #:export (get-config-path))\n"
    "(define (get-config-path) \"/tmp/fake-settings.scm\")\n";

static int call_int_public(const char *name) {
  SCM fn = scm_c_public_ref("lambdock core", name);
  return scm_to_int(scm_call_0(fn));
}

static char *call_string_public(const char *name) {
  SCM fn = scm_c_public_ref("lambdock core", name);
  SCM res = scm_call_0(fn);
  if (scm_is_true(res) && scm_is_string(res))
    return scm_to_utf8_string(res);
  return NULL;
}

static void test_boolean_getters(void) {
  g_assert_true(get_autohide_config_for("auto"));
  g_assert_false(get_autohide_config_for("nope"));
  /* A NULL path calls the zero-argument form. */
  g_assert_false(get_autohide_config_for(NULL));
  g_assert_false(get_autohide_config_for(""));

  g_assert_true(get_show_when_clear_config_for("show"));
  g_assert_false(get_show_when_clear_config_for("x"));

  g_assert_true(get_hover_animate_config_for("hover"));
  g_assert_false(get_hover_animate_config_for("x"));

  g_assert_true(get_track_windows_config_for("track"));
  g_assert_false(get_track_windows_config_for("x"));

  g_assert_true(get_repl_config_for("repl"));
  g_assert_false(get_repl_config_for("x"));
}

static void test_int_getters(void) {
  g_assert_cmpint(get_icon_size_config_for("big"), ==, 96);
  g_assert_cmpint(get_icon_size_config_for("default"), ==, 48);
  /* Non-positive sizes fall back to the default. */
  g_assert_cmpint(get_icon_size_config_for("zero"), ==, DEFAULT_ICON_SIZE);
  /* Non-numeric results also fall back to the default. */
  g_assert_cmpint(get_icon_size_config_for("junk"), ==, DEFAULT_ICON_SIZE);
  g_assert_cmpint(get_icon_size_config_for(NULL), ==, 48);
}

/* The launchpad settings are read through the same bridge and clamped so a typo
 * cannot make the grid unreadable. */
static void test_launchpad_config_getters(void) {
  g_assert_cmpint(get_launchpad_icon_size_config_for("launch-big"), ==, 120);
  g_assert_cmpint(get_launchpad_icon_size_config_for("launch-tiny"), ==,
                  LAUNCHPAD_ICON_SIZE_MIN);
  g_assert_cmpint(get_launchpad_icon_size_config_for("junk"), ==,
                  LAUNCHPAD_ICON_SIZE);
  /* No path at all: the fake answers with something unusable, which is what the
   * C-side default is for. */
  g_assert_cmpint(get_launchpad_icon_size_config_for(NULL), ==,
                  LAUNCHPAD_ICON_SIZE);

  g_assert_cmpint(get_launchpad_columns_config_for("cols"), ==, 5);
  g_assert_cmpint(get_launchpad_columns_config_for("cols-low"), ==,
                  LAUNCHPAD_COLUMNS_MIN);
  g_assert_cmpint(get_launchpad_columns_config_for("cols-zero"), ==,
                  LAUNCHPAD_COLUMNS_AUTO);
  g_assert_cmpint(get_launchpad_columns_config_for(NULL), ==,
                  LAUNCHPAD_COLUMNS_AUTO);

  g_assert_true(get_launchpad_blur_config_for("frost"));
  g_assert_false(get_launchpad_blur_config_for("solid"));
  g_assert_false(get_launchpad_blur_config_for(NULL));
}

static void test_string_getters(void) {
  char *theme = get_theme_config_for("plain");
  g_assert_cmpstr(theme, ==, "vanilla");
  free(theme);

  /* A Scheme symbol is converted to its string form. */
  theme = get_theme_config_for("sym");
  g_assert_cmpstr(theme, ==, "unity");
  free(theme);

  char *monitor = get_monitor_config_for("x");
  g_assert_cmpstr(monitor, ==, "default");
  free(monitor);

  monitor = get_monitor_config_for("hdmi");
  g_assert_cmpstr(monitor, ==, "HDMI-1");
  free(monitor);

  char *config_path = get_config_path_c();
  g_assert_cmpstr(config_path, ==, "/tmp/fake-settings.scm");
  free(config_path); /* scm_to_utf8_string allocation */
}

static void test_position_getter(void) {
  g_assert_cmpint(get_position_config_for("top"), ==,
                  GTK_LAYER_SHELL_EDGE_TOP);
  g_assert_cmpint(get_position_config_for("left"), ==,
                  GTK_LAYER_SHELL_EDGE_LEFT);
  g_assert_cmpint(get_position_config_for("right"), ==,
                  GTK_LAYER_SHELL_EDGE_RIGHT);
  g_assert_cmpint(get_position_config_for("bottom"), ==,
                  GTK_LAYER_SHELL_EDGE_BOTTOM);
  /* Unknown placements fall back to the bottom edge. */
  g_assert_cmpint(get_position_config_for("weird"), ==,
                  GTK_LAYER_SHELL_EDGE_BOTTOM);
  g_assert_cmpint(get_position_config_for(NULL), ==,
                  GTK_LAYER_SHELL_EDGE_BOTTOM);
}

static void test_is_app_excluded(void) {
  /* Symbols, strings and case-insensitive matching all work. */
  g_assert_true(is_app_excluded("bad-app", "cfg"));
  g_assert_true(is_app_excluded("BAD-APP", "cfg"));
  g_assert_true(is_app_excluded("TRACKER-DAEMON", "cfg"));
  g_assert_true(is_app_excluded("tracker-daemon", "cfg"));
  g_assert_true(is_app_excluded("empty-sym", "cfg"));

  g_assert_false(is_app_excluded("allowed", "cfg"));

  /* NULL / empty inputs are never considered excluded. */
  g_assert_false(is_app_excluded(NULL, "cfg"));
  g_assert_false(is_app_excluded("", "cfg"));
}

static void test_reload_and_load_config(void) {
  g_assert_cmpint(call_int_public("get-init-count"), ==, 0);

  reload_guile_config();
  g_assert_cmpint(call_int_public("get-init-count"), ==, 1);

  load_dock_config_scm("/tmp/some-settings.scm");
  char *loaded = call_string_public("get-loaded");
  g_assert_cmpstr(loaded, ==, "/tmp/some-settings.scm");
  free(loaded);

  /* NULL must be a silent no-op. */
  load_dock_config_scm(NULL);
  g_assert_cmpint(call_int_public("get-init-count"), ==, 1);
}

static void test_populate_dock_items(void) {
  LambdockState state = {0};
  state.box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
  state.orientation = GTK_ORIENTATION_HORIZONTAL;
  state.icon_size = 48;
  state.config_path = g_strdup("cfg");

  populate_dock_items_for(&state);

  /* The excluded "bad-app" launcher is skipped; app + dynamic + stack +
   * launchpad + meter + clock + separator + trash remain, in order. */
  g_assert_cmpint(g_list_length(state.dock_items), ==, 8);

  DockItem *app = (DockItem *)g_list_nth_data(state.dock_items, 0);
  g_assert_nonnull(app);
  g_assert_cmpstr(app->name, ==, "Good");
  g_assert_cmpstr(app->app_id, ==, "Good");
  g_assert_true(app->is_favorite);
  g_assert_true(app->hover_animate);
  /* App items now carry a window-title label, hidden until the app runs and
   * `show-window-titles?` is enabled. */
  g_assert_nonnull(app->label);
  g_assert_true(app->window_title_label);
  g_assert_false(gtk_widget_get_visible(app->label));
  g_assert_true(GTK_IS_BUTTON(app->button));

  DockItem *dyn = (DockItem *)g_list_nth_data(state.dock_items, 1);
  g_assert_nonnull(dyn);
  g_assert_cmpstr(dyn->name, ==, "Dyn");
  g_assert_false(dyn->hover_animate);
  g_assert_nonnull(dyn->label);
  /* The poller ran once on creation and wrote its text into the label. */
  g_assert_cmpstr(gtk_label_get_text(GTK_LABEL(dyn->label)), ==, "TICK");
  g_assert_true(gtk_widget_has_css_class(dyn->button, "dynamic-item"));
  g_assert_cmpint(dyn->poll_timer_id, >, 0);

  DockItem *stack = (DockItem *)g_list_nth_data(state.dock_items, 2);
  g_assert_nonnull(stack);
  g_assert_cmpint(stack->kind, ==, LAMBDOCK_ITEM_STACK);
  g_assert_cmpstr(stack->stack_path, ==, "/tmp/downloads");
  g_assert_cmpstr(stack->stack_view, ==, "list");
  g_assert_true(gtk_widget_has_css_class(stack->button, "stack-item"));
  g_assert_true(stack->is_favorite);

  DockItem *launchpad = (DockItem *)g_list_nth_data(state.dock_items, 3);
  g_assert_nonnull(launchpad);
  g_assert_cmpint(launchpad->kind, ==, LAMBDOCK_ITEM_LAUNCHPAD);
  g_assert_true(gtk_widget_has_css_class(launchpad->button, "launchpad-item"));

  /* A meter dynamic item is a gauge: the text form is dropped and the first
   * reading is already on it (42 out of 100 with the "%" unit). */
  DockItem *gauge = (DockItem *)g_list_nth_data(state.dock_items, 4);
  g_assert_nonnull(gauge);
  g_assert_cmpstr(gauge->name, ==, "Gauge");
  g_assert_nonnull(gauge->meter);
  g_assert_true(lambdock_meter_is_meter(gauge->meter));
  g_assert_cmpfloat(lambdock_meter_fraction(gauge->meter), ==, 0.42);
  g_assert_cmpstr(gauge->meter_unit, ==, "%");
  g_assert_cmpfloat(gauge->meter_max, ==, 100.0);
  g_assert_null(gauge->label);
  /* The gauge mirrors nothing, but it keeps its reflection slot: the band that
   * slot reserves is part of the cell's height, which is what keeps the gauges
   * level with the icons on a shelf theme. It draws nothing and takes no room
   * while the shelf is off. */
  g_assert_nonnull(gauge->reflection);
  g_assert_false(gtk_widget_get_visible(gauge->reflection));
  {
    const char *tooltip = gtk_widget_get_tooltip_text(gauge->button);
    g_assert_nonnull(tooltip);
    g_assert_true(strstr(tooltip, "42%") != NULL);
  }

  /* The icon-sized cell carries the cross size, not the button: the theme's
   * button padding sits outside the cell, so a label or a gauge ends up exactly
   * as tall as an icon. */
  {
    int width = 0;
    int height = 0;
    gtk_widget_get_size_request(gauge->cell, &width, &height);
    g_assert_cmpint(width, ==, -1);
    g_assert_cmpint(height, ==, state.icon_size);
  }

  /* A clock dynamic item is an analogue face: the text form goes, the hands come
   * from the system clock, and the item's poller keeps naming the reading for the
   * tooltip and the screen reader. */
  DockItem *clock = (DockItem *)g_list_nth_data(state.dock_items, 5);
  g_assert_nonnull(clock);
  g_assert_cmpstr(clock->name, ==, "Clock");
  g_assert_nonnull(clock->clock);
  g_assert_true(lambdock_clock_is_clock(clock->clock));
  g_assert_null(clock->meter);
  g_assert_null(clock->label);
  g_assert_nonnull(clock->reflection);
  g_assert_false(gtk_widget_get_visible(clock->reflection));
  g_assert_true(gtk_widget_has_css_class(clock->button, "dynamic-clock"));
  {
    const char *tooltip = gtk_widget_get_tooltip_text(clock->button);
    g_assert_nonnull(tooltip);
    g_assert_true(strstr(tooltip, "Thu Oct 29 13:07") != NULL);
  }
  /* The hands are already set: the face is painted on creation, not one poll
   * timer later. */
  {
    int seconds = lambdock_clock_get_time(clock->clock);
    g_assert_cmpint(seconds, >=, 0);
    g_assert_cmpint(seconds, <, 86400);
  }

  DockItem *sep = (DockItem *)g_list_nth_data(state.dock_items, 6);
  g_assert_nonnull(sep);
  g_assert_true(GTK_IS_SEPARATOR(sep->button));
  g_assert_true(sep->is_favorite);
  /* The line is shorter than an icon, so it lives in an icon-sized cell and
   * keeps a reflection slot: both are what centre it on the row instead of
   * leaving it hanging at the bottom edge. */
  g_assert_nonnull(sep->cell);
  g_assert_nonnull(sep->reflection);
  {
    int width = 0;
    int height = 0;
    gtk_widget_get_size_request(sep->cell, &width, &height);
    g_assert_cmpint(width, ==, -1);
    g_assert_cmpint(height, ==, state.icon_size);
  }

  DockItem *trash = (DockItem *)g_list_nth_data(state.dock_items, 7);
  g_assert_nonnull(trash);
  g_assert_cmpint(trash->kind, ==, LAMBDOCK_ITEM_TRASH);
  g_assert_cmpstr(trash->name, ==, "Trash");
  g_assert_true(gtk_widget_has_css_class(trash->button, "trash-item"));
  g_assert_true(trash->is_favorite);
  g_assert_cmpuint(trash->trash_timer_id, >, 0);

  /* Let the dynamic timer fire at least once more before teardown, proving
   * the C-side polling loop keeps calling the Scheme procedure. */
  g_usleep(200 * 1000);
  while (g_main_context_iteration(NULL, FALSE))
    ;
  g_assert_cmpstr(gtk_label_get_text(GTK_LABEL(dyn->label)), ==, "TICK");

  clear_favorite_dock_items(&state);
  g_assert_null(state.dock_items);

  g_object_ref_sink(state.box);
  g_object_unref(state.box);
  g_free(state.config_path);

  /* Missing box is a no-op. */
  populate_dock_items_for(NULL);
  LambdockState nobox = {0};
  populate_dock_items_for(&nobox);
}

/* Evaluates an expression inside the fake (lambdock core) module, where the
 * recorded layout calls live. */
static SCM scheme_eval(const char *expression) {
  SCM module = scm_c_resolve_module("lambdock core");
  SCM previous = scm_set_current_module(module);
  SCM result = scm_c_eval_string(expression);
  scm_set_current_module(previous);
  return result;
}

static gboolean scheme_true(const char *expression) {
  return scm_is_true(scheme_eval(expression));
}

static void test_layout_bridge(void) {
  scheme_eval("(clear-layout-calls!)");

  /* Additions: NULL optional strings become empty strings. */
  dock_layout_add_item("cfg", "app", "Slack", "slack", "slack --start", NULL);
  g_assert_true(scheme_true(
      "(equal? (get-layout-calls)"
      "        (list (list 'add \"cfg\" \"app\" \"Slack\" \"slack\""
      "                    \"slack --start\" \"\")))"));

  scheme_eval("(clear-layout-calls!)");
  dock_layout_remove_item("cfg", "app:Slack");
  g_assert_true(scheme_true("(equal? (get-layout-calls)"
                            "        (list (list 'remove \"cfg\" \"app:Slack\")))"));

  /* Order: a GList of keys becomes a Scheme list, in order. */
  scheme_eval("(clear-layout-calls!)");
  GList *keys = NULL;
  keys = g_list_append(keys, (gpointer)"app:A");
  keys = g_list_append(keys, (gpointer)"stack:Downloads");
  dock_layout_set_order("cfg", keys);
  g_list_free(keys);
  g_assert_true(scheme_true(
      "(equal? (get-layout-calls)"
      "        (list (list 'order \"cfg\" (list \"app:A\" \"stack:Downloads\"))))"));

  /* Stack view modes travel as symbols, and default to 'grid. */
  scheme_eval("(clear-layout-calls!)");
  dock_layout_set_view("cfg", "stack:Downloads", "list");
  dock_layout_set_view("cfg", "stack:Other", NULL);
  g_assert_true(scheme_true(
      "(equal? (get-layout-calls)"
      "        (list (list 'view \"cfg\" \"stack:Downloads\" 'list)"
      "              (list 'view \"cfg\" \"stack:Other\" 'grid)))"));

  /* No config path means no Scheme call at all. */
  scheme_eval("(clear-layout-calls!)");
  dock_layout_add_item(NULL, "app", "X", "x", "x", NULL);
  dock_layout_remove_item("cfg", NULL);
  dock_layout_set_view("", "k", "list");
  g_assert_true(scheme_true("(null? (get-layout-calls))"));
}

/* The poller contract: the dock stores an opaque handle, and the bridge turns
 * one call into a plain reading. A Scheme error is a missing reading, not a
 * crash; NULL is "no poller" and never reaches scm_*. */
static void test_poll_fn_bridge(void) {
  LambdockReading reading = {0};

  /* NULL is the dock's own "no poller" handle. */
  g_assert_null(lambdock_poll_fn_protect(NULL));
  g_assert_false(lambdock_poll_fn_is_callable(NULL));
  g_assert_false(lambdock_poll_fn_call(NULL, &reading));
  g_assert_cmpint(reading.kind, ==, LAMBDOCK_READING_NONE);
  lambdock_poll_fn_release(NULL); /* must not crash or unprotect anything */

  /* A number, a string, and an error. */
  SCM number_fn = scm_c_eval_string("(lambda () 42.5)");
  void *number = lambdock_poll_fn_protect((void *)number_fn);
  g_assert_nonnull(number);
  g_assert_true(lambdock_poll_fn_is_callable(number));

  g_assert_true(lambdock_poll_fn_call(number, &reading));
  g_assert_cmpint(reading.kind, ==, LAMBDOCK_READING_NUMBER);
  g_assert_cmpfloat_with_epsilon(reading.number, 42.5, 1e-9);
  g_assert_null(reading.text);

  SCM text_fn = scm_c_eval_string("(lambda () \"CPU 42%\")");
  void *text = lambdock_poll_fn_protect((void *)text_fn);
  g_assert_true(lambdock_poll_fn_call(text, &reading));
  g_assert_cmpint(reading.kind, ==, LAMBDOCK_READING_TEXT);
  g_assert_cmpstr(reading.text, ==, "CPU 42%");
  g_free(reading.text);

  SCM error_fn = scm_c_eval_string("(lambda () (error \"boom\"))");
  void *broken = lambdock_poll_fn_protect((void *)error_fn);
  g_assert_nonnull(broken);
  g_assert_false(lambdock_poll_fn_call(broken, &reading));
  g_assert_cmpint(reading.kind, ==, LAMBDOCK_READING_NONE);
  g_assert_null(reading.text);

  /* A value that is not a procedure is not a poller. */
  SCM not_fn = scm_c_eval_string("'not-a-procedure");
  g_assert_null(lambdock_poll_fn_protect((void *)not_fn));
  g_assert_false(lambdock_poll_fn_is_callable((void *)not_fn));

  lambdock_poll_fn_release(number);
  lambdock_poll_fn_release(text);
  lambdock_poll_fn_release(broken);

  /* Released handles are forgotten: the bridge no longer calls them. */
  g_assert_false(lambdock_poll_fn_call(broken, &reading));
  g_assert_cmpint(reading.kind, ==, LAMBDOCK_READING_NONE);
}

static void inner_test_guile_bridge(void *data, int argc, char **argv) {
  (void)data;

  g_test_init(&argc, &argv, NULL);
  scm_c_eval_string(FAKE_MODULES);

  test_boolean_getters();
  test_int_getters();
  test_launchpad_config_getters();
  test_string_getters();
  test_position_getter();
  test_is_app_excluded();
  test_reload_and_load_config();
  test_layout_bridge();
  test_poll_fn_bridge();

  if (gtk_init_check()) {
    test_populate_dock_items();
  } else {
    g_test_message("No display server: skipping populate_dock_items tests.");
  }

  g_print("# all guile_bridge assertions passed\n");
}

int main(int argc, char **argv) {
  scm_boot_guile(argc, argv, inner_test_guile_bridge, NULL);
  return 0;
}
