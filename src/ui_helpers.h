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

/* INCLUDE GUARD: Prevents duplicate symbol declarations if included multiple
 * times */
#ifndef UI_HELPERS_H
#define UI_HELPERS_H

/* Core header containing GTK 4 definitions and Lambdock structures */
#include "lambdock.h"

/* =========================================================================
 * ICON THEME & STYLING PROTOTYPES
 * ========================================================================= */

/**
 * setup_icon_theme:
 *
 * Configures system and user XDG icon search paths for GTK, and reads GNOME
 * GSettings desktop preferences to automatically apply the system icon theme
 * name.
 */
void setup_icon_theme(void);

/**
 * load_custom_css:
 *
 * Instantiates a GtkCssProvider, loads dock stylesheet rules from file paths,
 * and registers the CSS provider globally across the active GdkDisplay.
 */
void load_custom_css(void);

/* =========================================================================
 * WIDGET FACTORY CONSTRUCTOR PROTOTYPES
 * ========================================================================= */

/**
 * create_app_button:
 * @name: Display name used for the button's hover tooltip text.
 * @icon: Icon identifier string, theme icon name, or file path.
 * @exec: Shell command line execution string triggered when clicked.
 * @icon_size: Pixel dimension to render the icon image within the button.
 *
 * Constructs a GTK button configured with resolved icon images, CSS classes,
 * tooltips, and click signal handlers.
 *
 * Returns: A pointer to the newly allocated GtkWidget button context.
 */
GtkWidget *create_app_button(const char *name, const char *icon,
                             const char *exec, int icon_size);

/**
 * create_separator:
 *
 * Constructs a vertical GtkSeparator widget with margin padding to visually
 * segment launcher buttons or running app sections on the dock.
 *
 * Returns: A pointer to the newly allocated GtkWidget separator.
 */
GtkWidget *create_separator(void);

#endif // UI_HELPERS_H
