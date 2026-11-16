// lambdock -- hyper-hackable desktop dock for Wayland
//
// SPDX-License-Identifier: MIT
// Copyright © Josep Bigorra <jjbigorra@gmail.com>

#include "guile_bridge.h"

#include <libguile.h> // Header file defining function declarations and LambdockState
#include "core/config_values.h" // Pure, unit-tested range/name rules
#include "core/poll_budget.h"     // The per-dock poller budget
#include "dock_window.h"  // Re-deriving the layout from the finished row
#include "logger.h"       // Custom logging macros (e.g., LOG_C_INFO)
#include "ui/dock_item.h" // Functions to create visual dock items and separators
#include "ui/launchpad.h"  // Launchpad defaults and limits

/**
 * setup_guile_load_path:
 * Configures GNU Guile's internal directory search path (`%load-path`).
 *
 * Tells the Guile interpreter where to look on disk when a Scheme script
 * requests to load a module (for example, `(use-modules (lambdock core))`).
 */
void setup_guile_load_path(void) {
  // Tell Guile to search the local "scheme" folder relative to the execution
  // path
  scm_c_eval_string("(set! %load-path (cons \"scheme\" %load-path))");

#ifdef LAMBDOCK_SCHEME_DIR
  // If a system installation path was defined at compile-time (e.g.,
  // /usr/share/lambdock), add that directory to Guile's search path as well.
  scm_c_eval_string("(set! %load-path (cons \"" LAMBDOCK_SCHEME_DIR
                    "\" %load-path))");
#endif
}

/** Re-evaluates default Scheme configuration. */
void reload_guile_config(void) {
  LOG_C_INFO("GuileBridge", "Re-evaluating Scheme configuration file...");
  SCM init_fn = scm_c_public_ref("lambdock core", "init-dock!");
  if (scm_is_true(init_fn))
    scm_call_0(init_fn);
}

/**
 * call_core_proc_with_path:
 * Calls a procedure in `(lambdock core)` with a config path, or without an
 * argument for the default dock. The config readers moved to
 * rust/src/bridge.rs; this stays for the item builders below, which are still C.
 */
static SCM call_core_proc_with_path(const char *func_name, const char *path) {
  SCM fn = scm_c_public_ref("lambdock core", func_name);
  if (!scm_is_true(fn))
    return SCM_BOOL_F;

  if (path && *path != '\0') {
    return scm_call_1(fn, scm_from_utf8_string(path));
  }

  return scm_call_0(fn);
}

/* -------------------------------------------------------------------------
 * SCHEME -> C VALUE HELPERS
 * -------------------------------------------------------------------------
 * Every string that leaves this file is allocated with GLib (`g_strdup`), so
 * callers always pair it with `g_free`. Guile's own `scm_to_utf8_string()` uses
 * malloc, which used to leak into the rest of the program and made every free
 * site a guess; that conversion now stays local to these helpers.
 */

/** Converts a Scheme string/symbol to a `g_strdup`'d string, or NULL. */
static char *scm_to_c_string_or_null(SCM val) {
  if (!scm_is_true(val))
    return NULL;

  SCM str_scm = scm_is_symbol(val) ? scm_symbol_to_string(val) : val;
  if (!scm_is_string(str_scm))
    return NULL;

  char *raw = scm_to_utf8_string(str_scm);
  if (!raw)
    return NULL;

  char *copy = (*raw != '\0') ? g_strdup(raw) : NULL;
  free(raw);
  return copy;
}

/**
 * scm_symbol_name_or_null:
 * Reads the symbol name of @val. A string is accepted too, and anything else
 * (a list, `#f`, a number) yields NULL instead of raising a Guile type error
 * that would longjmp out of the C caller.
 */
static char *scm_symbol_name_or_null(SCM val) {
  if (scm_is_symbol(val))
    return scm_to_c_string_or_null(val);
  if (scm_is_string(val))
    return scm_to_c_string_or_null(val);

  return NULL;
}

/** A poll interval in ms, clamped to something a timer can use. */
static guint scm_to_poll_interval(SCM val, guint fallback) {
  if (!scm_is_number(val))
    return fallback;

  double interval = scm_to_double(val);
  if (!isfinite(interval) || interval < 1.0)
    return fallback;
  if (interval > (double)G_MAXUINT)
    return G_MAXUINT;

  return (guint)interval;
}

/* -------------------------------------------------------------------------
 * DOCK ITEM BUILDERS
 * -------------------------------------------------------------------------
 * Each Scheme item type has one small builder below and one row in the table
 * that follows. Adding a kind of item is therefore a new builder plus a new
 * row, instead of another branch in a long string comparison chain.
 */

/* Every `dock-item-*` accessor, resolved once per populate pass. */
typedef struct {
  SCM name;
  SCM icon;
  SCM exec;
  SCM poll;
  SCM poll_interval;
  SCM hover_animate;
  SCM path;
  SCM view;
  SCM meter;
  SCM clock;
  SCM unit;
  SCM max;
  SCM poll_name;
} DockItemAccessors;

typedef struct {
  LambdockState *state;
  SCM item;
  gboolean hover_animate;
  GtkOrientation separator_orientation;
  const DockItemAccessors *accessors;
  /* Poll periods of the dynamic items built this pass, for the budget check. */
  GArray *poll_intervals;
} DockItemBuildContext;

typedef void (*DockItemBuildFunc)(const DockItemBuildContext *context);

static void build_app_item(const DockItemBuildContext *context) {
  const DockItemAccessors *acc = context->accessors;

  char *name = scm_to_c_string_or_null(scm_call_1(acc->name, context->item));
  char *icon = scm_to_c_string_or_null(scm_call_1(acc->icon, context->item));
  char *exec = scm_to_c_string_or_null(scm_call_1(acc->exec, context->item));

  const char *config_path = context->state->config_path;
  if (is_app_excluded(name, config_path) ||
      is_app_excluded(icon, config_path) ||
      is_app_excluded(exec, config_path)) {
    LOG_C_INFO("GuileBridge", "Skipping excluded launcher item: %s",
               name ? name : "");
  } else {
    create_dock_item(context->state, name, icon, exec, name, TRUE,
                     context->hover_animate);
    LOG_C_INFO("GuileBridge", "Created favorite item: %s (%s)",
               name ? name : "unnamed", exec ? exec : "no-exec");
  }

  g_free(name);
  g_free(icon);
  g_free(exec);
}

static void build_dynamic_item(const DockItemBuildContext *context) {
  const DockItemAccessors *acc = context->accessors;

  char *name = scm_to_c_string_or_null(scm_call_1(acc->name, context->item));
  char *icon = scm_to_c_string_or_null(scm_call_1(acc->icon, context->item));
  char *exec = scm_to_c_string_or_null(scm_call_1(acc->exec, context->item));
  SCM poll_fn = scm_call_1(acc->poll, context->item);
  guint interval_ms =
      scm_to_poll_interval(scm_call_1(acc->poll_interval, context->item), 1000);

  const char *config_path = context->state->config_path;
  if (is_app_excluded(name, config_path) || is_app_excluded(exec, config_path)) {
    goto done;
  }

  DockItem *dynamic = create_dynamic_dock_item(context->state, name, icon, exec,
                                               (void *)poll_fn, interval_ms,
                                               context->hover_animate);

  if (dynamic) {
    char *poll_name =
        scm_to_c_string_or_null(scm_call_1(acc->poll_name, context->item));
    dock_item_set_poll_name(dynamic, poll_name);
    g_free(poll_name);

    if (context->poll_intervals)
      g_array_append_val(context->poll_intervals, interval_ms);
  }

  /* Meter items draw a gauge instead of text: the poll function returns a
   * number, which fills the ring up to `max' and is shown with `unit'. */
  if (dynamic && scm_is_true(scm_call_1(acc->meter, context->item))) {
    char *unit = scm_to_c_string_or_null(scm_call_1(acc->unit, context->item));
    SCM max_scm = scm_call_1(acc->max, context->item);
    double max_value = scm_is_number(max_scm) ? scm_to_double(max_scm) : 100.0;

    dock_item_enable_meter(dynamic, unit, max_value);
    g_free(unit);
  }

  /* Clock items draw an analogue face: the poll function still formats the
   * reading the tooltip and the screen reader use, the hands follow the system
   * clock. */
  if (dynamic && scm_is_true(scm_call_1(acc->clock, context->item)))
    dock_item_enable_clock(dynamic);

  LOG_C_INFO("GuileBridge",
             "Created dynamic item: %s (%s, poll interval: %u ms)",
             name ? name : "unnamed", exec ? exec : "no-exec", interval_ms);

done:
  g_free(name);
  g_free(icon);
  g_free(exec);
}

static void build_separator_item(const DockItemBuildContext *context) {
  create_separator_dock_item(context->state,
                             context->separator_orientation);
  LOG_C_INFO("GuileBridge", "Created layout separator");
}

static void build_stack_item(const DockItemBuildContext *context) {
  const DockItemAccessors *acc = context->accessors;

  char *name = scm_to_c_string_or_null(scm_call_1(acc->name, context->item));
  char *icon = scm_to_c_string_or_null(scm_call_1(acc->icon, context->item));
  char *path = scm_to_c_string_or_null(scm_call_1(acc->path, context->item));
  char *view = scm_symbol_name_or_null(scm_call_1(acc->view, context->item));

  create_stack_dock_item(context->state, name, icon, path, view,
                         context->hover_animate);

  g_free(name);
  g_free(icon);
  g_free(path);
  g_free(view);
}

static void build_launchpad_item(const DockItemBuildContext *context) {
  const DockItemAccessors *acc = context->accessors;

  char *name = scm_to_c_string_or_null(scm_call_1(acc->name, context->item));
  char *icon = scm_to_c_string_or_null(scm_call_1(acc->icon, context->item));

  create_launchpad_dock_item(context->state, name, icon,
                             context->hover_animate);

  g_free(name);
  g_free(icon);
}

static void build_trash_item(const DockItemBuildContext *context) {
  const DockItemAccessors *acc = context->accessors;

  char *name = scm_to_c_string_or_null(scm_call_1(acc->name, context->item));
  char *icon = scm_to_c_string_or_null(scm_call_1(acc->icon, context->item));

  create_trash_dock_item(context->state, name, icon, context->hover_animate);

  g_free(name);
  g_free(icon);
}

/* One row per Scheme item type: the type names are the `dock-item-type`
 * symbols produced by the Scheme side. */
static const struct {
  const char *type_name;
  DockItemBuildFunc build;
} DOCK_ITEM_BUILDERS[] = {
    {"app", build_app_item},
    {"dynamic", build_dynamic_item},
    {"separator", build_separator_item},
    {"stack", build_stack_item},
    {"launchpad", build_launchpad_item},
    {"trash", build_trash_item},
};

static DockItemBuildFunc builder_for_type(const char *type_name) {
  if (!type_name)
    return NULL;

  for (gsize i = 0; i < G_N_ELEMENTS(DOCK_ITEM_BUILDERS); i++) {
    if (g_strcmp0(type_name, DOCK_ITEM_BUILDERS[i].type_name) == 0)
      return DOCK_ITEM_BUILDERS[i].build;
  }

  return NULL;
}

/** Resolves every accessor the builders use; FALSE when `(lambdock core)` is
 * not loaded (the dock then simply stays empty instead of crashing). */
static gboolean resolve_item_accessors(DockItemAccessors *accessors) {
  accessors->name = scm_c_public_ref("lambdock core", "dock-item-name");
  accessors->icon = scm_c_public_ref("lambdock core", "dock-item-icon");
  accessors->exec = scm_c_public_ref("lambdock core", "dock-item-exec");
  accessors->poll = scm_c_public_ref("lambdock core", "dock-item-poll-fn");
  accessors->poll_interval =
      scm_c_public_ref("lambdock core", "dock-item-poll-interval");
  accessors->hover_animate =
      scm_c_public_ref("lambdock core", "dock-item-hover-animate?");
  accessors->path = scm_c_public_ref("lambdock core", "dock-item-path");
  accessors->view = scm_c_public_ref("lambdock core", "dock-item-view-mode");
  accessors->meter = scm_c_public_ref("lambdock core", "dock-item-meter?");
  accessors->clock = scm_c_public_ref("lambdock core", "dock-item-clock?");
  accessors->unit = scm_c_public_ref("lambdock core", "dock-item-unit");
  accessors->max = scm_c_public_ref("lambdock core", "dock-item-max");
  accessors->poll_name =
      scm_c_public_ref("lambdock core", "dock-item-poll-name");

  /* Check *every* accessor, not a sample: the item builders call all of them,
   * and applying a #f that scm_c_public_ref() could not resolve raises "wrong
   * type to apply" on the GTK thread -- where Guile has no handler and aborts
   * the process. A module that failed to export one accessor has to degrade to
   * "no items", never to a crash. */
  const SCM *procedures[] = {
      &accessors->name,      &accessors->icon,     &accessors->exec,
      &accessors->poll,      &accessors->hover_animate, &accessors->path,
      &accessors->view,      &accessors->meter,    &accessors->clock,
      &accessors->unit,      &accessors->max,      &accessors->poll_interval,
      &accessors->poll_name,
  };

  for (gsize i = 0; i < G_N_ELEMENTS(procedures); i++) {
    if (!scm_is_true(scm_procedure_p(*procedures[i])))
      return FALSE;
  }

  return TRUE;
}

/** Reads every configured item and builds one dock item per entry. */
void populate_dock_items_for(LambdockState *state) {
  if (!state || !state->box)
    return;

  DockItemAccessors accessors = {0};
  if (!resolve_item_accessors(&accessors))
    return;

  SCM get_type_fn = scm_c_public_ref("lambdock core", "dock-item-type");
  if (!scm_is_true(get_type_fn))
    return;

  GtkOrientation separator_orientation =
      (state->orientation == GTK_ORIENTATION_HORIZONTAL)
          ? GTK_ORIENTATION_VERTICAL
          : GTK_ORIENTATION_HORIZONTAL;

  g_autoptr(GArray) poll_intervals =
      g_array_new(FALSE, FALSE, sizeof(guint));

  SCM items = call_core_proc_with_path("get-dock-items", state->config_path);
  while (scm_is_pair(items)) {
    SCM item = scm_car(items);

    char *type_name = scm_symbol_name_or_null(scm_call_1(get_type_fn, item));
    DockItemBuildFunc build = builder_for_type(type_name);

    if (build) {
      DockItemBuildContext context = {
          .state = state,
          .item = item,
          .hover_animate = scm_is_true(scm_call_1(accessors.hover_animate, item)),
          .separator_orientation = separator_orientation,
          .accessors = &accessors,
          .poll_intervals = poll_intervals,
      };
      build(&context);
    } else if (type_name) {
      LOG_C_WARN("GuileBridge", "Ignoring unknown dock item type: %s",
                 type_name);
    }

    g_free(type_name);
    items = scm_cdr(items);
  }

  /* A configuration can schedule more polling work than one dock should: this
   * is the rule the developer guide used to only warn about. Nothing is
   * disabled -- the dock says what it is doing and lets the user decide. */
  if (poll_intervals->len > 0) {
    double rate = lambdock_polls_per_second(
        (const guint *)poll_intervals->data, poll_intervals->len);
    if (lambdock_poll_budget_exceeded((const guint *)poll_intervals->data,
                                      poll_intervals->len,
                                      LAMBDOCK_POLL_BUDGET_PER_SECOND)) {
      LOG_C_WARN("Poller",
                 "%u polling item(s) in [%s] schedule %.1f polls/second "
                 "(budget %.0f): raise a poll interval or drop a widget",
                 poll_intervals->len,
                 state->config_path ? state->config_path : "default", rate,
                 LAMBDOCK_POLL_BUDGET_PER_SECOND);
    }
  }

  /* The row exists only now, and the derived layout is measured from it: every
   * item's shelf slot is created hidden and the band it reserves is part of the
   * row's cross-axis size. Re-applying the layout here is the same pass a reload
   * runs, and without it the first frame kept every reflection hidden -- so the
   * icons sat one band lower (and unmirrored) until something reloaded the
   * dock. */
  update_dock_dimensions(state);
}

/**
 * dock_read_settings:
 * @state: dock state whose config_path names the settings file.
 *
 * Reads every value the dock takes from the settings file, in one place: the
 * theme and the mode flags the stylesheet depends on, the shelf geometry, and
 * the sizes the row is built from. Startup and a reload both go through here,
 * so neither can drift into reading a different subset of the other -- and the
 * sizes are already current when load_custom_css() asks the shelf for its band.
 */
void dock_read_settings(LambdockState *state) {
  if (!state)
    return;

  g_free(state->theme);
  state->theme = get_theme_config_for(state->config_path);

  /* Mode flags: the taskbar and window-title modifiers are attached from them,
   * so they are read before the stylesheet is loaded. */
  state->taskbar = get_taskbar_config_for(state->config_path);
  state->show_window_titles =
      get_show_window_titles_config_for(state->config_path);
  get_shelf_config_for(state->config_path, &state->shelf);

  state->auto_hide = get_autohide_config_for(state->config_path);
  state->track_windows = get_track_windows_config_for(state->config_path);
  state->show_when_clear = get_show_when_clear_config_for(state->config_path);
  state->hover_animate = get_hover_animate_config_for(state->config_path);
  state->reduce_motion = get_reduce_motion_config_for(state->config_path);
  state->icon_size = get_icon_size_config_for(state->config_path);
  state->item_spacing = get_item_spacing_config_for(state->config_path);
  state->capsule_padding = get_capsule_padding_config_for(state->config_path);
}

/* =================================================================================
 * DOCK LAYOUT OVERRIDES (drag and drop persistence)
 * =================================================================================
 * Drag and drop edits the *running* dock; these three calls hand the edit to
 * `(lambdock layout)`, which records it in the override file and replays it over
 * the configured items on the next load.
 * =================================================================================
 */

static SCM scm_from_optional_string(const char *value) {
  return (value && *value != '\0') ? scm_from_utf8_string(value)
                                   : scm_from_utf8_string("");
}

void dock_layout_add_item(const char *config_path, const char *kind,
                          const char *name, const char *icon, const char *arg,
                          const char *view) {
  if (!config_path || *config_path == '\0')
    return;

  SCM fn = scm_c_public_ref("lambdock core", "dock-layout-add!");
  if (!scm_is_true(fn))
    return;

  SCM args =
      scm_cons(scm_from_utf8_string(config_path),
               scm_list_5(scm_from_optional_string(kind),
                          scm_from_optional_string(name),
                          scm_from_optional_string(icon),
                          scm_from_optional_string(arg),
                          scm_from_optional_string(view)));

  scm_apply_0(fn, args);
}

void dock_layout_add_preset_item(const char *config_path, const char *preset) {
  if (!preset || *preset == '\0')
    return;

  SCM fn = scm_c_public_ref("lambdock core", "dock-layout-add!");
  if (!scm_is_true(fn))
    return;

  /* An empty path is the default dock: the Scheme side resolves it to the
   * default settings file's layout, which is the same file a path-ed dock
   * would use. */
  SCM args =
      scm_cons(scm_from_utf8_string(config_path ? config_path : ""),
               scm_list_5(scm_from_utf8_string("preset"),
                          scm_from_utf8_string(preset),
                          scm_from_utf8_string(""),
                          scm_from_utf8_string(""),
                          scm_from_utf8_string("")));

  scm_apply_0(fn, args);
}

void dock_layout_remove_item(const char *config_path, const char *key) {
  if (!config_path || *config_path == '\0' || !key)
    return;

  SCM fn = scm_c_public_ref("lambdock core", "dock-layout-remove!");
  if (!scm_is_true(fn))
    return;

  scm_call_2(fn, scm_from_utf8_string(config_path),
             scm_from_utf8_string(key));
}

void dock_layout_set_order(const char *config_path, GList *keys) {
  if (!config_path || *config_path == '\0')
    return;

  SCM fn = scm_c_public_ref("lambdock core", "dock-layout-set-order!");
  if (!scm_is_true(fn))
    return;

  SCM list = SCM_EOL;
  for (GList *l = keys; l != NULL; l = l->next) {
    if (l->data)
      list = scm_cons(scm_from_utf8_string((const char *)l->data), list);
  }
  list = scm_reverse_x(list, SCM_EOL);

  scm_call_2(fn, scm_from_utf8_string(config_path), list);
}

void dock_layout_set_view(const char *config_path, const char *key,
                          const char *view) {
  if (!config_path || *config_path == '\0' || !key)
    return;

  SCM fn = scm_c_public_ref("lambdock core", "dock-layout-set-view!");
  if (!scm_is_true(fn))
    return;

  SCM view_symbol = scm_string_to_symbol(
      scm_from_utf8_string((view && *view != '\0') ? view : "grid"));

  scm_call_3(fn, scm_from_utf8_string(config_path),
             scm_from_utf8_string(key), view_symbol);
}

/* =================================================================================
 * ITEM POLLERS
 * =================================================================================
 * The one place a dynamic item's Scheme procedure is kept and called. Moving
 * this here is what keeps scm_* out of dock_item.c: the item stores an opaque
 * handle, and what comes back is a LambdockReading.
 * =================================================================================
 */

/* Adapter: Guile's catch helper takes the procedure as a void*, not an SCM. */
static SCM call_poll_proc(void *data) {
  SCM fn = (SCM)data;
  return scm_call_0(fn);
}

/* Adapter: an error inside a poller is a missing reading, not a dead daemon. */
static SCM handle_poll_error(void *data, SCM key, SCM args) {
  (void)data;
  (void)key;
  (void)args;
  return SCM_BOOL_F;
}

void *lambdock_poll_fn_protect(void *procedure) {
  /* NULL is the dock's own "no poller" handle, not a Scheme value: handing it
   * to scm_* would be a NULL dereference (SCM's false is a tagged pointer, not
   * NULL). */
  if (procedure == NULL)
    return NULL;

  SCM fn = (SCM)procedure;

  if (!scm_is_true(fn) || !scm_is_true(scm_procedure_p(fn)))
    return NULL;

  scm_gc_protect_object(fn);
  return procedure;
}

gboolean lambdock_poll_fn_is_callable(void *poll_fn) {
  SCM fn = (SCM)poll_fn;

  return poll_fn != NULL && scm_is_true(fn) &&
         scm_is_true(scm_procedure_p(fn));
}

gboolean lambdock_poll_fn_call(void *poll_fn, LambdockReading *out) {
  if (!out)
    return FALSE;

  *out = (LambdockReading){LAMBDOCK_READING_NONE, 0.0, NULL};

  if (!lambdock_poll_fn_is_callable(poll_fn))
    return FALSE;

  SCM value = scm_internal_catch(SCM_BOOL_T, call_poll_proc, poll_fn,
                                 handle_poll_error, NULL);

  if (scm_is_number(value)) {
    out->kind = LAMBDOCK_READING_NUMBER;
    out->number = scm_to_double(value);
    return TRUE;
  }

  if (scm_is_string(value)) {
    out->kind = LAMBDOCK_READING_TEXT;
    out->text = scm_to_utf8_string(value); /* malloc'd, freed with free() */
    return TRUE;
  }

  return FALSE;
}

void lambdock_poll_fn_release(void *poll_fn) {
  SCM fn = (SCM)poll_fn;

  if (lambdock_poll_fn_is_callable(poll_fn))
    scm_gc_unprotect_object(fn);
}

/* =================================================================================
 * GUI PREFERENCES (Preferences window)
 * =================================================================================
 * The GTK preferences pane never edits settings.scm: it asks `(lambdock prefs)`
 * to apply the value to the running dock (through the ordinary setters) and to
 * remember it in the GUI override file, which the loader layers on top of the
 * settings file on the next start.
 * =================================================================================
 */

/* The `default' symbol targets the fallback dock, which has no settings file
 * and therefore nothing to persist against. */
static SCM scm_from_preference_target(const char *config_path) {
  if (config_path && *config_path != '\0')
    return scm_from_utf8_string(config_path);
  return scm_string_to_symbol(scm_from_utf8_string("default"));
}

static void call_set_preference(const char *key, SCM value,
                                const char *config_path) {
  if (!key || *key == '\0')
    return;

  SCM fn = scm_c_public_ref("lambdock core", "set-dock-preference!");
  if (!scm_is_true(fn))
    return;

  SCM key_symbol = scm_string_to_symbol(scm_from_utf8_string(key));
  scm_call_3(fn, key_symbol, value, scm_from_preference_target(config_path));
}

void set_dock_setting_bool(const char *key, const char *config_path,
                           gboolean value) {
  call_set_preference(key, scm_from_bool(value), config_path);
}

void set_dock_setting_int(const char *key, const char *config_path, int value) {
  call_set_preference(key, scm_from_int(value), config_path);
}

void set_dock_setting_double(const char *key, const char *config_path,
                             double value) {
  call_set_preference(key, scm_from_double(value), config_path);
}

void set_dock_setting_symbol(const char *key, const char *config_path,
                             const char *value) {
  if (!value || *value == '\0')
    return;
  call_set_preference(
      key, scm_string_to_symbol(scm_from_utf8_string(value)), config_path);
}

void reset_dock_preferences(const char *config_path) {
  if (!config_path || *config_path == '\0')
    return;

  SCM fn = scm_c_public_ref("lambdock core", "reset-dock-preferences!");
  if (!scm_is_true(fn))
    return;

  scm_call_1(fn, scm_from_utf8_string(config_path));
}
