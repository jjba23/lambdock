// lambdock -- hyper-hackable desktop dock for Wayland
//
// SPDX-License-Identifier: MIT
// Copyright © Josep Bigorra <jjbigorra@gmail.com>

#include "preferences.h"

#include "../accessibility.h"
#include "../core/config_values.h"
#include "../dock_registry.h"
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
 * usually change, grouped the way the settings file groups them.
 *
 * Every control applies the same way the REPL does: it asks `(lambdock prefs)`
 * to run the ordinary `set-dock-*!' setter, which mutates the in-memory config
 * and triggers `(reload-dock!)`, and to remember the value in the GUI override
 * file. That keeps the generated settings file untouched.
 *
 * Sliders fire `value-changed' on every pixel and each edit rebuilds the dock,
 * so applying is debounced: the newest value is written once the control has
 * been still for APPLY_DEBOUNCE_MS.
 *
 * The file follows one order, and it is worth keeping it:
 *
 *   types      the model: what a control knows, what a row declares
 *   pure       helpers with no widgets and no I/O (paths, choices, formatting)
 *   choices    the vocabularies a dropdown can offer
 *   readers    settings file -> plain value (no widgets)
 *   tables     the form itself, as data
 *   builders   small constructors that turn one declared row into widgets
 *   state      the typed per-control state and the debounced apply path
 *   adapters   signal handlers that only adapt arguments
 *   content    the form walker
 *   windows    window state, the registry, callbacks and the public API
 * ========================================================================= */

#define APPLY_DEBOUNCE_MS 140

/* Widget data keys. `lambdock-key' is also the contract the unit test and any
 * tooling use to find a control by the option it edits. */
#define PREF_CONTROL_DATA "lambdock-pref-control"
#define PREF_KEY_DATA "lambdock-key"
#define PREF_WINDOW_DATA "lambdock-prefs-window"

/* =========================================================================
 * TYPES
 * ========================================================================= */

/* How a value is written back to the configuration. It also picks the writer in
 * PREF_WRITERS, so adding a kind is a row there plus (rarely) a new reader. */
typedef enum {
  PREF_KIND_BOOL,
  PREF_KIND_INT,
  PREF_KIND_DOUBLE,
  PREF_KIND_SYMBOL,
} PrefKind;

/**
 * PrefControl:
 * @config_path: owned settings path, "" for the default dock.
 * @key: owned option name, e.g. "dock-icon-size".
 * @kind: how to write the value back.
 * @symbols: (nullable): owned symbol list of a dropdown, NULL-terminated.
 * @timer_id: id of the pending debounced apply, 0 when none is queued.
 *
 * The state of one control, attached to its widget. Everything except
 * @timer_id is written once at build time; grouping the mutable part here is
 * what removes the `GINT_TO_POINTER' juggling that used to be spread over four
 * `g_object_set_data' keys.
 */
typedef struct {
  char *config_path;
  char *key;
  PrefKind kind;
  char **symbols;
  guint timer_id;
} PrefControl;

/* A dropdown's choices: label for the user, symbol for the settings file.
 * Labels are plain literals, exactly as they were when this form was written as
 * inline code; marking them translatable would add new msgids to the catalogues
 * and is a separate change from this refactor. */
typedef struct {
  const char *label;
  const char *symbol;
} PrefChoice;

/* Readers answer "what is the value right now?" from the settings file. They
 * touch no widgets, which is what makes the table below readable as data. */
typedef gboolean (*PrefReadSwitch)(const char *config_path);
typedef double (*PrefReadNumber)(const char *config_path);
typedef guint (*PrefReadChoice)(const char *config_path);

/* One declared row of the form. */
typedef struct {
  const char *key;
  const char *title;    /* untranslated */
  const char *subtitle; /* untranslated */
  PrefReadSwitch read;
} PrefSwitchSpec;

typedef struct {
  const char *key;
  const char *title;
  const char *subtitle;
  PrefKind kind;
  double minimum;
  double maximum;
  double step;
  PrefReadNumber read;
} PrefScaleSpec;

typedef struct {
  const char *key;
  const char *title;
  const char *subtitle;
  const PrefChoice *choices; /* NULL-terminated (last .label == NULL) */
  PrefReadChoice read;
} PrefChoiceSpec;

/* An action row is a dropdown (what to act on) and a button (do it). It edits
 * no setting: it runs a command, which is what "add a widget to the dock"
 * is. The dropdown still carries a PrefControl, so it is keyed like every other
 * control and the row can read the selected symbol the same way. */
typedef struct {
  const char *key;
  const char *title;
  const char *subtitle;
  const char *button_label; /* untranslated */
  const PrefChoice *choices;
} PrefActionSpec;

/* One card of the form: its heading, its explanation and its rows. */
typedef struct {
  const char *title;
  const char *subtitle;
  const PrefSwitchSpec *switches;
  guint n_switches;
  const PrefScaleSpec *scales;
  guint n_scales;
  const PrefChoiceSpec *choices;
  guint n_choices;
  const PrefActionSpec *actions;
  guint n_actions;
} PrefSection;

/* What a preferences window knows about itself. */
typedef struct {
  char *config_path;  /* owned identity of the dock being edited */
  GtkWidget *scrolled; /* borrowed: the viewport whose child is rebuilt */
} PrefsWindow;

/* The one piece of process-wide state: the open windows, keyed by the settings
 * path they edit ("" for the default dock). */
typedef struct {
  GHashTable *windows; /* key (owned char*) -> GtkWindow* (borrowed) */
} PrefsRegistry;

static PrefsRegistry g_prefs_registry;

/* =========================================================================
 * PURE HELPERS (no widgets, no I/O)
 * ========================================================================= */

/** The settings path to use, normalising "no dock" to the default dock. */
static const char *config_path_or_default(const char *config_path) {
  return (config_path && *config_path != '\0') ? config_path : "";
}

/** TRUE when @config_path names the default dock. */
static gboolean config_path_is_default(const char *config_path) {
  return *config_path_or_default(config_path) == '\0';
}

/** How many digits a numeric control shows: integers show none. */
static int digits_for(PrefKind kind) {
  return kind == PREF_KIND_INT ? 0 : 2;
}

/** Number of entries in a NULL-terminated choice table. */
static guint choices_count(const PrefChoice *choices) {
  guint count = 0;

  while (choices && choices[count].label != NULL)
    count++;

  return count;
}

/** Index of @symbol in @choices, or 0 (a safe default) when it is absent. */
static guint choice_index_of(const PrefChoice *choices, const char *symbol) {
  if (!choices || !symbol)
    return 0;

  for (guint i = 0; choices[i].label != NULL; i++) {
    if (g_strcmp0(choices[i].symbol, symbol) == 0)
      return i;
  }

  return 0;
}

/** The labels of @choices as a NULL-terminated array for GtkStringList. */
static char **choice_labels_new(const PrefChoice *choices) {
  guint count = choices_count(choices);
  char **labels = g_new0(char *, count + 1);

  for (guint i = 0; i < count; i++)
    labels[i] = (char *)choices[i].label;

  return labels;
}

/* =========================================================================
 * CHOICES (the vocabularies a dropdown can offer)
 * ========================================================================= */

static const PrefChoice k_theme_choices[] = {
    {"Vanilla", "vanilla"},
    {"Vanilla Light", "vanilla-light"},
    {"Snow Leopard", "snow-leopard"},
    {"Snow Leopard Dark", "snow-leopard-dark"},
    {"Solarized Light", "solarized-light"},
    {"Nature", "nature"},
    {"Unity", "unity"},
    {"macOS Tiger", "macos-tiger"},
    {"macOS Big Sur", "macos-big-sur"},
    {"macOS Ventura", "macos-ventura"},
    {"macOS Tahoe", "macos-tahoe"},
    {"macOS Tahoe Dark", "macos-tahoe-dark"},
    {"macOS Mavericks", "macos-mavericks"},
    {"macOS Mavericks Dark", "macos-mavericks-dark"},
    {"Windows XP", "windows-xp"},
    {"Windows Vista", "windows-vista"},
    {"Windows 7", "windows-7"},
    {"Windows 10", "windows-10"},
    {"Windows 11", "windows-11"},
    {"None", "none"},
    {NULL, NULL},
};

static const PrefChoice k_position_choices[] = {
    {"Bottom", "bottom"},
    {"Top", "top"},
    {"Left", "left"},
    {"Right", "right"},
    {NULL, NULL},
};

static const PrefChoice k_mode_choices[] = {
    {"Dock", "dock"},
    {"Taskbar", "taskbar"},
    {NULL, NULL},
};

static const PrefChoice k_launchpad_close_side_choices[] = {
    {"Left", "left"},
    {"Right", "right"},
    {NULL, NULL},
};

/* What the "Add an item" row can put on the dock: the ready-made widgets of
 * (lambdock presets), by preset name. */
static const PrefChoice k_add_item_choices[] = {
    {"Trash", "trash"},
    {"Clock", "clock"},
    {"RAM usage", "ram"},
    {"CPU usage", "cpu"},
    {"CPU temperature", "cpu-temp"},
    {"Battery", "battery"},
    {NULL, NULL},
};

/* =========================================================================
 * READERS (settings file -> plain value; no widgets)
 * ========================================================================= */

/* The shelf is read as one struct, so each of its rows needs a small adapter.
 * Keeping them here means the tables stay uniform: every scale row has a
 * `double read(const char *)`, whether the setting lives in a struct or has a
 * getter of its own. */
#define DOCK_SHELF_READER(name, field)                                        \
  static double name(const char *config_path) {                               \
    LambdockShelfParams shelf;                                                \
    get_shelf_config_for(config_path, &shelf);                                \
    return (double)shelf.field;                                               \
  }

#define DOCK_INT_READER(name, getter)                                         \
  static double name(const char *config_path) {                               \
    return (double)getter(config_path);                                       \
  }

DOCK_SHELF_READER(read_shelf_margin, margin_px)
DOCK_SHELF_READER(read_shelf_front, front_px)
DOCK_SHELF_READER(read_shelf_tilt, tilt_deg)
DOCK_SHELF_READER(read_shelf_flare, flare_px)
DOCK_SHELF_READER(read_shelf_perspective, perspective)
DOCK_SHELF_READER(read_shelf_reflection, reflection_ratio)
DOCK_SHELF_READER(read_shelf_reflection_intensity, reflection_intensity)
DOCK_SHELF_READER(read_shelf_reflection_blur, reflection_blur_ratio)

DOCK_INT_READER(read_icon_size, get_icon_size_config_for)
DOCK_INT_READER(read_item_spacing, get_item_spacing_config_for)
DOCK_INT_READER(read_capsule_padding, get_capsule_padding_config_for)
DOCK_INT_READER(read_launchpad_icon_size, get_launchpad_icon_size_config_for)
DOCK_INT_READER(read_launchpad_columns, get_launchpad_columns_config_for)

/* Choice readers: the selected row of a dropdown. Each one maps the settings
 * vocabulary onto the dropdown's, so the tables can just state a row. */
static guint read_position_choice(const char *config_path) {
  GtkLayerShellEdge edge = get_position_config_for(config_path);

  return choice_index_of(k_position_choices, lambdock_edge_name(edge));
}

static guint read_mode_choice(const char *config_path) {
  return get_taskbar_config_for(config_path) ? 1 : 0;
}

static guint read_launchpad_close_side_choice(const char *config_path) {
  return get_launchpad_close_on_right_config_for(config_path) ? 1 : 0;
}

static guint read_theme_choice(const char *config_path) {
  g_autofree char *theme = get_theme_config_for(config_path);

  return choice_index_of(k_theme_choices, theme);
}

/* =========================================================================
 * THE FORM, AS DATA
 * ========================================================================= */

static const PrefSwitchSpec k_behaviour_switches[] = {
    {"dock-auto-hide?", N_("Auto-hide"),
     N_("Slide the dock away when the pointer leaves it."),
     get_autohide_config_for},
    {"dock-show-when-clear?", N_("Show when no windows"),
     N_("Reveal the dock when the last window is closed."),
     get_show_when_clear_config_for},
    {"dock-hover-animate?", N_("Hover magnification"),
     N_("Grow the icon under the pointer."), get_hover_animate_config_for},
    {"dock-track-windows?", N_("Track windows"),
     N_("Show running indicators and window menus."),
     get_track_windows_config_for},
    {"dock-show-window-titles?", N_("Window titles"),
     N_("Show window titles on running taskbar buttons."),
     get_show_window_titles_config_for},
};

static const PrefChoiceSpec k_behaviour_choices[] = {
    {"dock-mode", N_("Window mode"),
     N_("A floating dock or a full-width taskbar."), k_mode_choices,
     read_mode_choice},
};

static const PrefSwitchSpec k_accessibility_switches[] = {
    {"dock-reduce-motion?", N_("Reduce motion"),
     N_("Skip the dock's slide and hover magnification. Also follows the "
        "desktop's \"enable animations\" setting."),
     get_reduce_motion_config_for},
};

static const PrefScaleSpec k_material_scales[] = {
    {"dock-icon-size", N_("Icon size"),
     N_("Pixel resolution of the launcher icons."), PREF_KIND_INT, 16, 128, 2,
     read_icon_size},
    {"dock-item-spacing", N_("Item spacing"),
     N_("Gap between icons, in pixels."), PREF_KIND_INT, 0, 32, 1,
     read_item_spacing},
    {"dock-capsule-padding", N_("Capsule padding"),
     N_("Room between the capsule's ends and the icons."), PREF_KIND_INT, 0, 96,
     1, read_capsule_padding},
};

static const PrefChoiceSpec k_material_choices[] = {
    {"dock-position", N_("Screen edge"),
     N_("Which edge the dock is attached to."), k_position_choices,
     read_position_choice},
    {"dock-theme", N_("Theme"), N_("The material the dock is dressed in."),
     k_theme_choices, read_theme_choice},
};

static const PrefScaleSpec k_launchpad_scales[] = {
    {"dock-launchpad-icon-size", N_("Icon size"),
     N_("Pixel size of an application icon in the grid."), PREF_KIND_INT,
     LAUNCHPAD_ICON_SIZE_MIN, LAUNCHPAD_ICON_SIZE_MAX, 2,
     read_launchpad_icon_size},
    {"dock-launchpad-columns", N_("Columns"),
     N_("Tiles per row; 0 fits as many as the screen allows."), PREF_KIND_INT,
     0, LAUNCHPAD_COLUMNS_MAX, 1, read_launchpad_columns},
};

static const PrefSwitchSpec k_launchpad_switches[] = {
    {"dock-launchpad-blur?", N_("Frosted background"),
     N_("Translucent backdrop. Compositors that blur layer surfaces (KWin by "
        "default, niri with a layer-rule) frost it; the tint is dense enough "
        "to read the grid without one."),
     get_launchpad_blur_config_for},
};

static const PrefScaleSpec k_shelf_scales[] = {
    {"dock-shelf-margin", N_("Icon overhang"),
     N_("Glass above the icons; negative lets them overhang the slab."),
     PREF_KIND_INT, -200, 200, 1, read_shelf_margin},
    {"dock-shelf-front", N_("Front glass"),
     N_("Glass in front of the icons, where the mirrors live."), PREF_KIND_INT,
     0, 200, 1, read_shelf_front},
    {"dock-reflection", N_("Reflection height"),
     N_("Mirror band as a fraction of the icon size (0 = none)."),
     PREF_KIND_DOUBLE, 0, 2, 0.05, read_shelf_reflection},
    {"dock-reflection-intensity", N_("Reflection intensity"),
     N_("Alpha of the mirror where it touches the icon."), PREF_KIND_DOUBLE, 0,
     1, 0.05, read_shelf_reflection_intensity},
    {"dock-reflection-blur", N_("Reflection blur"),
     N_("Blur radius as a fraction of the icon size."), PREF_KIND_DOUBLE, 0,
     0.25, 0.01, read_shelf_reflection_blur},
    {"dock-shelf-flare", N_("Keystone flare"),
     N_("How much wider the slab is at the screen edge, per side."),
     PREF_KIND_INT, -80, 80, 1, read_shelf_flare},
    {"dock-shelf-tilt", N_("Shelf tilt"),
     N_("Tilt away from the viewer, in degrees (0 = flat)."), PREF_KIND_DOUBLE,
     0, SHELF_TILT_MAX_DEG, 1, read_shelf_tilt},
    {"dock-shelf-perspective", N_("Shelf perspective"),
     N_("Projection distance in pixels; smaller is a stronger 3D."),
     PREF_KIND_DOUBLE, SHELF_PERSPECTIVE_MIN, SHELF_PERSPECTIVE_MAX, 10,
     read_shelf_perspective},
};

/* Each section lists whatever kinds of row it has; the macros keep the pair
 * (array, count) impossible to get out of step. */
#define PREF_SWITCHES(rows) rows, G_N_ELEMENTS(rows)
#define PREF_SCALES(rows) rows, G_N_ELEMENTS(rows)
#define PREF_CHOICES(rows) rows, G_N_ELEMENTS(rows)
#define PREF_ACTIONS(rows) rows, G_N_ELEMENTS(rows)
#define PREF_NO_SWITCHES NULL, 0
#define PREF_NO_SCALES NULL, 0
#define PREF_NO_CHOICES NULL, 0
#define PREF_NO_ACTIONS NULL, 0

static const PrefActionSpec k_dock_item_actions[] = {
    {"dock-add-item", N_("Add an item"),
     N_("Puts a trash can, a clock or a live meter on the dock. The item is "
        "appended and remembered, like the ones you drag in; right-click it in "
        "the dock to remove it again."),
     N_("Add"), k_add_item_choices},
};

static const PrefChoiceSpec k_launchpad_choices[] = {
    {"dock-launchpad-close-side", N_("Close button"),
     N_("The end of the launchpad's header the close button sits on."),
     k_launchpad_close_side_choices, read_launchpad_close_side_choice},
};

static const PrefSection k_sections[] = {
    {N_("Behaviour"),
     N_("How the dock responds to windows, the pointer and the session."),
     PREF_SWITCHES(k_behaviour_switches), PREF_NO_SCALES,
     PREF_CHOICES(k_behaviour_choices), PREF_NO_ACTIONS},
    {N_("Dock items"),
     N_("Put a widget on this dock. Nothing is installed or uninstalled: the "
        "item is added to the dock and remembered with its layout."),
     PREF_NO_SWITCHES, PREF_NO_SCALES, PREF_NO_CHOICES,
     PREF_ACTIONS(k_dock_item_actions)},
    {N_("Accessibility"),
     N_("Motion comfort. The desktop's own accessibility settings are honoured "
        "too."),
     PREF_SWITCHES(k_accessibility_switches), PREF_NO_SCALES,
     PREF_NO_CHOICES, PREF_NO_ACTIONS},
    {N_("Layout & material"),
     N_("Size, spacing, screen edge and the material the dock is dressed in."),
     PREF_NO_SWITCHES, PREF_SCALES(k_material_scales),
     PREF_CHOICES(k_material_choices), PREF_NO_ACTIONS},
    {N_("Launchpad"),
     N_("How the full screen application grid looks when this dock opens it."),
     PREF_SWITCHES(k_launchpad_switches), PREF_SCALES(k_launchpad_scales),
     PREF_CHOICES(k_launchpad_choices), PREF_NO_ACTIONS},
    {N_("Glass shelf"),
     N_("The 3D slab, its overhang and the reflections on it."),
     PREF_NO_SWITCHES,
     PREF_SCALES(k_shelf_scales), PREF_NO_CHOICES, PREF_NO_ACTIONS},
};

/* =========================================================================
 * SMALL WIDGET CONSTRUCTORS
 * ========================================================================= */

/** A left-aligned label: the default alignment is centred, which reads wrong
 * in a form. */
static GtkWidget *make_label(const char *text) {
  GtkWidget *label = gtk_label_new(text);

  gtk_widget_set_halign(label, GTK_ALIGN_START);
  gtk_label_set_xalign(GTK_LABEL(label), 0.0f);

  return label;
}

/** The smaller, dimmer second line of a row or card. */
static GtkWidget *make_caption(const char *text) {
  GtkWidget *label = make_label(text);

  gtk_label_set_wrap(GTK_LABEL(label), TRUE);
  gtk_widget_add_css_class(label, "dim-label");
  gtk_widget_add_css_class(label, "caption");

  return label;
}

/** A row: title (and explanation) on the left, control on the right. */
static GtkWidget *make_row(const char *title, const char *subtitle,
                           GtkWidget *control) {
  GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 18);
  gtk_widget_set_margin_top(row, 5);
  gtk_widget_set_margin_bottom(row, 5);
  gtk_widget_add_css_class(row, "pref-row");

  GtkWidget *text = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
  gtk_widget_set_hexpand(text, TRUE);
  gtk_widget_set_valign(text, GTK_ALIGN_CENTER);
  gtk_box_append(GTK_BOX(text), make_label(title));
  if (subtitle)
    gtk_box_append(GTK_BOX(text), make_caption(subtitle));
  gtk_box_append(GTK_BOX(row), text);

  gtk_widget_set_valign(control, GTK_ALIGN_CENTER);
  gtk_box_append(GTK_BOX(row), control);

  return row;
}

/* A section is a rounded card: the title, an optional explanation and the rows
 * it groups. Cards turn a long flat column of controls into something that can
 * be scanned. */
static GtkWidget *make_card(const char *title, const char *subtitle) {
  GtkWidget *card = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
  gtk_widget_add_css_class(card, "pref-section");

  GtkWidget *heading = make_label(title);
  gtk_widget_add_css_class(heading, "heading");
  gtk_box_append(GTK_BOX(card), heading);

  if (subtitle) {
    GtkWidget *caption = make_caption(subtitle);
    gtk_widget_set_margin_bottom(caption, 4);
    gtk_box_append(GTK_BOX(card), caption);
  }

  return card;
}

/* =========================================================================
 * CONTROL STATE
 * ========================================================================= */

static void pref_control_free(PrefControl *control) {
  if (!control)
    return;

  g_clear_handle_id(&control->timer_id, g_source_remove);
  g_free(control->config_path);
  g_free(control->key);
  g_strfreev(control->symbols);
  g_free(control);
}

/* Lets each row builder construct a control with `g_autoptr' and hand it to the
 * widget with `g_steal_pointer', so ownership transfer is visible on the line
 * that does it. */
G_DEFINE_AUTOPTR_CLEANUP_FUNC(PrefControl, pref_control_free)

static PrefControl *pref_control_new(const char *config_path, const char *key,
                                     PrefKind kind) {
  PrefControl *control = g_new0(PrefControl, 1);

  control->config_path = g_strdup(config_path_or_default(config_path));
  control->key = g_strdup(key);
  control->kind = kind;

  return control;
}

/** Attaches @control to @widget; the widget owns it from now on. */
static void pref_control_attach(GtkWidget *widget, PrefControl *control) {
  g_object_set_data_full(G_OBJECT(widget), PREF_CONTROL_DATA, control,
                         (GDestroyNotify)pref_control_free);

  /* Borrowed pointer into @control: the documented way for tests and tooling to
   * find a control by the option it edits. */
  g_object_set_data(G_OBJECT(widget), PREF_KEY_DATA, control->key);
}

/** The state attached to @widget, or NULL for a widget that has none. */
static PrefControl *pref_control_of(GtkWidget *widget) {
  return widget ? g_object_get_data(G_OBJECT(widget), PREF_CONTROL_DATA) : NULL;
}

/** The symbol of the selected row of a dropdown control. */
static const char *pref_control_selected_symbol(const PrefControl *control,
                                                guint selected) {
  if (!control || !control->symbols)
    return NULL;

  return control->symbols[selected];
}

/* =========================================================================
 * APPLYING A VALUE (widget -> setter)
 * ========================================================================= */

/* One writer per PrefKind. Each reads its widget and calls the matching bridge
 * setter; that is the whole side effect of an edit. */
static void write_bool(const PrefControl *control, GtkWidget *widget) {
  set_dock_setting_bool(control->key, control->config_path,
                        gtk_switch_get_active(GTK_SWITCH(widget)));
}

static void write_int(const PrefControl *control, GtkWidget *widget) {
  set_dock_setting_int(
      control->key, control->config_path,
      (int)llround(gtk_range_get_value(GTK_RANGE(widget))));
}

static void write_double(const PrefControl *control, GtkWidget *widget) {
  set_dock_setting_double(control->key, control->config_path,
                          gtk_range_get_value(GTK_RANGE(widget)));
}

static void write_symbol(const PrefControl *control, GtkWidget *widget) {
  guint selected = gtk_drop_down_get_selected(GTK_DROP_DOWN(widget));
  const char *symbol = pref_control_selected_symbol(control, selected);

  if (symbol)
    set_dock_setting_symbol(control->key, control->config_path, symbol);
}

typedef void (*PrefWriteFunc)(const PrefControl *control, GtkWidget *widget);

static const PrefWriteFunc PREF_WRITERS[] = {
    [PREF_KIND_BOOL] = write_bool,
    [PREF_KIND_INT] = write_int,
    [PREF_KIND_DOUBLE] = write_double,
    [PREF_KIND_SYMBOL] = write_symbol,
};

/** Writes @widget's value through @control, if both are still usable. */
static void pref_apply_value(PrefControl *control, GtkWidget *widget) {
  gsize index = (gsize)control->kind;

  if (index < G_N_ELEMENTS(PREF_WRITERS) && PREF_WRITERS[index])
    PREF_WRITERS[index](control, widget);
}

/* The timeout's data is the widget itself, held by a reference that the source
 * drops when it fires or is removed: the control and its state therefore live
 * at least as long as the timer, without a separate payload struct. */
static gboolean on_apply_timeout(gpointer data) {
  GtkWidget *widget = data;
  PrefControl *control = pref_control_of(widget);

  /* The source is gone once we return, so forget its id first: teardown must
   * never try to remove a dead (possibly recycled) source id. */
  if (control)
    control->timer_id = 0;

  /* The window may have been destroyed while the edit was pending: an
   * unparented widget has nothing left to apply to. */
  if (!control || !gtk_widget_get_root(widget))
    return G_SOURCE_REMOVE;

  pref_apply_value(control, widget);
  return G_SOURCE_REMOVE;
}

/** Queues the control's value to be written once editing settles. */
static void prefs_schedule_apply(GtkWidget *widget) {
  PrefControl *control = pref_control_of(widget);
  if (!control)
    return;

  g_clear_handle_id(&control->timer_id, g_source_remove);

  control->timer_id = g_timeout_add_full(
      G_PRIORITY_DEFAULT, APPLY_DEBOUNCE_MS, on_apply_timeout,
      g_object_ref(widget), g_object_unref);
}

/* =========================================================================
 * SIGNAL ADAPTERS
 * =========================================================================
 * These exist only to turn GTK's callback arguments into "this control
 * changed"; the work happens in prefs_schedule_apply().
 */

static void on_switch_changed(GObject *object, GParamSpec *pspec,
                              gpointer user_data) {
  (void)pspec;
  (void)user_data;
  prefs_schedule_apply(GTK_WIDGET(object));
}

static void on_range_changed(GtkRange *range, gpointer user_data) {
  (void)user_data;
  prefs_schedule_apply(GTK_WIDGET(range));
}

static void on_dropdown_changed(GObject *object, GParamSpec *pspec,
                                gpointer user_data) {
  (void)pspec;
  (void)user_data;
  prefs_schedule_apply(GTK_WIDGET(object));
}

/* A numeric row carries a spin button beside its slider: the slider is for
 * feeling a value out, the spin button for typing the exact one. They mirror
 * each other, and the slider stays the keyed control so both go through the
 * same debounced path. */
static gboolean values_differ(double a, double b) {
  return fabs(a - b) > 1e-9;
}

static void on_spin_changed(GtkSpinButton *spin, gpointer user_data) {
  GtkRange *range = GTK_RANGE(user_data);
  double value = gtk_spin_button_get_value(spin);

  if (values_differ(gtk_range_get_value(range), value))
    gtk_range_set_value(range, value);
}

static void on_range_syncs_spin(GtkRange *range, gpointer user_data) {
  GtkSpinButton *spin = GTK_SPIN_BUTTON(user_data);
  double value = gtk_range_get_value(range);

  if (values_differ(gtk_spin_button_get_value(spin), value))
    gtk_spin_button_set_value(spin, value);
}

/* =========================================================================
 * ROW BUILDERS (declared row -> widgets)
 * ========================================================================= */

static void append_switch_row(GtkBox *box, const char *config_path,
                              const PrefSwitchSpec *spec) {
  const char *title = gettext(spec->title);
  const char *subtitle = gettext(spec->subtitle);

  GtkWidget *sw = gtk_switch_new();
  gtk_switch_set_active(GTK_SWITCH(sw), spec->read(config_path));

  /* A bare GtkSwitch has no text of its own: without this a screen reader only
   * says "switch, off" and the user cannot tell which setting it belongs to. */
  lambdock_accessible_update(sw, title, subtitle);

  g_autoptr(PrefControl) control =
      pref_control_new(config_path, spec->key, PREF_KIND_BOOL);
  pref_control_attach(sw, g_steal_pointer(&control));

  g_signal_connect(sw, "notify::active", G_CALLBACK(on_switch_changed), NULL);

  gtk_box_append(box, make_row(title, subtitle, sw));
}

/**
 * make_range_control:
 * @spec: the declared row.
 * @config_path: settings file this control edits.
 * @value: the value to show.
 * @title: (already translated) label used for accessibility.
 * @subtitle: (already translated) explanation used for accessibility.
 *
 * Returns: a box holding the slider and its exact-value field. The *slider* is
 * the widget that carries the control state, so both inputs go through the same
 * debounced apply.
 */
static GtkWidget *make_range_control(const PrefScaleSpec *spec,
                                     const char *config_path, double value,
                                     const char *title, const char *subtitle) {
  int digits = digits_for(spec->kind);

  GtkWidget *control = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
  gtk_widget_set_valign(control, GTK_ALIGN_CENTER);

  GtkWidget *scale = gtk_scale_new_with_range(GTK_ORIENTATION_HORIZONTAL,
                                              spec->minimum, spec->maximum,
                                              spec->step);
  gtk_scale_set_digits(GTK_SCALE(scale), digits);
  gtk_scale_set_value_pos(GTK_SCALE(scale), GTK_POS_RIGHT);
  gtk_range_set_value(GTK_RANGE(scale), value);
  gtk_widget_set_size_request(scale, 180, -1);
  gtk_widget_set_valign(scale, GTK_ALIGN_CENTER);

  GtkWidget *spin =
      gtk_spin_button_new_with_range(spec->minimum, spec->maximum, spec->step);
  gtk_spin_button_set_digits(GTK_SPIN_BUTTON(spin), digits);
  gtk_spin_button_set_numeric(GTK_SPIN_BUTTON(spin), TRUE);
  gtk_spin_button_set_increments(GTK_SPIN_BUTTON(spin), spec->step,
                                 MAX(spec->step, spec->step * 10.0));
  gtk_editable_set_width_chars(GTK_EDITABLE(spin), digits > 0 ? 6 : 5);
  gtk_spin_button_set_value(GTK_SPIN_BUTTON(spin), value);
  gtk_widget_set_valign(spin, GTK_ALIGN_CENTER);
  gtk_widget_set_tooltip_text(spin, _("Type an exact value"));

  /* The slider and the field are one setting; both say which. */
  lambdock_accessible_update(scale, title, subtitle);
  g_autofree char *spin_title = g_strdup_printf(_("%s, exact value"), title);
  lambdock_accessible_update(spin, spin_title,
                             _("Type a value, or use the arrow keys."));

  g_signal_connect(scale, "value-changed", G_CALLBACK(on_range_changed), NULL);
  g_signal_connect(scale, "value-changed", G_CALLBACK(on_range_syncs_spin),
                   spin);
  g_signal_connect(spin, "value-changed", G_CALLBACK(on_spin_changed), scale);

  g_autoptr(PrefControl) state =
      pref_control_new(config_path, spec->key, spec->kind);
  pref_control_attach(scale, g_steal_pointer(&state));

  gtk_box_append(GTK_BOX(control), scale);
  gtk_box_append(GTK_BOX(control), spin);

  return control;
}

static void append_scale_row(GtkBox *box, const char *config_path,
                             const PrefScaleSpec *spec) {
  const char *title = gettext(spec->title);
  const char *subtitle = gettext(spec->subtitle);

  GtkWidget *control = make_range_control(spec, config_path,
                                          spec->read(config_path), title,
                                          subtitle);

  gtk_box_append(box, make_row(title, subtitle, control));
}

static void append_choice_row(GtkBox *box, const char *config_path,
                              const PrefChoiceSpec *spec) {  const char *title = gettext(spec->title);
  const char *subtitle = gettext(spec->subtitle);

  guint count = choices_count(spec->choices);
  g_autofree char **labels = choice_labels_new(spec->choices);

  /* gtk_drop_down_new takes ownership of the model. */
  GtkStringList *list = gtk_string_list_new((const char *const *)labels);
  GtkWidget *dropdown = gtk_drop_down_new(G_LIST_MODEL(list), NULL);

  guint selected = spec->read(config_path);
  if (selected < count)
    gtk_drop_down_set_selected(GTK_DROP_DOWN(dropdown), selected);

  lambdock_accessible_update(dropdown, title, subtitle);

  g_autoptr(PrefControl) state =
      pref_control_new(config_path, spec->key, PREF_KIND_SYMBOL);
  state->symbols = g_new0(char *, count + 1);
  for (guint i = 0; i < count; i++)
    state->symbols[i] = g_strdup(spec->choices[i].symbol);
  pref_control_attach(dropdown, g_steal_pointer(&state));
  g_signal_connect(dropdown, "notify::selected",
                   G_CALLBACK(on_dropdown_changed), NULL);

  gtk_box_append(box, make_row(title, subtitle, dropdown));
}

/* =========================================================================
 * ACTION ROWS (a dropdown plus a button)
 * =========================================================================
 * Some rows do something instead of storing a value. The dock's own widgets
 * are one: the item is appended to the dock's layout override (the same file
 * drag and drop writes), which is what makes it survive a restart without
 * touching the settings file.
 */

/* Defined next to the "Reset" button, which is the other thing that needs it. */
static void reload_docks_for(const char *config_path);

typedef struct {
  char *config_path;   /* owned, "" for the default dock */
  GtkWidget *dropdown; /* borrowed: the row's choice, still owned by the row */
} PrefActionContext;

static void pref_action_context_free(PrefActionContext *context) {
  if (!context)
    return;

  g_free(context->config_path);
  g_free(context);
}

/* The destroy notify a GClosure wants: data first, closure second. */
static void pref_action_context_destroy(gpointer data, GClosure *closure) {
  (void)closure;
  pref_action_context_free((PrefActionContext *)data);
}

G_DEFINE_AUTOPTR_CLEANUP_FUNC(PrefActionContext, pref_action_context_free)

/** The label of the selected row of a dropdown, for messages. */
static char *dropdown_selected_label(GtkWidget *dropdown) {
  GObject *item =
      G_OBJECT(gtk_drop_down_get_selected_item(GTK_DROP_DOWN(dropdown)));

  if (item && GTK_IS_STRING_OBJECT(item))
    return g_strdup(gtk_string_object_get_string(GTK_STRING_OBJECT(item)));

  return NULL;
}

/** Runs the row's command for the symbol currently selected in its dropdown. */
static void run_action_row(GtkWidget *dropdown, const char *config_path,
                           GtkWidget *announce_on) {
  PrefControl *control = pref_control_of(dropdown);
  if (!control)
    return;

  guint selected = gtk_drop_down_get_selected(GTK_DROP_DOWN(dropdown));
  const char *preset = pref_control_selected_symbol(control, selected);
  if (!preset)
    return;

  /* "" is the default dock: the bridge resolves it to the default settings
   * file's layout, the same file a path-ed dock would write. */
  dock_layout_add_preset_item(config_path, preset);
  reload_docks_for(config_path);

  const char *target =
      *config_path_or_default(config_path) ? config_path : "default";
  g_autofree char *label = dropdown_selected_label(dropdown);
  LOG_C_INFO("UI", "Added dock item '%s' (%s) to [%s]",
             label ? label : preset, preset, target);

  /* A screen reader has no way of knowing the dock behind the window changed,
   * so say it out loud where the button is. */
  if (announce_on) {
    g_autofree char *message =
        g_strdup_printf(_("%s added to the dock"), label ? label : preset);
    lambdock_accessible_announce(announce_on, message, FALSE);
  }
}

/* Adapter: the row's button. */
static void on_action_clicked(GtkButton *button, gpointer user_data) {
  (void)button;

  const PrefActionContext *context = user_data;
  if (!context)
    return;

  run_action_row(context->dropdown, context->config_path, GTK_WIDGET(button));
}

static void append_action_row(GtkBox *box, const char *config_path,
                              const PrefActionSpec *spec) {
  const char *title = gettext(spec->title);
  const char *subtitle = gettext(spec->subtitle);
  const char *button_label = gettext(spec->button_label);

  guint count = choices_count(spec->choices);
  g_autofree char **labels = choice_labels_new(spec->choices);

  GtkStringList *list = gtk_string_list_new((const char *const *)labels);
  GtkWidget *dropdown = gtk_drop_down_new(G_LIST_MODEL(list), NULL);
  gtk_drop_down_set_selected(GTK_DROP_DOWN(dropdown), 0);
  lambdock_accessible_update(dropdown, title, subtitle);

  /* The dropdown is a control like any other (it is keyed by the row's option
   * name), so the selected symbol is read through the same helper. It never
   * writes a setting, so its kind is only there to carry the symbols. */
  g_autoptr(PrefControl) state =
      pref_control_new(config_path, spec->key, PREF_KIND_SYMBOL);
  state->symbols = g_new0(char *, count + 1);
  for (guint i = 0; i < count; i++)
    state->symbols[i] = g_strdup(spec->choices[i].symbol);
  pref_control_attach(dropdown, g_steal_pointer(&state));

  GtkWidget *button = gtk_button_new_with_label(button_label);
  gtk_widget_set_valign(button, GTK_ALIGN_CENTER);
  lambdock_accessible_update(button, button_label, subtitle);

  /* The same keyed contract every control in this form has: "dock-add-item" is
   * the choice, so its button is that name plus "-button". */
  g_autofree char *button_key = g_strdup_printf("%s-button", spec->key);
  g_object_set_data_full(G_OBJECT(button), PREF_KEY_DATA, g_strdup(button_key),
                         g_free);

  g_autoptr(PrefActionContext) context = g_new0(PrefActionContext, 1);
  context->config_path = g_strdup(config_path_or_default(config_path));
  context->dropdown = dropdown;
  g_signal_connect_data(button, "clicked", G_CALLBACK(on_action_clicked),
                        g_steal_pointer(&context),
                        pref_action_context_destroy, 0);

  GtkWidget *control = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
  gtk_widget_set_valign(control, GTK_ALIGN_CENTER);
  gtk_box_append(GTK_BOX(control), dropdown);
  gtk_box_append(GTK_BOX(control), button);

  gtk_box_append(box, make_row(title, subtitle, control));
}

/* =========================================================================
 * CONTENT
 * ========================================================================= */

static void append_section(GtkBox *box, PrefSection section,
                           const char *config_path) {
  GtkWidget *card =
      make_card(gettext(section.title), gettext(section.subtitle));

  for (guint i = 0; i < section.n_switches; i++)
    append_switch_row(GTK_BOX(card), config_path, &section.switches[i]);

  for (guint i = 0; i < section.n_scales; i++)
    append_scale_row(GTK_BOX(card), config_path, &section.scales[i]);

  for (guint i = 0; i < section.n_choices; i++)
    append_choice_row(GTK_BOX(card), config_path, &section.choices[i]);

  for (guint i = 0; i < section.n_actions; i++)
    append_action_row(GTK_BOX(card), config_path, &section.actions[i]);

  gtk_box_append(box, card);
}

static GtkWidget *build_content(const char *config_path) {
  const char *path = config_path_or_default(config_path);

  GtkWidget *content = gtk_box_new(GTK_ORIENTATION_VERTICAL, 18);
  gtk_widget_set_margin_start(content, 26);
  gtk_widget_set_margin_end(content, 26);
  gtk_widget_set_margin_top(content, 14);
  gtk_widget_set_margin_bottom(content, 26);

  for (gsize i = 0; i < G_N_ELEMENTS(k_sections); i++)
    append_section(GTK_BOX(content), k_sections[i], path);

  return content;
}

GtkWidget *lambdock_preferences_content_for(const char *config_path) {
  return build_content(config_path);
}

/* =========================================================================
 * DOCK RELOAD (the only thing "Reset" shares with the controls)
 * ========================================================================= */

static void reload_if_bound_visitor(LambdockState *state, gpointer user_data) {
  const char *wanted = user_data;

  if (g_strcmp0(config_path_or_default(state->config_path), wanted) == 0)
    reload_dock_instance(state);
}

/** Reloads every dock bound to @config_path, "" being the default dock. */
static void reload_docks_for(const char *config_path) {
  /* dock_registry_foreach walks a snapshot, so a reload that tears a dock down
   * cannot pull the ground out from under the walk. */
  dock_registry_foreach(reload_if_bound_visitor,
                        (gpointer)config_path_or_default(config_path));
}

/* =========================================================================
 * WINDOW STATE
 * ========================================================================= */

static void prefs_window_free(PrefsWindow *window) {
  if (!window)
    return;

  g_free(window->config_path);
  g_free(window);
}

static PrefsWindow *prefs_window_new(const char *config_path) {
  PrefsWindow *window = g_new0(PrefsWindow, 1);

  window->config_path = g_strdup(config_path_or_default(config_path));

  return window;
}

static PrefsWindow *prefs_window_of(GtkWidget *widget) {
  return widget ? g_object_get_data(G_OBJECT(widget), PREF_WINDOW_DATA) : NULL;
}

/* =========================================================================
 * THE WINDOW REGISTRY
 * ========================================================================= */

static GHashTable *prefs_registry_windows(void) {
  if (!g_prefs_registry.windows)
    g_prefs_registry.windows =
        g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);

  return g_prefs_registry.windows;
}

static GtkWindow *prefs_registry_lookup(const char *key) {
  GtkWindow *window = g_hash_table_lookup(prefs_registry_windows(), key);

  /* A window is removed from the registry when it is destroyed; this check
   * keeps a stale entry from handing out a dead pointer. */
  return (window && GTK_IS_WINDOW(window)) ? window : NULL;
}

static void prefs_registry_publish(const char *key, GtkWindow *window) {
  g_hash_table_insert(prefs_registry_windows(), g_strdup(key), window);
}

static void prefs_registry_forget(const char *key) {
  if (g_prefs_registry.windows && key)
    g_hash_table_remove(g_prefs_registry.windows, key);
}

/* =========================================================================
 * WINDOW CALLBACKS
 * ========================================================================= */

/* Escape closes the preferences window, as it does the launchpad and the stack
 * popovers. */
static gboolean on_key_pressed(GtkEventControllerKey *controller, guint keyval,
                               guint keycode, GdkModifierType state,
                               gpointer user_data) {
  (void)controller;
  (void)keycode;
  (void)state;

  if (keyval != GDK_KEY_Escape)
    return FALSE;

  gtk_window_close(GTK_WINDOW(user_data));
  return TRUE;
}

static void on_window_destroy(GtkWidget *window, gpointer user_data) {
  (void)user_data;

  const PrefsWindow *state = prefs_window_of(window);
  if (state)
    prefs_registry_forget(state->config_path);
}

static void on_reset_clicked(GtkButton *button, gpointer user_data) {
  (void)button;

  GtkWidget *window = GTK_WIDGET(user_data);
  const PrefsWindow *state = prefs_window_of(window);
  if (!state || config_path_is_default(state->config_path))
    return;

  /* Forget the GUI overrides, reload the settings file on top of the now-empty
   * override, and redraw every dock bound to it. */
  reset_dock_preferences(state->config_path);
  load_dock_config_scm(state->config_path);
  reload_docks_for(state->config_path);

  if (state->scrolled) {
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(state->scrolled),
                                  build_content(state->config_path));
  }
}

/* =========================================================================
 * WINDOW CONSTRUCTION
 * ========================================================================= */

/** The header bar: the window title and the "Reset" action. */
static GtkWidget *make_header_bar(GtkWidget *window) {
  GtkWidget *header = gtk_header_bar_new();

  GtkWidget *title = gtk_label_new(_("Lambdock Preferences"));
  gtk_widget_add_css_class(title, "title");
  gtk_header_bar_set_title_widget(GTK_HEADER_BAR(header), title);

  GtkWidget *reset = gtk_button_new_with_label(_("Reset"));
  gtk_widget_set_tooltip_text(
      reset,
      _("Forget the choices made here and go back to the settings file"));
  g_signal_connect(reset, "clicked", G_CALLBACK(on_reset_clicked), window);
  gtk_header_bar_pack_end(GTK_HEADER_BAR(header), reset);

  return header;
}

static GtkWidget *make_scrolled_content(const char *config_path) {
  GtkWidget *scrolled = gtk_scrolled_window_new();

  gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scrolled),
                                 GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
  gtk_scrolled_window_set_propagate_natural_width(
      GTK_SCROLLED_WINDOW(scrolled), TRUE);
  gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scrolled),
                                build_content(config_path));

  return scrolled;
}

static GtkWidget *prefs_window_new_widget(GtkApplication *app,
                                          const char *config_path) {
  GtkWidget *window = gtk_application_window_new(app);

  gtk_window_set_title(GTK_WINDOW(window), _("Lambdock Preferences"));
  gtk_window_set_default_size(GTK_WINDOW(window), 760, 860);
  gtk_window_set_hide_on_close(GTK_WINDOW(window), TRUE);
  gtk_widget_add_css_class(window, "lambdock-preferences");

  PrefsWindow *state = prefs_window_new(config_path);
  g_object_set_data_full(G_OBJECT(window), PREF_WINDOW_DATA, state,
                         (GDestroyNotify)prefs_window_free);

  gtk_window_set_titlebar(GTK_WINDOW(window), make_header_bar(window));

  state->scrolled = make_scrolled_content(state->config_path);
  gtk_window_set_child(GTK_WINDOW(window), state->scrolled);

  GtkEventController *keys = gtk_event_controller_key_new();
  gtk_event_controller_set_propagation_phase(keys, GTK_PHASE_CAPTURE);
  g_signal_connect(keys, "key-pressed", G_CALLBACK(on_key_pressed), window);
  gtk_widget_add_controller(window, keys);

  g_signal_connect(window, "destroy", G_CALLBACK(on_window_destroy), NULL);

  return window;
}

static void prefs_window_show(GtkApplication *app, const char *config_path) {
  const char *key = config_path_or_default(config_path);

  GtkWindow *existing = prefs_registry_lookup(key);
  if (existing) {
    gtk_window_present(existing);
    return;
  }

  GtkWidget *window = prefs_window_new_widget(app, key);

  prefs_registry_publish(key, GTK_WINDOW(window));
  gtk_window_present(GTK_WINDOW(window));
}

/* =========================================================================
 * PUBLIC ENTRY POINTS
 * ========================================================================= */

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

  if (parameter && g_variant_is_of_type(parameter, G_VARIANT_TYPE_STRING))
    config_path = g_variant_get_string(parameter, NULL);

  prefs_window_show(app, config_path);
}

void lambdock_preferences_shutdown(void) {
  if (!g_prefs_registry.windows)
    return;

  GList *windows = g_hash_table_get_values(g_prefs_registry.windows);
  for (GList *l = windows; l != NULL; l = l->next) {
    if (l->data && GTK_IS_WINDOW(l->data))
      gtk_window_destroy(GTK_WINDOW(l->data));
  }
  g_list_free(windows);

  g_clear_pointer(&g_prefs_registry.windows, g_hash_table_destroy);
}
