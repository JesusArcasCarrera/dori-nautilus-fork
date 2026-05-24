/*
 * Copyright (C) 2026 The Nemo project contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "nautilus-other-locations-view.h"

#include <glib/gi18n.h>

#include "nautilus-application.h"
#include "nautilus-list-base-private.h"
#include "nautilus-scheme.h"
#include "nautilus-view-model.h"

/*
 * NautilusOtherLocationsView
 *
 * Cards-based view that backs `other-locations:///`. Despite being a subclass of
 * NautilusListBase (so that NautilusFilesView can host it through its existing
 * dispatch path), the visible content is built independently from the directory
 * model: drives, volumes and mounts are read directly from GVolumeMonitor and
 * rendered as two flow-boxes — "This Computer" for native mountables and
 * "Network" for remote ones.
 *
 * The base model still exists (otherwise NautilusFilesView misbehaves) but its
 * contents are not displayed.
 */

#define CARD_ICON_SIZE 48

struct _NautilusOtherLocationsView
{
    NautilusListBase parent_instance;

    GtkBox      *content_box;
    GtkLabel    *this_computer_label;
    GtkFlowBox  *this_computer_box;
    GtkLabel    *network_label;
    GtkFlowBox  *network_box;

    GVolumeMonitor *volume_monitor;
    guint           rebuild_idle_id;
};

G_DEFINE_TYPE (NautilusOtherLocationsView, nautilus_other_locations_view, NAUTILUS_TYPE_LIST_BASE)

static const NautilusViewInfo other_locations_view_info =
{
    .view_id = NAUTILUS_VIEW_OTHER_LOCATIONS_ID,
    .zoom_level_min = NAUTILUS_LIST_ZOOM_LEVEL_SMALL,
    .zoom_level_max = NAUTILUS_LIST_ZOOM_LEVEL_SMALL,
    .zoom_level_standard = NAUTILUS_LIST_ZOOM_LEVEL_SMALL,
};

/* ---------- NautilusListBase vmethods (mostly no-ops) ---------- */

static NautilusViewInfo
real_get_view_info (NautilusListBase *list_base)
{
    return other_locations_view_info;
}

static guint
real_get_icon_size (NautilusListBase *list_base)
{
    return NAUTILUS_LIST_ICON_SIZE_SMALL;
}

static int
real_get_zoom_level (NautilusListBase *list_base)
{
    return other_locations_view_info.zoom_level_standard;
}

static void
real_set_zoom_level (NautilusListBase *list_base,
                     int               new_level)
{
    /* No-op: the cards view doesn't expose user-controlled zoom. */
}

static GVariant *
real_get_sort_state (NautilusListBase *list_base)
{
    return g_variant_take_ref (g_variant_new ("(sb)", "invalid", FALSE));
}

static void
real_set_sort_state (NautilusListBase *list_base,
                     GVariant         *value)
{
    /* No-op */
}

static void
real_set_enable_rubberband (NautilusListBase *list_base,
                            gboolean          enabled)
{
    /* No-op: there's no rubberband on a flowbox of cards. */
}

static void
real_scroll_to (NautilusListBase   *list_base,
                guint               position,
                GtkListScrollFlags  flags,
                GtkScrollInfo      *scroll)
{
    /* No-op: we don't drive scrolling by item position. Ownership of @scroll
     * follows the same convention as gtk_list_view_scroll_to(): callee takes
     * the reference. Drop it so it doesn't leak. */
    if (scroll != NULL)
    {
        gtk_scroll_info_unref (scroll);
    }
}

static void
real_popup_background_context_menu (NautilusListBase *self,
                                    double            x,
                                    double            y)
{
    /* No-op: the view has no useful background menu. Stop emission so the
     * parent doesn't fall back to the default files-view menu. */
    g_signal_stop_emission_by_name (G_OBJECT (self), "popup-background-context-menu");
}

/* ---------- Card construction ---------- */

typedef struct
{
    NautilusOtherLocationsView *self;
    GFile                      *location;
} CardActivateData;

static void
card_activate_data_free (gpointer data)
{
    CardActivateData *cd = data;

    g_clear_object (&cd->location);
    g_free (cd);
}

static void
on_card_clicked (GtkButton *button,
                 gpointer   user_data)
{
    CardActivateData *cd = user_data;
    NautilusApplication *app = NAUTILUS_APPLICATION (g_application_get_default ());

    if (cd->location == NULL)
    {
        return;
    }

    nautilus_application_open_location_full (app, cd->location, 0, NULL, NULL);
}

static gboolean
query_filesystem_usage (GFile   *root,
                        guint64 *out_total,
                        guint64 *out_free)
{
    /* g_file_query_filesystem_info() can block on remote mounts. We only ever
     * call this for native (local) roots. */
    g_autoptr (GError) error = NULL;
    g_autoptr (GFileInfo) info = g_file_query_filesystem_info (
        root,
        G_FILE_ATTRIBUTE_FILESYSTEM_SIZE "," G_FILE_ATTRIBUTE_FILESYSTEM_FREE,
        NULL, &error);

    if (info == NULL)
    {
        return FALSE;
    }

    if (!g_file_info_has_attribute (info, G_FILE_ATTRIBUTE_FILESYSTEM_SIZE) ||
        !g_file_info_has_attribute (info, G_FILE_ATTRIBUTE_FILESYSTEM_FREE))
    {
        return FALSE;
    }

    *out_total = g_file_info_get_attribute_uint64 (info, G_FILE_ATTRIBUTE_FILESYSTEM_SIZE);
    *out_free = g_file_info_get_attribute_uint64 (info, G_FILE_ATTRIBUTE_FILESYSTEM_FREE);
    return TRUE;
}

static GtkWidget *
build_card (NautilusOtherLocationsView *self,
            const char                 *name,
            const char                 *path_label,
            GIcon                      *icon,
            GFile                      *location,
            gboolean                    is_native_and_mounted)
{
    /* Use a GtkButton as the card so it gets keyboard activation and the
     * "card" + "flat" libadwaita styling without us having to redo any of it. */
    GtkWidget *card = gtk_button_new ();
    gtk_widget_add_css_class (card, "card");
    gtk_widget_add_css_class (card, "activatable");
    gtk_widget_set_size_request (card, 220, -1);
    gtk_widget_set_hexpand (card, FALSE);
    gtk_widget_set_vexpand (card, FALSE);

    GtkWidget *box = gtk_box_new (GTK_ORIENTATION_VERTICAL, 6);
    gtk_widget_set_margin_top (box, 12);
    gtk_widget_set_margin_bottom (box, 12);
    gtk_widget_set_margin_start (box, 12);
    gtk_widget_set_margin_end (box, 12);
    gtk_button_set_child (GTK_BUTTON (card), box);

    /* Icon. Fallback to a generic disk symbol when the volume/mount didn't
     * provide one. The temporary GIcon is owned via g_autoptr so it doesn't
     * leak when gtk_image_new_from_gicon() takes its own ref. */
    g_autoptr (GIcon) fallback_icon = NULL;
    GIcon *effective_icon = icon;

    if (effective_icon == NULL)
    {
        fallback_icon = g_themed_icon_new ("drive-harddisk-symbolic");
        effective_icon = fallback_icon;
    }

    GtkWidget *image = gtk_image_new_from_gicon (effective_icon);
    gtk_image_set_pixel_size (GTK_IMAGE (image), CARD_ICON_SIZE);
    gtk_widget_set_halign (image, GTK_ALIGN_CENTER);
    gtk_box_append (GTK_BOX (box), image);

    /* Name. */
    GtkWidget *name_label = gtk_label_new (name);
    gtk_widget_add_css_class (name_label, "heading");
    gtk_label_set_ellipsize (GTK_LABEL (name_label), PANGO_ELLIPSIZE_END);
    gtk_label_set_xalign (GTK_LABEL (name_label), 0.5);
    gtk_label_set_max_width_chars (GTK_LABEL (name_label), 22);
    gtk_box_append (GTK_BOX (box), name_label);

    /* Path / subtitle. */
    if (path_label != NULL && *path_label != '\0')
    {
        GtkWidget *path_lbl = gtk_label_new (path_label);
        gtk_widget_add_css_class (path_lbl, "dim-label");
        gtk_widget_add_css_class (path_lbl, "caption");
        gtk_label_set_ellipsize (GTK_LABEL (path_lbl), PANGO_ELLIPSIZE_MIDDLE);
        gtk_label_set_xalign (GTK_LABEL (path_lbl), 0.5);
        gtk_label_set_max_width_chars (GTK_LABEL (path_lbl), 24);
        gtk_box_append (GTK_BOX (box), path_lbl);
    }

    /* Progress bar + size only for mounted native mounts where we can query
     * the filesystem cheaply. */
    if (is_native_and_mounted && location != NULL)
    {
        guint64 total = 0;
        guint64 free_b = 0;

        if (query_filesystem_usage (location, &total, &free_b) && total > 0)
        {
            guint64 used = (free_b > total) ? 0 : total - free_b;
            double fraction = (double) used / (double) total;

            GtkWidget *bar = gtk_progress_bar_new ();
            gtk_progress_bar_set_fraction (GTK_PROGRESS_BAR (bar), fraction);
            gtk_widget_set_margin_top (bar, 4);
            gtk_box_append (GTK_BOX (box), bar);

            g_autofree char *free_str = g_format_size (free_b);
            g_autofree char *total_str = g_format_size (total);
            /* Translators: "<free> free of <total>" on a drive card. */
            g_autofree char *usage_str = g_strdup_printf (_("%s free of %s"),
                                                          free_str, total_str);
            GtkWidget *usage_lbl = gtk_label_new (usage_str);
            gtk_widget_add_css_class (usage_lbl, "dim-label");
            gtk_widget_add_css_class (usage_lbl, "caption");
            gtk_label_set_xalign (GTK_LABEL (usage_lbl), 0.5);
            gtk_box_append (GTK_BOX (box), usage_lbl);
        }
    }

    /* Click → navigate. Disable activation when there's nowhere to go. */
    if (location != NULL)
    {
        CardActivateData *cd = g_new0 (CardActivateData, 1);
        cd->self = self;
        cd->location = g_object_ref (location);
        g_signal_connect_data (card, "clicked",
                               G_CALLBACK (on_card_clicked), cd,
                               (GClosureNotify) card_activate_data_free,
                               G_CONNECT_DEFAULT);
    }
    else
    {
        gtk_widget_set_sensitive (card, FALSE);
    }

    return card;
}

/* ---------- Volume monitor → cards ---------- */

static void
add_mount_card (NautilusOtherLocationsView *self,
                GMount                     *mount)
{
    g_autofree char *name = g_mount_get_name (mount);
    g_autoptr (GIcon) icon = g_mount_get_symbolic_icon (mount);
    g_autoptr (GFile) root = g_mount_get_root (mount);
    gboolean is_native = g_file_is_native (root);
    g_autofree char *path = NULL;

    if (is_native)
    {
        path = g_file_get_path (root);
    }
    else
    {
        path = g_file_get_uri (root);
    }

    GtkWidget *card = build_card (self, name, path, icon, root, is_native);

    GtkFlowBox *target = is_native ? self->this_computer_box : self->network_box;
    gtk_flow_box_append (target, card);
}

static void
add_volume_unmounted_card (NautilusOtherLocationsView *self,
                           GVolume                    *volume,
                           gboolean                    is_native_hint)
{
    g_autofree char *name = g_volume_get_name (volume);
    g_autoptr (GIcon) icon = g_volume_get_symbolic_icon (volume);

    /* Unmounted volumes don't have a usable GFile location for navigation. We
     * still show them as informational cards. */
    GtkWidget *card = build_card (self,
                                  name,
                                  _("Not mounted"),
                                  icon,
                                  NULL,
                                  FALSE);

    GtkFlowBox *target = is_native_hint ? self->this_computer_box : self->network_box;
    gtk_flow_box_append (target, card);
}

static gboolean
mount_already_added (GHashTable *seen_mounts,
                     GMount     *mount)
{
    if (g_hash_table_contains (seen_mounts, mount))
    {
        return TRUE;
    }
    g_hash_table_add (seen_mounts, g_object_ref (mount));
    return FALSE;
}

static void
clear_flowbox (GtkFlowBox *box)
{
    GtkWidget *child;

    while ((child = gtk_widget_get_first_child (GTK_WIDGET (box))) != NULL)
    {
        gtk_flow_box_remove (box, child);
    }
}

static void
rebuild_cards (NautilusOtherLocationsView *self)
{
    clear_flowbox (self->this_computer_box);
    clear_flowbox (self->network_box);

    g_autoptr (GHashTable) seen_mounts = g_hash_table_new_full (g_direct_hash,
                                                                g_direct_equal,
                                                                g_object_unref,
                                                                NULL);

    /* 1) Drives → volumes → mounts. Mirrors how nautilus-sidebar walks the
     *    storage tree. */
    GList *drives = g_volume_monitor_get_connected_drives (self->volume_monitor);
    for (GList *d = drives; d != NULL; d = d->next)
    {
        GDrive *drive = d->data;
        GList *volumes = g_drive_get_volumes (drive);

        if (volumes == NULL)
        {
            /* Drive without any volume (e.g. empty optical tray). Render a
             * placeholder card on the local side. */
            g_autofree char *name = g_drive_get_name (drive);
            g_autoptr (GIcon) icon = g_drive_get_symbolic_icon (drive);
            GtkWidget *card = build_card (self, name, _("No media"), icon, NULL, FALSE);

            gtk_flow_box_append (self->this_computer_box, card);
        }

        for (GList *v = volumes; v != NULL; v = v->next)
        {
            GVolume *vol = v->data;
            g_autoptr (GMount) mount = g_volume_get_mount (vol);

            if (mount != NULL)
            {
                if (!mount_already_added (seen_mounts, mount))
                {
                    add_mount_card (self, mount);
                }
            }
            else
            {
                /* Native is a best guess based on the drive; reasonable for
                 * the local-vs-network split. */
                add_volume_unmounted_card (self, vol, TRUE);
            }
        }
        g_list_free_full (volumes, g_object_unref);
    }
    g_list_free_full (drives, g_object_unref);

    /* 2) Volumes without a drive (e.g. some loopback or virtual devices). */
    GList *volumes = g_volume_monitor_get_volumes (self->volume_monitor);
    for (GList *v = volumes; v != NULL; v = v->next)
    {
        GVolume *vol = v->data;
        g_autoptr (GDrive) drive = g_volume_get_drive (vol);
        if (drive != NULL)
        {
            continue; /* Already handled in pass 1. */
        }

        g_autoptr (GMount) mount = g_volume_get_mount (vol);

        if (mount != NULL)
        {
            if (!mount_already_added (seen_mounts, mount))
            {
                add_mount_card (self, mount);
            }
        }
        else
        {
            /* No drive, no mount: cannot tell native vs remote reliably.
             * Default to local. */
            add_volume_unmounted_card (self, vol, TRUE);
        }
    }
    g_list_free_full (volumes, g_object_unref);

    /* 3) Mounts not associated with a volume (typically GVfs network shares:
     *    SFTP, SMB, WebDAV, …). */
    GList *mounts = g_volume_monitor_get_mounts (self->volume_monitor);
    for (GList *m = mounts; m != NULL; m = m->next)
    {
        GMount *mount = m->data;
        g_autoptr (GVolume) vol = g_mount_get_volume (mount);

        if (vol != NULL)
        {
            continue; /* Handled via its volume in passes 1 or 2. */
        }
        if (mount_already_added (seen_mounts, mount))
        {
            continue;
        }
        add_mount_card (self, mount);
    }
    g_list_free_full (mounts, g_object_unref);

    /* Hide each section that ended up empty so we don't display orphan
     * headers. */
    gboolean has_local = (gtk_widget_get_first_child (GTK_WIDGET (self->this_computer_box)) != NULL);
    gboolean has_network = (gtk_widget_get_first_child (GTK_WIDGET (self->network_box)) != NULL);

    gtk_widget_set_visible (GTK_WIDGET (self->this_computer_label), has_local);
    gtk_widget_set_visible (GTK_WIDGET (self->this_computer_box), has_local);
    gtk_widget_set_visible (GTK_WIDGET (self->network_label), has_network);
    gtk_widget_set_visible (GTK_WIDGET (self->network_box), has_network);
}

static gboolean
rebuild_idle_cb (gpointer user_data)
{
    NautilusOtherLocationsView *self = user_data;

    self->rebuild_idle_id = 0;
    rebuild_cards (self);
    return G_SOURCE_REMOVE;
}

static void
schedule_rebuild (NautilusOtherLocationsView *self)
{
    /* Coalesce several monitor signals firing during a single mount/unmount
     * into one rebuild. */
    if (self->rebuild_idle_id != 0)
    {
        return;
    }
    self->rebuild_idle_id = g_idle_add (rebuild_idle_cb, self);
}

static void
on_volume_monitor_changed (GVolumeMonitor             *monitor,
                           gpointer                    object,
                           NautilusOtherLocationsView *self)
{
    schedule_rebuild (self);
}

/* ---------- GObject lifecycle ---------- */

static void
nautilus_other_locations_view_dispose (GObject *object)
{
    NautilusOtherLocationsView *self = NAUTILUS_OTHER_LOCATIONS_VIEW (object);

    if (self->rebuild_idle_id != 0)
    {
        g_source_remove (self->rebuild_idle_id);
        self->rebuild_idle_id = 0;
    }

    if (self->volume_monitor != NULL)
    {
        g_signal_handlers_disconnect_by_data (self->volume_monitor, self);
        g_clear_object (&self->volume_monitor);
    }

    G_OBJECT_CLASS (nautilus_other_locations_view_parent_class)->dispose (object);
}

static void
nautilus_other_locations_view_class_init (NautilusOtherLocationsViewClass *klass)
{
    GObjectClass *object_class = G_OBJECT_CLASS (klass);
    NautilusListBaseClass *list_base_class = NAUTILUS_LIST_BASE_CLASS (klass);

    object_class->dispose = nautilus_other_locations_view_dispose;

    list_base_class->get_view_info = real_get_view_info;
    list_base_class->get_icon_size = real_get_icon_size;
    list_base_class->get_zoom_level = real_get_zoom_level;
    list_base_class->set_zoom_level = real_set_zoom_level;
    list_base_class->get_sort_state = real_get_sort_state;
    list_base_class->set_sort_state = real_set_sort_state;
    list_base_class->set_enable_rubberband = real_set_enable_rubberband;
    list_base_class->scroll_to = real_scroll_to;
    list_base_class->popup_background_context_menu = real_popup_background_context_menu;
}

static GtkLabel *
make_section_label (const char *text)
{
    GtkWidget *label = gtk_label_new (text);

    gtk_widget_add_css_class (label, "title-2");
    gtk_label_set_xalign (GTK_LABEL (label), 0.0);
    gtk_widget_set_margin_top (label, 12);
    gtk_widget_set_margin_bottom (label, 6);
    return GTK_LABEL (label);
}

static GtkFlowBox *
make_flow_box (void)
{
    GtkWidget *box = gtk_flow_box_new ();

    gtk_flow_box_set_selection_mode (GTK_FLOW_BOX (box), GTK_SELECTION_NONE);
    gtk_flow_box_set_homogeneous (GTK_FLOW_BOX (box), TRUE);
    gtk_flow_box_set_column_spacing (GTK_FLOW_BOX (box), 12);
    gtk_flow_box_set_row_spacing (GTK_FLOW_BOX (box), 12);
    gtk_flow_box_set_min_children_per_line (GTK_FLOW_BOX (box), 1);
    gtk_flow_box_set_max_children_per_line (GTK_FLOW_BOX (box), 12);
    gtk_widget_set_halign (box, GTK_ALIGN_FILL);
    return GTK_FLOW_BOX (box);
}

static void
nautilus_other_locations_view_init (NautilusOtherLocationsView *self)
{
    GtkWidget *scrolled_window = nautilus_list_base_get_scrolled_window (NAUTILUS_LIST_BASE (self));

    gtk_widget_add_css_class (GTK_WIDGET (self), "nautilus-other-locations-view");

    /* The content lives directly in the base class's scrolled window — there
     * is no GtkListView in this view. */
    GtkWidget *content = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
    gtk_widget_set_margin_top (content, 18);
    gtk_widget_set_margin_bottom (content, 18);
    gtk_widget_set_margin_start (content, 18);
    gtk_widget_set_margin_end (content, 18);
    gtk_widget_set_valign (content, GTK_ALIGN_START);
    gtk_widget_set_hexpand (content, TRUE);

    self->content_box = GTK_BOX (content);

    self->this_computer_label = make_section_label (_("This Computer"));
    gtk_box_append (self->content_box, GTK_WIDGET (self->this_computer_label));

    self->this_computer_box = make_flow_box ();
    gtk_box_append (self->content_box, GTK_WIDGET (self->this_computer_box));

    self->network_label = make_section_label (_("Network"));
    gtk_box_append (self->content_box, GTK_WIDGET (self->network_label));

    self->network_box = make_flow_box ();
    gtk_box_append (self->content_box, GTK_WIDGET (self->network_box));

    gtk_scrolled_window_set_child (GTK_SCROLLED_WINDOW (scrolled_window), content);

    /* Wire up GVolumeMonitor live updates. */
    self->volume_monitor = g_volume_monitor_get ();
    g_signal_connect (self->volume_monitor, "drive-connected",
                      G_CALLBACK (on_volume_monitor_changed), self);
    g_signal_connect (self->volume_monitor, "drive-disconnected",
                      G_CALLBACK (on_volume_monitor_changed), self);
    g_signal_connect (self->volume_monitor, "volume-added",
                      G_CALLBACK (on_volume_monitor_changed), self);
    g_signal_connect (self->volume_monitor, "volume-removed",
                      G_CALLBACK (on_volume_monitor_changed), self);
    g_signal_connect (self->volume_monitor, "mount-added",
                      G_CALLBACK (on_volume_monitor_changed), self);
    g_signal_connect (self->volume_monitor, "mount-removed",
                      G_CALLBACK (on_volume_monitor_changed), self);
    g_signal_connect (self->volume_monitor, "mount-changed",
                      G_CALLBACK (on_volume_monitor_changed), self);

    rebuild_cards (self);

    nautilus_list_base_set_zoom_level (NAUTILUS_LIST_BASE (self),
                                       other_locations_view_info.zoom_level_standard);
}

NautilusOtherLocationsView *
nautilus_other_locations_view_new (void)
{
    return g_object_new (NAUTILUS_TYPE_OTHER_LOCATIONS_VIEW, NULL);
}
