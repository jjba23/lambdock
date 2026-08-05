#ifndef THEME_ENGINE_H
#define THEME_ENGINE_H

#include "lambdock.h"

/**
 * load_custom_css:
 * @state: Pointer to global LambdockState context.
 *
 * Manages GTK CSS provider lifecycle for base GResource stylesheets
 * and user override files (~/.config/lambdock/styles.css). Clears/unbinds
 * providers if theme is set to 'none'.
 */
void load_custom_css(LambdockState *state);

#endif // THEME_ENGINE_H
