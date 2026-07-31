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

#ifndef UI_HELPERS_H
#define UI_HELPERS_H

#include "lambdock.h"

void setup_icon_theme(void);
GtkWidget *create_app_button(const char *name, const char *icon,
                             const char *exec, int icon_size);
GtkWidget *create_separator(void);

void load_custom_css(void);

#endif // UI_HELPERS_H
