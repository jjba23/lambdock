// lambdock -- hyper-hackable desktop dock for Wayland
//
// SPDX-License-Identifier: MIT
// Copyright © Josep Bigorra <jjbigorra@gmail.com>

#ifndef LAMBDOCK_LOGGER_H
#define LAMBDOCK_LOGGER_H

#include <glib.h>

/* =========================================================================
 * LOGGING MACROS
 * =========================================================================
 * LOG_C_DEBUG/INFO/WARN/ERROR(subsystem, fmt, ...): colourised diagnostics on
 * stdout (or g_warning/g_critical for the two loud ones).
 *
 * Diagnostics only: nothing here is translated, and nothing here is control
 * flow.
 */

// ANSI Color definitions
#define LOG_CLR_RESET "\x1b[0m"
#define LOG_CLR_DIM "\x1b[2m"
#define LOG_CLR_GREEN "\x1b[32m"
#define LOG_CLR_YELLOW "\x1b[33m"
#define LOG_CLR_RED "\x1b[31m"
#define LOG_CLR_MAGENTA "\x1b[35m"

#define LOG_C_DEBUG(subsystem, fmt, ...)                                       \
  g_print(LOG_CLR_DIM "[DEBUG]" LOG_CLR_RESET "[lambdock]" LOG_CLR_MAGENTA     \
                      "[C:%s]" LOG_CLR_RESET " " fmt "\n",                     \
          subsystem, ##__VA_ARGS__)

#define LOG_C_INFO(subsystem, fmt, ...)                                        \
  g_print(LOG_CLR_GREEN "[INFO]" LOG_CLR_RESET "[lambdock]" LOG_CLR_MAGENTA    \
                        "[C:%s]" LOG_CLR_RESET " " fmt "\n",                   \
          subsystem, ##__VA_ARGS__)

#define LOG_C_WARN(subsystem, fmt, ...)                                        \
  g_warning(LOG_CLR_YELLOW "[WARN]" LOG_CLR_RESET "[lambdock]" LOG_CLR_MAGENTA \
                           "[C:%s]" LOG_CLR_RESET " " fmt,                     \
            subsystem, ##__VA_ARGS__)

#define LOG_C_ERROR(subsystem, fmt, ...)                                       \
  g_critical(LOG_CLR_RED "[ERROR]" LOG_CLR_RESET "[lambdock]" LOG_CLR_MAGENTA  \
                         "[C:%s]" LOG_CLR_RESET " " fmt,                       \
             subsystem, ##__VA_ARGS__)

#endif // LAMBDOCK_LOGGER_H
