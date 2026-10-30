// lambdock -- hyper-hackable desktop dock for Wayland
//
// SPDX-License-Identifier: MIT
// Copyright © Josep Bigorra <jjbigorra@gmail.com>

#ifndef STACK_H
#define STACK_H

#include "../lambdock.h"

/* =========================================================================
 * FOLDER STACKS
 * =========================================================================
 * List a folder, render it as a grid or a list, and open its entries.
 *
 * View behaviour is a row of STACK_VIEW_OPS in the .c file, which is also where
 * the settings symbol mapping lives.
 */

/* A stack never renders more than this many entries: a folder with 10k files
 * must not turn a popover into a memory and layout disaster. */
#define STACK_MAX_ENTRIES 300

/**
 * LambdockStackView:
 * @LAMBDOCK_STACK_VIEW_GRID: Icon grid (icon + name tiles).
 * @LAMBDOCK_STACK_VIEW_LIST: One row per entry, like a file manager list.
 *
 * How a stack lays out the contents of its folder.
 */
typedef enum {
  LAMBDOCK_STACK_VIEW_GRID = 0,
  LAMBDOCK_STACK_VIEW_LIST
} LambdockStackView;

/**
 * LambdockStackEntry:
 * @name: Base file name shown to the user.
 * @path: Absolute path of the entry.
 * @is_dir: TRUE for directories.
 *
 * One item inside a stack folder.
 */
typedef struct LambdockStackEntry {
  char *name;
  char *path;
  gboolean is_dir;
} LambdockStackEntry;

/**
 * lambdock_stack_list_directory:
 * @path: Directory to list ("~" is expanded).
 *
 * Returns the visible (non dot-file) children of @path, directories first and
 * then case-insensitively by name, capped at STACK_MAX_ENTRIES.
 *
 * Returns: (transfer full): GPtrArray of LambdockStackEntry*.
 */
GPtrArray *lambdock_stack_list_directory(const char *path);

/**
 * lambdock_stack_list_directory_limited:
 * @path: Directory to list ("~" is expanded).
 * @limit: Maximum number of entries to return.
 * @truncated: (out) (optional): set to TRUE when @path holds more entries than
 *   @limit, so the panel can say so instead of quietly hiding the rest.
 *
 * The same listing as lambdock_stack_list_directory(), with the cap and the
 * "there is more" answer made explicit.
 *
 * Returns: (transfer full): GPtrArray of LambdockStackEntry*.
 */
GPtrArray *lambdock_stack_list_directory_limited(const char *path, guint limit,
                                                 gboolean *truncated);

/**
 * lambdock_stack_child_path:
 * @dir: (nullable): the folder being shown.
 * @name: (nullable): a child's name.
 *
 * Returns: (transfer full) (nullable): the path of @name inside @dir, or NULL
 * when either is empty.
 */
char *lambdock_stack_child_path(const char *dir, const char *name);

/**
 * lambdock_stack_parent_path:
 * @path: (nullable): the folder being shown.
 * @root: (nullable): the stack's own folder, which is the ceiling.
 *
 * The folder one level above @path, or NULL when @path is already @root (or
 * above it), which is what the panel's back button is shown from. Trailing
 * slashes are ignored, so "/a/b/" and "/a/b" are the same place.
 *
 * Returns: (transfer full) (nullable): the parent folder, or NULL.
 */
char *lambdock_stack_parent_path(const char *path, const char *root);

/**
 * lambdock_stack_breadcrumb:
 * @path: (nullable): the folder being shown.
 * @root: (nullable): the stack's own folder.
 * @root_label: (nullable): what to call @root; defaults to its base name.
 *
 * The title of the panel: "Downloads" at the root, "Downloads / Shots / 2024"
 * after walking into two folders.
 *
 * Returns: (transfer full): the breadcrumb, never NULL.
 */
char *lambdock_stack_breadcrumb(const char *path, const char *root,
                                const char *root_label);

/**
 * lambdock_stack_thumbnail_for_path:
 * @path: A local file path.
 * @size: Target width/height of the square thumbnail, in pixels.
 *
 * Decodes @path into a square, centre-cropped preview. Only real images are
 * decoded: a directory, a non-image file, an unreadable file or an image above
 * the size limits returns NULL, and the caller falls back to a file-type icon.
 *
 * Returns: (transfer full) (nullable): a square preview, or NULL.
 */
GdkPaintable *lambdock_stack_thumbnail_for_path(const char *path, int size);

/**
 * lambdock_stack_entry_free:
 * @entry: (nullable): a single entry.
 */
void lambdock_stack_entry_free(LambdockStackEntry *entry);

/**
 * lambdock_stack_entries_free:
 * @entries: (nullable): array returned by lambdock_stack_list_directory().
 */
void lambdock_stack_entries_free(GPtrArray *entries);

/**
 * lambdock_stack_view_from_string / lambdock_stack_view_to_string:
 * Convert between the Scheme symbol names ("grid", "list") and the
 * enum. Unknown strings fall back to LAMBDOCK_STACK_VIEW_GRID.
 */
LambdockStackView lambdock_stack_view_from_string(const char *name);
const char *lambdock_stack_view_to_string(LambdockStackView view);

/**
 * lambdock_stack_view_new:
 * @entries: Entries to show (borrowed, not retained).
 * @view: Layout to use.
 * @icon_size: Pixel size for the entry icons.
 *
 * Builds the widget that renders @entries in the requested view. Grid and list
 * views are scrollable containers of buttons.
 *
 * Returns: (transfer none): the view widget.
 */
GtkWidget *lambdock_stack_view_new(GPtrArray *entries, LambdockStackView view,
                                   int icon_size);

/**
 * lambdock_stack_panel_new:
 * @item: A stack dock item.
 * @path: (nullable): the folder to show; the stack's own folder when NULL.
 *
 * Builds the stack panel: the header (back, breadcrumb, count, view toggle, open
 * folder), the grid or list of @path's entries, and the note that says when a
 * folder holds more than STACK_MAX_ENTRIES. Clicking a folder walks into it
 * inside the panel; the file manager stays one header button away.
 *
 * Returns: (transfer none) (nullable): the panel widget, or NULL for a NULL item.
 */
GtkWidget *lambdock_stack_panel_new(DockItem *item, const char *path);

/**
 * lambdock_stack_show:
 * @item: A stack dock item.
 *
 * Fills the item's popover with the current contents of its folder and pops it
 * up. An empty folder shows an explanatory label, which is friendlier than the
 * click doing nothing at all.
 */
void lambdock_stack_show(DockItem *item);

/**
 * lambdock_stack_set_view:
 * @item: A stack dock item.
 * @view: New layout.
 *
 * Switches the stack view mode, redraws the dock item and remembers the choice
 * in the dock layout overrides so it survives a restart.
 */
void lambdock_stack_set_view(DockItem *item, LambdockStackView view);

/**
 * lambdock_stack_expand_path:
 * @path: A path that may start with "~".
 *
 * Returns: (transfer full) (nullable): the path with the home directory
 * expanded.
 */
char *lambdock_stack_expand_path(const char *path);

/**
 * lambdock_stack_view_focus_step:
 * @view: a stack grid (GtkFlowBox) or list (GtkBox) of entry buttons.
 * @dx: how many entries to move sideways (-1, 0 or 1).
 * @dy: how many rows to move (-1, 0 or 1).
 *
 * Moves the keyboard focus inside an open stack. The grid steps a whole row for
 * Up/Down, using the columns it actually laid out; the list steps by one. With
 * nothing focused, either direction starts at the first entry.
 *
 * Returns: TRUE when the focus moved (or was already at that end).
 */
gboolean lambdock_stack_view_focus_step(GtkWidget *view, int dx, int dy);

/**
 * lambdock_stack_view_focus_edge:
 * Focuses the first (or, with @last, the last) entry of a stack view.
 */
gboolean lambdock_stack_view_focus_edge(GtkWidget *view, gboolean last);

#endif // STACK_H
