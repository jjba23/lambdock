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

#ifndef LAMBDOCK_LOGGER_H
#define LAMBDOCK_LOGGER_H

#include <glib.h>

// ANSI Color definitions
#define LOG_CLR_RESET "\x1b[0m"
#define LOG_CLR_DIM "\x1b[2m"
#define LOG_CLR_GREEN "\x1b[32m"
#define LOG_CLR_YELLOW "\x1b[33m"
#define LOG_CLR_RED "\x1b[31m"
#define LOG_CLR_MAGENTA "\x1b[35m"

#define LOG_C_DEBUG(subsystem, fmt, ...)                                       \
  g_print(LOG_CLR_MAGENTA "[C:%s]" LOG_CLR_RESET " " LOG_CLR_DIM               \
                          "[DEBUG]" LOG_CLR_RESET " " fmt "\n",                \
          subsystem, ##__VA_ARGS__)

#define LOG_C_INFO(subsystem, fmt, ...)                                        \
  g_print(LOG_CLR_MAGENTA "[C:%s]" LOG_CLR_RESET " " LOG_CLR_GREEN             \
                          "[INFO]" LOG_CLR_RESET " " fmt "\n",                 \
          subsystem, ##__VA_ARGS__)

#define LOG_C_WARN(subsystem, fmt, ...)                                        \
  g_warning(LOG_CLR_MAGENTA "[C:%s]" LOG_CLR_RESET " " LOG_CLR_YELLOW          \
                            "[WARN]" LOG_CLR_RESET " " fmt                     \
            subsystem, ##__VA_ARGS__)

#define LOG_C_ERROR(subsystem, fmt, ...)                                       \
  g_critical(LOG_CLR_MAGENTA "[C:%s]" LOG_CLR_RESET " " LOG_CLR_RED            \
                             "[ERROR]" LOG_CLR_RESET " " fmt,                  \
             subsystem, ##__VA_ARGS__)

#endif // LAMBDOCK_LOGGER_H
