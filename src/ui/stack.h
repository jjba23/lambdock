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

#ifndef STACK_H
#define STACK_H

#include "../lambdock.h"

/* A stack never renders more than this many entries: a folder with 10k files
 * must not turn a popover into a memory and layout disaster. */
#define STACK_MAX_ENTRIES 300

/* The fan view stops at a readable handful of cards; the rest stay reachable
 * through the folder itself (and through the grid/list views). */
#define STACK_FAN_MAX_CARDS 16

/**
 * LambdockStackView:
 * @LAMBDOCK_STACK_VIEW_GRID: Icon grid (icon + name tiles).
 * @LAMBDOCK_STACK_VIEW_LIST: One row per entry, like a file manager list.
 * @LAMBDOCK_STACK_VIEW_FAN: macOS-style fanned arc of tilted cards.
 *
 * How a stack lays out the contents of its folder.
 */
typedef enum {
  LAMBDOCK_STACK_VIEW_GRID = 0,
  LAMBDOCK_STACK_VIEW_LIST,
  LAMBDOCK_STACK_VIEW_FAN
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
 * Convert between the Scheme symbol names ("grid", "list", "fan") and the
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
 * views are scrollable containers of buttons; the fan view is a custom widget
 * that paints tilted cards and hit-tests them itself, wrapped in a horizontal
 * scroller so a large folder stays reachable at a readable card size.
 *
 * Returns: (transfer none): the view widget.
 */
GtkWidget *lambdock_stack_view_new(GPtrArray *entries, LambdockStackView view,
                                   int icon_size);

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
 * lambdock_stack_fan_contains:
 * @x: Pointer x, in widget coordinates.
 * @y: Pointer y, in widget coordinates.
 * @cx: Centre of the card, x.
 * @cy: Centre of the card, y.
 * @angle_degrees: Rotation of the card around its centre.
 * @width: Card width.
 * @height: Card height.
 *
 * The fan view paints rotated cards itself, so it has to hit-test them itself
 * too. This is that test: rotate the point back into the card's own frame and
 * compare it with the card rectangle.
 *
 * Returns: TRUE when the point lies inside the rotated card.
 */
gboolean lambdock_stack_fan_contains(double x, double y, double cx, double cy,
                                     double angle_degrees, double width,
                                     double height);

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
