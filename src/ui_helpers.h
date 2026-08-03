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

#ifndef UI_HELPERS_H
#define UI_HELPERS_H

#include "lambdock.h"

void setup_icon_theme(void);
void load_custom_css(void);

DockItem *create_dock_item(LambdockState *state, const char *name,
                           const char *icon, const char *exec,
                           const char *app_id, gboolean is_favorite);
void free_dock_item(DockItem *item);

AppWindow *dock_item_add_window(DockItem *item,
                                struct zwlr_foreign_toplevel_handle_v1 *handle);
void dock_item_remove_window(DockItem *item,
                             struct zwlr_foreign_toplevel_handle_v1 *handle);
void update_dock_item_status(DockItem *item);

DockItem *find_dock_item_by_app_id(LambdockState *state, const char *app_id);

GtkWidget *create_app_button(const char *name, const char *icon,
                             const char *exec, int icon_size);
GtkWidget *create_separator(GtkOrientation orientation);

#endif // UI_HELPERS_H
