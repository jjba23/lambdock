// lambdock

// Copyright © Josep Bigorra <jjbigorra@gmail.com>

// lambdock is free software : you can redistribute it and / or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.

// lambdock is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.See the
// GNU General Public License for more details.

// You should have received a copy of the GNU General Public License
// along with lambdock.If not, see <https://www.gnu.org/licenses/>.

#ifndef DOCK_WINDOW_H
#define DOCK_WINDOW_H

#include "lambdock.h"

void setup_layer_shell_window(GtkWindow *window, gboolean auto_hide);
void setup_dock_motion_controller(LambdockState *state);
void reveal_dock(LambdockState *state);
void hide_dock(LambdockState *state);
void schedule_initial_autohide(LambdockState *state);
void on_quit_action(GSimpleAction *action, GVariant *parameter,
                    gpointer user_data);
void on_right_click(GtkGestureClick *gesture, gint n_press, gdouble x,
                    gdouble y, gpointer user_data);

#endif // DOCK_WINDOW_H
