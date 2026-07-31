#ifndef GUILE_BRIDGE_H
#define GUILE_BRIDGE_H

#include "lambdock.h"

void setup_guile_load_path(void);
gboolean get_autohide_config(void);
void populate_dock_items(GtkWidget *box);

#endif // GUILE_BRIDGE_H
