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

void lambdock_stack_entry_free(LambdockStackEntry *entry) {
  if (!entry)
    return;
  g_free(entry->name);
  g_free(entry->path);
  g_free(entry);
}

char *lambdock_stack_expand_path(const char *path) {
  if (!path || *path == '\0')
    return NULL;

  if (g_strcmp0(path, "~") == 0)
    return g_strdup(g_get_home_dir());

  if (g_str_has_prefix(path, "~/"))
    return g_build_filename(g_get_home_dir(), path + 2, NULL);

  return g_strdup(path);
}

/** @path without trailing slashes (but "/" stays "/"). */
static char *strip_trailing_slashes(const char *path) {
  char *copy = g_strdup(path);
  gsize length = strlen(copy);

  while (length > 1 && copy[length - 1] == '/')
    copy[--length] = '\0';

  return copy;
}

char *lambdock_stack_child_path(const char *dir, const char *name) {
  if (!dir || *dir == '\0' || !name || *name == '\0')
    return NULL;

  return g_build_filename(dir, name, NULL);
}

char *lambdock_stack_parent_path(const char *path, const char *root) {
  if (!path || *path == '\0' || !root || *root == '\0')
    return NULL;

  g_autofree char *here = strip_trailing_slashes(path);
  g_autofree char *ceiling = strip_trailing_slashes(root);

  /* At the stack's own folder there is nowhere to go back to, and a path that
   * is not inside the stack at all is not walked out of. */
  if (g_strcmp0(here, ceiling) == 0 || !g_str_has_prefix(here, ceiling))
    return NULL;

  g_autofree char *parent = g_path_get_dirname(here);
  if (g_strcmp0(parent, ceiling) == 0)
    return g_strdup(ceiling);

  /* Only report a parent that is still inside the stack. */
  if (!g_str_has_prefix(parent, ceiling))
    return g_strdup(ceiling);

  return g_strdup(parent);
}

char *lambdock_stack_breadcrumb(const char *path, const char *root,
                                const char *root_label) {
  g_autofree char *here =
      (path && *path != '\0') ? strip_trailing_slashes(path) : NULL;
  g_autofree char *ceiling =
      (root && *root != '\0') ? strip_trailing_slashes(root) : NULL;

  g_autofree char *fallback = ceiling ? g_path_get_basename(ceiling) : NULL;
  const char *label = (root_label && *root_label != '\0')
                          ? root_label
                          : (fallback ? fallback : _("Stack"));

  /* Not inside the stack (or no stack at all): just say where we are. */
  if (!here || !ceiling || g_strcmp0(here, ceiling) == 0 ||
      !g_str_has_prefix(here, ceiling))
    return g_strdup(label);

  GString *crumb = g_string_new(label);
  const char *rest = here + strlen(ceiling);
  g_auto(GStrv) parts = g_strsplit(rest, "/", -1);

  for (int i = 0; parts[i] != NULL; i++) {
    if (*parts[i] == '\0')
      continue;
    g_string_append(crumb, " / ");
    g_string_append(crumb, parts[i]);
  }

  return g_string_free(crumb, FALSE);
}

static gint compare_stack_entries(gconstpointer a, gconstpointer b) {
  const LambdockStackEntry *entry_a = *(const LambdockStackEntry *const *)a;
  const LambdockStackEntry *entry_b = *(const LambdockStackEntry *const *)b;

  /* Folders first, then names: the order a file manager would show. */
  if (entry_a->is_dir != entry_b->is_dir)
    return entry_a->is_dir ? -1 : 1;

  return g_utf8_collate(entry_a->name, entry_b->name);
}

GPtrArray *lambdock_stack_list_directory_limited(const char *path, guint limit,
                                                 gboolean *truncated) {
  GPtrArray *entries = g_ptr_array_new_with_free_func(
      (GDestroyNotify)lambdock_stack_entry_free);

  if (truncated)
    *truncated = FALSE;

  if (limit == 0)
    limit = STACK_MAX_ENTRIES;

  char *dir_path = lambdock_stack_expand_path(path);
  if (!dir_path)
    return entries;

  GDir *dir = g_dir_open(dir_path, 0, NULL);
  if (!dir) {
    LOG_C_WARN("UI", "Stack path is not a readable directory: %s", dir_path);
    g_free(dir_path);
    return entries;
  }

  const char *filename;
  while ((filename = g_dir_read_name(dir)) != NULL) {
    if (filename[0] == '.')
      continue; /* Hidden files stay hidden, as in any file manager. */

    /* One name past the cap is enough to know the folder has more: the panel
     * says so instead of silently hiding the rest. */
    if (entries->len >= limit) {
      if (truncated)
        *truncated = TRUE;
      break;
    }

    LambdockStackEntry *entry = g_new0(LambdockStackEntry, 1);
    entry->name = g_strdup(filename);
    entry->path = g_build_filename(dir_path, filename, NULL);
    entry->is_dir = g_file_test(entry->path, G_FILE_TEST_IS_DIR);
    g_ptr_array_add(entries, entry);
  }

  g_dir_close(dir);
  g_free(dir_path);

  g_ptr_array_sort(entries, compare_stack_entries);
  return entries;
}

GPtrArray *lambdock_stack_list_directory(const char *path) {
  return lambdock_stack_list_directory_limited(path, STACK_MAX_ENTRIES, NULL);
}

void lambdock_stack_entries_free(GPtrArray *entries) {
  if (entries)
    g_ptr_array_free(entries, TRUE);
}

/* ============================================================================
 * VIEW MODES
 * ============================================================================
 *
 * A stack view is a name (the Scheme symbol), a builder, and that is all. Both
 * the name conversion and the factory below walk this table, so adding a third
 * way to lay out a folder is one row here plus one builder function -- no
 * switch statements to keep in sync.
 */

static GtkWidget *build_grid_view(GPtrArray *entries, int icon_size,
                                  DockItem *item);
static GtkWidget *build_list_view(GPtrArray *entries, int icon_size,
                                  DockItem *item);

/* @item is the stack the view belongs to: clicking a folder then walks into it
 * instead of opening a file manager. A detached view (the tests, a custom
 * consumer) passes NULL and keeps the "open it and close the panel" behaviour. */
typedef GtkWidget *(*StackViewBuildFunc)(GPtrArray *entries, int icon_size,
                                         DockItem *item);

typedef struct {
  LambdockStackView view;
  const char *name; /* the Scheme symbol this view is stored as */
  StackViewBuildFunc build;
} LambdockStackViewOps;

static const LambdockStackViewOps STACK_VIEW_OPS[] = {
    {LAMBDOCK_STACK_VIEW_GRID, "grid", build_grid_view},
    {LAMBDOCK_STACK_VIEW_LIST, "list", build_list_view},
};

/** The descriptor of @view; unknown views fall back to the first row. */
static const LambdockStackViewOps *stack_view_ops(LambdockStackView view) {
  for (gsize i = 0; i < G_N_ELEMENTS(STACK_VIEW_OPS); i++) {
    if (STACK_VIEW_OPS[i].view == view)
      return &STACK_VIEW_OPS[i];
  }

  return &STACK_VIEW_OPS[0];
}

LambdockStackView lambdock_stack_view_from_string(const char *name) {
  if (!name)
    return LAMBDOCK_STACK_VIEW_GRID;

  for (gsize i = 0; i < G_N_ELEMENTS(STACK_VIEW_OPS); i++) {
    if (g_ascii_strcasecmp(name, STACK_VIEW_OPS[i].name) == 0)
      return STACK_VIEW_OPS[i].view;
  }

  return LAMBDOCK_STACK_VIEW_GRID;
}

const char *lambdock_stack_view_to_string(LambdockStackView view) {
  return stack_view_ops(view)->name;
}

/* ============================================================================
 * ENTRY ICONS
 * ============================================================================
 */

/* A thumbnail decodes a real image, so only files that are plausibly images
 * are worth opening: no giant files, and no images so large that decoding them
 * would stall the dock for a second. */
#define STACK_THUMBNAIL_MAX_BYTES (32 * 1024 * 1024)
#define STACK_THUMBNAIL_MAX_PIXELS (80 * 1000 * 1000)

/* Reading an image header is cheap, so the view can decide up front which cells
 * deserve a decode and which should keep their type icon. */
static gboolean stack_path_has_thumbnail(const char *path) {
  if (!path || *path == '\0' || !g_file_test(path, G_FILE_TEST_IS_REGULAR))
    return FALSE; /* Directories, FIFOs and sockets have no thumbnail. */

  GStatBuf info;
  if (g_stat(path, &info) != 0 || info.st_size <= 0 ||
      info.st_size > STACK_THUMBNAIL_MAX_BYTES)
    return FALSE;

  int width = 0;
  int height = 0;
  if (!gdk_pixbuf_get_file_info(path, &width, &height) || width <= 0 ||
      height <= 0)
    return FALSE; /* Not an image GdkPixbuf can read. */

  return (gint64)width * height <= STACK_THUMBNAIL_MAX_PIXELS;
}

/* Decodes one square, centre-cropped preview. Safe to call off the main thread:
 * it only touches GdkPixbuf and the file system. */
static GdkPixbuf *stack_thumbnail_pixbuf(const char *path, int size) {
  if (!stack_path_has_thumbnail(path) || size <= 0)
    return NULL;

  /* Scale into a box twice the target size (so the square crop keeps enough
   * pixels even for a wide image) and let GdkPixbuf preserve the aspect ratio. */
  GError *error = NULL;
  GdkPixbuf *scaled = gdk_pixbuf_new_from_file_at_scale(
      path, size * 2, size * 2, TRUE, &error);
  if (!scaled) {
    LOG_C_DEBUG("UI", "No thumbnail for %s: %s", path,
                error ? error->message : "unknown error");
    g_clear_error(&error);
    return NULL;
  }

  /* Cards are square: crop the centre out of the scaled image instead of
   * squashing it. */
  int scaled_w = gdk_pixbuf_get_width(scaled);
  int scaled_h = gdk_pixbuf_get_height(scaled);
  int side = MIN(scaled_w, scaled_h);
  GdkPixbuf *square = gdk_pixbuf_new_subpixbuf(
      scaled, (scaled_w - side) / 2, (scaled_h - side) / 2, side, side);
  g_object_unref(scaled);

  return square;
}

/* gdk_texture_new_for_pixbuf() is deprecated; hand GTK the pixels directly.
 * GdkPixbuf stores 8-bit RGB or RGBA in memory in exactly the order these
 * memory formats describe. */
static GdkTexture *stack_texture_from_pixbuf(GdkPixbuf *pixbuf) {
  int width = gdk_pixbuf_get_width(pixbuf);
  int height = gdk_pixbuf_get_height(pixbuf);
  int rowstride = gdk_pixbuf_get_rowstride(pixbuf);
  gsize length = (gsize)rowstride * (gsize)height;
  GBytes *pixels = g_bytes_new(gdk_pixbuf_read_pixels(pixbuf), length);
  GdkMemoryFormat format = gdk_pixbuf_get_has_alpha(pixbuf)
                               ? GDK_MEMORY_R8G8B8A8
                               : GDK_MEMORY_R8G8B8;
  GdkTexture *texture =
      gdk_memory_texture_new(width, height, format, pixels, rowstride);
  g_bytes_unref(pixels);

  return texture;
}

GdkPaintable *lambdock_stack_thumbnail_for_path(const char *path, int size) {
  GdkPixbuf *pixbuf = stack_thumbnail_pixbuf(path, size);
  if (!pixbuf)
    return NULL;

  GdkTexture *texture = stack_texture_from_pixbuf(pixbuf);
  g_object_unref(pixbuf);
  return GDK_PAINTABLE(texture);
}

static GIcon *icon_for_entry(const LambdockStackEntry *entry) {
  if (entry->is_dir)
    return g_themed_icon_new("folder");

  char *content_type = g_content_type_guess(entry->name, NULL, 0, NULL);
  GIcon *icon = content_type ? g_content_type_get_icon(content_type) : NULL;
  g_free(content_type);

  return icon ? icon : g_themed_icon_new("text-x-generic");
}

static GtkWidget *image_for_entry(GtkIconTheme *theme,
                                  const LambdockStackEntry *entry,
                                  int size) {
  GIcon *icon = icon_for_entry(entry);
  GtkWidget *image = NULL;

  if (theme && gtk_icon_theme_has_gicon(theme, icon)) {
    image = gtk_image_new_from_gicon(icon);
    gtk_image_set_pixel_size(GTK_IMAGE(image), size);
  } else {
    /* Unknown content type: let the resolver run its own fallback chain. */
    image = entry->is_dir ? create_image_for_icon(theme, "folder", size)
                          : create_image_for_icon(theme, "text-x-generic", size);
  }
  g_object_unref(icon);

  if (image) {
    /* A fixed box keeps the grid and list steady when a thumbnail replaces the
     * icon, and lets the image scale the (larger) preview down to its slot. */
    gtk_widget_set_valign(image, GTK_ALIGN_CENTER);
    gtk_widget_set_size_request(image, size, size);
  }

  return image;
}

/* ============================================================================
 * GRID / LIST THUMBNAILS
 * ============================================================================
 *
 * Grid and list cells are ordinary GtkImages, so a decoded preview is swapped
 * into the image widget. Decoding happens off the main thread
 * and one image at a time; the queue belongs to the view's content box and is
 * emptied when that view is unmapped, so closing a stack stops a large folder
 * from decoding hundreds of pictures in the background.
 */

#define VIEW_THUMBNAIL_QUEUE_KEY "lambdock-view-thumbnail-queue"

typedef struct {
  GWeakRef image;
  char *path;
  int size;
} ViewThumbnailRequest;

typedef struct {
  GQueue *pending; /* ViewThumbnailRequest* */
  gboolean running;
} ViewThumbnailQueue;

typedef struct {
  GWeakRef view;
  GtkWidget *image; /* Strong ref for the duration of the decode */
  char *path;
  int size;
} ViewThumbnailJob;

static void view_thumbnail_request_free(ViewThumbnailRequest *request) {
  g_weak_ref_clear(&request->image);
  g_free(request->path);
  g_free(request);
}

static void view_thumbnail_job_free(gpointer data) {
  ViewThumbnailJob *job = (ViewThumbnailJob *)data;
  g_weak_ref_clear(&job->view);
  g_clear_object(&job->image);
  g_free(job->path);
  g_free(job);
}

static void view_thumbnail_thread(GTask *task, gpointer source_object,
                                  gpointer task_data,
                                  GCancellable *cancellable) {
  (void)source_object;
  (void)cancellable;
  ViewThumbnailJob *job = (ViewThumbnailJob *)task_data;
  GdkPixbuf *pixbuf = stack_thumbnail_pixbuf(job->path, job->size);

  g_task_return_pointer(task, pixbuf, pixbuf ? g_object_unref : NULL);
}

static void view_thumbnail_queue_clear(ViewThumbnailQueue *queue) {
  while (!g_queue_is_empty(queue->pending))
    view_thumbnail_request_free(g_queue_pop_head(queue->pending));
}

static void view_thumbnail_queue_free(gpointer data) {
  ViewThumbnailQueue *queue = (ViewThumbnailQueue *)data;
  view_thumbnail_queue_clear(queue);
  g_queue_free(queue->pending);
  g_free(queue);
}

static void on_view_thumbnail_unmap(GtkWidget *view, gpointer user_data) {
  (void)user_data;
  ViewThumbnailQueue *queue =
      g_object_get_data(G_OBJECT(view), VIEW_THUMBNAIL_QUEUE_KEY);
  if (queue)
    view_thumbnail_queue_clear(queue);
}

static void view_thumbnail_run_next(GtkWidget *view);

static void on_view_thumbnail_ready(GObject *source_object,
                                    GAsyncResult *result,
                                    gpointer user_data) {
  (void)source_object;
  (void)user_data;

  GTask *task = G_TASK(result);
  GError *error = NULL;
  GdkPixbuf *pixbuf = g_task_propagate_pointer(task, &error);
  if (!pixbuf && error) {
    LOG_C_DEBUG("UI", "Thumbnail decode failed: %s", error->message);
    g_clear_error(&error);
  }

  ViewThumbnailJob *job = (ViewThumbnailJob *)g_task_get_task_data(task);
  GtkWidget *view = g_weak_ref_get(&job->view);
  if (view) {
    ViewThumbnailQueue *queue =
        g_object_get_data(G_OBJECT(view), VIEW_THUMBNAIL_QUEUE_KEY);

    if (queue) {
      if (pixbuf && job->image && GTK_IS_IMAGE(job->image)) {
        GdkTexture *texture = stack_texture_from_pixbuf(pixbuf);
        gtk_image_set_from_paintable(GTK_IMAGE(job->image),
                                    GDK_PAINTABLE(texture));
        g_object_unref(texture);
      }

      queue->running = FALSE;
      view_thumbnail_run_next(view);
    }
    g_object_unref(view);
  }

  if (pixbuf)
    g_object_unref(pixbuf);
}

static void view_thumbnail_run_next(GtkWidget *view) {
  ViewThumbnailQueue *queue =
      g_object_get_data(G_OBJECT(view), VIEW_THUMBNAIL_QUEUE_KEY);
  if (!queue || queue->running)
    return;

  while (!g_queue_is_empty(queue->pending)) {
    ViewThumbnailRequest *request = g_queue_pop_head(queue->pending);
    GtkWidget *image = g_weak_ref_get(&request->image);
    if (!image) {
      view_thumbnail_request_free(request);
      continue;
    }

    ViewThumbnailJob *job = g_new0(ViewThumbnailJob, 1);
    g_weak_ref_init(&job->view, view);
    job->image = image; /* Takes the reference from the weak lookup. */
    job->path = g_strdup(request->path);
    job->size = request->size;
    view_thumbnail_request_free(request);

    queue->running = TRUE;
    GTask *task = g_task_new(NULL, NULL, on_view_thumbnail_ready, NULL);
    g_task_set_task_data(task, job, view_thumbnail_job_free);
    g_task_run_in_thread(task, view_thumbnail_thread);
    g_object_unref(task);
    return;
  }
}

static void view_thumbnail_schedule(GtkWidget *view, GtkWidget *image,
                                    const char *path, int size) {
  ViewThumbnailQueue *queue =
      g_object_get_data(G_OBJECT(view), VIEW_THUMBNAIL_QUEUE_KEY);
  if (!queue || !image || !path || size <= 0)
    return;

  ViewThumbnailRequest *request = g_new0(ViewThumbnailRequest, 1);
  g_weak_ref_init(&request->image, image);
  request->path = g_strdup(path);
  request->size = size;
  g_queue_push_tail(queue->pending, request);
  view_thumbnail_run_next(view);
}

static void view_thumbnail_queue_attach(GtkWidget *view) {
  if (g_object_get_data(G_OBJECT(view), VIEW_THUMBNAIL_QUEUE_KEY))
    return;

  ViewThumbnailQueue *queue = g_new0(ViewThumbnailQueue, 1);
  queue->pending = g_queue_new();
  g_object_set_data_full(G_OBJECT(view), VIEW_THUMBNAIL_QUEUE_KEY, queue,
                         view_thumbnail_queue_free);
  g_signal_connect(view, "unmap", G_CALLBACK(on_view_thumbnail_unmap), NULL);
}

/* Opens an entry and dismisses the stack popover it was clicked in. The item
 * has been handed off to the desktop, so leaving the panel covering it serves no
 * purpose. */
static void stack_close_popover(GtkWidget *widget) {
  GtkWidget *popover = gtk_widget_get_ancestor(widget, GTK_TYPE_POPOVER);
  if (!popover) {
    /* A popover's content can report the popover itself as its root rather than
     * as an ancestor, depending on how GTK parents the child widget. */
    GtkRoot *root = gtk_widget_get_root(widget);
    if (root && GTK_IS_POPOVER(root))
      popover = GTK_WIDGET(root);
  }

  if (popover && gtk_widget_get_visible(popover))
    gtk_popover_popdown(GTK_POPOVER(popover));
}

/* Where a click on an entry should go: into the folder, or out to the desktop. */
typedef struct {
  DockItem *item; /* NULL for a detached view */
  char *path;
  gboolean is_dir;
} StackEntryAction;

static void free_entry_action(gpointer data, GClosure *closure) {
  (void)closure;
  StackEntryAction *action = (StackEntryAction *)data;
  g_free(action->path);
  g_free(action);
}

/* Rebuilds the open panel at @path; defined with the popover, below. */
static void stack_popover_rebuild(DockItem *item, const char *path);

static void on_stack_entry_clicked(GtkButton *button, gpointer user_data) {
  StackEntryAction *action = (StackEntryAction *)user_data;
  if (!action || !action->path)
    return;

  /* A folder is a place to go, not a program to start: the panel walks into it
   * (the file manager is one header button away). */
  if (action->item && action->is_dir) {
    stack_popover_rebuild(action->item, action->path);
    return;
  }

  if (lambdock_open_path(action->path))
    stack_close_popover(GTK_WIDGET(button));
}

/* ============================================================================
 * GRID & LIST VIEWS
 * ============================================================================
 */

/* The stack popover is a panel, not a strip: it gets a real minimum box so a
 * folder with a handful of entries still opens at a size worth looking at, and a
 * ceiling so a folder with hundreds of screenshots scrolls instead of growing
 * past the screen. */
#define STACK_MIN_WIDTH 560
#define STACK_MIN_HEIGHT 400
#define STACK_MAX_HEIGHT 680

/* One grid cell: wide enough for a thumbnail and two lines of name. A cell keeps
 * this width whatever the file is called -- without it a long name stretches the
 * cell (and, because the flow box is homogeneous, every other cell with it) and
 * the panel grows past the screen. */
#define STACK_CELL_WIDTH 132

static GtkWidget *scrolled_view(GtkWidget *child, int max_height) {
  GtkWidget *scrolled = gtk_scrolled_window_new();
  gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scrolled), GTK_POLICY_NEVER,
                                 GTK_POLICY_AUTOMATIC);

  /* The content must not dictate the popover's width: a flow box's natural width
   * is everything on one line, which is how the stack ended up as a full-width
   * strip one row tall. The viewport keeps the size the popover gives it and the
   * grid wraps inside that. */
  gtk_scrolled_window_set_propagate_natural_width(
      GTK_SCROLLED_WINDOW(scrolled), FALSE);
  gtk_scrolled_window_set_min_content_width(GTK_SCROLLED_WINDOW(scrolled),
                                            STACK_MIN_WIDTH);
  gtk_scrolled_window_set_min_content_height(GTK_SCROLLED_WINDOW(scrolled),
                                             STACK_MIN_HEIGHT);
  gtk_scrolled_window_set_max_content_height(GTK_SCROLLED_WINDOW(scrolled),
                                             max_height);
  gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scrolled), child);
  gtk_widget_add_css_class(scrolled, "stack-scroll");
  return scrolled;
}

/* ============================================================================
 * STACK KEYBOARD NAVIGATION
 * ============================================================================
 * Tab reaches every entry, but a panel of files is expected to answer the arrow
 * keys: a folder full of screenshots should not force a keyboard user to tab
 * through a hundred cells to reach the one they can see.
 */

static GtkWidget *stack_view_child_at(GtkWidget *view, int index) {
  if (!view || index < 0)
    return NULL;

  if (GTK_IS_FLOW_BOX(view)) {
    GtkFlowBoxChild *child =
        gtk_flow_box_get_child_at_index(GTK_FLOW_BOX(view), (guint)index);
    if (!child)
      return NULL;

    /* The flow box wraps each entry in a GtkFlowBoxChild; the focusable widget
     * is the button inside it. */
    GtkWidget *inner = gtk_flow_box_child_get_child(child);
    return inner ? inner : GTK_WIDGET(child);
  }

  int i = 0;
  for (GtkWidget *child = gtk_widget_get_first_child(view); child != NULL;
       child = gtk_widget_get_next_sibling(child), i++) {
    if (i == index)
      return child;
  }
  return NULL;
}

static int stack_view_child_count(GtkWidget *view) {
  if (!view)
    return 0;

  if (GTK_IS_FLOW_BOX(view)) {
    int count = 0;
    while (gtk_flow_box_get_child_at_index(GTK_FLOW_BOX(view), (guint)count))
      count++;
    return count;
  }

  int count = 0;
  for (GtkWidget *child = gtk_widget_get_first_child(view); child != NULL;
       child = gtk_widget_get_next_sibling(child))
    count++;
  return count;
}

/* How many cards the wrapped grid put on its first row, which is how far one
 * "row" is for Up/Down. A grid that has not been allocated yet reports one. */
static int stack_grid_columns(GtkFlowBox *flow) {
  GtkWidget *first =
      GTK_WIDGET(gtk_flow_box_get_child_at_index(flow, 0));
  if (!first)
    return 1;

  graphene_rect_t first_bounds;
  if (!gtk_widget_compute_bounds(first, GTK_WIDGET(flow), &first_bounds))
    return 1;

  int columns = 0;
  for (guint i = 0;; i++) {
    GtkWidget *child =
        GTK_WIDGET(gtk_flow_box_get_child_at_index(flow, i));
    if (!child)
      break;

    graphene_rect_t bounds;
    if (!gtk_widget_compute_bounds(child, GTK_WIDGET(flow), &bounds))
      continue;
    if (fabs(bounds.origin.y - first_bounds.origin.y) > 1.0)
      break;
    columns++;
  }

  return columns > 0 ? columns : 1;
}

/* Index of the entry that holds the keyboard focus, or -1. */
static int stack_view_focused_index(GtkWidget *view) {
  if (!view)
    return -1;

  GtkRoot *root = gtk_widget_get_root(view);
  GtkWidget *focus = (root && GTK_IS_WINDOW(root))
                         ? gtk_window_get_focus(GTK_WINDOW(root))
                         : NULL;
  if (!focus)
    return -1;

  int index = 0;
  for (GtkWidget *child = gtk_widget_get_first_child(view); child != NULL;
       child = gtk_widget_get_next_sibling(child), index++) {
    for (GtkWidget *w = focus; w != NULL; w = gtk_widget_get_parent(w)) {
      if (w == child)
        return index;
      if (w == view)
        break;
    }
  }
  return -1;
}

gboolean lambdock_stack_view_focus_step(GtkWidget *view, int dx, int dy) {
  if (!view)
    return FALSE;

  int count = stack_view_child_count(view);
  if (count == 0)
    return FALSE;

  int current = stack_view_focused_index(view);
  if (current < 0)
    return lambdock_stack_view_focus_edge(view, FALSE);

  int columns = GTK_IS_FLOW_BOX(view) ? stack_grid_columns(GTK_FLOW_BOX(view)) : 1;
  int next = current + dx + dy * columns;
  next = CLAMP(next, 0, count - 1);
  if (next == current)
    return TRUE;

  GtkWidget *target = stack_view_child_at(view, next);
  if (!target)
    return FALSE;

  gtk_widget_grab_focus(target);
  return TRUE;
}

gboolean lambdock_stack_view_focus_edge(GtkWidget *view, gboolean last) {
  int count = stack_view_child_count(view);
  if (count == 0)
    return FALSE;

  GtkWidget *target = stack_view_child_at(view, last ? count - 1 : 0);
  if (!target)
    return FALSE;

  gtk_widget_grab_focus(target);
  return TRUE;
}

static gboolean on_stack_view_key_pressed(GtkEventControllerKey *controller,
                                          guint keyval, guint keycode,
                                          GdkModifierType state,
                                          gpointer user_data) {
  (void)controller;
  (void)keycode;
  (void)state;

  GtkWidget *view = GTK_WIDGET(user_data);

  switch (keyval) {
  case GDK_KEY_Left:
    lambdock_stack_view_focus_step(view, -1, 0);
    return TRUE;
  case GDK_KEY_Right:
    lambdock_stack_view_focus_step(view, 1, 0);
    return TRUE;
  case GDK_KEY_Up:
    lambdock_stack_view_focus_step(view, 0, -1);
    return TRUE;
  case GDK_KEY_Down:
    lambdock_stack_view_focus_step(view, 0, 1);
    return TRUE;
  case GDK_KEY_Home:
    lambdock_stack_view_focus_edge(view, FALSE);
    return TRUE;
  case GDK_KEY_End:
    lambdock_stack_view_focus_edge(view, TRUE);
    return TRUE;
  default:
    return FALSE;
  }
}

static void stack_view_attach_keys(GtkWidget *view) {
  if (!view)
    return;

  GtkEventController *keys = gtk_event_controller_key_new();
  /* Capture: a flow box has its own arrow-key bindings for moving its cursor,
   * and those would otherwise swallow the keys the stack navigation needs. */
  gtk_event_controller_set_propagation_phase(keys, GTK_PHASE_CAPTURE);
  g_signal_connect(keys, "key-pressed", G_CALLBACK(on_stack_view_key_pressed),
                   view);
  gtk_widget_add_controller(view, keys);
}

/* The icon theme of the default display, or NULL while it is starting up. */
static GtkIconTheme *stack_icon_theme(void) {
  GdkDisplay *display = gdk_display_get_default();

  return display ? gtk_icon_theme_get_for_display(display) : NULL;
}

/** What a screen reader says about one entry. */
static const char *stack_entry_description(const LambdockStackEntry *entry) {
  return entry->is_dir ? _("Folder. Press Enter to open it.")
                       : _("File. Press Enter to open it.");
}

/** Opens (or walks into) @entry when @button is clicked. */
static void stack_entry_attach_open(GtkWidget *button,
                                    const LambdockStackEntry *entry,
                                    DockItem *item) {
  StackEntryAction *action = g_new0(StackEntryAction, 1);
  action->item = item;
  action->path = g_strdup(entry->path);
  action->is_dir = entry->is_dir;

  g_signal_connect_data(button, "clicked",
                        G_CALLBACK(on_stack_entry_clicked), action,
                        free_entry_action, 0);
}

/** The tooltip and accessibility text every entry button shares. */
static void stack_entry_label_button(GtkWidget *button,
                                     const LambdockStackEntry *entry) {
  gtk_widget_set_tooltip_text(button, entry->name);
  lambdock_accessible_update(button, entry->name,
                             stack_entry_description(entry));
}

static GtkWidget *build_grid_view(GPtrArray *entries, int icon_size,
                                  DockItem *item) {
  GtkIconTheme *theme = stack_icon_theme();
  GtkWidget *flow = gtk_flow_box_new();
  view_thumbnail_queue_attach(flow);

  /* A stack is browsed, not skimmed: its icons are drawn considerably larger
   * than the dock's, and every cell has room for a two-line name. */
  int cell_icon_size = MAX(56, (int)(icon_size * 1.45));

  gtk_flow_box_set_selection_mode(GTK_FLOW_BOX(flow), GTK_SELECTION_NONE);
  gtk_flow_box_set_homogeneous(GTK_FLOW_BOX(flow), TRUE);
  gtk_flow_box_set_min_children_per_line(GTK_FLOW_BOX(flow), 4);
  gtk_flow_box_set_max_children_per_line(GTK_FLOW_BOX(flow), 7);
  gtk_flow_box_set_column_spacing(GTK_FLOW_BOX(flow), 8);
  gtk_flow_box_set_row_spacing(GTK_FLOW_BOX(flow), 8);
  gtk_widget_add_css_class(flow, "stack-grid");
  lambdock_accessible_update(
      flow, _("Folder contents"),
      _("Use the arrow keys to choose an entry and Enter to open it."));

  for (guint i = 0; i < entries->len; i++) {
    LambdockStackEntry *entry = g_ptr_array_index(entries, i);

    GtkWidget *content = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
    gtk_widget_set_valign(content, GTK_ALIGN_CENTER);

    GtkWidget *image = image_for_entry(theme, entry, cell_icon_size);
    if (image) {
      gtk_box_append(GTK_BOX(content), image);
      /* The worker decides whether the file really is an image; doing that
       * check here would put a few hundred header reads on the main thread. */
      if (!entry->is_dir)
        view_thumbnail_schedule(flow, image, entry->path,
                                cell_icon_size * 2);
    }

    /* Wrapping is what caps the label's natural width: with it, the cell is as
     * wide as STACK_CELL_WIDTH and a long name wraps (and then ellipsizes)
     * instead of stretching the grid. */
    GtkWidget *label = gtk_label_new(entry->name);
    gtk_label_set_wrap(GTK_LABEL(label), TRUE);
    gtk_label_set_wrap_mode(GTK_LABEL(label), PANGO_WRAP_WORD_CHAR);
    gtk_label_set_ellipsize(GTK_LABEL(label), PANGO_ELLIPSIZE_MIDDLE);
    gtk_label_set_max_width_chars(GTK_LABEL(label), 16);
    gtk_label_set_lines(GTK_LABEL(label), 2);
    gtk_label_set_justify(GTK_LABEL(label), GTK_JUSTIFY_CENTER);
    gtk_widget_set_hexpand(label, TRUE);
    gtk_box_append(GTK_BOX(content), label);

    GtkWidget *button = gtk_button_new();
    gtk_widget_set_size_request(button, STACK_CELL_WIDTH, -1);
    gtk_widget_add_css_class(button, "flat");
    gtk_widget_add_css_class(button, "stack-entry");
    stack_entry_label_button(button, entry);
    gtk_button_set_child(GTK_BUTTON(button), content);
    stack_entry_attach_open(button, entry, item);

    gtk_flow_box_append(GTK_FLOW_BOX(flow), button);
  }

  stack_view_attach_keys(flow);
  return scrolled_view(flow, STACK_MAX_HEIGHT);
}

static GtkWidget *build_list_view(GPtrArray *entries, int icon_size,
                                  DockItem *item) {
  GtkIconTheme *theme = stack_icon_theme();
  GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
  gtk_widget_add_css_class(box, "stack-list");
  lambdock_accessible_update(
      box, _("Folder contents"),
      _("Use the arrow keys to choose an entry and Enter to open it."));
  view_thumbnail_queue_attach(box);

  int list_icon_size = MAX(24, (int)(icon_size * 0.7));

  for (guint i = 0; i < entries->len; i++) {
    LambdockStackEntry *entry = g_ptr_array_index(entries, i);

    GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);

    GtkWidget *image = image_for_entry(theme, entry, list_icon_size);
    if (image) {
      gtk_box_append(GTK_BOX(row), image);
      if (!entry->is_dir)
        view_thumbnail_schedule(box, image, entry->path,
                                list_icon_size * 2);
    }

    GtkWidget *label = gtk_label_new(entry->name);
    gtk_label_set_ellipsize(GTK_LABEL(label), PANGO_ELLIPSIZE_MIDDLE);
    gtk_label_set_xalign(GTK_LABEL(label), 0.0f);
    gtk_widget_set_hexpand(label, TRUE);
    gtk_box_append(GTK_BOX(row), label);

    GtkWidget *arrow = gtk_image_new_from_icon_name(
        entry->is_dir ? "go-next-symbolic" : "document-open-symbolic");
    gtk_image_set_pixel_size(GTK_IMAGE(arrow), 16);
    gtk_widget_set_opacity(arrow, 0.55);
    gtk_box_append(GTK_BOX(row), arrow);

    GtkWidget *button = gtk_button_new();
    gtk_widget_add_css_class(button, "flat");
    gtk_widget_add_css_class(button, "stack-row");
    gtk_button_set_child(GTK_BUTTON(button), row);
    stack_entry_label_button(button, entry);
    stack_entry_attach_open(button, entry, item);

    gtk_box_append(GTK_BOX(box), button);
  }

  stack_view_attach_keys(box);
  return scrolled_view(box, STACK_MAX_HEIGHT);
}


/* ============================================================================
 * PUBLIC VIEW FACTORY
 * ============================================================================
 */

GtkWidget *lambdock_stack_view_new(GPtrArray *entries, LambdockStackView view,
                                   int icon_size) {
  if (icon_size <= 0)
    icon_size = DEFAULT_ICON_SIZE;

  if (!entries || entries->len == 0) {
    GtkWidget *empty = gtk_label_new(_("This folder is empty"));
    gtk_widget_add_css_class(empty, "dim-label");
    gtk_widget_set_margin_top(empty, 12);
    gtk_widget_set_margin_bottom(empty, 12);
    return empty;
  }

  return stack_view_ops(view)->build(entries, icon_size, NULL);
}

/* The same view, wired to the stack it belongs to: folders walk in, files open
 * and close the panel. */
static GtkWidget *stack_view_for_item(GPtrArray *entries, LambdockStackView view,
                                      int icon_size, DockItem *item) {
  if (!entries || entries->len == 0)
    return lambdock_stack_view_new(entries, view, icon_size);

  return stack_view_ops(view)->build(entries, icon_size, item);
}

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
      listing ? _("Show as a grid") : _("Show as a list"),
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

  GtkWidget *view_widget = stack_view_for_item(entries, view, icon_size, item);
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
  gtk_popover_set_child(GTK_POPOVER(item->popover), panel);
}

/** Rebuilds the open panel at @path and lands the keyboard on its first entry. */
static void stack_popover_rebuild(DockItem *item, const char *path) {
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
    stack_popover_rebuild(nav->item, parent);
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
    stack_popover_rebuild(item, stack_current_path(item));

  LOG_C_INFO("UI", "Stack '%s' now uses the %s view",
             item->name ? item->name : "stack", name);
}
