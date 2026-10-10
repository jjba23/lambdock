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

#ifndef THEME_ENGINE_H
#define THEME_ENGINE_H

#include "../lambdock.h"

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
