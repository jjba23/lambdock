// lambdock -- hyper-hackable desktop dock for Wayland
//
// SPDX-License-Identifier: MIT
// Copyright © Josep Bigorra <jjbigorra@gmail.com>

#ifndef DESKTOP_ENTRY_H
#define DESKTOP_ENTRY_H

#include <glib.h>

/* =========================================================================
 * XDG DESKTOP ENTRIES
 * =========================================================================
 * Parse .desktop files, index them under every name a window might report, and
 * answer name/icon/exec lookups.
 *
 * Owns a process-wide cache: callers must invalidate it when desktop files
 * change rather than holding onto returned pointers across a rescan.
 */

/** Milliseconds a built cache is trusted before it is rebuilt on demand. */
#define DESKTOP_CACHE_TTL_MS 60000

/**
 * LambdockDesktopEntry:
 * @id: Desktop file name, e.g. "org.gnome.Terminal.desktop".
 * @stem: @id without the ".desktop" suffix -- what Wayland compositors usually
 * report as an application id, e.g. "org.gnome.Terminal".
 * @name: Localized display name (Name=, honouring the current locale).
 * @exec: Command line with the freedesktop field codes (%f, %U, ...) removed.
 * @icon: Icon name or absolute file path from Icon=.
 * @wm_class: StartupWMClass= of the entry (first value), the window class the
 * application announces to the compositor. Often the only reliable link
 * between an app id and its desktop file.
 * @categories: Categories= of the entry, semicolon separated and kept as
 * written ("Network;WebBrowser;"). NULL when the file has none.
 * @no_display: NoDisplay=true (do not list it in launchers).
 * @hidden: Hidden=true (the user or the distribution removed it).
 * @path: Absolute path of the desktop file the entry was read from, or NULL
 * when it was parsed from memory. Tells a Flatpak export from a distro package.
 * @alternates: (element-type LambdockDesktopEntry) (nullable): the other
 * desktop files that describe the same application. Owned by the entry, and
 * filled in by lambdock_desktop_entries_scan() for the copies it does not list
 * itself.
 *
 * One installed application, parsed from a freedesktop.org desktop entry. The
 * launchpad lists these, dropping a .desktop file on the dock turns one into a
 * dock item, and the cache built from them is what maps a window's app id back
 * to the application behind it.
 */
typedef struct LambdockDesktopEntry {
  char *id;
  char *stem;
  char *name;
  char *exec;
  char *icon;
  char *wm_class;
  char *categories;
  gboolean no_display;
  gboolean hidden;
  char *path;
  GPtrArray *alternates;
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
 * lambdock_desktop_entry_has_category:
 * @entry: (nullable): a desktop entry.
 * @category: a freedesktop.org main category such as "WebBrowser" or "Music".
 *
 * Whether @entry lists @category, compared case-insensitively. The dock uses
 * this to give an application the context-menu actions of its kind (a browser
 * gets its private-window command, a player its playback controls).
 *
 * Returns: TRUE when the category is listed.
 */
gboolean lambdock_desktop_entry_has_category(const LambdockDesktopEntry *entry,
                                             const char *category);

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
 * exports dirs) and returns a name-sorted array of entries that are meant to be
 * visible in launchers. Entries are deduplicated by desktop file id, and the
 * same application installed twice (a distro package and a Flatpak export of
 * it, say) is listed once: the first copy in scan order stays in the array and
 * carries the others in its @alternates.
 *
 * Returns: (transfer full): GPtrArray of LambdockDesktopEntry*.
 */
GPtrArray *lambdock_desktop_entries_scan(void);

/**
 * lambdock_desktop_entry_source_label:
 * @entry: (nullable): a desktop entry.
 *
 * Where the entry's desktop file comes from, as something worth showing next to
 * it: "Flatpak" for an export, "Your files" for one of the user's own
 * applications, "System" for one a distribution installed, and "Application"
 * for an entry that was never read from a file. Used to tell the copies of an
 * application apart in the launchpad's menu.
 *
 * Returns: (transfer full) (nullable): the translated label, or NULL when
 * @entry is NULL.
 */
char *lambdock_desktop_entry_source_label(const LambdockDesktopEntry *entry);

/**
 * lambdock_desktop_entries_free:
 * @entries: (nullable): array returned by lambdock_desktop_entries_scan().
 */
void lambdock_desktop_entries_free(GPtrArray *entries);

/**
 * lambdock_desktop_entry_lookup:
 * @app_id: An application id as the compositor reports it, with or without the
 * ".desktop" suffix ("org.gnome.Nautilus", "nautilus", "wezterm", ...).
 *
 * Resolves @app_id against the cached desktop entries. The order is: the desktop
 * id itself, then StartupWMClass, then the executable's base name, then the last
 * component of a reverse-DNS id, then a case- and punctuation-insensitive
 * comparison. This is what makes a window track the dock item that launched it
 * even when the two spell the application differently.
 *
 * The cache is built on first use and rebuilt when it is older than
 * DESKTOP_CACHE_TTL_MS or after lambdock_desktop_cache_invalidate().
 *
 * Returns: (transfer none) (nullable): the entry, owned by the cache.
 */
const LambdockDesktopEntry *lambdock_desktop_entry_lookup(const char *app_id);

/**
 * lambdock_desktop_entries_cached:
 * The cached, name-sorted array of entries that should be shown in launchers.
 * Same lifetime rules as lambdock_desktop_entry_lookup().
 *
 * Returns: (transfer none): GPtrArray of LambdockDesktopEntry*.
 */
GPtrArray *lambdock_desktop_entries_cached(void);

/**
 * lambdock_desktop_cache_invalidate:
 * Drops the cache, so the next lookup rescans the applications directories. Used
 * after installing or removing an application, and by the tests.
 */
void lambdock_desktop_cache_invalidate(void);

/**
 * lambdock_app_id_to_name:
 * @app_id: An application id ("org.gnome.SystemMonitor", "google-chrome",
 * "wezterm", ...).
 *
 * Turns an app id into something worth showing in a tooltip, for applications
 * that have no desktop file at all: the reverse-DNS prefix is dropped, camel
 * case and dashes are split into words and the first letter of each
 * lower-case word is capitalised. "org.gnome.SystemMonitor" becomes
 * "System Monitor", "google-chrome" becomes "Google Chrome", "wezterm" becomes
 * "Wezterm".
 *
 * Returns: (transfer full) (nullable): the derived name, or NULL for a NULL or
 * empty @app_id.
 */
char *lambdock_app_id_to_name(const char *app_id);

/**
 * lambdock_app_name_for:
 * @app_id: An application id.
 *
 * The display name of the application behind @app_id: the localized name from
 * its desktop entry when there is one, the derived name from
 * lambdock_app_id_to_name() when there is not.
 *
 * Returns: (transfer full) (nullable): the name.
 */
char *lambdock_app_name_for(const char *app_id);

/**
 * lambdock_app_icon_for:
 * @app_id: An application id.
 *
 * The icon of the application behind @app_id: the entry's Icon= when there is a
 * desktop file, the app id itself otherwise (icon themes often ship the app id
 * as an icon name).
 *
 * Returns: (transfer full) (nullable): the icon name or path.
 */
char *lambdock_app_icon_for(const char *app_id);

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
