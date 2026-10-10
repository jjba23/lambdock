// lambdock

// Copyright © Josep Bigorra <jjbigorra@gmail.com>

// lambdock is free software : you can redistribute it and / or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.

// lambdock is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
// GNU General Public License for more details.

// You should have received a copy of the GNU General Public License
// along with lambdock. If not, see <https://www.gnu.org/licenses/>.

#include "../src/guile_bridge.h"
#include "../src/lambdock.h"
#include "../src/ui/dock_item.h"
#include <glib.h>
#include <gtk/gtk.h>
#include <libguile.h>

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
    "            get-dock-items\n"
    "            dock-item-type dock-item-name dock-item-icon dock-item-exec\n"
    "            dock-item-poll-fn dock-item-poll-interval\n"
    "            dock-item-hover-animate?\n"
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
    "(define (get-dock-items . a)\n"
    "  (list (vector 'app \"Good\" \"utilities-terminal\" \"good\" #f 0 #t)\n"
    "        (vector 'app \"bad-app\" \"utilities-terminal\" \"bad\" #f 0 #t)\n"
    "        (vector 'dynamic \"Dyn\" \"appointment-new\" \"dyn\"\n"
    "                (lambda () \"TICK\") 111 #f)\n"
    "        (vector 'separator \"\" \"\" \"\" #f 0 #f)))\n"
    "(define (dock-item-type v) (vector-ref v 0))\n"
    "(define (dock-item-name v) (vector-ref v 1))\n"
    "(define (dock-item-icon v) (vector-ref v 2))\n"
    "(define (dock-item-exec v) (vector-ref v 3))\n"
    "(define (dock-item-poll-fn v) (vector-ref v 4))\n"
    "(define (dock-item-poll-interval v) (vector-ref v 5))\n"
    "(define (dock-item-hover-animate? v) (vector-ref v 6))\n"
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

  /* The excluded "bad-app" launcher is skipped; app + dynamic + separator
   * remain, in order. */
  g_assert_cmpint(g_list_length(state.dock_items), ==, 3);

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

  DockItem *sep = (DockItem *)g_list_nth_data(state.dock_items, 2);
  g_assert_nonnull(sep);
  g_assert_true(GTK_IS_SEPARATOR(sep->button));
  g_assert_true(sep->is_favorite);

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

static void inner_test_guile_bridge(void *data, int argc, char **argv) {
  (void)data;

  g_test_init(&argc, &argv, NULL);
  scm_c_eval_string(FAKE_MODULES);

  test_boolean_getters();
  test_int_getters();
  test_string_getters();
  test_position_getter();
  test_is_app_excluded();
  test_reload_and_load_config();

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
