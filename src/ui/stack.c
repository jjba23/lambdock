// lambdock -- hyper-hackable desktop dock for Wayland
//
// SPDX-License-Identifier: MIT
// Copyright © Josep Bigorra <jjbigorra@gmail.com>

#include "stack.h"
#include "../accessibility.h"
#include "../guile_bridge.h"
#include "../logger.h"
#include "desktop_entry.h"
#include "dock_item.h"
#include "icon_resolver.h"
#include <gdk-pixbuf/gdk-pixbuf.h>
#include <glib/gi18n.h>
#include <glib/gstdio.h>
#include <math.h>
#include <string.h>

/* ============================================================================
 * FOLDER LISTING
 * ============================================================================
 */

/* The listing itself -- lambdock_stack_entry_free(), compare_stack_entries(),
 * lambdock_stack_list_directory(), its _limited() form and
 * lambdock_stack_entries_free() -- is in rust/src/ui/stack.rs: reading a directory,
 * hiding dot files, capping the listing and sorting folders first is a decision
 * about a folder, not about a widget. What stays here is what draws the result.
 */

/* ============================================================================
 * THE VIEWS -- in rust/src/ui/stack.rs
 * ============================================================================
 * Everything between the listing and the panel is Rust now. The two builders
 * with their cells, the entry's icon, the click that walks into a folder or
 * hands a file to the desktop, the accessibility text a cell carries, the
 * scrolled view that keeps a folder from growing past the screen, and the queue
 * that decodes a cell's picture one at a time off the main loop: a view is a
 * decision about entries, so it is tested there with a real directory and a real
 * PNG.
 *
 * What the panel below uses is two entry points -- lambdock_stack_view_new() for
 * a detached view and lambdock_stack_view_for_item() for one that walks folders
 * -- and lambdock_stack_popover_rebuild(), which is what a cell calls when it is
 * a folder. The rebuild is still here, one step behind the views.
 */

/* ============================================================================
 * THE STACK POPOVER
 * ============================================================================
 *
 * The panel is a small file browser, not a printout: it opens on the stack's own
 * folder, walks into any folder below it (clicking a folder used to hand it to
 * the file manager), walks back out, and can be flipped between the grid and the
 * list from its own header. Where it is looking is stored on the popover, so a
 * rebuild -- a view switch, a walk in or out -- stays in place.
 *
 * The file manager is still one button away, on the folder being shown.
 */

/* The folder the open panel is looking at, and the view widget it built, both
 * stored on the popover so a rebuild (and the keyboard focus on open) does not
 * need to re-derive them. */
#define STACK_PATH_KEY "lambdock-stack-path"
#define STACK_VIEW_WIDGET_KEY "lambdock-stack-view"

static void stack_popover_focus(GtkPopover *popover);
static void on_stack_back_clicked(GtkButton *button, gpointer user_data);
static void on_stack_open_folder(GtkButton *button, gpointer user_data);
static void on_stack_view_toggle_clicked(GtkButton *button, gpointer user_data);
static GtkWidget *stack_first_entry(GtkWidget *view);

/** The folder the open panel is showing; the stack's own when it never moved. */
static const char *stack_current_path(DockItem *item) {
  const char *stored =
      (item && item->popover) ? g_object_get_data(G_OBJECT(item->popover),
                                                  STACK_PATH_KEY)
                              : NULL;

  if (stored && *stored != '\0')
    return stored;

  return item ? item->stack_path : NULL;
}

/* A navigation payload: the item, and where to go. */
typedef struct {
  DockItem *item;
  char *path;
} StackNav;

static void free_stack_nav(gpointer data, GClosure *closure) {
  (void)closure;
  StackNav *nav = (StackNav *)data;
  g_free(nav->path);
  g_free(nav);
}

/** One small icon-only header button. */
static GtkWidget *stack_header_button(const char *icon_name, const char *label,
                                      const char *description) {
  GtkWidget *button = gtk_button_new_from_icon_name(icon_name);

  gtk_widget_add_css_class(button, "flat");
  gtk_widget_add_css_class(button, "stack-header-button");
  gtk_widget_set_tooltip_text(button, label);
  lambdock_accessible_update(button, label, description);

  return button;
}

/** Header: back (inside a folder), where we are, how much, and the controls. */
static GtkWidget *stack_panel_header(DockItem *item, const char *path,
                                     guint count, gboolean truncated) {
  GtkWidget *header = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
  gtk_widget_add_css_class(header, "stack-header");

  g_autofree char *parent = lambdock_stack_parent_path(path, item->stack_path);
  if (parent) {
    GtkWidget *back = stack_header_button(
        "go-previous-symbolic", _("Back"),
        _("Goes back up one folder, without leaving the panel."));
    gtk_widget_add_css_class(back, "stack-back");

    /* The payload is where the panel is, not where back goes: the handler asks
     * for the parent, so it never walks out of the stack by accident. */
    StackNav *nav = g_new0(StackNav, 1);
    nav->item = item;
    nav->path = g_strdup(path);
    g_signal_connect_data(back, "clicked", G_CALLBACK(on_stack_back_clicked),
                          nav, free_stack_nav, 0);
    gtk_box_append(GTK_BOX(header), back);
  }

  g_autofree char *crumb = lambdock_stack_breadcrumb(path, item->stack_path,
                                                     item->name);
  GtkWidget *title = gtk_label_new(crumb);
  gtk_widget_add_css_class(title, "title");
  gtk_widget_add_css_class(title, "stack-title");
  gtk_label_set_ellipsize(GTK_LABEL(title), PANGO_ELLIPSIZE_MIDDLE);
  gtk_label_set_max_width_chars(GTK_LABEL(title), 32);
  gtk_box_append(GTK_BOX(header), title);

  /* "300+" when the folder holds more than the panel is willing to list. */
  g_autofree char *count_text =
      truncated ? g_strdup_printf(_("%u+ items"), count)
                : g_strdup_printf(ngettext("%u item", "%u items", count),
                                  count);
  GtkWidget *count_label = gtk_label_new(count_text);
  gtk_widget_add_css_class(count_label, "dim-label");
  gtk_widget_add_css_class(count_label, "stack-count");
  gtk_box_append(GTK_BOX(header), count_label);

  GtkWidget *spacer = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
  gtk_widget_set_hexpand(spacer, TRUE);
  gtk_box_append(GTK_BOX(header), spacer);

  /* Flip the panel between the grid and the list without going back to the
   * dock's context menu; the choice is the same one, and it persists. */
  LambdockStackView view = lambdock_stack_view_from_string(item->stack_view);
  gboolean listing =
      (view == LAMBDOCK_STACK_VIEW_LIST); /* what a click would switch to */
  GtkWidget *toggle = stack_header_button(
      listing ? "view-grid-symbolic" : "view-list-symbolic",
      listing ? _("View content as a grid") : _("View content as a list"),
      _("Changes how this folder is laid out, and remembers it."));
  gtk_widget_add_css_class(toggle, "stack-view-toggle");
  g_signal_connect(toggle, "clicked", G_CALLBACK(on_stack_view_toggle_clicked),
                   item);
  gtk_box_append(GTK_BOX(header), toggle);

  GtkWidget *open = stack_header_button(
      "folder-open-symbolic", _("Open folder"),
      _("Opens this folder in the file manager."));
  gtk_widget_add_css_class(open, "stack-open");

  StackNav *open_nav = g_new0(StackNav, 1);
  open_nav->item = item;
  open_nav->path = g_strdup(path ? path : "");
  g_signal_connect_data(open, "clicked", G_CALLBACK(on_stack_open_folder),
                        open_nav, free_stack_nav, 0);
  gtk_box_append(GTK_BOX(header), open);

  return header;
}

GtkWidget *lambdock_stack_panel_new(DockItem *item, const char *path) {
  if (!item)
    return NULL;

  int icon_size =
      (item->state && item->state->icon_size > 0) ? item->state->icon_size
                                                  : DEFAULT_ICON_SIZE;
  LambdockStackView view = lambdock_stack_view_from_string(item->stack_view);

  gboolean truncated = FALSE;
  GPtrArray *entries = lambdock_stack_list_directory_limited(
      path ? path : item->stack_path, STACK_MAX_ENTRIES, &truncated);
  guint count = entries ? entries->len : 0;

  GtkWidget *vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
  /* Focusable so the popover can take the keyboard when it opens: Escape then
   * arrives at the popover's controller instead of being lost to the dock. */
  gtk_widget_set_focusable(vbox, TRUE);
  gtk_widget_set_margin_start(vbox, POPOVER_VBOX_MARGIN);
  gtk_widget_set_margin_end(vbox, POPOVER_VBOX_MARGIN);
  gtk_widget_set_margin_top(vbox, POPOVER_VBOX_MARGIN);
  gtk_widget_set_margin_bottom(vbox, POPOVER_VBOX_MARGIN);
  gtk_widget_add_css_class(vbox, "stack-popover");
  lambdock_accessible_update(vbox, _("Folder contents"), NULL);

  gtk_box_append(GTK_BOX(vbox),
                 stack_panel_header(item, path, count, truncated));
  gtk_box_append(GTK_BOX(vbox), gtk_separator_new(GTK_ORIENTATION_HORIZONTAL));

  GtkWidget *view_widget =
      lambdock_stack_view_for_item(entries, view, icon_size, item);
  gtk_box_append(GTK_BOX(vbox), view_widget);
  /* Remembered on the panel so opening it can put the keyboard focus straight on
   * the first entry instead of on the panel around it. */
  g_object_set_data(G_OBJECT(vbox), STACK_VIEW_WIDGET_KEY, view_widget);

  if (truncated) {
    g_autofree char *note =
        g_strdup_printf(_("Only the first %u entries are shown."),
                        (guint)STACK_MAX_ENTRIES);
    GtkWidget *label = gtk_label_new(note);
    gtk_widget_add_css_class(label, "dim-label");
    gtk_widget_add_css_class(label, "stack-note");
    gtk_widget_set_halign(label, GTK_ALIGN_START);
    gtk_box_append(GTK_BOX(vbox), label);
  }

  lambdock_stack_entries_free(entries);
  return vbox;
}

/** Builds the panel for @path and installs it in the item's popover. */
static void stack_popover_fill(DockItem *item, const char *path) {
  if (!item || !item->popover)
    return;

  GtkWidget *panel = lambdock_stack_panel_new(item, path);
  if (!panel)
    return;

  g_object_set_data_full(G_OBJECT(item->popover), STACK_PATH_KEY,
                         g_strdup(path ? path : ""), g_free);
  /* The item's popover is also the right-click menu, so the stack panel has to
   * say who it is: the themes style this panel differently from the HUD menus,
   * and popover_menu.c takes the class off again when it installs a menu. */
  gtk_widget_add_css_class(item->popover, "stack-panel");
  gtk_popover_set_child(GTK_POPOVER(item->popover), panel);
}

/** Rebuilds the open panel at @path and lands the keyboard on its first entry. */
void lambdock_stack_popover_rebuild(DockItem *item, const char *path) {
  if (!item || !item->popover)
    return;

  stack_popover_fill(item, path);
  stack_popover_focus(GTK_POPOVER(item->popover));
}

/* Adapter: walk back up one folder. */
static void on_stack_back_clicked(GtkButton *button, gpointer user_data) {
  (void)button;
  StackNav *nav = (StackNav *)user_data;
  if (!nav || !nav->item)
    return;

  g_autofree char *parent =
      lambdock_stack_parent_path(nav->path, nav->item->stack_path);
  if (parent)
    lambdock_stack_popover_rebuild(nav->item, parent);
}

/* Adapter: open the folder being shown in the file manager. */
static void on_stack_open_folder(GtkButton *button, gpointer user_data) {
  (void)button;
  StackNav *nav = (StackNav *)user_data;
  if (!nav || !nav->item)
    return;

  if (!lambdock_open_path(nav->path))
    LOG_C_WARN("UI", "Could not open the folder '%s'",
               nav->path ? nav->path : "");
}

/* Adapter: flip between the grid and the list. */
static void on_stack_view_toggle_clicked(GtkButton *button, gpointer user_data) {
  (void)button;
  DockItem *item = (DockItem *)user_data;
  if (!item)
    return;

  LambdockStackView current =
      lambdock_stack_view_from_string(item->stack_view);
  lambdock_stack_set_view(item,
                          current == LAMBDOCK_STACK_VIEW_LIST
                              ? LAMBDOCK_STACK_VIEW_GRID
                              : LAMBDOCK_STACK_VIEW_LIST);
}

/* Moves the keyboard focus into an open stack popover. Without it the focus can
 * stay on the dock icon, and Escape reaches the dock window instead of the
 * popover that is covering it.
 *
 * The first entry is the useful place to land, so a keyboard user can start
 * walking the folder immediately; the panel itself is only the fallback for an
 * empty folder. Escape still bubbles from the entry to the popover's controller. */
static void stack_popover_focus(GtkPopover *popover) {
  GtkWidget *child = gtk_popover_get_child(popover);
  if (!child)
    return;

  GtkWidget *view = g_object_get_data(G_OBJECT(child), STACK_VIEW_WIDGET_KEY);
  GtkWidget *target = NULL;

  if (view && GTK_IS_FLOW_BOX(view)) {
    GtkFlowBoxChild *first =
        gtk_flow_box_get_child_at_index(GTK_FLOW_BOX(view), 0);
    if (first) {
      GtkWidget *inner = gtk_flow_box_child_get_child(first);
      target = inner ? inner : GTK_WIDGET(first);
    }
  } else if (view && GTK_IS_SCROLLED_WINDOW(view)) {
    /* The grid and the list come wrapped in a scrolled window. */
    GtkWidget *inner =
        gtk_scrolled_window_get_child(GTK_SCROLLED_WINDOW(view));
    if (inner && GTK_IS_VIEWPORT(inner))
      inner = gtk_viewport_get_child(GTK_VIEWPORT(inner));
    target = inner ? stack_first_entry(inner) : NULL;
  } else if (view && GTK_IS_BOX(view)) {
    target = stack_first_entry(view);
  } else if (view) {
    target = view; /* A custom view is a single focusable widget. */
  }

  if (!target || !gtk_widget_get_focusable(target))
    target = child;

  if (gtk_widget_get_focusable(target))
    gtk_widget_grab_focus(target);
}

/** The first focusable entry of a view: a grid cell's button, or a list row. */
static GtkWidget *stack_first_entry(GtkWidget *view) {
  if (!view)
    return NULL;

  for (GtkWidget *child = gtk_widget_get_first_child(view); child != NULL;
       child = gtk_widget_get_next_sibling(child)) {
    if (GTK_IS_FLOW_BOX_CHILD(child)) {
      GtkWidget *inner = gtk_flow_box_child_get_child(
          GTK_FLOW_BOX_CHILD(child));
      if (inner && gtk_widget_get_focusable(inner))
        return inner;
      continue;
    }

    if (gtk_widget_get_focusable(child))
      return child;
  }

  return NULL;
}

void lambdock_stack_show(DockItem *item) {
  if (!item || !item->popover)
    return;

  stack_popover_fill(item, item->stack_path);

  if (!gtk_widget_get_visible(item->popover) && item->state)
    item->state->active_popovers++;

  gtk_popover_popup(GTK_POPOVER(item->popover));
  stack_popover_focus(GTK_POPOVER(item->popover));
}

void lambdock_stack_set_view(DockItem *item, LambdockStackView view) {
  if (!item)
    return;

  const char *name = lambdock_stack_view_to_string(view);
  if (g_strcmp0(item->stack_view, name) == 0)
    return;

  g_free(item->stack_view);
  item->stack_view = g_strdup(name);

  if (item->state) {
    char *key = dock_item_key(item);
    dock_layout_set_view(item->state->config_path, key, name);
    g_free(key);
  }

  /* If the stack is on screen, redraw it in the new view mode right away --
   * where it is looking, not necessarily at its root. */
  if (item->popover && gtk_widget_get_visible(item->popover))
    lambdock_stack_popover_rebuild(item, stack_current_path(item));

  LOG_C_INFO("UI", "Stack '%s' now uses the %s view",
             item->name ? item->name : "stack", name);
}
