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

#include "preferences.h"

#include "../accessibility.h"
#include "../dock_window.h"
#include "../guile_bridge.h"
#include "../logger.h"
#include "dock_item.h"
#include "launchpad.h"
#include <glib/gi18n.h>

/* =========================================================================
 * PREFERENCES WINDOW
 * =========================================================================
 * A normal GTK4 window (not a layer shell surface) listing the settings people
 * usually change, grouped the way the settings file groups them:
 *
 *   - Behaviour: the switches that change how the dock acts.
 *   - Layout & material: size, spacing, edge, theme.
 *   - Glass shelf: the geometry knobs that shape the slab.
 *
 * Every control applies the same way the REPL does: it asks `(lambdock prefs)`
 * to run the ordinary `set-dock-*!' setter, which mutates the in-memory config
 * and triggers `(reload-dock!)`, and to remember the value in the GUI override
 * file. That keeps the generated settings file untouched.
 *
 * Sliders fire `value-changed' on every pixel, and each edit rebuilds the dock,
 * so the apply is debounced: the newest value is written once the control has
 * been still for APPLY_DEBOUNCE_MS.
 * ========================================================================= */

#define APPLY_DEBOUNCE_MS 140

/* Which kind of value a control carries. It decides how the debounced apply
 * reads the widget and which bridge call it makes. */
typedef enum {
  PREF_KIND_BOOL,
  PREF_KIND_INT,
  PREF_KIND_DOUBLE,
  PREF_KIND_SYMBOL,
} PrefKind;

/* One pending, debounced edit. The widget is referenced so it stays alive until
 * the timeout fires; the config path and key are copied so an edit can never
 * read freed window state. */
typedef struct {
  GtkWidget *widget;
  char *config_path;
  char *key;
  PrefKind kind;
} PrefApply;

/* Open preferences windows, keyed by the dock's settings path ("" = default). */
static GHashTable *g_prefs_windows = NULL;

static GtkWidget *build_content(const char *config_path);

static void pref_apply_free(gpointer data) {
  PrefApply *apply = data;
  if (!apply)
    return;
  g_object_unref(apply->widget);
  g_free(apply->config_path);
  g_free(apply->key);
  g_free(apply);
}

static gboolean pref_apply_now(gpointer data) {
  PrefApply *apply = data;

  /* The window may have been destroyed while the edit was pending: an
   * unparented widget has nothing left to apply to. */
  if (!gtk_widget_get_root(apply->widget)) {
    return G_SOURCE_REMOVE;
  }

  g_object_set_data(G_OBJECT(apply->widget), "lambdock-pref-timer", NULL);

  switch (apply->kind) {
  case PREF_KIND_BOOL:
    set_dock_setting_bool(apply->key, apply->config_path,
                          gtk_switch_get_active(GTK_SWITCH(apply->widget)));
    break;
  case PREF_KIND_INT:
    set_dock_setting_int(
        apply->key, apply->config_path,
        (int)llround(gtk_range_get_value(GTK_RANGE(apply->widget))));
    break;
  case PREF_KIND_DOUBLE:
    set_dock_setting_double(apply->key, apply->config_path,
                            gtk_range_get_value(GTK_RANGE(apply->widget)));
    break;
  case PREF_KIND_SYMBOL: {
    guint selected = gtk_drop_down_get_selected(GTK_DROP_DOWN(apply->widget));
    char **symbols =
        g_object_get_data(G_OBJECT(apply->widget), "lambdock-symbols");
    if (symbols && symbols[selected] != NULL)
      set_dock_setting_symbol(apply->key, apply->config_path, symbols[selected]);
    break;
  }
  }

  return G_SOURCE_REMOVE;
}

static void schedule_apply(GtkWidget *widget) {
  guint pending = GPOINTER_TO_UINT(
      g_object_get_data(G_OBJECT(widget), "lambdock-pref-timer"));
  if (pending != 0)
    g_source_remove(pending);

  PrefApply *apply = g_new0(PrefApply, 1);
  apply->widget = g_object_ref(widget);
  apply->config_path =
      g_strdup(g_object_get_data(G_OBJECT(widget), "lambdock-config-path"));
  apply->key =
      g_strdup(g_object_get_data(G_OBJECT(widget), "lambdock-key"));
  apply->kind = (PrefKind)GPOINTER_TO_INT(
      g_object_get_data(G_OBJECT(widget), "lambdock-kind"));

  guint id =
      g_timeout_add_full(G_PRIORITY_DEFAULT, APPLY_DEBOUNCE_MS, pref_apply_now,
                         apply, pref_apply_free);
  g_object_set_data(G_OBJECT(widget), "lambdock-pref-timer",
                    GUINT_TO_POINTER(id));
}

static void on_switch_changed(GObject *object, GParamSpec *pspec,
                              gpointer user_data) {
  (void)pspec;
  (void)user_data;
  schedule_apply(GTK_WIDGET(object));
}

static void on_scale_changed(GtkRange *range, gpointer user_data) {
  (void)user_data;
  schedule_apply(GTK_WIDGET(range));
}

/* Every numeric row carries a spin button next to its slider: the slider is for
 * feeling a value out, the spin button is for typing the exact one (a 53 px
 * icon, a 0.07 blur). They mirror each other, and the scale stays the keyed
 * control so the debounced apply path is exactly the same for both. */
static void on_spin_changed(GtkSpinButton *spin, gpointer user_data) {
  GtkRange *range = GTK_RANGE(user_data);
  double value = gtk_spin_button_get_value(spin);

  if (fabs(gtk_range_get_value(range) - value) > 1e-9)
    gtk_range_set_value(range, value);
}

static void on_scale_syncs_spin(GtkRange *range, gpointer user_data) {
  GtkSpinButton *spin = GTK_SPIN_BUTTON(user_data);
  double value = gtk_range_get_value(range);

  if (fabs(gtk_spin_button_get_value(spin) - value) > 1e-9)
    gtk_spin_button_set_value(spin, value);
}

static void on_dropdown_changed(GObject *object, GParamSpec *pspec,
                                gpointer user_data) {
  (void)pspec;
  (void)user_data;
  schedule_apply(GTK_WIDGET(object));
}

/* =========================================================================
 * WIDGET BUILDERS
 * ========================================================================= */

static void pref_control_init(GtkWidget *control, const char *config_path,
                              const char *key, PrefKind kind) {
  g_object_set_data_full(G_OBJECT(control), "lambdock-config-path",
                         g_strdup(config_path ? config_path : ""), g_free);
  g_object_set_data_full(G_OBJECT(control), "lambdock-key", g_strdup(key),
                         g_free);
  g_object_set_data(G_OBJECT(control), "lambdock-kind",
                    GINT_TO_POINTER((gint)kind));
}

static GtkWidget *row_new(const char *title, const char *subtitle,
                          GtkWidget *control) {
  GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 18);
  gtk_widget_set_margin_top(row, 5);
  gtk_widget_set_margin_bottom(row, 5);
  gtk_widget_add_css_class(row, "pref-row");

  GtkWidget *text = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
  gtk_widget_set_hexpand(text, TRUE);
  gtk_widget_set_valign(text, GTK_ALIGN_CENTER);

  GtkWidget *title_label = gtk_label_new(title);
  gtk_widget_set_halign(title_label, GTK_ALIGN_START);
  gtk_label_set_xalign(GTK_LABEL(title_label), 0.0f);
  gtk_box_append(GTK_BOX(text), title_label);

  if (subtitle) {
    GtkWidget *sub_label = gtk_label_new(subtitle);
    gtk_widget_set_halign(sub_label, GTK_ALIGN_START);
    gtk_label_set_xalign(GTK_LABEL(sub_label), 0.0f);
    gtk_label_set_wrap(GTK_LABEL(sub_label), TRUE);
    gtk_widget_add_css_class(sub_label, "dim-label");
    gtk_widget_add_css_class(sub_label, "caption");
    gtk_box_append(GTK_BOX(text), sub_label);
  }

  gtk_box_append(GTK_BOX(row), text);

  gtk_widget_set_valign(control, GTK_ALIGN_CENTER);
  gtk_box_append(GTK_BOX(row), control);
  return row;
}

/* A section is a rounded card: the title, an optional one-line explanation and
 * the rows it groups. Cards are what turn a long flat column of controls into
 * something that can be scanned, and the spacing between them does the rest. */
static GtkBox *section_new(GtkBox *parent, const char *title,
                           const char *subtitle) {
  GtkWidget *section = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
  gtk_widget_add_css_class(section, "pref-section");

  GtkWidget *heading = gtk_label_new(title);
  gtk_widget_set_halign(heading, GTK_ALIGN_START);
  gtk_label_set_xalign(GTK_LABEL(heading), 0.0f);
  gtk_widget_add_css_class(heading, "heading");
  gtk_box_append(GTK_BOX(section), heading);

  if (subtitle) {
    GtkWidget *sub = gtk_label_new(subtitle);
    gtk_widget_set_halign(sub, GTK_ALIGN_START);
    gtk_label_set_xalign(GTK_LABEL(sub), 0.0f);
    gtk_label_set_wrap(GTK_LABEL(sub), TRUE);
    gtk_widget_add_css_class(sub, "dim-label");
    gtk_widget_add_css_class(sub, "caption");
    gtk_widget_set_margin_bottom(sub, 4);
    gtk_box_append(GTK_BOX(section), sub);
  }

  gtk_box_append(parent, section);
  return GTK_BOX(section);
}

static void add_switch(GtkBox *box, const char *config_path, const char *key,
                       const char *title, const char *subtitle,
                       gboolean value) {
  GtkWidget *sw = gtk_switch_new();
  gtk_switch_set_active(GTK_SWITCH(sw), value);
  /* A bare GtkSwitch has no text of its own: without this a screen reader only
   * says "switch, off" and the user cannot tell which setting it belongs to. */
  lambdock_accessible_update(sw, title, subtitle);
  pref_control_init(sw, config_path, key, PREF_KIND_BOOL);
  g_signal_connect(sw, "notify::active", G_CALLBACK(on_switch_changed), NULL);
  gtk_box_append(box, row_new(title, subtitle, sw));
}

static void add_scale(GtkBox *box, const char *config_path, const char *key,
                      const char *title, const char *subtitle, double minimum,
                      double maximum, double step, double value,
                      PrefKind kind) {
  int digits = (kind == PREF_KIND_INT) ? 0 : 2;

  GtkWidget *control = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
  gtk_widget_set_valign(control, GTK_ALIGN_CENTER);

  GtkWidget *scale =
      gtk_scale_new_with_range(GTK_ORIENTATION_HORIZONTAL, minimum, maximum,
                               step);
  gtk_scale_set_digits(GTK_SCALE(scale), digits);
  gtk_scale_set_value_pos(GTK_SCALE(scale), GTK_POS_RIGHT);
  gtk_range_set_value(GTK_RANGE(scale), value);
  gtk_widget_set_size_request(scale, 180, -1);
  gtk_widget_set_valign(scale, GTK_ALIGN_CENTER);

  GtkWidget *spin = gtk_spin_button_new_with_range(minimum, maximum, step);
  gtk_spin_button_set_digits(GTK_SPIN_BUTTON(spin), digits);
  gtk_spin_button_set_numeric(GTK_SPIN_BUTTON(spin), TRUE);
  gtk_spin_button_set_increments(GTK_SPIN_BUTTON(spin), step,
                                 MAX(step, step * 10.0));
  gtk_editable_set_width_chars(GTK_EDITABLE(spin), digits > 0 ? 6 : 5);
  gtk_spin_button_set_value(GTK_SPIN_BUTTON(spin), value);
  gtk_widget_set_valign(spin, GTK_ALIGN_CENTER);
  gtk_widget_set_tooltip_text(spin, _("Type an exact value"));

  gtk_box_append(GTK_BOX(control), scale);
  gtk_box_append(GTK_BOX(control), spin);

  /* The slider and the exact-value field are one setting; both say which. */
  lambdock_accessible_update(scale, title, subtitle);
  char *spin_label = g_strdup_printf(_("%s, exact value"), title);
  lambdock_accessible_update(spin, spin_label,
                             _("Type a value, or use the arrow keys."));
  g_free(spin_label);

  pref_control_init(scale, config_path, key, kind);
  g_signal_connect(scale, "value-changed", G_CALLBACK(on_scale_changed), NULL);
  g_signal_connect(scale, "value-changed", G_CALLBACK(on_scale_syncs_spin),
                   spin);
  g_signal_connect(spin, "value-changed", G_CALLBACK(on_spin_changed), scale);

  gtk_box_append(box, row_new(title, subtitle, control));
}

static void free_symbols(gpointer data) {
  g_strfreev((char **)data);
}

static guint string_index(const char *const *values, const char *value) {
  if (!value)
    return 0;
  for (guint i = 0; values[i] != NULL; i++) {
    if (g_strcmp0(values[i], value) == 0)
      return i;
  }
  return 0;
}

static void add_symbol_dropdown(GtkBox *box, const char *config_path,
                                const char *key, const char *title,
                                const char *subtitle,
                                const char *const *labels,
                                const char *const *symbols, guint selected) {
  GtkStringList *list = gtk_string_list_new(labels);
  /* gtk_drop_down_new takes ownership of the model (transfer full). */
  GtkWidget *dropdown = gtk_drop_down_new(G_LIST_MODEL(list), NULL);

  guint count = 0;
  while (symbols[count] != NULL)
    count++;

  char **copy = g_new0(char *, count + 1);
  for (guint i = 0; i < count; i++)
    copy[i] = g_strdup(symbols[i]);
  g_object_set_data_full(G_OBJECT(dropdown), "lambdock-symbols", copy,
                         free_symbols);

  if (selected < count)
    gtk_drop_down_set_selected(GTK_DROP_DOWN(dropdown), selected);

  lambdock_accessible_update(dropdown, title, subtitle);
  pref_control_init(dropdown, config_path, key, PREF_KIND_SYMBOL);
  g_signal_connect(dropdown, "notify::selected",
                   G_CALLBACK(on_dropdown_changed), NULL);
  gtk_box_append(box, row_new(title, subtitle, dropdown));
}

/* =========================================================================
 * CONTENT
 * ========================================================================= */

static const char *const k_position_labels[] = {"Bottom", "Top", "Left",
                                                "Right", NULL};
static const char *const k_position_symbols[] = {"bottom", "top", "left",
                                                 "right", NULL};

static const char *const k_mode_labels[] = {"Dock", "Taskbar", NULL};
static const char *const k_mode_symbols[] = {"dock", "taskbar", NULL};

static const char *const k_theme_labels[] = {
    "Vanilla",          "Vanilla Light",     "Snow Leopard",
    "Snow Leopard Dark", "Solarized Light",  "Nature",
    "Unity",            "macOS Big Sur",     "macOS Ventura",
    "macOS Mavericks",  "macOS Mavericks Dark", "Windows XP",
    "Windows 10",       "Windows 11",        "None",
    NULL};

static const char *const k_theme_symbols[] = {
    "vanilla",          "vanilla-light",      "snow-leopard",
    "snow-leopard-dark", "solarized-light",   "nature",
    "unity",            "macos-big-sur",      "macos-ventura",
    "macos-mavericks",  "macos-mavericks-dark", "windows-xp",
    "windows-10",       "windows-11",         "none",
    NULL};

static guint position_index(GtkLayerShellEdge edge) {
  if (edge == GTK_LAYER_SHELL_EDGE_TOP)
    return 1;
  if (edge == GTK_LAYER_SHELL_EDGE_LEFT)
    return 2;
  if (edge == GTK_LAYER_SHELL_EDGE_RIGHT)
    return 3;
  return 0;
}

static GtkWidget *build_content(const char *config_path) {
  GtkWidget *content = gtk_box_new(GTK_ORIENTATION_VERTICAL, 18);
  gtk_widget_set_margin_start(content, 26);
  gtk_widget_set_margin_end(content, 26);
  gtk_widget_set_margin_top(content, 14);
  gtk_widget_set_margin_bottom(content, 26);
  GtkBox *box = GTK_BOX(content);

  LambdockShelfParams shelf;
  get_shelf_config_for(config_path, &shelf);

  /* --- Behaviour ------------------------------------------------------- */
  GtkBox *behaviour = section_new(
      box, _("Behaviour"),
      _("How the dock responds to windows, the pointer and the session."));

  add_switch(
      behaviour, config_path, "dock-auto-hide?", _("Auto-hide"),
      _("Slide the dock away when the pointer leaves it."),
      get_autohide_config_for(config_path));

  add_switch(
      behaviour, config_path, "dock-show-when-clear?", _("Show when no windows"),
      _("Reveal the dock when the last window is closed."),
      get_show_when_clear_config_for(config_path));

  add_switch(behaviour, config_path, "dock-hover-animate?",
             _("Hover magnification"), _("Grow the icon under the pointer."),
             get_hover_animate_config_for(config_path));

  add_switch(behaviour, config_path, "dock-track-windows?", _("Track windows"),
             _("Show running indicators and window menus."),
             get_track_windows_config_for(config_path));

  add_switch(
      behaviour, config_path, "dock-show-window-titles?", _("Window titles"),
      _("Show window titles on running taskbar buttons."),
      get_show_window_titles_config_for(config_path));

  add_symbol_dropdown(behaviour, config_path, "dock-mode", _("Window mode"),
                      _("A floating dock or a full-width taskbar."),
                      k_mode_labels, k_mode_symbols,
                      get_taskbar_config_for(config_path) ? 1 : 0);

  /* --- Accessibility ---------------------------------------------------- */
  GtkBox *accessibility = section_new(
      box, _("Accessibility"),
      _("Motion comfort. The desktop's own accessibility settings are "
        "honoured too."));

  add_switch(accessibility, config_path, "dock-reduce-motion?",
             _("Reduce motion"),
             _("Skip the dock's slide and hover magnification. Also follows "
               "the desktop's \"enable animations\" setting."),
             get_reduce_motion_config_for(config_path));

  /* --- Layout & material ----------------------------------------------- */
  GtkBox *material = section_new(
      box, _("Layout & material"),
      _("Size, spacing, screen edge and the material the dock is dressed in."));

  add_scale(material, config_path, "dock-icon-size", _("Icon size"),
            _("Pixel resolution of the launcher icons."), 16, 128, 2,
            get_icon_size_config_for(config_path), PREF_KIND_INT);

  add_scale(material, config_path, "dock-item-spacing", _("Item spacing"),
            _("Gap between icons, in pixels."), 0, 32, 1,
            get_item_spacing_config_for(config_path), PREF_KIND_INT);

  add_scale(material, config_path, "dock-capsule-padding", _("Capsule padding"),
            _("Room between the capsule's ends and the icons."), 0, 96, 1,
            get_capsule_padding_config_for(config_path), PREF_KIND_INT);

  add_symbol_dropdown(material, config_path, "dock-position", _("Screen edge"),
                      _("Which edge the dock is attached to."),
                      k_position_labels, k_position_symbols,
                      position_index(get_position_config_for(config_path)));

  char *theme = get_theme_config_for(config_path);
  add_symbol_dropdown(material, config_path, "dock-theme", _("Theme"),
                      _("The material the dock is dressed in."), k_theme_labels,
                      k_theme_symbols, string_index(k_theme_symbols, theme));
  free(theme);

  /* --- Launchpad -------------------------------------------------------- */
  GtkBox *launchpad = section_new(
      box, _("Launchpad"),
      _("How the full screen application grid looks when this dock opens it."));

  add_scale(launchpad, config_path, "dock-launchpad-icon-size",
            _("Icon size"),
            _("Pixel size of an application icon in the grid."),
            LAUNCHPAD_ICON_SIZE_MIN, LAUNCHPAD_ICON_SIZE_MAX, 2,
            get_launchpad_icon_size_config_for(config_path), PREF_KIND_INT);

  add_scale(launchpad, config_path, "dock-launchpad-columns", _("Columns"),
            _("Tiles per row; 0 fits as many as the screen allows."),
            0, LAUNCHPAD_COLUMNS_MAX, 1,
            get_launchpad_columns_config_for(config_path), PREF_KIND_INT);

  add_switch(launchpad, config_path, "dock-launchpad-blur?",
             _("Frosted background"),
             _("Translucent backdrop. Compositors that blur layer surfaces "
               "(KWin by default, niri with a layer-rule) frost it; others "
               "show a subtle tint."),
             get_launchpad_blur_config_for(config_path));

  /* --- Glass shelf ------------------------------------------------------ */
  GtkBox *glass = section_new(
      box, _("Glass shelf"),
      _("The 3D slab, its overhang and the reflections on it."));

  add_scale(glass, config_path, "dock-shelf-margin", _("Icon overhang"),
            _("Glass above the icons; negative lets them overhang the slab."),
            -200, 200, 1, shelf.margin_px, PREF_KIND_INT);

  add_scale(glass, config_path, "dock-shelf-front", _("Front glass"),
            _("Glass in front of the icons, where the mirrors live."), 0, 200,
            1, shelf.front_px, PREF_KIND_INT);

  add_scale(glass, config_path, "dock-reflection", _("Reflection height"),
            _("Mirror band as a fraction of the icon size (0 = none)."), 0, 2,
            0.05, shelf.reflection_ratio, PREF_KIND_DOUBLE);

  add_scale(glass, config_path, "dock-reflection-intensity",
            _("Reflection intensity"),
            _("Alpha of the mirror where it touches the icon."), 0, 1, 0.05,
            shelf.reflection_intensity, PREF_KIND_DOUBLE);

  add_scale(glass, config_path, "dock-reflection-blur", _("Reflection blur"),
            _("Blur radius as a fraction of the icon size."), 0, 0.25, 0.01,
            shelf.reflection_blur_ratio, PREF_KIND_DOUBLE);

  add_scale(glass, config_path, "dock-shelf-flare", _("Keystone flare"),
            _("How much wider the slab is at the screen edge, per side."), -80,
            80, 1, shelf.flare_px, PREF_KIND_INT);

  add_scale(glass, config_path, "dock-shelf-tilt", _("Shelf tilt"),
            _("Tilt away from the viewer, in degrees (0 = flat)."), 0,
            SHELF_TILT_MAX_DEG, 1, shelf.tilt_deg, PREF_KIND_DOUBLE);

  add_scale(glass, config_path, "dock-shelf-perspective", _("Shelf perspective"),
            _("Projection distance in pixels; smaller is a stronger 3D."),
            SHELF_PERSPECTIVE_MIN, SHELF_PERSPECTIVE_MAX, 10,
            shelf.perspective, PREF_KIND_DOUBLE);

  return content;
}

GtkWidget *lambdock_preferences_content_for(const char *config_path) {
  return build_content(config_path);
}

/* =========================================================================
 * WINDOW LIFECYCLE
 * ========================================================================= */

/* Escape closes the preferences window, as it does the launchpad and the
 * stack popovers. */
static gboolean on_prefs_key_pressed(GtkEventControllerKey *controller,
                                     guint keyval, guint keycode,
                                     GdkModifierType state,
                                     gpointer user_data) {
  (void)controller;
  (void)keycode;
  (void)state;

  if (keyval != GDK_KEY_Escape)
    return FALSE;

  gtk_window_close(GTK_WINDOW(user_data));
  return TRUE;
}

static void on_prefs_destroy(GtkWidget *window, gpointer user_data) {
  (void)user_data;
  const char *key =
      g_object_get_data(G_OBJECT(window), "lambdock-prefs-key");
  if (g_prefs_windows && key)
    g_hash_table_remove(g_prefs_windows, key);
}

static void on_reset_clicked(GtkButton *button, gpointer user_data) {
  (void)button;
  GtkWidget *window = GTK_WIDGET(user_data);
  const char *config_path =
      g_object_get_data(G_OBJECT(window), "lambdock-prefs-key");
  if (!config_path || *config_path == '\0')
    return;

  /* Forget the GUI overrides, reload the settings file on top of the now-empty
   * override, and redraw every dock bound to it. */
  reset_dock_preferences(config_path);
  load_dock_config_scm(config_path);

  for (GList *l = get_active_dock_states(); l != NULL; l = l->next) {
    LambdockState *state = (LambdockState *)l->data;
    if (state->config_path && g_strcmp0(state->config_path, config_path) == 0) {
      reload_dock_instance(state);
    }
  }

  GtkWidget *scrolled =
      g_object_get_data(G_OBJECT(window), "lambdock-prefs-scrolled");
  if (scrolled) {
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scrolled),
                                  build_content(config_path));
  }
}

static void prefs_window_show(GtkApplication *app, const char *config_path) {
  if (!g_prefs_windows) {
    g_prefs_windows =
        g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
  }

  const char *key = (config_path && *config_path != '\0') ? config_path : "";

  GtkWidget *existing = g_hash_table_lookup(g_prefs_windows, key);
  if (existing != NULL && GTK_IS_WINDOW(existing)) {
    gtk_window_present(GTK_WINDOW(existing));
    return;
  }

  GtkWidget *window = gtk_application_window_new(app);
  gtk_window_set_title(GTK_WINDOW(window), _("Lambdock Preferences"));
  gtk_window_set_default_size(GTK_WINDOW(window), 760, 860);
  gtk_window_set_hide_on_close(GTK_WINDOW(window), TRUE);
  gtk_widget_add_css_class(window, "lambdock-preferences");
  g_object_set_data_full(G_OBJECT(window), "lambdock-prefs-key", g_strdup(key),
                         g_free);

  GtkWidget *header = gtk_header_bar_new();
  GtkWidget *title = gtk_label_new(_("Lambdock Preferences"));
  gtk_widget_add_css_class(title, "title");
  gtk_header_bar_set_title_widget(GTK_HEADER_BAR(header), title);
  GtkWidget *reset = gtk_button_new_with_label(_("Reset"));
  gtk_widget_set_tooltip_text(
      reset, _("Forget the choices made here and go back to the settings file"));
  g_signal_connect(reset, "clicked", G_CALLBACK(on_reset_clicked), window);
  gtk_header_bar_pack_end(GTK_HEADER_BAR(header), reset);
  gtk_window_set_titlebar(GTK_WINDOW(window), header);

  GtkWidget *scrolled = gtk_scrolled_window_new();
  gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scrolled),
                                 GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
  gtk_scrolled_window_set_propagate_natural_width(
      GTK_SCROLLED_WINDOW(scrolled), TRUE);
  gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scrolled),
                                build_content(key));
  g_object_set_data(G_OBJECT(window), "lambdock-prefs-scrolled", scrolled);
  gtk_window_set_child(GTK_WINDOW(window), scrolled);

  GtkEventController *keys = gtk_event_controller_key_new();
  gtk_event_controller_set_propagation_phase(keys, GTK_PHASE_CAPTURE);
  g_signal_connect(keys, "key-pressed", G_CALLBACK(on_prefs_key_pressed),
                   window);
  gtk_widget_add_controller(window, keys);

  g_hash_table_insert(g_prefs_windows, g_strdup(key), window);
  g_signal_connect(window, "destroy", G_CALLBACK(on_prefs_destroy), NULL);

  gtk_window_present(GTK_WINDOW(window));
}

void lambdock_preferences_show(const char *config_path) {
  GtkApplication *app = GTK_APPLICATION(g_application_get_default());
  if (!app) {
    LOG_C_WARN("UI", "Cannot open the preferences without an application");
    return;
  }
  prefs_window_show(app, config_path);
}

void on_preferences_action(GSimpleAction *action, GVariant *parameter,
                           gpointer user_data) {
  (void)action;
  GtkApplication *app = GTK_APPLICATION(user_data);
  const char *config_path = NULL;

  if (parameter && g_variant_is_of_type(parameter, G_VARIANT_TYPE_STRING)) {
    config_path = g_variant_get_string(parameter, NULL);
  }

  prefs_window_show(app, config_path);
}

void lambdock_preferences_shutdown(void) {
  if (!g_prefs_windows)
    return;

  GList *windows = g_hash_table_get_values(g_prefs_windows);
  for (GList *l = windows; l != NULL; l = l->next) {
    if (l->data && GTK_IS_WINDOW(l->data)) {
      gtk_window_destroy(GTK_WINDOW(l->data));
    }
  }
  g_list_free(windows);

  g_hash_table_destroy(g_prefs_windows);
  g_prefs_windows = NULL;
}
