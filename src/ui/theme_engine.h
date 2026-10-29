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
 * load_custom_css:
 * @state: Pointer to global LambdockState context.
 *
 * Manages GTK CSS provider lifecycle for base GResource stylesheets
 * and user override files (~/.config/lambdock/styles.css). Clears/unbinds
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
