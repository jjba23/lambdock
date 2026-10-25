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

#include "stack.h"
#include "../guile_bridge.h"
#include "../logger.h"
#include "desktop_entry.h"
#include "dock_item.h"
#include "icon_resolver.h"
#include <glib/gi18n.h>
#include <math.h>

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

static gint compare_stack_entries(gconstpointer a, gconstpointer b) {
  const LambdockStackEntry *entry_a = *(const LambdockStackEntry *const *)a;
  const LambdockStackEntry *entry_b = *(const LambdockStackEntry *const *)b;

  /* Folders first, then names: the order a file manager would show. */
  if (entry_a->is_dir != entry_b->is_dir)
    return entry_a->is_dir ? -1 : 1;

  return g_utf8_collate(entry_a->name, entry_b->name);
}

GPtrArray *lambdock_stack_list_directory(const char *path) {
  GPtrArray *entries = g_ptr_array_new_with_free_func(
      (GDestroyNotify)lambdock_stack_entry_free);

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
  while ((filename = g_dir_read_name(dir)) != NULL &&
         entries->len < STACK_MAX_ENTRIES) {
    if (filename[0] == '.')
      continue; /* Hidden files stay hidden, as in any file manager. */

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

void lambdock_stack_entries_free(GPtrArray *entries) {
  if (entries)
    g_ptr_array_free(entries, TRUE);
}

/* ============================================================================
 * VIEW MODE NAMES
 * ============================================================================
 */

LambdockStackView lambdock_stack_view_from_string(const char *name) {
  if (!name)
    return LAMBDOCK_STACK_VIEW_GRID;
  if (g_ascii_strcasecmp(name, "list") == 0)
    return LAMBDOCK_STACK_VIEW_LIST;
  if (g_ascii_strcasecmp(name, "fan") == 0)
    return LAMBDOCK_STACK_VIEW_FAN;
  return LAMBDOCK_STACK_VIEW_GRID;
}

const char *lambdock_stack_view_to_string(LambdockStackView view) {
  switch (view) {
  case LAMBDOCK_STACK_VIEW_LIST:
    return "list";
  case LAMBDOCK_STACK_VIEW_FAN:
    return "fan";
  case LAMBDOCK_STACK_VIEW_GRID:
  default:
    return "grid";
  }
}

/* ============================================================================
 * ENTRY ICONS
 * ============================================================================
 */

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

  if (theme && gtk_icon_theme_has_gicon(theme, icon)) {
    GtkWidget *image = gtk_image_new_from_gicon(icon);
    gtk_image_set_pixel_size(GTK_IMAGE(image), size);
    gtk_widget_set_valign(image, GTK_ALIGN_CENTER);
    g_object_unref(icon);
    return image;
  }

  g_object_unref(icon);

  /* Unknown content type: let the resolver run its own fallback chain. */
  return entry->is_dir ? create_image_for_icon(theme, "folder", size)
                       : create_image_for_icon(theme, "text-x-generic", size);
}

static void on_stack_entry_clicked(GtkButton *button, gpointer user_data) {
  (void)button;
  const char *path = (const char *)user_data;
  if (path)
    lambdock_open_path(path);
}

/* GClosureNotify for the entry paths handed to the click handlers. */
static void free_clicked_path(gpointer data, GClosure *closure) {
  (void)closure;
  g_free(data);
}

/* ============================================================================
 * GRID & LIST VIEWS
 * ============================================================================
 */

static GtkWidget *scrolled_view(GtkWidget *child, int max_height) {
  GtkWidget *scrolled = gtk_scrolled_window_new();
  gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scrolled), GTK_POLICY_NEVER,
                                 GTK_POLICY_AUTOMATIC);
  gtk_scrolled_window_set_propagate_natural_width(
      GTK_SCROLLED_WINDOW(scrolled), TRUE);
  gtk_scrolled_window_set_max_content_height(GTK_SCROLLED_WINDOW(scrolled),
                                             max_height);
  gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scrolled), child);
  gtk_widget_add_css_class(scrolled, "stack-scroll");
  return scrolled;
}

static GtkWidget *build_grid_view(GPtrArray *entries, int icon_size) {
  GtkIconTheme *theme =
      gtk_icon_theme_get_for_display(gdk_display_get_default());
  GtkWidget *flow = gtk_flow_box_new();

  gtk_flow_box_set_selection_mode(GTK_FLOW_BOX(flow), GTK_SELECTION_NONE);
  gtk_flow_box_set_homogeneous(GTK_FLOW_BOX(flow), TRUE);
  gtk_flow_box_set_min_children_per_line(GTK_FLOW_BOX(flow), 4);
  gtk_flow_box_set_max_children_per_line(GTK_FLOW_BOX(flow), 6);
  gtk_flow_box_set_column_spacing(GTK_FLOW_BOX(flow), 4);
  gtk_flow_box_set_row_spacing(GTK_FLOW_BOX(flow), 4);
  gtk_widget_add_css_class(flow, "stack-grid");

  for (guint i = 0; i < entries->len; i++) {
    LambdockStackEntry *entry = g_ptr_array_index(entries, i);

    GtkWidget *content = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
    gtk_widget_set_valign(content, GTK_ALIGN_CENTER);

    GtkWidget *image = image_for_entry(theme, entry, icon_size);
    if (image)
      gtk_box_append(GTK_BOX(content), image);

    GtkWidget *label = gtk_label_new(entry->name);
    gtk_label_set_ellipsize(GTK_LABEL(label), PANGO_ELLIPSIZE_MIDDLE);
    gtk_label_set_max_width_chars(GTK_LABEL(label), 12);
    gtk_label_set_lines(GTK_LABEL(label), 2);
    gtk_label_set_justify(GTK_LABEL(label), GTK_JUSTIFY_CENTER);
    gtk_box_append(GTK_BOX(content), label);

    GtkWidget *button = gtk_button_new();
    gtk_widget_add_css_class(button, "flat");
    gtk_widget_add_css_class(button, "stack-entry");
    gtk_widget_set_tooltip_text(button, entry->name);
    gtk_button_set_child(GTK_BUTTON(button), content);

    g_signal_connect_data(button, "clicked",
                          G_CALLBACK(on_stack_entry_clicked),
                          g_strdup(entry->path), free_clicked_path, 0);

    gtk_flow_box_append(GTK_FLOW_BOX(flow), button);
  }

  return scrolled_view(flow, 420);
}

static GtkWidget *build_list_view(GPtrArray *entries, int icon_size) {
  GtkIconTheme *theme =
      gtk_icon_theme_get_for_display(gdk_display_get_default());
  GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
  gtk_widget_add_css_class(box, "stack-list");

  for (guint i = 0; i < entries->len; i++) {
    LambdockStackEntry *entry = g_ptr_array_index(entries, i);

    GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);

    GtkWidget *image = image_for_entry(theme, entry,
                                       MAX(16, (int)(icon_size * 0.5)));
    if (image)
      gtk_box_append(GTK_BOX(row), image);

    GtkWidget *label = gtk_label_new(entry->name);
    gtk_label_set_ellipsize(GTK_LABEL(label), PANGO_ELLIPSIZE_MIDDLE);
    gtk_label_set_xalign(GTK_LABEL(label), 0.0f);
    gtk_widget_set_hexpand(label, TRUE);
    gtk_box_append(GTK_BOX(row), label);

    GtkWidget *arrow = gtk_image_new_from_icon_name(
        entry->is_dir ? "go-next-symbolic" : "document-open-symbolic");
    gtk_image_set_pixel_size(GTK_IMAGE(arrow), 14);
    gtk_widget_set_opacity(arrow, 0.55);
    gtk_box_append(GTK_BOX(row), arrow);

    GtkWidget *button = gtk_button_new();
    gtk_widget_add_css_class(button, "flat");
    gtk_widget_add_css_class(button, "stack-row");
    gtk_button_set_child(GTK_BUTTON(button), row);
    gtk_widget_set_tooltip_text(button, entry->name);

    g_signal_connect_data(button, "clicked",
                          G_CALLBACK(on_stack_entry_clicked),
                          g_strdup(entry->path), free_clicked_path, 0);

    gtk_box_append(GTK_BOX(box), button);
  }

  return scrolled_view(box, 420);
}

/* ============================================================================
 * FAN VIEW
 * ============================================================================
 *
 * GTK cannot rotate child widgets (CSS transforms only cover affine transforms
 * of the whole widget), so the fan is painted by hand: each card is a rounded
 * rectangle with the entry icon and name, rotated around its own centre. Hit
 * testing undoes the rotation arithmetically, which keeps the widget free of
 * child widgets entirely.
 */

#define LAMBDOCK_TYPE_STACK_FAN (lambdock_stack_fan_get_type())
G_DECLARE_FINAL_TYPE(LambdockStackFan, lambdock_stack_fan, LAMBDOCK, STACK_FAN,
                     GtkWidget)

struct _LambdockStackFan {
  GtkWidget parent_instance;
  GPtrArray *paintables; /* GdkPaintable*, one per card */
  GPtrArray *names;      /* char*, one per card */
  GPtrArray *paths;      /* char*, one per card */
  int card_width;
  int card_height;
  int hovered;
};

G_DEFINE_FINAL_TYPE(LambdockStackFan, lambdock_stack_fan, GTK_TYPE_WIDGET)

/* Card geometry for a fan of N cards, in widget-local coordinates. */
static void fan_card_geometry(LambdockStackFan *self, int index, double width,
                              double height, double *cx, double *cy,
                              double *angle) {
  int count = (int)self->paintables->len;
  double middle = (count - 1) / 2.0;
  double offset = index - middle;
  double step = self->card_width * 0.36;

  *cx = width / 2.0 + offset * step;
  *cy = height / 2.0 + offset * offset * 3.0;
  *angle = offset * 4.0;
}

gboolean lambdock_stack_fan_contains(double x, double y, double cx, double cy,
                                     double angle_degrees, double width,
                                     double height) {
  double dx = x - cx;
  double dy = y - cy;
  double radians = -angle_degrees * G_PI / 180.0;
  double local_x = dx * cos(radians) - dy * sin(radians);
  double local_y = dx * sin(radians) + dy * cos(radians);

  return fabs(local_x) <= width / 2.0 && fabs(local_y) <= height / 2.0;
}

static int fan_hit_test(LambdockStackFan *self, double x, double y) {
  int count = (int)self->paintables->len;
  double width = gtk_widget_get_width(GTK_WIDGET(self));
  double height = gtk_widget_get_height(GTK_WIDGET(self));

  /* Topmost card first: the last painted one wins, as with real cards. */
  for (int i = count - 1; i >= 0; i--) {
    double cx;
    double cy;
    double angle;
    fan_card_geometry(self, i, width, height, &cx, &cy, &angle);

    if (lambdock_stack_fan_contains(x, y, cx, cy, angle, self->card_width,
                                    self->card_height))
      return i;
  }
  return -1;
}

/* Painttables may be NULL (no icon theme), so they need a guarded free. */
static void free_paintable(gpointer data) {
  if (data)
    g_object_unref(data);
}

static void stack_fan_measure(GtkWidget *widget, GtkOrientation orientation,
                              int for_size, int *minimum, int *natural,
                              int *minimum_baseline, int *natural_baseline) {
  (void)for_size;
  LambdockStackFan *self = LAMBDOCK_STACK_FAN(widget);
  int count = (int)self->paintables->len;
  double middle = (count > 0) ? (count - 1) / 2.0 : 0.0;
  double step = self->card_width * 0.36;

  int width = self->card_width +
              (count > 1 ? (int)ceil((count - 1) * step) : 0);
  int height = self->card_height + (int)ceil(2.0 * middle * middle * 3.0) + 8;

  if (minimum)
    *minimum = (orientation == GTK_ORIENTATION_HORIZONTAL) ? width : height;
  if (natural)
    *natural = (orientation == GTK_ORIENTATION_HORIZONTAL) ? width : height;
  if (minimum_baseline)
    *minimum_baseline = -1;
  if (natural_baseline)
    *natural_baseline = -1;
}

static void stack_fan_snapshot(GtkWidget *widget, GtkSnapshot *snapshot) {
  LambdockStackFan *self = LAMBDOCK_STACK_FAN(widget);
  int count = (int)self->paintables->len;
  if (count == 0)
    return;

  double width = gtk_widget_get_width(widget);
  double height = gtk_widget_get_height(widget);

  const GdkRGBA card_color = {0.97f, 0.97f, 0.99f, 0.96f};
  const GdkRGBA text_color = {0.12f, 0.12f, 0.15f, 0.92f};
  const GdkRGBA border_color = {0.0f, 0.0f, 0.0f, 0.18f};

  double card_w = self->card_width;
  double card_h = self->card_height;

  for (int i = 0; i < count; i++) {
    double cx;
    double cy;
    double angle;
    fan_card_geometry(self, i, width, height, &cx, &cy, &angle);

    gtk_snapshot_save(snapshot);
    graphene_point_t center = GRAPHENE_POINT_INIT((float)cx, (float)cy);
    gtk_snapshot_translate(snapshot, &center);
    gtk_snapshot_rotate(snapshot, (float)angle);

    graphene_rect_t card = GRAPHENE_RECT_INIT((float)(-card_w / 2.0),
                                             (float)(-card_h / 2.0),
                                             (float)card_w, (float)card_h);
    GskRoundedRect outline;
    gsk_rounded_rect_init_from_rect(&outline, &card, 10.0f);

    gtk_snapshot_push_rounded_clip(snapshot, &outline);
    gtk_snapshot_append_color(snapshot, &card_color, &card);
    gtk_snapshot_pop(snapshot);

    float widths[4] = {1.0f, 1.0f, 1.0f, 1.0f};
    GdkRGBA colors[4] = {border_color, border_color, border_color, border_color};
    gtk_snapshot_append_border(snapshot, &outline, widths, colors);

    /* Icon, centred in the upper part of the card. */
    GdkPaintable *paintable =
        (GdkPaintable *)g_ptr_array_index(self->paintables, i);
    if (paintable) {
      double icon_size = card_w - 20.0;
      graphene_point_t icon_origin = GRAPHENE_POINT_INIT(
          (float)(-icon_size / 2.0), (float)(-card_h / 2.0 + 10.0));
      gtk_snapshot_save(snapshot);
      gtk_snapshot_translate(snapshot, &icon_origin);
      gdk_paintable_snapshot(paintable, snapshot, icon_size, icon_size);
      gtk_snapshot_restore(snapshot);
    }

    /* File name, under the icon. */
    const char *name = g_ptr_array_index(self->names, i);
    PangoLayout *layout = gtk_widget_create_pango_layout(widget, name);
    pango_layout_set_width(layout, (int)((card_w - 10.0) * PANGO_SCALE));
    pango_layout_set_ellipsize(layout, PANGO_ELLIPSIZE_MIDDLE);
    pango_layout_set_alignment(layout, PANGO_ALIGN_CENTER);

    int text_w = 0;
    int text_h = 0;
    pango_layout_get_pixel_size(layout, &text_w, &text_h);

    graphene_point_t text_origin = GRAPHENE_POINT_INIT(
        (float)(-text_w / 2.0), (float)(card_h / 2.0 - text_h - 6.0));
    gtk_snapshot_save(snapshot);
    gtk_snapshot_translate(snapshot, &text_origin);
    gtk_snapshot_append_layout(snapshot, layout, &text_color);
    gtk_snapshot_restore(snapshot);
    g_object_unref(layout);

    /* A little emphasis for the card under the pointer. */
    if (i == self->hovered) {
      GskRoundedRect glow;
      gsk_rounded_rect_init_from_rect(&glow, &card, 10.0f);
      float glow_widths[4] = {2.0f, 2.0f, 2.0f, 2.0f};
      const GdkRGBA glow_color = {0.30f, 0.55f, 1.0f, 0.85f};
      GdkRGBA glow_colors[4] = {glow_color, glow_color, glow_color, glow_color};
      gtk_snapshot_append_border(snapshot, &glow, glow_widths, glow_colors);
    }

    gtk_snapshot_restore(snapshot);
  }
}

static void on_fan_pressed(GtkGestureClick *gesture, int n_press, double x,
                           double y, gpointer user_data) {
  (void)gesture;
  (void)n_press;
  LambdockStackFan *self = LAMBDOCK_STACK_FAN(user_data);

  int index = fan_hit_test(self, x, y);
  if (index < 0)
    return;

  const char *path = g_ptr_array_index(self->paths, index);
  if (path)
    lambdock_open_path(path);
}

static void on_fan_motion(GtkEventControllerMotion *controller, double x,
                          double y, gpointer user_data) {
  (void)controller;
  LambdockStackFan *self = LAMBDOCK_STACK_FAN(user_data);

  int hovered = fan_hit_test(self, x, y);
  if (hovered != self->hovered) {
    self->hovered = hovered;
    gtk_widget_queue_draw(GTK_WIDGET(self));
  }
}

static void on_fan_leave(GtkEventControllerMotion *controller,
                         gpointer user_data) {
  (void)controller;
  LambdockStackFan *self = LAMBDOCK_STACK_FAN(user_data);

  if (self->hovered != -1) {
    self->hovered = -1;
    gtk_widget_queue_draw(GTK_WIDGET(self));
  }
}

static void lambdock_stack_fan_dispose(GObject *object) {
  LambdockStackFan *self = LAMBDOCK_STACK_FAN(object);

  g_clear_pointer(&self->paintables, g_ptr_array_unref);
  g_clear_pointer(&self->names, g_ptr_array_unref);
  g_clear_pointer(&self->paths, g_ptr_array_unref);

  G_OBJECT_CLASS(lambdock_stack_fan_parent_class)->dispose(object);
}

static void lambdock_stack_fan_class_init(LambdockStackFanClass *klass) {
  GObjectClass *object_class = G_OBJECT_CLASS(klass);
  GtkWidgetClass *widget_class = GTK_WIDGET_CLASS(klass);

  object_class->dispose = lambdock_stack_fan_dispose;
  widget_class->measure = stack_fan_measure;
  widget_class->snapshot = stack_fan_snapshot;

  gtk_widget_class_set_css_name(widget_class, "stack-fan");
}

static void lambdock_stack_fan_init(LambdockStackFan *self) {
  gtk_widget_set_overflow(GTK_WIDGET(self), GTK_OVERFLOW_VISIBLE);
}

static GtkWidget *build_fan_view(GPtrArray *entries, int icon_size) {
  LambdockStackFan *self = g_object_new(LAMBDOCK_TYPE_STACK_FAN, NULL);
  GtkIconTheme *theme =
      gtk_icon_theme_get_for_display(gdk_display_get_default());

  self->card_width = icon_size + 28;
  self->card_height = icon_size + 44;
  self->hovered = -1;
  self->paintables = g_ptr_array_new_with_free_func(free_paintable);
  self->names = g_ptr_array_new_with_free_func(g_free);
  self->paths = g_ptr_array_new_with_free_func(g_free);

  guint count = MIN(entries->len, (guint)STACK_FAN_MAX_CARDS);
  for (guint i = 0; i < count; i++) {
    LambdockStackEntry *entry = g_ptr_array_index(entries, i);

    GIcon *icon = icon_for_entry(entry);
    GtkIconPaintable *paintable =
        theme ? gtk_icon_theme_lookup_by_gicon(theme, icon, icon_size, 1,
                                               GTK_TEXT_DIR_NONE,
                                               GTK_ICON_LOOKUP_FORCE_REGULAR)
              : NULL;
    g_object_unref(icon);

    if (paintable) {
      g_ptr_array_add(self->paintables, paintable);
    } else {
      /* No display or icon theme (headless tests): keep a NULL slot, which the
       * snapshot skips while preserving the card geometry. */
      g_ptr_array_add(self->paintables, NULL);
    }

    g_ptr_array_add(self->names, g_strdup(entry->name));
    g_ptr_array_add(self->paths, g_strdup(entry->path));
  }

  GtkGesture *click = gtk_gesture_click_new();
  g_signal_connect(click, "pressed", G_CALLBACK(on_fan_pressed), self);
  gtk_widget_add_controller(GTK_WIDGET(self), GTK_EVENT_CONTROLLER(click));

  GtkEventController *motion = gtk_event_controller_motion_new();
  g_signal_connect(motion, "motion", G_CALLBACK(on_fan_motion), self);
  g_signal_connect(motion, "leave", G_CALLBACK(on_fan_leave), self);
  gtk_widget_add_controller(GTK_WIDGET(self), motion);

  return GTK_WIDGET(self);
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

  switch (view) {
  case LAMBDOCK_STACK_VIEW_LIST:
    return build_list_view(entries, icon_size);
  case LAMBDOCK_STACK_VIEW_FAN:
    return build_fan_view(entries, icon_size);
  case LAMBDOCK_STACK_VIEW_GRID:
  default:
    return build_grid_view(entries, icon_size);
  }
}

/* ============================================================================
 * THE STACK POPOVER
 * ============================================================================
 */

static GtkWidget *stack_popover_content(DockItem *item) {
  int icon_size =
      (item->state && item->state->icon_size > 0) ? item->state->icon_size
                                                 : DEFAULT_ICON_SIZE;
  LambdockStackView view = lambdock_stack_view_from_string(item->stack_view);
  GPtrArray *entries = lambdock_stack_list_directory(item->stack_path);

  GtkWidget *vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
  gtk_widget_set_margin_start(vbox, POPOVER_VBOX_MARGIN);
  gtk_widget_set_margin_end(vbox, POPOVER_VBOX_MARGIN);
  gtk_widget_set_margin_top(vbox, POPOVER_VBOX_MARGIN);
  gtk_widget_set_margin_bottom(vbox, POPOVER_VBOX_MARGIN);
  gtk_widget_add_css_class(vbox, "stack-popover");

  /* Header: folder name, entry count and a way to open the folder itself. */
  GtkWidget *header = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);

  GtkWidget *title = gtk_label_new(item->name && *item->name != '\0'
                                       ? item->name
                                       : _("Stack"));
  gtk_widget_add_css_class(title, "title");
  gtk_label_set_ellipsize(GTK_LABEL(title), PANGO_ELLIPSIZE_END);
  gtk_box_append(GTK_BOX(header), title);

  unsigned int count = entries ? entries->len : 0;
  char *count_text =
      g_strdup_printf(ngettext("%u item", "%u items", count), count);
  GtkWidget *count_label = gtk_label_new(count_text);
  g_free(count_text);
  gtk_widget_add_css_class(count_label, "dim-label");
  gtk_box_append(GTK_BOX(header), count_label);

  GtkWidget *spacer = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
  gtk_widget_set_hexpand(spacer, TRUE);
  gtk_box_append(GTK_BOX(header), spacer);

  GtkWidget *open_button = gtk_button_new_from_icon_name("folder-open-symbolic");
  gtk_widget_add_css_class(open_button, "flat");
  gtk_widget_set_tooltip_text(open_button, _("Open folder"));
  g_signal_connect_data(open_button, "clicked",
                        G_CALLBACK(on_stack_entry_clicked),
                        g_strdup(item->stack_path ? item->stack_path : ""),
                        free_clicked_path, 0);
  gtk_box_append(GTK_BOX(header), open_button);

  gtk_box_append(GTK_BOX(vbox), header);
  gtk_box_append(GTK_BOX(vbox), gtk_separator_new(GTK_ORIENTATION_HORIZONTAL));
  gtk_box_append(GTK_BOX(vbox),
                 lambdock_stack_view_new(entries, view, icon_size));

  lambdock_stack_entries_free(entries);
  return vbox;
}

void lambdock_stack_show(DockItem *item) {
  if (!item || !item->popover)
    return;

  gtk_popover_set_child(GTK_POPOVER(item->popover), stack_popover_content(item));

  if (!gtk_widget_get_visible(item->popover) && item->state)
    item->state->active_popovers++;

  gtk_popover_popup(GTK_POPOVER(item->popover));
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

  /* If the stack is on screen, redraw it in the new view mode right away. */
  if (item->popover && gtk_widget_get_visible(item->popover))
    gtk_popover_set_child(GTK_POPOVER(item->popover),
                          stack_popover_content(item));

  LOG_C_INFO("UI", "Stack '%s' now uses the %s view",
             item->name ? item->name : "stack", name);
}
