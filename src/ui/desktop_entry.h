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

#ifndef DESKTOP_ENTRY_H
#define DESKTOP_ENTRY_H

#include <glib.h>

/**
 * LambdockDesktopEntry:
 * @id: Desktop file name, e.g. "org.gnome.Terminal.desktop". Used as the app id
 * so windows can be matched back to the launcher.
 * @name: Localized display name (Name=, honouring the current locale).
 * @exec: Command line with the freedesktop field codes (%f, %U, ...) removed.
 * @icon: Icon name or absolute file path from Icon=.
 *
 * One installed application, parsed from a freedesktop.org desktop entry. The
 * launchpad lists these, and dropping a .desktop file on the dock turns one
 * into a dock item.
 */
typedef struct LambdockDesktopEntry {
  char *id;
  char *name;
  char *exec;
  char *icon;
  gboolean no_display;
  gboolean hidden;
} LambdockDesktopEntry;

/**
 * lambdock_desktop_entry_new_from_data:
 * @data: Contents of a desktop file.
 * @id: Desktop file name to record as the entry id (may be NULL).
 *
 * Parses a [Desktop Entry] group. Returns NULL when the data is not a loadable
 * desktop file, and an entry whose @hidden / @no_display flags say the
 * application must not be shown in launchers (the caller decides).
 *
 * Returns: (transfer full) (nullable): a new entry.
 */
LambdockDesktopEntry *lambdock_desktop_entry_new_from_data(const char *data,
                                                           const char *id);

/**
 * lambdock_desktop_entry_new_from_file:
 * @path: Absolute path of a .desktop file.
 *
 * Reads and parses @path. Returns NULL for missing/unreadable/invalid files.
 *
 * Returns: (transfer full) (nullable): a new entry.
 */
LambdockDesktopEntry *lambdock_desktop_entry_new_from_file(const char *path);

/**
 * lambdock_desktop_entry_free:
 * @entry: (nullable): entry to free.
 */
void lambdock_desktop_entry_free(LambdockDesktopEntry *entry);

/**
 * lambdock_exec_strip_field_codes:
 * @exec: Raw Exec= value.
 *
 * Removes the freedesktop field codes (%f, %F, %u, %U, %i, %c, %k and friends)
 * and turns "%%" into "%", leaving a command line that can be handed straight
 * to g_spawn_command_line_async().
 *
 * Returns: (transfer full) (nullable): the cleaned command line, or NULL when
 * @exec is NULL.
 */
char *lambdock_exec_strip_field_codes(const char *exec);

/**
 * lambdock_desktop_entries_scan:
 * Scans every applications directory of the XDG data dirs (plus the flatpak
 * exports dirs) and returns a name-sorted, id-deduplicated array of entries
 * that are meant to be visible in launchers.
 *
 * Returns: (transfer full): GPtrArray of LambdockDesktopEntry*.
 */
GPtrArray *lambdock_desktop_entries_scan(void);

/**
 * lambdock_desktop_entries_free:
 * @entries: (nullable): array returned by lambdock_desktop_entries_scan().
 */
void lambdock_desktop_entries_free(GPtrArray *entries);

/**
 * lambdock_open_path:
 * @path: File or directory to open.
 *
 * Opens @path with the desktop's default handler (xdg-open). Fire and forget.
 *
 * Returns: TRUE when the helper could be spawned.
 */
gboolean lambdock_open_path(const char *path);

/**
 * lambdock_spawn_command:
 * @command: Shell command line.
 *
 * Spawns @command asynchronously, logging (instead of crashing) on failure.
 *
 * Returns: TRUE when the command was spawned.
 */
gboolean lambdock_spawn_command(const char *command);

#endif // DESKTOP_ENTRY_H
