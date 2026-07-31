#ifndef UI_HELPERS_H
#define UI_HELPERS_H

#include "lambdock.h"

void setup_icon_theme(void);
GtkWidget *create_app_button(const char *name, const char *icon,
                             const char *exec, int icon_size);
GtkWidget *create_separator(void);

#endif // UI_HELPERS_H
