// lambdock -- hyper-hackable desktop dock for Wayland
//
// SPDX-License-Identifier: MIT
// Copyright © Josep Bigorra <jjbigorra@gmail.com>

#include "theme_engine.h"
#include "../lambdock.h"
#include "../logger.h"
#include <gtk/gtk.h>

/* ============================================================================
 * SELECTOR SCOPING UTILITIES
 * ============================================================================
 */

/**
 * scope_css_selector_list:
 * Rewrites the selector list in @selectors (the text before a `{`, possibly
 * spanning several lines) under @scope_class and appends it to @out.
 */
static void scope_css_selector_list(GString *out, const char *selectors,
                                    const char *scope_class) {
  gchar **parts = g_strsplit(selectors, ",", -1);
  gboolean first = TRUE;

  for (int j = 0; parts[j] != NULL; j++) {
    char *sel = g_strstrip(parts[j]);
    if (strlen(sel) == 0)
      continue;

    if (!first)
      g_string_append(out, ", ");
    first = FALSE;

    // Compound the 'window' element selector directly with the scope
    // class, but only when 'window' is a whole element (e.g. "window",
    // "window.background"), never a different element whose name merely
    // starts with it (e.g. "windowcontrols"). The prefix check must come
    // first: reading sel[6] unconditionally overflows short selectors.
    if (g_str_has_prefix(sel, "window")) {
      char after = sel[6];
      if (after == '\0' || after == '.' || after == ':' || after == '[' ||
          g_ascii_isspace(after)) {
        g_string_append_printf(out, "window.%s%s", scope_class, sel + 6);
      } else {
        g_string_append_printf(out, ".%s %s", scope_class, sel);
      }
    } else {
      g_string_append_printf(out, ".%s %s", scope_class, sel);
    }
  }

  g_strfreev(parts);
}

/**
 * scope_css_selectors:
 * Transforms raw CSS rules to be scoped strictly under a target class wrapper.
 *
 * Examples with scope_class = "theme-unity":
 * - ".lambdock-window"  =>  ".theme-unity .lambdock-window"
 * - "window"            =>  "window.theme-unity"
 * - "window.background" =>  "window.theme-unity.background"
 *
 * A selector list may span lines (`button.window-focused,` then `:hover {`), so
 * the text before a `{` is accumulated until the block actually opens. Missing a
 * selector is not a cosmetic bug: an unscoped rule applies to *every* dock and to
 * the rest of the application, so one theme's indicator would be painted under
 * another theme's icons.
 */
char *theme_scope_css_selectors(const char *css_text,
                                const char *scope_class) {
  if (!css_text || *css_text == '\0')
    return g_strdup("");

  GString *result = g_string_new("");
  GString *pending = g_string_new(""); /* selector text, not scoped yet */
  gchar **lines = g_strsplit(css_text, "\n", -1);
  gboolean in_ruleset = FALSE;

  for (int i = 0; lines[i] != NULL; i++) {
    char *line = lines[i];
    char *trimmed = g_strstrip(g_strdup(line));

    // Inside a declaration block nothing is a selector: pass it through.
    if (in_ruleset) {
      if (strchr(line, '}'))
        in_ruleset = FALSE;
      g_string_append_printf(result, "%s\n", line);
      g_free(trimmed);
      continue;
    }

    // Pass empty lines, comments, and top-level @-rules through unchanged
    if (strlen(trimmed) == 0 || g_str_has_prefix(trimmed, "/*") ||
        g_str_has_prefix(trimmed, "@") || g_str_has_prefix(trimmed, "*")) {
      g_string_append_printf(result, "%s\n", line);
      g_free(trimmed);
      continue;
    }

    char *brace = strchr(line, '{');
    if (!brace) {
      /* Part of a selector list that continues on the next line. */
      g_string_append(pending, line);
      g_string_append_c(pending, '\n');
      g_free(trimmed);
      continue;
    }

    g_string_append_len(pending, line, brace - line);
    scope_css_selector_list(result, pending->str, scope_class);
    g_string_truncate(pending, 0);

    g_string_append(result, " {");
    in_ruleset = strchr(brace + 1, '}') == NULL;
    g_string_append_printf(result, "%s\n", brace + 1);
    g_free(trimmed);
  }

  g_strfreev(lines);
  g_string_free(pending, TRUE);
  return g_string_free(result, FALSE);
}

/* ============================================================================
 * THE THEME REQUEST (PURE)
 * ============================================================================
 * What load_custom_css() must decide before touching a widget, as a value.
 * Nothing here reads the display or allocates a provider, which is what makes
 * the rules below unit testable.
 */

LambdockThemeRequest lambdock_theme_request(const char *requested_theme,
                                            gboolean taskbar,
                                            gboolean show_window_titles,
                                            gboolean shelf_active) {
  const char *theme_name =
      (requested_theme && *requested_theme != '\0') ? requested_theme : "vanilla";

  LambdockThemeRequest request = {
      .theme_name = g_strdup(theme_name),
      .theme_class = g_strdup_printf("theme-%s", theme_name),
      .disabled = g_ascii_strcasecmp(theme_name, "none") == 0,
      .taskbar_modifier = taskbar || show_window_titles,
      .shelf_modifier = shelf_active,
  };

  return request;
}

void lambdock_theme_request_clear(LambdockThemeRequest *request) {
  if (!request)
    return;

  g_clear_pointer(&request->theme_name, g_free);
  g_clear_pointer(&request->theme_class, g_free);
  request->disabled = FALSE;
  request->taskbar_modifier = FALSE;
  request->shelf_modifier = FALSE;
}

/* ============================================================================
 * APPLIERS (the only part that touches GTK)
 * ============================================================================
 */

/** "default" for the config-less fallback dock, matching every other log line. */
static const char *dock_name(const LambdockState *state) {
  return (state && state->config_path) ? state->config_path : "default";
}

/**
 * provider_from_scoped_css:
 * Scopes @raw_css under @theme_class, installs it on @display and returns it.
 * NULL when there is nothing to load, so callers can chain modifiers freely.
 */
static GtkCssProvider *provider_from_scoped_css(GdkDisplay *display,
                                                const char *raw_css,
                                                const char *theme_class,
                                                guint priority) {
  if (!raw_css || *raw_css == '\0')
    return NULL;

  g_autofree char *scoped_css =
      theme_scope_css_selectors(raw_css, theme_class);
  GtkCssProvider *provider = gtk_css_provider_new();
  gtk_css_provider_load_from_string(provider, scoped_css);

  gtk_style_context_add_provider_for_display(
      display, GTK_STYLE_PROVIDER(provider), priority);

  return provider;
}

/** The same, for a stylesheet embedded in the GResource bundle. */
static GtkCssProvider *provider_from_resource(GdkDisplay *display,
                                              const char *resource_path,
                                              const char *theme_class,
                                              guint priority) {
  g_autoptr(GBytes) bytes = g_resources_lookup_data(
      resource_path, G_RESOURCE_LOOKUP_FLAGS_NONE, NULL);
  if (!bytes)
    return NULL;

  gsize size = 0;
  const char *raw_css = g_bytes_get_data(bytes, &size);

  return provider_from_scoped_css(display, raw_css, theme_class, priority);
}

/** Removes the previous theme class and tags the window with the new one. */
static void theme_swap_window_class(LambdockState *state,
                                    const char *theme_class) {
  if (state->active_theme_class) {
    gtk_widget_remove_css_class(state->window, state->active_theme_class);
    g_clear_pointer(&state->active_theme_class, g_free);
  }

  state->active_theme_class = g_strdup(theme_class);
  gtk_widget_add_css_class(state->window, theme_class);
}

/**
 * theme_detach_providers:
 * Unhooks every provider this engine installed for @state, in one pass.
 *
 * A reload must swap providers, never stack them: the dock state holds the four
 * pointers precisely so they can be released here.
 */
static void theme_detach_providers(LambdockState *state, GdkDisplay *display) {
  GtkCssProvider **providers[] = {
      &state->base_css_provider, &state->taskbar_css_provider,
      &state->shelf_css_provider, &state->user_css_provider,
  };

  for (gsize i = 0; i < G_N_ELEMENTS(providers); i++) {
    if (!*providers[i])
      continue;

    gtk_style_context_remove_provider_for_display(
        display, GTK_STYLE_PROVIDER(*providers[i]));
    g_clear_object(providers[i]);
  }
}

/**
 * theme_attach_modifiers:
 * Layers the taskbar and shelf modifiers in before the theme, so a theme rule of
 * equal specificity still wins over them.
 */
static void theme_attach_modifiers(LambdockState *state, GdkDisplay *display,
                                   const LambdockThemeRequest *request) {
  const guint priority = GTK_STYLE_PROVIDER_PRIORITY_APPLICATION;

  if (request->taskbar_modifier) {
    state->taskbar_css_provider = provider_from_resource(
        display, "/org/jointhefreeworld/lambdock/css/taskbar.css",
        request->theme_class, priority);
    if (state->taskbar_css_provider)
      LOG_C_INFO("UI", "Loaded taskbar/window-title modifier for [%s]",
                 dock_name(state));
  }

  if (request->shelf_modifier) {
    state->shelf_css_provider = provider_from_resource(
        display, "/org/jointhefreeworld/lambdock/css/shelf.css",
        request->theme_class, priority);
    if (state->shelf_css_provider)
      LOG_C_INFO("UI", "Loaded glass shelf modifier for [%s]",
                 dock_name(state));
  }
}

/** Loads the base theme, falling back to vanilla when it is not shipped. */
static void theme_attach_base_theme(LambdockState *state, GdkDisplay *display,
                                    const LambdockThemeRequest *request) {
  g_autofree char *resource_path = g_strdup_printf(
      "/org/jointhefreeworld/lambdock/css/%s.css", request->theme_name);

  GtkCssProvider *provider =
      provider_from_resource(display, resource_path, request->theme_class,
                             GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);

  if (!provider) {
    LOG_C_WARN("UI", "Theme resource '%s' not found, falling back to vanilla",
               resource_path);
    provider = provider_from_resource(
        display, "/org/jointhefreeworld/lambdock/css/vanilla.css",
        request->theme_class, GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
  }

  state->base_css_provider = provider;

  if (provider)
    LOG_C_INFO("UI", "Loaded & scoped CSS theme [%s] for [%s]",
               request->theme_name, dock_name(state));
}

/**
 * theme_attach_user_css:
 * Layers ~/.config/lambdock/styles.css on top of everything, at user priority,
 * so a hand-written override wins over any shipped theme.
 */
static void theme_attach_user_css(LambdockState *state, GdkDisplay *display,
                                  const LambdockThemeRequest *request) {
  g_autofree char *path =
      g_build_filename(g_get_user_config_dir(), "lambdock", "styles.css", NULL);

  if (!g_file_test(path, G_FILE_TEST_EXISTS))
    return;

  g_autofree char *raw_css = NULL;
  if (!g_file_get_contents(path, &raw_css, NULL, NULL))
    return;

  state->user_css_provider =
      provider_from_scoped_css(display, raw_css, request->theme_class,
                               GTK_STYLE_PROVIDER_PRIORITY_USER);

  if (state->user_css_provider)
    LOG_C_INFO("UI", "Loaded & scoped user CSS override for [%s]",
               dock_name(state));
}

/* ============================================================================
 * PUBLIC ENGINE INTERFACE
 * ============================================================================
 */

void load_custom_css(LambdockState *state) {
  if (!state || !state->window)
    return;

  GdkDisplay *display = gdk_display_get_default();
  if (!display)
    return;

  g_auto(LambdockThemeRequest) request =
      lambdock_theme_request(state->theme, state->taskbar,
                             state->show_window_titles,
                             lambdock_shelf_active(state));

  theme_swap_window_class(state, request.theme_class);
  theme_detach_providers(state, display);

  if (request.disabled) {
    LOG_C_INFO("UI", "Theme set to 'none' for [%s].", dock_name(state));
    return;
  }

  /* Order matters: modifiers first, then the theme over them, then the user's
   * own stylesheet over everything. */
  theme_attach_modifiers(state, display, &request);
  theme_attach_base_theme(state, display, &request);
  theme_attach_user_css(state, display, &request);
}
