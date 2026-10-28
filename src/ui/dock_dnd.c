// lambdock -- hyper-hackable desktop dock for Wayland
//
// SPDX-License-Identifier: MIT
// Copyright © Josep Bigorra <jjbigorra@gmail.com>

#include "dock_dnd.h"
#include "../dock_window.h"
#include "../guile_bridge.h"
#include "../logger.h"
#include "desktop_entry.h"
#include "dock_item.h"
#include "stack.h"
#include "trash.h"

/* ============================================================================
 * PURE LIST / GEOMETRY HELPERS
 * ============================================================================
 */

GList *dock_list_move(GList *list, gpointer data, int index) {
  GList *link = g_list_find(list, data);
  if (!link)
    return list;

  int length = (int)g_list_length(list);

  list = g_list_delete_link(list, link);

  /* After the removal the list is one element shorter, so the last valid
   * insertion index is length - 1 (which appends). */
  if (index < 0)
    index = 0;
  if (index > length - 1)
    index = length - 1;

  return g_list_insert(list, data, index);
}

int dock_drop_index_for_position(const double *centers, int n,
                                 double position) {
  if (!centers || n <= 0)
    return 0;

  for (int i = 0; i < n; i++) {
    if (position < centers[i])
      return i;
  }
  return n;
}

/* ============================================================================
 * REMOVAL
 * ============================================================================
 */

void dock_dnd_persist_order(LambdockState *state) {
  if (!state || !state->config_path)
    return;

  GList *keys = NULL;
  for (GList *l = state->dock_items; l != NULL; l = l->next) {
    DockItem *item = (DockItem *)l->data;
    if (!item || item->kind == LAMBDOCK_ITEM_SEPARATOR)
      continue; /* No stable identity: re-placed between its neighbours. */

    char *key = dock_item_key(item);
    if (key)
      keys = g_list_append(keys, key);
  }

  dock_layout_set_order(state->config_path, keys);
  g_list_free_full(keys, g_free);
}

void remove_dock_item(LambdockState *state, DockItem *item) {
  if (!item)
    return;

  if (!state)
    state = item->state;

  if (state) {
    if (item->kind != LAMBDOCK_ITEM_SEPARATOR) {
      char *key = dock_item_key(item);
      dock_layout_remove_item(state->config_path, key);
      g_free(key);
    }
    state->dock_items = g_list_remove(state->dock_items, item);
  }

  LOG_C_INFO("UI", "Removing dock item: %s",
             item->name && *item->name != '\0' ? item->name : "(unnamed)");

  free_dock_item(item);

  if (state) {
    update_dock_dimensions(state);
    dock_dnd_persist_order(state);
  }
}

/* A drag that ends outside every drop target removes the dragged item. It runs
 * from an idle callback because the widget must not disappear while the drag
 * machinery is still unwinding, and it looks the item up through the button's
 * data so a config reload in between cannot leave it with a dangling pointer. */
static gboolean on_drop_outside_idle(gpointer user_data) {
  GtkWidget *button = GTK_WIDGET(user_data);
  DockItem *item = dock_item_from_widget(button);

  if (item)
    remove_dock_item(item->state, item);

  return G_SOURCE_REMOVE;
}

void schedule_remove_dock_item(DockItem *item) {
  if (!item || !item->button)
    return;

  if (item->popover && gtk_widget_get_visible(item->popover))
    gtk_popover_popdown(GTK_POPOVER(item->popover));

  g_idle_add_full(G_PRIORITY_DEFAULT_IDLE, on_drop_outside_idle,
                  g_object_ref(item->button), g_object_unref);
}

/* ============================================================================
 * DRAG SOURCES (ONE PER DOCK ITEM)
 * ============================================================================
 */

static GdkContentProvider *on_drag_prepare(GtkDragSource *source, double x,
                                           double y, gpointer user_data) {
  (void)source;
  (void)x;
  (void)y;

  DockItem *item = (DockItem *)user_data;
  char *key = dock_item_key(item);
  if (!key)
    return NULL;

  char *payload = g_strconcat(DOCK_DND_ITEM_PREFIX, key, NULL);
  GdkContentProvider *provider =
      gdk_content_provider_new_typed(G_TYPE_STRING, payload);

  g_free(payload);
  g_free(key);
  return provider;
}

static void on_drag_begin(GtkDragSource *source, GdkDrag *drag,
                          gpointer user_data) {
  (void)source;
  (void)drag;

  DockItem *item = (DockItem *)user_data;
  item->dragging = TRUE;

  if (item->button)
    gtk_widget_add_css_class(item->button, "dragging");

  if (item->popover && gtk_widget_get_visible(item->popover))
    gtk_popover_popdown(GTK_POPOVER(item->popover));
}

static void on_drag_end(GtkDragSource *source, GdkDrag *drag,
                        gboolean delete_data, gpointer user_data) {
  (void)source;
  (void)drag;
  (void)delete_data;

  DockItem *item = (DockItem *)user_data;
  item->dragging = FALSE;

  if (item->button)
    gtk_widget_remove_css_class(item->button, "dragging");
}

static gboolean on_drag_cancel(GtkDragSource *source, GdkDrag *drag,
                               GdkDragCancelReason reason, gpointer user_data) {
  (void)source;
  (void)drag;

  DockItem *item = (DockItem *)user_data;
  item->dragging = FALSE;

  if (item->button)
    gtk_widget_remove_css_class(item->button, "dragging");

  if (reason == GDK_DRAG_CANCEL_NO_TARGET && item->button) {
    /* Nothing accepted the drop: the icon was dragged off the dock. */
    schedule_remove_dock_item(item);
    return TRUE;
  }

  /* Escape (or any other cancellation) just aborts the drag. */
  return FALSE;
}

/* ============================================================================
 * DROPPING FILES ON THE TRASH
 * ============================================================================
 */

/**
 * TrashDropJob:
 * @button: The trash item's button, referenced for as long as the job lives, so
 * the item can be found when the drop finishes (a reload may have replaced it
 * in the meantime).
 * @files: The dropped files, each referenced.
 */
typedef struct {
  GtkWidget *button;
  GSList *files;
} TrashDropJob;

static void free_trash_drop_job(gpointer data) {
  TrashDropJob *job = (TrashDropJob *)data;
  g_clear_object(&job->button);
  g_slist_free_full(job->files, g_object_unref);
  g_free(job);
}

/* Trashing a folder, or a file GIO has to copy instead of renaming because its
 * trash is on another filesystem, can take long enough to be noticed. It runs
 * off the main thread for the same reason emptying the trash does: the dock
 * must keep drawing (and auto-hiding) until it is done. */
static void trash_dropped_files_in_thread(GTask *task, gpointer source_object,
                                          gpointer task_data,
                                          GCancellable *cancellable) {
  (void)source_object;
  (void)cancellable;

  TrashDropJob *job = (TrashDropJob *)task_data;
  guint trashed = 0;

  for (GSList *l = job->files; l != NULL; l = l->next) {
    GFile *file = G_FILE(l->data);
    GError *error = NULL;

    if (lambdock_trash_file(file, &error)) {
      trashed++;
    } else {
      char *name = g_file_get_parse_name(file);
      LOG_C_WARN("UI", "Could not trash '%s': %s", name,
                 error ? error->message : "unknown error");
      g_free(name);
      g_clear_error(&error);
    }
  }

  g_task_return_int(task, (gssize)trashed);
}

static void on_trash_drop_finished(GObject *source_object,
                                    GAsyncResult *result, gpointer user_data) {
  (void)source_object;
  (void)user_data;

  GTask *task = G_TASK(result);
  GError *error = NULL;
  gssize trashed = g_task_propagate_int(task, &error);

  if (error) {
    LOG_C_WARN("UI", "Trashing dropped files failed: %s", error->message);
    g_clear_error(&error);
  }

  TrashDropJob *job = (TrashDropJob *)g_task_get_task_data(task);
  if (!job || trashed <= 0)
    return;

  LOG_C_INFO("UI", "Trashed %" G_GSSIZE_FORMAT " of %u dropped item(s)",
             trashed, g_slist_length(job->files));

  DockItem *item = job->button ? dock_item_from_widget(job->button) : NULL;
  if (item)
    dock_item_refresh_trash(item);
}

static gboolean on_trash_drop(GtkDropTarget *target, const GValue *value,
                              double x, double y, gpointer user_data) {
  (void)target;
  (void)x;
  (void)y;

  if (!G_VALUE_HOLDS(value, GDK_TYPE_FILE_LIST))
    return FALSE;

  GdkFileList *dropped = g_value_get_boxed(value);
  if (!dropped)
    return FALSE;

  GSList *files = gdk_file_list_get_files(dropped);
  if (!files)
    return FALSE; /* An empty drag is not a drop the trash can take. */

  GtkWidget *zone = GTK_WIDGET(user_data);
  DockItem *item = dock_item_from_widget(zone);
  if (!item || item->kind != LAMBDOCK_ITEM_TRASH || !item->button)
    return FALSE;

  TrashDropJob *job = g_new0(TrashDropJob, 1);
  job->button = g_object_ref(item->button);
  for (GSList *l = files; l != NULL; l = l->next)
    job->files = g_slist_prepend(job->files, g_object_ref(l->data));
  job->files = g_slist_reverse(job->files);

  GTask *task = g_task_new(NULL, NULL, on_trash_drop_finished, NULL);
  g_task_set_task_data(task, job, free_trash_drop_job);
  g_task_run_in_thread(task, trash_dropped_files_in_thread);
  g_object_unref(task);

  return TRUE;
}

/* Makes a trash item accept files dragged onto it. The target sits on the item's
 * whole container (reflection included), which is also what makes it win over
 * the dock-wide target: GTK looks for a drop target from the widget under the
 * pointer upwards, and the first one that accepts the payload gets the drop. A
 * drag of a dock icon carries a string the trash ignores, so reordering across
 * the trash still goes to the dock. */
static void dock_dnd_setup_trash_drop(DockItem *item) {
  if (!item || !item->button)
    return;

  GtkWidget *zone = item->container ? item->container : item->button;

  /* COPY is accepted as well as MOVE: a file manager dragging from a read-only
   * location may only offer COPY, and either way the file ends up trashed. */
  GtkDropTarget *target = gtk_drop_target_new(
      GDK_TYPE_FILE_LIST, GDK_ACTION_COPY | GDK_ACTION_MOVE);
  g_signal_connect(target, "drop", G_CALLBACK(on_trash_drop), zone);
  gtk_widget_add_controller(zone, GTK_EVENT_CONTROLLER(target));
}

void dock_dnd_setup_item(DockItem *item) {
  if (!item || !item->button)
    return;

  GtkDragSource *source = gtk_drag_source_new();
  gtk_drag_source_set_actions(source, GDK_ACTION_MOVE);

  g_signal_connect(source, "prepare", G_CALLBACK(on_drag_prepare), item);
  g_signal_connect(source, "drag-begin", G_CALLBACK(on_drag_begin), item);
  g_signal_connect(source, "drag-end", G_CALLBACK(on_drag_end), item);
  g_signal_connect(source, "drag-cancel", G_CALLBACK(on_drag_cancel), item);

  gtk_widget_add_controller(item->button, GTK_EVENT_CONTROLLER(source));

  if (item->kind == LAMBDOCK_ITEM_TRASH)
    dock_dnd_setup_trash_drop(item);
}

/* ============================================================================
 * REORDERING
 * ============================================================================
 */

static DockItem *find_item_by_key(LambdockState *state, const char *key) {
  if (!state || !key)
    return NULL;

  for (GList *l = state->dock_items; l != NULL; l = l->next) {
    DockItem *item = (DockItem *)l->data;
    char *item_key = dock_item_key(item);
    gboolean match = (g_strcmp0(item_key, key) == 0);
    g_free(item_key);

    if (match)
      return item;
  }
  return NULL;
}

gboolean dock_drop_handle_item(LambdockState *state, const char *key, double x,
                               double y) {
  if (!state || !state->box || !key)
    return FALSE;

  DockItem *item = find_item_by_key(state, key);
  if (!item) {
    LOG_C_WARN("UI", "Dropped dock item is not part of this dock: %s", key);
    return FALSE;
  }

  graphene_point_t window_point = GRAPHENE_POINT_INIT((float)x, (float)y);
  graphene_point_t box_point;
  if (!gtk_widget_compute_point(state->window, state->box, &window_point,
                                &box_point))
    return FALSE;

  int count = 0;
  for (GtkWidget *child = gtk_widget_get_first_child(state->box); child;
       child = gtk_widget_get_next_sibling(child))
    count++;

  if (count == 0)
    return FALSE;

  gboolean horizontal = (state->orientation == GTK_ORIENTATION_HORIZONTAL);
  double *centers = g_new0(double, count);
  int slot = 0;
  for (GtkWidget *child = gtk_widget_get_first_child(state->box); child;
       child = gtk_widget_get_next_sibling(child)) {
    graphene_rect_t bounds;
    if (gtk_widget_compute_bounds(child, state->box, &bounds)) {
      centers[slot] = horizontal ? bounds.origin.x + bounds.size.width / 2.0
                                 : bounds.origin.y + bounds.size.height / 2.0;
    }
    slot++;
  }

  int drop_index = dock_drop_index_for_position(
      centers, count, horizontal ? box_point.x : box_point.y);
  g_free(centers);

  int current = g_list_index(state->dock_items, item);
  if (current < 0)
    return FALSE;

  /* Removing the item first shifts everything after it one slot left. */
  int destination = (drop_index > current) ? drop_index - 1 : drop_index;
  if (destination > count - 1)
    destination = count - 1;
  if (destination == current)
    return TRUE;

  state->dock_items = dock_list_move(state->dock_items, item, destination);

  /* Re-order the box children to match the list. */
  GtkWidget *previous = NULL;
  for (GList *l = state->dock_items; l != NULL; l = l->next) {
    DockItem *each = (DockItem *)l->data;
    GtkWidget *child = each->container ? each->container : each->button;
    if (!child)
      continue;

    gtk_box_reorder_child_after(GTK_BOX(state->box), child, previous);
    previous = child;
  }

  dock_dnd_persist_order(state);

  LOG_C_INFO("UI", "Moved dock item '%s' to position %d", item->name ? item->name : "",
             destination);
  return TRUE;
}

/* ============================================================================
 * DROPPING FILES (ADD ITEMS)
 * ============================================================================
 */

static gboolean dock_add_file(LambdockState *state, GFile *file) {
  if (!state || !file)
    return FALSE;

  char *path = g_file_get_path(file);
  if (!path)
    return FALSE;

  gboolean added = FALSE;

  if (g_file_test(path, G_FILE_TEST_IS_DIR)) {
    /* A folder becomes a stack, which is the dock's version of the macOS
     * Downloads stack. */
    char *name = g_path_get_basename(path);
    create_stack_dock_item(state, name, "folder", path, "grid", TRUE);
    dock_layout_add_item(state->config_path, "stack", name, "folder", path,
                         "grid");
    LOG_C_INFO("UI", "Added stack for folder: %s", path);
    g_free(name);
    added = TRUE;
  } else if (g_str_has_suffix(path, ".desktop")) {
    LambdockDesktopEntry *entry = lambdock_desktop_entry_new_from_file(path);
    if (entry) {
      gboolean known = (find_dock_item_by_app_id(state, entry->id) != NULL) ||
                       (find_dock_item_by_app_id(state, entry->name) != NULL);

      if (known) {
        LOG_C_INFO("UI", "Application is already in the dock: %s",
                   entry->name);
      } else {
        create_dock_item(state, entry->name, entry->icon, entry->exec,
                         entry->id, TRUE, TRUE);
        dock_layout_add_item(state->config_path, "app", entry->name,
                             entry->icon, entry->exec, "");
        LOG_C_INFO("UI", "Added dock item from desktop entry: %s (%s)",
                   entry->name, entry->exec);
        added = TRUE;
      }
      lambdock_desktop_entry_free(entry);
    } else {
      LOG_C_WARN("UI", "Not a usable desktop entry: %s", path);
    }
  } else {
    LOG_C_INFO("UI", "Ignoring dropped file (only .desktop files and folders "
                     "become dock items): %s",
               path);
  }

  g_free(path);
  return added;
}

static gboolean on_dock_drop(GtkDropTarget *target, const GValue *value,
                             double x, double y, gpointer user_data) {
  (void)target;
  LambdockState *state = (LambdockState *)user_data;
  if (!state)
    return FALSE;

  if (G_VALUE_HOLDS(value, G_TYPE_STRING)) {
    const char *payload = g_value_get_string(value);
    if (payload && g_str_has_prefix(payload, DOCK_DND_ITEM_PREFIX)) {
      return dock_drop_handle_item(
          state, payload + strlen(DOCK_DND_ITEM_PREFIX), x, y);
    }
    return FALSE;
  }

  if (G_VALUE_HOLDS(value, GDK_TYPE_FILE_LIST)) {
    GdkFileList *files = g_value_get_boxed(value);
    if (!files)
      return FALSE;

    gboolean added = FALSE;
    for (GSList *l = gdk_file_list_get_files(files); l != NULL; l = l->next)
      added = dock_add_file(state, G_FILE(l->data)) || added;

    if (added) {
      update_dock_dimensions(state);
      dock_dnd_persist_order(state);
    }
    return added;
  }

  return FALSE;
}

void dock_dnd_setup(LambdockState *state) {
  if (!state || !state->window)
    return;

  /* Both kinds of payload are offered to the same drop target: a drag that
   * started inside the dock carries a "lambdock-item:" string, while a drag
   * from a file manager carries a file list. */
  GType drop_types[2];
  drop_types[0] = G_TYPE_STRING;
  drop_types[1] = GDK_TYPE_FILE_LIST;

  GtkDropTarget *target = gtk_drop_target_new(G_TYPE_INVALID, GDK_ACTION_MOVE);
  gtk_drop_target_set_gtypes(target, drop_types, G_N_ELEMENTS(drop_types));

  g_signal_connect(target, "drop", G_CALLBACK(on_dock_drop), state);
  gtk_widget_add_controller(state->window, GTK_EVENT_CONTROLLER(target));

  LOG_C_INFO("UI", "Drag and drop enabled for [%s]",
             state->config_path ? state->config_path : "default");
}
