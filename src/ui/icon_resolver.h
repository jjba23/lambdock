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

#ifndef ICON_RESOLVER_H
#define ICON_RESOLVER_H

#include <gtk/gtk.h>

/**
 * setup_icon_theme:
 *
 * Registers system and user XDG icon search paths (~/.local/share/icons, etc.)
 * and detects GNOME desktop icon preferences.
 */
void setup_icon_theme(void);

/**
 * create_image_for_icon:
 * @theme: Active GtkIconTheme instance.
 * @icon: Icon name, GResource URI, file path, or symbolic identifier.
 * @icon_size: Pixel size for rendering the image widget.
 *
 * Executes a 6-stage fallback pipeline to resolve and construct a GtkImage
 * widget.
 *
 * Returns: Newly created GtkWidget image.
 */
GtkWidget *create_image_for_icon(GtkIconTheme *theme, const char *icon,
                                 int icon_size);

#endif // ICON_RESOLVER_H
