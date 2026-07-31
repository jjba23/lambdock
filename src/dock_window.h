#ifndef DOCK_WINDOW_H
#define DOCK_WINDOW_H

#include "lambdock.h"

void setup_layer_shell_window(GtkWindow *window, gboolean auto_hide);
void setup_dock_motion_controller(LambdockState *state);
void reveal_dock(LambdockState *state);
void hide_dock(LambdockState *state);
void schedule_initial_autohide(LambdockState *state);

#endif // DOCK_WINDOW_H
