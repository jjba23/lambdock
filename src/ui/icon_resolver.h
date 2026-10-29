// lambdock -- hyper-hackable desktop dock for Wayland
//
// SPDX-License-Identifier: MIT
// Copyright © Josep Bigorra <jjbigorra@gmail.com>

#ifndef ICON_RESOLVER_H
#define ICON_RESOLVER_H

#include <gtk/gtk.h>

/* =========================================================================
 * ICON LOOKUP
 * =========================================================================
 * The fallback pipeline: GResource path, absolute path, icon theme, XDG
 * pixmaps, a -symbolic variant, then the generic icon. Also sets up the icon
 * theme.
 */

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
