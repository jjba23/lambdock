// lambdock -- hyper-hackable desktop dock for Wayland
//
// SPDX-License-Identifier: MIT
// Copyright © Josep Bigorra <jjbigorra@gmail.com>

#ifndef THEME_ENGINE_H
#define THEME_ENGINE_H

#include "../lambdock.h"

/* =========================================================================
 * THEMES AND CSS
 * =========================================================================
 * Load the built-in theme from GResource, scope its selectors to one dock's CSS
 * class, and layer the user's styles.css over it.
 *
 * Owns the providers it creates; the dock state keeps the pointers so it can
 * swap them on reload.
 */

/**
 * LambdockThemeRequest:
 * @theme_name: (transfer full): the theme to load, normalised ("vanilla" when
 *   the dock asked for none at all).
 * @theme_class: (transfer full): the window CSS class that scopes it, i.e.
 *   "theme-<name>".
 * @disabled: TRUE for the `none` theme: detach everything, load nothing.
 * @taskbar_modifier: TRUE when taskbar.css should be layered in (taskbar mode or
 *   window titles).
 * @shelf_modifier: TRUE when shelf.css should be layered in.
 *
 * Everything load_custom_css() has to decide before it touches a widget, as a
 * value: which theme, which class, which modifiers. Computed from plain
 * arguments by lambdock_theme_request(), so the rules are unit tested without a
 * display. Free it with lambdock_theme_request_clear(), or let ~g_auto~ do it.
 */
typedef struct {
  char *theme_name;
  char *theme_class;
  gboolean disabled;
  gboolean taskbar_modifier;
  gboolean shelf_modifier;
} LambdockThemeRequest;

/**
 * lambdock_theme_request:
 * @requested_theme: (nullable): the dock's theme name, as configured.
 * @taskbar: whether the dock renders as a taskbar.
 * @show_window_titles: whether running items show their window title.
 * @shelf_active: whether the glass shelf geometry is in use.
 *
 * Pure: same arguments, same request. A NULL or empty @requested_theme means
 * "vanilla"; the comparison for `none` is case-insensitive, matching the
 * stylesheet names on disk.
 *
 * Returns: (transfer full): the request to hand to the appliers.
 */
LambdockThemeRequest lambdock_theme_request(const char *requested_theme,
                                            gboolean taskbar,
                                            gboolean show_window_titles,
                                            gboolean shelf_active);

/** Releases the strings a LambdockThemeRequest owns; safe on a cleared value. */
void lambdock_theme_request_clear(LambdockThemeRequest *request);

G_DEFINE_AUTO_CLEANUP_CLEAR_FUNC(LambdockThemeRequest,
                                 lambdock_theme_request_clear)

/**
 * load_custom_css:
 * @state: Pointer to global LambdockState context.
 *
 * Computes the theme request for @state, then applies it: swaps the scoping
 * class, detaches the previous providers and loads the new ones. Clears/unbinds
 * providers if theme is set to 'none'.
 */
void load_custom_css(LambdockState *state);

/**
 * theme_scope_css_selectors:
 * @css_text: Raw, unscoped CSS source text.
 * @scope_class: CSS class the selectors should be scoped under (e.g.
 * "theme-vanilla").
 *
 * Rewrites every rule selector so it only applies inside a scope wrapper:
 * element selectors like `window` are compounded (`window.theme-vanilla`),
 * everything else is nested (`.theme-vanilla .foo`). Comments and top-level
 * @-rules pass through untouched.
 *
 * Returns: (transfer full): a newly allocated CSS string. Free with
 * g_free().
 */
char *theme_scope_css_selectors(const char *css_text, const char *scope_class);

#endif // THEME_ENGINE_H
