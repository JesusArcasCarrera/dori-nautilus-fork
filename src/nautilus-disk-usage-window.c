/*
 * Copyright © 2026 The Dori authors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "nautilus-disk-usage-window.h"

#include "nautilus-disk-usage-model.h"

#include <glib/gi18n.h>
#include <math.h>
#include <pango/pangocairo.h>

#define MAP_GAP 2.0
#define MAP_RADIUS 5.0
#define PROGRESS_REFRESH_MSEC 120

typedef struct
{
    NautilusDiskUsageNode *node;
    NautilusDiskUsageRect rect;
    gboolean direct_files;
} MapItem;

typedef struct
{
    GFile *location;
    GMutex lock;
    guint64 files_scanned;
    guint64 directories_scanned;
    guint64 bytes_scanned;
} ScanJob;

typedef struct
{
    double red;
    double green;
    double blue;
} MapColor;

#define NAUTILUS_TYPE_DISK_USAGE_WINDOW (nautilus_disk_usage_window_get_type ())
G_DECLARE_FINAL_TYPE (NautilusDiskUsageWindow,
                      nautilus_disk_usage_window,
                      NAUTILUS,
                      DISK_USAGE_WINDOW,
                      AdwWindow)

static const MapColor map_palette[] = {
    { 0.15, 0.43, 0.47 },
    { 0.57, 0.30, 0.20 },
    { 0.30, 0.40, 0.22 },
    { 0.43, 0.29, 0.40 },
    { 0.23, 0.37, 0.51 },
    { 0.51, 0.36, 0.15 },
    { 0.23, 0.40, 0.31 },
    { 0.36, 0.31, 0.49 },
    { 0.46, 0.31, 0.20 },
    { 0.20, 0.39, 0.43 },
};

struct _NautilusDiskUsageWindow
{
    AdwWindow parent_instance;

    GFile *location;
    NautilusDiskUsageNode *root_node;
    NautilusDiskUsageNode *current_node;
    GCancellable *cancellable;
    ScanJob *scan_job;
    GArray *map_items;

    AdwWindowTitle *window_title;
    GtkButton *back_button;
    GtkButton *refresh_button;
    GtkLabel *summary_label;
    GtkLabel *notice_label;
    GtkProgressBar *progress_bar;
    GtkDrawingArea *map;
    AdwStatusPage *status_page;
    GtkButton *status_button;

    guint progress_timeout_id;
    int hovered_item;
    int selected_item;
    gboolean scanning;
    gboolean closing;
};

G_DEFINE_FINAL_TYPE (NautilusDiskUsageWindow, nautilus_disk_usage_window, ADW_TYPE_WINDOW)

static void start_scan (NautilusDiskUsageWindow *self);

static void
scan_job_free (ScanJob *job)
{
    g_clear_object (&job->location);
    g_mutex_clear (&job->lock);
    g_free (job);
}

static void
scan_progress_cb (guint64  files_scanned,
                  guint64  directories_scanned,
                  guint64  bytes_scanned,
                  gpointer user_data)
{
    ScanJob *job = user_data;

    g_mutex_lock (&job->lock);
    job->files_scanned = files_scanned;
    job->directories_scanned = directories_scanned;
    job->bytes_scanned = bytes_scanned;
    g_mutex_unlock (&job->lock);
}

static void
scan_task_thread (GTask        *task,
                  gpointer      source_object,
                  gpointer      task_data,
                  GCancellable *cancellable)
{
    ScanJob *job = task_data;
    g_autoptr (GError) error = NULL;
    NautilusDiskUsageNode *root;

    root = nautilus_disk_usage_scan (job->location,
                                     cancellable,
                                     scan_progress_cb,
                                     job,
                                     &error);
    if (root == NULL)
    {
        g_task_return_error (task, g_steal_pointer (&error));
        return;
    }

    g_task_return_pointer (task,
                           root,
                           (GDestroyNotify) nautilus_disk_usage_node_free);
}

static gboolean
point_is_in_rect (double                  x,
                  double                  y,
                  NautilusDiskUsageRect  *rect)
{
    return x >= rect->x && x < rect->x + rect->width &&
           y >= rect->y && y < rect->y + rect->height;
}

static int
find_item_at (NautilusDiskUsageWindow *self,
              double                   x,
              double                   y)
{
    for (guint i = 0; i < self->map_items->len; i++)
    {
        MapItem *item = &g_array_index (self->map_items, MapItem, i);

        if (point_is_in_rect (x, y, &item->rect))
        {
            return i;
        }
    }

    return -1;
}

static void
rounded_rectangle (cairo_t *cr,
                   double   x,
                   double   y,
                   double   width,
                   double   height,
                   double   radius)
{
    radius = MIN (radius, MIN (width, height) / 2.0);

    cairo_new_sub_path (cr);
    cairo_arc (cr, x + width - radius, y + radius, radius, -G_PI_2, 0);
    cairo_arc (cr, x + width - radius, y + height - radius, radius, 0, G_PI_2);
    cairo_arc (cr, x + radius, y + height - radius, radius, G_PI_2, G_PI);
    cairo_arc (cr, x + radius, y + radius, radius, G_PI, 3 * G_PI_2);
    cairo_close_path (cr);
}

static void
draw_text (GtkWidget              *widget,
           cairo_t                *cr,
           const char             *name,
           guint64                 size,
           guint64                 total,
           NautilusDiskUsageRect  *rect)
{
    g_autoptr (PangoLayout) layout = NULL;
    g_autoptr (PangoAttrList) attributes = NULL;
    g_autofree char *size_string = NULL;
    g_autofree char *text = NULL;
    PangoRectangle logical;
    double padding;
    int lines;

    if (rect->width < 42 || rect->height < 24)
    {
        return;
    }

    lines = rect->height >= 72 && rect->width >= 105 ? 3 : 1;
    size_string = g_format_size_full (size, G_FORMAT_SIZE_IEC_UNITS);
    if (lines == 3)
    {
        double percentage = total > 0 ? (100.0 * size / total) : 0.0;
        text = g_strdup_printf ("%s\n%s\n%.1f%%", name, size_string, percentage);
    }
    else
    {
        text = g_strdup (name);
    }

    layout = gtk_widget_create_pango_layout (widget, text);
    attributes = pango_attr_list_new ();
    pango_attr_list_insert (attributes, pango_attr_weight_new (PANGO_WEIGHT_BOLD));
    pango_attr_list_insert (attributes,
                            pango_attr_size_new (lines == 3 ? 12 * PANGO_SCALE : 10 * PANGO_SCALE));
    pango_layout_set_attributes (layout, attributes);
    pango_layout_set_alignment (layout, PANGO_ALIGN_CENTER);
    pango_layout_set_ellipsize (layout, PANGO_ELLIPSIZE_END);
    padding = rect->width >= 96 ? 16 : 8;
    pango_layout_set_width (layout, MAX (1, rect->width - padding) * PANGO_SCALE);
    pango_layout_set_height (layout, MAX (1, rect->height - 8) * PANGO_SCALE);
    pango_layout_get_pixel_extents (layout, NULL, &logical);

    cairo_set_source_rgb (cr, 1.0, 1.0, 1.0);
    cairo_move_to (cr,
                   rect->x + (rect->width - logical.width) / 2.0 - logical.x,
                   rect->y + (rect->height - logical.height) / 2.0 - logical.y);
    pango_cairo_show_layout (cr, layout);
}

static void
draw_map_item (NautilusDiskUsageWindow *self,
               cairo_t                 *cr,
               MapItem                 *item,
               guint                    index,
               guint64                  total)
{
    const char *name;
    guint64 size;
    MapColor color;
    double x = item->rect.x + MAP_GAP / 2.0;
    double y = item->rect.y + MAP_GAP / 2.0;
    double width = MAX (0.0, item->rect.width - MAP_GAP);
    double height = MAX (0.0, item->rect.height - MAP_GAP);
    NautilusDiskUsageRect inner = { x, y, width, height };

    if (width < 1 || height < 1)
    {
        return;
    }

    if (item->direct_files)
    {
        name = _("Files");
        size = nautilus_disk_usage_node_get_direct_size (self->current_node);
        color = (MapColor) { 0.32, 0.30, 0.28 };
    }
    else
    {
        name = nautilus_disk_usage_node_get_name (item->node);
        size = nautilus_disk_usage_node_get_size (item->node);
        color = map_palette[index % G_N_ELEMENTS (map_palette)];
    }

    rounded_rectangle (cr, x, y, width, height, MAP_RADIUS);
    cairo_set_source_rgb (cr, color.red, color.green, color.blue);
    cairo_fill_preserve (cr);

    if ((int) index == self->hovered_item || (int) index == self->selected_item)
    {
        cairo_set_source_rgba (cr, 1.0, 1.0, 1.0,
                               (int) index == self->selected_item ? 0.95 : 0.65);
        cairo_set_line_width (cr, (int) index == self->selected_item ? 3.0 : 2.0);
        cairo_stroke (cr);
    }
    else
    {
        cairo_new_path (cr);
    }

    cairo_save (cr);
    rounded_rectangle (cr, x, y, width, height, MAP_RADIUS);
    cairo_clip (cr);
    draw_text (GTK_WIDGET (self->map), cr, name, size, total, &inner);
    cairo_restore (cr);
}

static void
draw_skeleton (NautilusDiskUsageWindow *self,
               cairo_t                 *cr,
               int                      width,
               int                      height)
{
    const guint64 sizes[] = { 36, 24, 17, 11, 7, 5 };
    NautilusDiskUsageRect bounds = { 0, 0, width, height };
    NautilusDiskUsageRect rectangles[G_N_ELEMENTS (sizes)];
    gboolean dark = adw_style_manager_get_dark (adw_style_manager_get_default ());

    nautilus_disk_usage_layout (sizes, G_N_ELEMENTS (sizes), bounds, rectangles);
    for (guint i = 0; i < G_N_ELEMENTS (sizes); i++)
    {
        double shade = dark ? 0.21 + i * 0.012 : 0.83 - i * 0.012;
        double x = rectangles[i].x + MAP_GAP / 2.0;
        double y = rectangles[i].y + MAP_GAP / 2.0;
        double rect_width = MAX (0.0, rectangles[i].width - MAP_GAP);
        double rect_height = MAX (0.0, rectangles[i].height - MAP_GAP);

        rounded_rectangle (cr, x, y, rect_width, rect_height, MAP_RADIUS);
        cairo_set_source_rgb (cr, shade, shade * 0.98, shade * 0.94);
        cairo_fill (cr);
    }
}

static void
map_draw_cb (GtkDrawingArea *drawing_area,
             cairo_t        *cr,
             int             width,
             int             height,
             gpointer        user_data)
{
    NautilusDiskUsageWindow *self = user_data;
    gboolean dark = adw_style_manager_get_dark (adw_style_manager_get_default ());
    guint n_children;
    guint n_items;
    guint64 total;
    g_autofree guint64 *sizes = NULL;
    g_autofree NautilusDiskUsageRect *rectangles = NULL;
    NautilusDiskUsageRect bounds = { 0, 0, width, height };

    cairo_set_source_rgb (cr,
                          dark ? 0.10 : 0.96,
                          dark ? 0.095 : 0.95,
                          dark ? 0.09 : 0.93);
    cairo_paint (cr);

    g_array_set_size (self->map_items, 0);
    if (self->scanning)
    {
        draw_skeleton (self, cr, width, height);
        return;
    }

    if (self->current_node == NULL)
    {
        return;
    }

    n_children = nautilus_disk_usage_node_get_n_children (self->current_node);
    n_items = n_children +
              (nautilus_disk_usage_node_get_direct_file_count (self->current_node) > 0);
    if (n_items == 0)
    {
        return;
    }

    sizes = g_new0 (guint64, n_items);
    rectangles = g_new0 (NautilusDiskUsageRect, n_items);
    for (guint i = 0; i < n_children; i++)
    {
        NautilusDiskUsageNode *child = nautilus_disk_usage_node_get_child (self->current_node, i);
        sizes[i] = nautilus_disk_usage_node_get_size (child);
    }
    if (n_items > n_children)
    {
        sizes[n_children] = nautilus_disk_usage_node_get_direct_size (self->current_node);
    }

    nautilus_disk_usage_layout (sizes, n_items, bounds, rectangles);
    total = nautilus_disk_usage_node_get_size (self->current_node);
    for (guint i = 0; i < n_items; i++)
    {
        MapItem item = {
            .node = i < n_children ? nautilus_disk_usage_node_get_child (self->current_node, i) : NULL,
            .rect = rectangles[i],
            .direct_files = i >= n_children,
        };

        g_array_append_val (self->map_items, item);
        draw_map_item (self, cr, &item, i, total);
    }
}

static void
update_current_node (NautilusDiskUsageWindow *self,
                     NautilusDiskUsageNode   *node)
{
    g_autofree char *path = NULL;
    g_autofree char *size = NULL;
    g_autofree char *summary = NULL;
    g_autofree char *notice = NULL;
    g_autofree char *file_count = NULL;
    g_autofree char *directory_count = NULL;
    g_autofree char *unreadable_count = NULL;
    g_autofree char *excluded_count = NULL;
    guint64 unreadable;
    guint64 excluded;

    self->current_node = node;
    self->hovered_item = -1;
    self->selected_item = -1;

    path = g_file_get_parse_name (nautilus_disk_usage_node_get_location (node));
    adw_window_title_set_title (self->window_title,
                                nautilus_disk_usage_node_get_name (node));
    adw_window_title_set_subtitle (self->window_title, path);

    size = g_format_size_full (nautilus_disk_usage_node_get_size (node),
                               G_FORMAT_SIZE_IEC_UNITS);
    file_count = g_strdup_printf ("%'" G_GUINT64_FORMAT,
                                  nautilus_disk_usage_node_get_file_count (node));
    directory_count = g_strdup_printf ("%'" G_GUINT64_FORMAT,
                                       nautilus_disk_usage_node_get_directory_count (node));
    summary = g_strdup_printf (_("%s · %s files · %s folders"),
                               size,
                               file_count,
                               directory_count);
    gtk_label_set_text (self->summary_label, summary);

    unreadable = nautilus_disk_usage_node_get_unreadable_count (node);
    excluded = nautilus_disk_usage_node_get_excluded_filesystem_count (node);
    if (unreadable > 0 || excluded > 0)
    {
        unreadable_count = g_strdup_printf ("%'" G_GUINT64_FORMAT, unreadable);
        excluded_count = g_strdup_printf ("%'" G_GUINT64_FORMAT, excluded);
        notice = g_strdup_printf (_("Incomplete: %s unreadable · %s on other file systems"),
                                  unreadable_count,
                                  excluded_count);
        gtk_label_set_text (self->notice_label, notice);
        gtk_widget_set_visible (GTK_WIDGET (self->notice_label), TRUE);
    }
    else
    {
        gtk_widget_set_visible (GTK_WIDGET (self->notice_label), FALSE);
    }

    gtk_widget_set_sensitive (GTK_WIDGET (self->back_button),
                              nautilus_disk_usage_node_get_parent (node) != NULL);

    if (nautilus_disk_usage_node_get_n_children (node) == 0 &&
        nautilus_disk_usage_node_get_direct_file_count (node) == 0)
    {
        adw_status_page_set_icon_name (self->status_page, "folder-symbolic");
        adw_status_page_set_title (self->status_page, _("This Folder Is Empty"));
        adw_status_page_set_description (self->status_page,
                                         _("There is no content to show in the map."));
        gtk_button_set_label (self->status_button, _("Scan Again"));
        gtk_widget_set_visible (GTK_WIDGET (self->status_page), TRUE);
    }
    else
    {
        gtk_widget_set_visible (GTK_WIDGET (self->status_page), FALSE);
    }

    gtk_widget_queue_draw (GTK_WIDGET (self->map));
}

static void
navigate_to_item (NautilusDiskUsageWindow *self,
                  int                      index)
{
    MapItem *item;

    if (index < 0 || index >= (int) self->map_items->len)
    {
        return;
    }

    item = &g_array_index (self->map_items, MapItem, index);
    if (!item->direct_files && item->node != NULL)
    {
        update_current_node (self, item->node);
    }
}

static void
map_pressed_cb (GtkGestureClick *gesture,
                int              n_press,
                double           x,
                double           y,
                gpointer         user_data)
{
    NautilusDiskUsageWindow *self = user_data;

    gtk_widget_grab_focus (GTK_WIDGET (self->map));
    navigate_to_item (self, find_item_at (self, x, y));
}

static void
map_motion_cb (GtkEventControllerMotion *controller,
               double                    x,
               double                    y,
               gpointer                  user_data)
{
    NautilusDiskUsageWindow *self = user_data;
    int hovered = find_item_at (self, x, y);
    gboolean can_navigate = FALSE;

    if (hovered == self->hovered_item)
    {
        return;
    }

    self->hovered_item = hovered;
    if (hovered >= 0)
    {
        MapItem *item = &g_array_index (self->map_items, MapItem, hovered);
        can_navigate = !item->direct_files;
    }

    gtk_widget_set_cursor_from_name (GTK_WIDGET (self->map),
                                     can_navigate ? "pointer" : "default");
    gtk_widget_trigger_tooltip_query (GTK_WIDGET (self->map));
    gtk_widget_queue_draw (GTK_WIDGET (self->map));
}

static void
map_leave_cb (GtkEventControllerMotion *controller,
              gpointer                  user_data)
{
    NautilusDiskUsageWindow *self = user_data;

    self->hovered_item = -1;
    gtk_widget_set_cursor_from_name (GTK_WIDGET (self->map), "default");
    gtk_widget_queue_draw (GTK_WIDGET (self->map));
}

static gboolean
map_query_tooltip_cb (GtkWidget  *widget,
                      int         x,
                      int         y,
                      gboolean    keyboard_mode,
                      GtkTooltip *tooltip,
                      gpointer    user_data)
{
    NautilusDiskUsageWindow *self = user_data;
    int index = keyboard_mode ? self->selected_item : find_item_at (self, x, y);
    MapItem *item;
    const char *name;
    guint64 size;
    guint64 files;
    g_autofree char *size_string = NULL;
    g_autofree char *file_count = NULL;
    g_autofree char *text = NULL;

    if (index < 0 || index >= (int) self->map_items->len)
    {
        return FALSE;
    }

    item = &g_array_index (self->map_items, MapItem, index);
    if (item->direct_files)
    {
        name = _("Files");
        size = nautilus_disk_usage_node_get_direct_size (self->current_node);
        files = nautilus_disk_usage_node_get_direct_file_count (self->current_node);
    }
    else
    {
        name = nautilus_disk_usage_node_get_name (item->node);
        size = nautilus_disk_usage_node_get_size (item->node);
        files = nautilus_disk_usage_node_get_file_count (item->node);
    }

    size_string = g_format_size_full (size, G_FORMAT_SIZE_IEC_UNITS);
    file_count = g_strdup_printf ("%'" G_GUINT64_FORMAT, files);
    text = g_strdup_printf (_("%s\n%s · %s files"),
                            name,
                            size_string,
                            file_count);
    gtk_tooltip_set_text (tooltip, text);

    return TRUE;
}

static gboolean
map_key_pressed_cb (GtkEventControllerKey *controller,
                    guint                  keyval,
                    guint                  keycode,
                    GdkModifierType        state,
                    gpointer               user_data)
{
    NautilusDiskUsageWindow *self = user_data;

    if (keyval == GDK_KEY_BackSpace ||
        (keyval == GDK_KEY_Left && (state & GDK_ALT_MASK) != 0))
    {
        NautilusDiskUsageNode *parent = self->current_node != NULL ?
                                        nautilus_disk_usage_node_get_parent (self->current_node) : NULL;
        if (parent != NULL)
        {
            update_current_node (self, parent);
            return GDK_EVENT_STOP;
        }
    }

    if (self->map_items->len == 0)
    {
        return GDK_EVENT_PROPAGATE;
    }

    if (keyval == GDK_KEY_Return || keyval == GDK_KEY_KP_Enter)
    {
        navigate_to_item (self, self->selected_item);
        return GDK_EVENT_STOP;
    }

    if (keyval == GDK_KEY_Left || keyval == GDK_KEY_Up)
    {
        self->selected_item = self->selected_item <= 0 ?
                              (int) self->map_items->len - 1 :
                              self->selected_item - 1;
    }
    else if (keyval == GDK_KEY_Right || keyval == GDK_KEY_Down)
    {
        self->selected_item = (self->selected_item + 1) % self->map_items->len;
    }
    else
    {
        return GDK_EVENT_PROPAGATE;
    }

    gtk_widget_queue_draw (GTK_WIDGET (self->map));
    gtk_widget_trigger_tooltip_query (GTK_WIDGET (self->map));
    return GDK_EVENT_STOP;
}

static void
back_clicked_cb (GtkButton *button,
                 gpointer   user_data)
{
    NautilusDiskUsageWindow *self = user_data;
    NautilusDiskUsageNode *parent = self->current_node != NULL ?
                                    nautilus_disk_usage_node_get_parent (self->current_node) : NULL;

    if (parent != NULL)
    {
        update_current_node (self, parent);
    }
}

static void
refresh_clicked_cb (GtkButton *button,
                    gpointer   user_data)
{
    start_scan (NAUTILUS_DISK_USAGE_WINDOW (user_data));
}

static gboolean
progress_timeout_cb (gpointer user_data)
{
    NautilusDiskUsageWindow *self = user_data;
    guint64 files;
    guint64 directories;
    guint64 bytes;
    g_autofree char *size = NULL;
    g_autofree char *file_count = NULL;
    g_autofree char *directory_count = NULL;
    g_autofree char *summary = NULL;

    if (!self->scanning || self->scan_job == NULL)
    {
        self->progress_timeout_id = 0;
        return G_SOURCE_REMOVE;
    }

    g_mutex_lock (&self->scan_job->lock);
    files = self->scan_job->files_scanned;
    directories = self->scan_job->directories_scanned;
    bytes = self->scan_job->bytes_scanned;
    g_mutex_unlock (&self->scan_job->lock);

    size = g_format_size_full (bytes, G_FORMAT_SIZE_IEC_UNITS);
    file_count = g_strdup_printf ("%'" G_GUINT64_FORMAT, files);
    directory_count = g_strdup_printf ("%'" G_GUINT64_FORMAT, directories);
    summary = g_strdup_printf (_("Scanning… %s · %s files · %s folders"),
                               size,
                               file_count,
                               directory_count);
    gtk_label_set_text (self->summary_label, summary);
    gtk_progress_bar_pulse (self->progress_bar);

    return G_SOURCE_CONTINUE;
}

static void
scan_finished_cb (GObject      *source_object,
                  GAsyncResult *result,
                  gpointer      user_data)
{
    NautilusDiskUsageWindow *self = NAUTILUS_DISK_USAGE_WINDOW (source_object);
    g_autoptr (GError) error = NULL;
    NautilusDiskUsageNode *root;

    g_clear_handle_id (&self->progress_timeout_id, g_source_remove);
    self->scanning = FALSE;
    self->scan_job = NULL;

    root = g_task_propagate_pointer (G_TASK (result), &error);
    if (self->closing)
    {
        nautilus_disk_usage_node_free (root);
        return;
    }

    gtk_widget_set_visible (GTK_WIDGET (self->progress_bar), FALSE);
    gtk_widget_set_sensitive (GTK_WIDGET (self->refresh_button), TRUE);
    if (root == NULL)
    {
        if (g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
        {
            return;
        }

        adw_status_page_set_icon_name (self->status_page, "dialog-error-symbolic");
        adw_status_page_set_title (self->status_page, _("Could Not Scan This Folder"));
        adw_status_page_set_description (self->status_page, error->message);
        gtk_button_set_label (self->status_button, _("Try Again"));
        gtk_widget_set_visible (GTK_WIDGET (self->status_page), TRUE);
        gtk_label_set_text (self->summary_label, _("The scan did not finish."));
        gtk_widget_queue_draw (GTK_WIDGET (self->map));
        return;
    }

    g_clear_pointer (&self->root_node, nautilus_disk_usage_node_free);
    self->root_node = root;
    update_current_node (self, root);
}

static void
start_scan (NautilusDiskUsageWindow *self)
{
    g_autoptr (GTask) task = NULL;
    ScanJob *job;
    g_autofree char *path = NULL;

    if (self->scanning)
    {
        return;
    }

    g_clear_pointer (&self->root_node, nautilus_disk_usage_node_free);
    self->current_node = NULL;
    g_clear_object (&self->cancellable);
    self->cancellable = g_cancellable_new ();
    self->hovered_item = -1;
    self->selected_item = -1;
    self->scanning = TRUE;

    path = g_file_get_parse_name (self->location);
    adw_window_title_set_title (self->window_title, _("Disk Usage Map"));
    adw_window_title_set_subtitle (self->window_title, path);
    gtk_label_set_text (self->summary_label, _("Starting scan…"));
    gtk_widget_set_visible (GTK_WIDGET (self->notice_label), FALSE);
    gtk_widget_set_visible (GTK_WIDGET (self->status_page), FALSE);
    gtk_widget_set_visible (GTK_WIDGET (self->progress_bar), TRUE);
    gtk_widget_set_sensitive (GTK_WIDGET (self->refresh_button), FALSE);
    gtk_widget_set_sensitive (GTK_WIDGET (self->back_button), FALSE);
    gtk_widget_queue_draw (GTK_WIDGET (self->map));

    job = g_new0 (ScanJob, 1);
    job->location = g_object_ref (self->location);
    g_mutex_init (&job->lock);
    self->scan_job = job;

    task = g_task_new (self, self->cancellable, scan_finished_cb, NULL);
    g_task_set_task_data (task, job, (GDestroyNotify) scan_job_free);
    g_task_set_return_on_cancel (task, FALSE);
    g_task_run_in_thread (task, scan_task_thread);

    self->progress_timeout_id = g_timeout_add (PROGRESS_REFRESH_MSEC,
                                               progress_timeout_cb,
                                               self);
}

static gboolean
nautilus_disk_usage_window_close_request (GtkWindow *window)
{
    NautilusDiskUsageWindow *self = NAUTILUS_DISK_USAGE_WINDOW (window);

    self->closing = TRUE;
    if (self->cancellable != NULL)
    {
        g_cancellable_cancel (self->cancellable);
    }

    return GTK_WINDOW_CLASS (nautilus_disk_usage_window_parent_class)->close_request (window);
}

static void
nautilus_disk_usage_window_dispose (GObject *object)
{
    NautilusDiskUsageWindow *self = NAUTILUS_DISK_USAGE_WINDOW (object);

    g_clear_handle_id (&self->progress_timeout_id, g_source_remove);
    if (self->cancellable != NULL)
    {
        g_cancellable_cancel (self->cancellable);
    }

    g_clear_pointer (&self->root_node, nautilus_disk_usage_node_free);
    self->current_node = NULL;
    g_clear_pointer (&self->map_items, g_array_unref);
    g_clear_object (&self->cancellable);
    g_clear_object (&self->location);

    G_OBJECT_CLASS (nautilus_disk_usage_window_parent_class)->dispose (object);
}

static void
nautilus_disk_usage_window_class_init (NautilusDiskUsageWindowClass *klass)
{
    GObjectClass *object_class = G_OBJECT_CLASS (klass);
    GtkWindowClass *window_class = GTK_WINDOW_CLASS (klass);

    object_class->dispose = nautilus_disk_usage_window_dispose;
    window_class->close_request = nautilus_disk_usage_window_close_request;
}

static void
nautilus_disk_usage_window_init (NautilusDiskUsageWindow *self)
{
    GtkWidget *toolbar_view;
    GtkWidget *header_bar;
    GtkWidget *content;
    GtkWidget *summary_box;
    GtkWidget *map_overlay;
    GtkWidget *hint_label;
    GtkEventController *motion_controller;
    GtkGesture *click_gesture;
    GtkEventController *key_controller;

    self->hovered_item = -1;
    self->selected_item = -1;
    self->map_items = g_array_new (FALSE, FALSE, sizeof (MapItem));

    gtk_window_set_title (GTK_WINDOW (self), _("Disk Usage Map"));
    gtk_window_set_default_size (GTK_WINDOW (self), 1080, 700);

    toolbar_view = adw_toolbar_view_new ();
    adw_window_set_content (ADW_WINDOW (self), toolbar_view);

    header_bar = adw_header_bar_new ();
    adw_toolbar_view_add_top_bar (ADW_TOOLBAR_VIEW (toolbar_view), header_bar);

    self->back_button = GTK_BUTTON (gtk_button_new_from_icon_name ("go-previous-symbolic"));
    gtk_widget_set_tooltip_text (GTK_WIDGET (self->back_button), _("Go to Parent Folder"));
    gtk_widget_add_css_class (GTK_WIDGET (self->back_button), "flat");
    adw_header_bar_pack_start (ADW_HEADER_BAR (header_bar), GTK_WIDGET (self->back_button));
    g_signal_connect (self->back_button, "clicked", G_CALLBACK (back_clicked_cb), self);

    self->window_title = ADW_WINDOW_TITLE (adw_window_title_new (_("Disk Usage Map"), ""));
    adw_header_bar_set_title_widget (ADW_HEADER_BAR (header_bar), GTK_WIDGET (self->window_title));

    self->refresh_button = GTK_BUTTON (gtk_button_new_from_icon_name ("view-refresh-symbolic"));
    gtk_widget_set_tooltip_text (GTK_WIDGET (self->refresh_button), _("Scan Again"));
    gtk_widget_add_css_class (GTK_WIDGET (self->refresh_button), "flat");
    adw_header_bar_pack_end (ADW_HEADER_BAR (header_bar), GTK_WIDGET (self->refresh_button));
    g_signal_connect (self->refresh_button, "clicked", G_CALLBACK (refresh_clicked_cb), self);

    content = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
    adw_toolbar_view_set_content (ADW_TOOLBAR_VIEW (toolbar_view), content);

    summary_box = gtk_box_new (GTK_ORIENTATION_VERTICAL, 4);
    gtk_widget_set_margin_start (summary_box, 12);
    gtk_widget_set_margin_end (summary_box, 12);
    gtk_widget_set_margin_top (summary_box, 8);
    gtk_widget_set_margin_bottom (summary_box, 8);
    gtk_box_append (GTK_BOX (content), summary_box);

    self->summary_label = GTK_LABEL (gtk_label_new (NULL));
    gtk_label_set_xalign (self->summary_label, 0);
    gtk_label_set_ellipsize (self->summary_label, PANGO_ELLIPSIZE_END);
    gtk_widget_add_css_class (GTK_WIDGET (self->summary_label), "numeric");
    gtk_box_append (GTK_BOX (summary_box), GTK_WIDGET (self->summary_label));

    self->notice_label = GTK_LABEL (gtk_label_new (NULL));
    gtk_label_set_xalign (self->notice_label, 0);
    gtk_widget_add_css_class (GTK_WIDGET (self->notice_label), "caption");
    gtk_widget_add_css_class (GTK_WIDGET (self->notice_label), "warning");
    gtk_box_append (GTK_BOX (summary_box), GTK_WIDGET (self->notice_label));

    self->progress_bar = GTK_PROGRESS_BAR (gtk_progress_bar_new ());
    gtk_progress_bar_set_pulse_step (self->progress_bar, 0.035);
    gtk_box_append (GTK_BOX (content), GTK_WIDGET (self->progress_bar));

    map_overlay = gtk_overlay_new ();
    gtk_widget_set_hexpand (map_overlay, TRUE);
    gtk_widget_set_vexpand (map_overlay, TRUE);
    gtk_box_append (GTK_BOX (content), map_overlay);

    self->map = GTK_DRAWING_AREA (gtk_drawing_area_new ());
    gtk_widget_set_hexpand (GTK_WIDGET (self->map), TRUE);
    gtk_widget_set_vexpand (GTK_WIDGET (self->map), TRUE);
    gtk_widget_set_focusable (GTK_WIDGET (self->map), TRUE);
    gtk_widget_set_has_tooltip (GTK_WIDGET (self->map), TRUE);
    gtk_drawing_area_set_draw_func (self->map, map_draw_cb, self, NULL);
    gtk_overlay_set_child (GTK_OVERLAY (map_overlay), GTK_WIDGET (self->map));
    g_signal_connect (self->map, "query-tooltip", G_CALLBACK (map_query_tooltip_cb), self);

    click_gesture = gtk_gesture_click_new ();
    gtk_widget_add_controller (GTK_WIDGET (self->map), GTK_EVENT_CONTROLLER (click_gesture));
    g_signal_connect (click_gesture, "pressed", G_CALLBACK (map_pressed_cb), self);

    motion_controller = gtk_event_controller_motion_new ();
    gtk_widget_add_controller (GTK_WIDGET (self->map), motion_controller);
    g_signal_connect (motion_controller, "motion", G_CALLBACK (map_motion_cb), self);
    g_signal_connect (motion_controller, "leave", G_CALLBACK (map_leave_cb), self);

    key_controller = gtk_event_controller_key_new ();
    gtk_widget_add_controller (GTK_WIDGET (self->map), key_controller);
    g_signal_connect (key_controller, "key-pressed", G_CALLBACK (map_key_pressed_cb), self);

    self->status_page = ADW_STATUS_PAGE (adw_status_page_new ());
    gtk_widget_set_halign (GTK_WIDGET (self->status_page), GTK_ALIGN_FILL);
    gtk_widget_set_valign (GTK_WIDGET (self->status_page), GTK_ALIGN_FILL);
    gtk_widget_add_css_class (GTK_WIDGET (self->status_page), "view");
    self->status_button = GTK_BUTTON (gtk_button_new_with_label (_("Try Again")));
    gtk_widget_set_halign (GTK_WIDGET (self->status_button), GTK_ALIGN_CENTER);
    adw_status_page_set_child (self->status_page, GTK_WIDGET (self->status_button));
    gtk_overlay_add_overlay (GTK_OVERLAY (map_overlay), GTK_WIDGET (self->status_page));
    gtk_widget_set_visible (GTK_WIDGET (self->status_page), FALSE);
    g_signal_connect (self->status_button, "clicked", G_CALLBACK (refresh_clicked_cb), self);

    hint_label = gtk_label_new (_("Click a folder to explore it · Backspace returns to the parent folder"));
    gtk_widget_set_margin_start (hint_label, 12);
    gtk_widget_set_margin_end (hint_label, 12);
    gtk_widget_set_margin_top (hint_label, 8);
    gtk_widget_set_margin_bottom (hint_label, 8);
    gtk_label_set_xalign (GTK_LABEL (hint_label), 0);
    gtk_widget_add_css_class (hint_label, "caption");
    gtk_widget_add_css_class (hint_label, "dim-label");
    gtk_box_append (GTK_BOX (content), hint_label);
}

void
nautilus_disk_usage_window_present (GFile     *location,
                                    GtkWindow *parent)
{
    NautilusDiskUsageWindow *window;

    g_return_if_fail (G_IS_FILE (location));
    g_return_if_fail (parent == NULL || GTK_IS_WINDOW (parent));

    window = g_object_new (nautilus_disk_usage_window_get_type (), NULL);
    window->location = g_object_ref (location);
    if (parent != NULL)
    {
        gtk_window_set_transient_for (GTK_WINDOW (window), parent);
        gtk_window_set_destroy_with_parent (GTK_WINDOW (window), TRUE);
    }

    start_scan (window);
    gtk_window_present (GTK_WINDOW (window));
}
