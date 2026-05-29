/*
 * Copyright (C) 2026 The Nemo project contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "nautilus-other-locations-view.h"

#include <glib/gi18n.h>

#include "nautilus-application.h"
#include "nautilus-global-preferences.h"
#include "nautilus-list-base-private.h"
#include "nautilus-recent-servers.h"
#include "nautilus-scheme.h"
#include "nautilus-view-model.h"

/*
 * NautilusOtherLocationsView
 *
 * View that backs `other-locations:///`, rendered as a Windows "This PC"-style
 * grid of drive tiles:
 *
 *     [icon]  Name
 *             /real/path
 *     [███████      ]  used / total   (only when mounted)
 *
 * The tiles flow horizontally and wrap. Content is read straight from
 * GVolumeMonitor and grouped into "This Computer" (native mountables) and
 * "Network" (remote ones). It subclasses NautilusListBase only so
 * NautilusFilesView can host it; the "Connect to Server" bar is provided by
 * the window-level network address bar (shown for other-locations:///).
 */

#define TILE_ICON_SIZE 48
#define TILE_WIDTH     280

struct _NautilusOtherLocationsView
{
    NautilusListBase parent_instance;

    GtkBox      *content_box;
    GtkWidget   *this_computer_label;
    GtkFlowBox  *this_computer_box;
    GtkWidget   *network_label;
    GtkFlowBox  *network_box;

    GVolumeMonitor *volume_monitor;
    guint           rebuild_idle_id;

    /* Async enumeration of network:/// peers for the Networks section. */
    GCancellable   *network_cancellable;

    /* Previously connected servers (same source as the Network view's
     * "Previous" group). */
    NautilusRecentServers *recent_servers;
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
    /* No-op: the tiles view doesn't expose user-controlled zoom. */
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
    /* No-op: there's no rubberband on a flowbox of tiles. */
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

/* ---------- Tile construction ---------- */

typedef struct
{
    NautilusOtherLocationsView *self;
    GFile                      *location;
} TileActivateData;

static void
tile_activate_data_free (gpointer data)
{
    TileActivateData *td = data;

    g_clear_object (&td->location);
    g_free (td);
}

static void
on_tile_clicked (GtkButton *button,
                 gpointer   user_data)
{
    TileActivateData *td = user_data;
    NautilusApplication *app = NAUTILUS_APPLICATION (g_application_get_default ());

    if (td->location == NULL)
    {
        return;
    }

    nautilus_application_open_location_full (app, td->location, 0, NULL, NULL);
}

static gboolean
query_filesystem_usage (GFile   *root,
                        guint64 *out_total,
                        guint64 *out_used)
{
    /* g_file_query_filesystem_info() can block on remote mounts. We only ever
     * call this for native (local) roots. */
    g_autoptr (GError) error = NULL;
    g_autoptr (GFileInfo) info = g_file_query_filesystem_info (
        root,
        G_FILE_ATTRIBUTE_FILESYSTEM_SIZE "," G_FILE_ATTRIBUTE_FILESYSTEM_USED,
        NULL, &error);

    if (info == NULL)
    {
        return FALSE;
    }

    if (!g_file_info_has_attribute (info, G_FILE_ATTRIBUTE_FILESYSTEM_SIZE) ||
        !g_file_info_has_attribute (info, G_FILE_ATTRIBUTE_FILESYSTEM_USED))
    {
        return FALSE;
    }

    *out_total = g_file_info_get_attribute_uint64 (info, G_FILE_ATTRIBUTE_FILESYSTEM_SIZE);
    *out_used = g_file_info_get_attribute_uint64 (info, G_FILE_ATTRIBUTE_FILESYSTEM_USED);
    return TRUE;
}

/*
 * Build a Windows-style tile:
 *
 *   [icon]  Name
 *           real path
 *   [bar]   used / total
 *
 * @location is the navigation target (NULL → inert tile). @path_label is the
 * real path / device (omitted when empty). Capacity is shown only when
 * @total > 0 (i.e. the volume is mounted).
 */
static GtkWidget *
build_tile (NautilusOtherLocationsView *self,
            const char                 *name,
            const char                 *path_label,
            GIcon                      *icon,
            GFile                      *location,
            guint64                     total,
            guint64                     used)
{
    /* A GtkButton gives keyboard activation and the "card" libadwaita styling
     * for free. */
    GtkWidget *tile = gtk_button_new ();
    gtk_widget_add_css_class (tile, "card");
    gtk_widget_add_css_class (tile, "activatable");
    gtk_widget_set_size_request (tile, TILE_WIDTH, -1);
    gtk_widget_set_hexpand (tile, FALSE);
    gtk_widget_set_vexpand (tile, FALSE);
    gtk_widget_set_valign (tile, GTK_ALIGN_START);

    /* Sort keys read back by the flowbox sort func: mounted (navigable) tiles
     * first, then alphabetical by name. */
    g_object_set_data (G_OBJECT (tile), "sort-mounted", GINT_TO_POINTER (location != NULL));
    g_object_set_data_full (G_OBJECT (tile), "sort-name", g_strdup (name), g_free);

    GtkWidget *box = gtk_box_new (GTK_ORIENTATION_VERTICAL, 6);
    gtk_widget_set_margin_top (box, 10);
    gtk_widget_set_margin_bottom (box, 10);
    gtk_widget_set_margin_start (box, 12);
    gtk_widget_set_margin_end (box, 12);
    gtk_button_set_child (GTK_BUTTON (tile), box);

    /* --- Top: [icon]  name / path  --- */
    GtkWidget *head = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 12);

    g_autoptr (GIcon) fallback_icon = NULL;
    GIcon *effective_icon = icon;

    if (effective_icon == NULL)
    {
        fallback_icon = g_themed_icon_new ("drive-harddisk-symbolic");
        effective_icon = fallback_icon;
    }

    GtkWidget *image = gtk_image_new_from_gicon (effective_icon);
    gtk_image_set_pixel_size (GTK_IMAGE (image), TILE_ICON_SIZE);
    gtk_widget_set_valign (image, GTK_ALIGN_CENTER);
    gtk_box_append (GTK_BOX (head), image);

    /* Name on top, real path right below it, stacked next to the icon. */
    GtkWidget *text = gtk_box_new (GTK_ORIENTATION_VERTICAL, 2);
    gtk_widget_set_valign (text, GTK_ALIGN_CENTER);
    gtk_widget_set_hexpand (text, TRUE);

    GtkWidget *name_label = gtk_label_new (name);
    gtk_widget_add_css_class (name_label, "heading");
    gtk_label_set_xalign (GTK_LABEL (name_label), 0.0);
    gtk_label_set_ellipsize (GTK_LABEL (name_label), PANGO_ELLIPSIZE_END);
    gtk_box_append (GTK_BOX (text), name_label);

    if (path_label != NULL && *path_label != '\0')
    {
        GtkWidget *path_lbl = gtk_label_new (path_label);
        gtk_widget_add_css_class (path_lbl, "dim-label");
        gtk_widget_add_css_class (path_lbl, "caption");
        gtk_label_set_xalign (GTK_LABEL (path_lbl), 0.0);
        gtk_label_set_ellipsize (GTK_LABEL (path_lbl), PANGO_ELLIPSIZE_MIDDLE);
        gtk_box_append (GTK_BOX (text), path_lbl);
    }

    gtk_box_append (GTK_BOX (head), text);
    gtk_box_append (GTK_BOX (box), head);

    /* --- Capacity: thin bar + "used / total" on one compact line, only when
     *     mounted (total known) --- */
    if (total > 0)
    {
        double fraction = (double) used / (double) total;

        GtkWidget *cap_row = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 8);

        GtkWidget *bar = gtk_progress_bar_new ();
        gtk_progress_bar_set_fraction (GTK_PROGRESS_BAR (bar), CLAMP (fraction, 0.0, 1.0));
        gtk_widget_set_hexpand (bar, TRUE);
        gtk_widget_set_valign (bar, GTK_ALIGN_CENTER);
        gtk_widget_add_css_class (bar, "otherlocations-usage");
        gtk_box_append (GTK_BOX (cap_row), bar);

        g_autofree char *used_str = g_format_size (used);
        g_autofree char *total_str = g_format_size (total);
        /* Translators: "<used> / <total>" capacity on a drive tile. */
        g_autofree char *cap_str = g_strdup_printf (_("%s / %s"), used_str, total_str);
        GtkWidget *cap_lbl = gtk_label_new (cap_str);
        gtk_widget_add_css_class (cap_lbl, "dim-label");
        gtk_widget_add_css_class (cap_lbl, "numeric");
        gtk_widget_add_css_class (cap_lbl, "caption");
        gtk_label_set_xalign (GTK_LABEL (cap_lbl), 1.0);
        gtk_box_append (GTK_BOX (cap_row), cap_lbl);

        gtk_box_append (GTK_BOX (box), cap_row);
    }

    /* Click → navigate. Disable activation when there's nowhere to go. */
    if (location != NULL)
    {
        TileActivateData *td = g_new0 (TileActivateData, 1);
        td->self = self;
        td->location = g_object_ref (location);
        g_signal_connect_data (tile, "clicked",
                               G_CALLBACK (on_tile_clicked), td,
                               (GClosureNotify) tile_activate_data_free,
                               G_CONNECT_DEFAULT);
    }
    else
    {
        gtk_widget_set_sensitive (tile, FALSE);
    }

    return tile;
}

/* ---------- Volume monitor → tiles ---------- */

static void
add_mount_tile (NautilusOtherLocationsView *self,
                GMount                     *mount)
{
    g_autofree char *name = g_mount_get_name (mount);
    g_autoptr (GIcon) icon = g_mount_get_symbolic_icon (mount);
    g_autoptr (GFile) root = g_mount_get_default_location (mount);
    gboolean is_native = g_file_is_native (root);
    g_autofree char *path = NULL;
    guint64 total = 0;
    guint64 used = 0;

    /* The Networks section can be turned off independently from the sidebar
     * Network entry. */
    if (!is_native &&
        !g_settings_get_boolean (nautilus_preferences,
                                 NAUTILUS_PREFERENCES_OTHER_LOCATIONS_SHOW_NETWORK))
    {
        return;
    }

    if (is_native)
    {
        path = g_file_get_path (root);
        query_filesystem_usage (root, &total, &used);
    }
    else
    {
        path = g_file_get_uri (root);
    }

    GtkWidget *tile = build_tile (self, name, path, icon, root, total, used);
    GtkFlowBox *target = is_native ? self->this_computer_box : self->network_box;

    gtk_flow_box_append (target, tile);
}

static void
add_unmounted_volume_tile (NautilusOtherLocationsView *self,
                           GVolume                    *volume,
                           gboolean                    is_native_hint)
{
    g_autofree char *name = g_volume_get_name (volume);
    g_autoptr (GIcon) icon = g_volume_get_symbolic_icon (volume);
    /* Device node (e.g. /dev/sda1) stands in for the real path while the
     * volume is unmounted. */
    g_autofree char *device = g_volume_get_identifier (volume, G_VOLUME_IDENTIFIER_KIND_UNIX_DEVICE);

    GtkWidget *tile = build_tile (self, name, device, icon, NULL, 0, 0);
    GtkFlowBox *target = is_native_hint ? self->this_computer_box : self->network_box;

    gtk_flow_box_append (target, tile);
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
update_section_visibility (NautilusOtherLocationsView *self)
{
    gboolean has_local = (gtk_widget_get_first_child (GTK_WIDGET (self->this_computer_box)) != NULL);
    gboolean has_network = (gtk_widget_get_first_child (GTK_WIDGET (self->network_box)) != NULL);

    gtk_widget_set_visible (self->this_computer_label, has_local);
    gtk_widget_set_visible (GTK_WIDGET (self->this_computer_box), has_local);
    gtk_widget_set_visible (self->network_label, has_network);
    gtk_widget_set_visible (GTK_WIDGET (self->network_box), has_network);
}

/* Async enumeration of network:/// → network location tiles (LAN peers,
 * "Windows Network", …). Self is kept alive by a ref for the operation's
 * lifetime; the captured cancellable is cancelled on rebuild/dispose. */
typedef struct
{
    NautilusOtherLocationsView *self;
    GCancellable               *cancellable;
    GFileEnumerator            *enumerator;
} NetEnum;

static void
net_enum_free (NetEnum *ne)
{
    g_clear_object (&ne->enumerator);
    g_clear_object (&ne->cancellable);
    g_clear_object (&ne->self);
    g_free (ne);
}

static void
on_net_next (GObject      *source,
             GAsyncResult *res,
             gpointer      data)
{
    NetEnum *ne = data;
    g_autoptr (GError) error = NULL;
    GList *infos = g_file_enumerator_next_files_finish (ne->enumerator, res, &error);

    if (g_cancellable_is_cancelled (ne->cancellable))
    {
        g_list_free_full (infos, g_object_unref);
        net_enum_free (ne);
        return;
    }

    for (GList *l = infos; l != NULL; l = l->next)
    {
        GFileInfo *info = l->data;
        const char *display_name = g_file_info_get_display_name (info);
        GIcon *icon = g_file_info_get_symbolic_icon (info);
        g_autoptr (GFile) child = g_file_enumerator_get_child (ne->enumerator, info);

        GtkWidget *tile = build_tile (ne->self, display_name, NULL, icon, child, 0, 0);
        gtk_flow_box_append (ne->self->network_box, tile);
    }

    gboolean had_items = (infos != NULL);
    g_list_free_full (infos, g_object_unref);

    if (had_items)
    {
        g_file_enumerator_next_files_async (ne->enumerator, 32, G_PRIORITY_DEFAULT,
                                            ne->cancellable, on_net_next, ne);
        return;
    }

    update_section_visibility (ne->self);
    net_enum_free (ne);
}

static void
on_net_enumerate (GObject      *source,
                  GAsyncResult *res,
                  gpointer      data)
{
    NetEnum *ne = data;
    g_autoptr (GError) error = NULL;
    GFileEnumerator *enumerator = g_file_enumerate_children_finish (G_FILE (source), res, &error);

    if (enumerator == NULL || g_cancellable_is_cancelled (ne->cancellable))
    {
        g_clear_object (&enumerator);
        net_enum_free (ne);
        return;
    }

    ne->enumerator = enumerator;
    g_file_enumerator_next_files_async (ne->enumerator, 32, G_PRIORITY_DEFAULT,
                                        ne->cancellable, on_net_next, ne);
}

static void
start_network_enumeration (NautilusOtherLocationsView *self)
{
    g_autoptr (GFile) net = g_file_new_for_uri (SCHEME_NETWORK ":///");
    NetEnum *ne = g_new0 (NetEnum, 1);

    self->network_cancellable = g_cancellable_new ();
    ne->self = g_object_ref (self);
    ne->cancellable = g_object_ref (self->network_cancellable);

    g_file_enumerate_children_async (
        net,
        G_FILE_ATTRIBUTE_STANDARD_NAME ","
        G_FILE_ATTRIBUTE_STANDARD_DISPLAY_NAME ","
        G_FILE_ATTRIBUTE_STANDARD_SYMBOLIC_ICON ","
        G_FILE_ATTRIBUTE_STANDARD_TARGET_URI,
        G_FILE_QUERY_INFO_NONE, G_PRIORITY_DEFAULT,
        ne->cancellable, on_net_enumerate, ne);
}

/* Previously connected servers (FTP/SMB/WebDAV…), the "Previous" group in the
 * Network view. */
static void
add_recent_server_tiles (NautilusOtherLocationsView *self)
{
    GList *infos = nautilus_recent_servers_get_infos (self->recent_servers);

    for (GList *l = infos; l != NULL; l = l->next)
    {
        GFileInfo *info = l->data;
        const char *display_name = g_file_info_get_display_name (info);
        const char *uri = g_file_info_get_attribute_string (info, G_FILE_ATTRIBUTE_STANDARD_TARGET_URI);
        GIcon *icon = g_file_info_get_symbolic_icon (info);

        if (uri == NULL)
        {
            continue;
        }

        g_autoptr (GFile) target = g_file_new_for_uri (uri);
        const char *title = (display_name != NULL && *display_name != '\0' &&
                             g_strcmp0 (display_name, "/") != 0) ? display_name : uri;
        GtkWidget *tile = build_tile (self, title, uri, icon, target, 0, 0);

        gtk_flow_box_append (self->network_box, tile);
    }

    g_list_free_full (infos, g_object_unref);
}

static void
rebuild_tiles (NautilusOtherLocationsView *self)
{
    /* Abort any in-flight network enumeration from a previous rebuild. */
    g_cancellable_cancel (self->network_cancellable);
    g_clear_object (&self->network_cancellable);

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
            /* Drive without any volume (e.g. empty optical tray). */
            g_autofree char *name = g_drive_get_name (drive);
            g_autoptr (GIcon) icon = g_drive_get_symbolic_icon (drive);
            GtkWidget *tile = build_tile (self, name, _("No media"), icon, NULL, 0, 0);

            gtk_flow_box_append (self->this_computer_box, tile);
        }

        for (GList *v = volumes; v != NULL; v = v->next)
        {
            GVolume *vol = v->data;
            g_autoptr (GMount) mount = g_volume_get_mount (vol);

            if (mount != NULL)
            {
                if (!mount_already_added (seen_mounts, mount))
                {
                    add_mount_tile (self, mount);
                }
            }
            else
            {
                /* Native is a best guess based on the drive; reasonable for
                 * the local-vs-network split. */
                add_unmounted_volume_tile (self, vol, TRUE);
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
                add_mount_tile (self, mount);
            }
        }
        else
        {
            /* No drive, no mount: cannot tell native vs remote reliably.
             * Default to local. */
            add_unmounted_volume_tile (self, vol, TRUE);
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
        add_mount_tile (self, mount);
    }
    g_list_free_full (mounts, g_object_unref);

    /* Hide each section that ended up empty so we don't display orphan
     * headers. (Re-evaluated again when async network peers arrive.) */
    update_section_visibility (self);

    /* Network locations (LAN peers, Windows network, …) are listed via an
     * async enumeration of network:///, gated by the same preference that
     * controls remote mounts and the server bar. */
    if (g_settings_get_boolean (nautilus_preferences,
                                NAUTILUS_PREFERENCES_OTHER_LOCATIONS_SHOW_NETWORK))
    {
        add_recent_server_tiles (self);
        update_section_visibility (self);
        start_network_enumeration (self);
    }
}

static gboolean
rebuild_idle_cb (gpointer user_data)
{
    NautilusOtherLocationsView *self = user_data;

    self->rebuild_idle_id = 0;
    rebuild_tiles (self);
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

    g_cancellable_cancel (self->network_cancellable);
    g_clear_object (&self->network_cancellable);

    if (self->recent_servers != NULL)
    {
        g_signal_handlers_disconnect_by_data (self->recent_servers, self);
        g_clear_object (&self->recent_servers);
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

static GtkWidget *
make_section_label (const char *text)
{
    GtkWidget *label = gtk_label_new (text);

    gtk_widget_add_css_class (label, "heading");
    gtk_label_set_xalign (GTK_LABEL (label), 0.0);
    gtk_widget_set_margin_top (label, 6);
    gtk_widget_set_margin_bottom (label, 6);
    return label;
}

/* Order tiles: mounted (navigable) first, then alphabetical by name. */
static int
sort_tiles (GtkFlowBoxChild *a,
            GtkFlowBoxChild *b,
            gpointer         user_data)
{
    GtkWidget *ta = gtk_flow_box_child_get_child (a);
    GtkWidget *tb = gtk_flow_box_child_get_child (b);
    int ma = GPOINTER_TO_INT (g_object_get_data (G_OBJECT (ta), "sort-mounted"));
    int mb = GPOINTER_TO_INT (g_object_get_data (G_OBJECT (tb), "sort-mounted"));

    if (ma != mb)
    {
        return mb - ma; /* mounted (1) before unmounted (0) */
    }

    const char *na = g_object_get_data (G_OBJECT (ta), "sort-name");
    const char *nb = g_object_get_data (G_OBJECT (tb), "sort-name");

    return g_utf8_collate (na != NULL ? na : "", nb != NULL ? nb : "");
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
    gtk_flow_box_set_sort_func (GTK_FLOW_BOX (box), sort_tiles, NULL, NULL);
    gtk_widget_set_halign (box, GTK_ALIGN_START);
    return GTK_FLOW_BOX (box);
}

static void
nautilus_other_locations_view_init (NautilusOtherLocationsView *self)
{
    GtkWidget *scrolled_window = nautilus_list_base_get_scrolled_window (NAUTILUS_LIST_BASE (self));

    gtk_widget_add_css_class (GTK_WIDGET (self), "nautilus-other-locations-view");

    /* Thin usage bar (the default progressbar trough is too tall for a tile).
     * Installed once per display. */
    static gsize css_once = 0;
    if (g_once_init_enter (&css_once))
    {
        g_autoptr (GtkCssProvider) provider = gtk_css_provider_new ();
        gtk_css_provider_load_from_string (
            provider,
            "progressbar.otherlocations-usage,"
            "progressbar.otherlocations-usage > trough,"
            "progressbar.otherlocations-usage > trough > progress {"
            "  min-height: 4px;"
            "}");
        gtk_style_context_add_provider_for_display (
            gdk_display_get_default (),
            GTK_STYLE_PROVIDER (provider),
            GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
        g_once_init_leave (&css_once, 1);
    }

    /* The content lives directly in the base class's scrolled window — there
     * is no GtkListView in this view. */
    GtkWidget *content = gtk_box_new (GTK_ORIENTATION_VERTICAL, 12);
    gtk_widget_set_margin_top (content, 18);
    gtk_widget_set_margin_bottom (content, 18);
    gtk_widget_set_margin_start (content, 18);
    gtk_widget_set_margin_end (content, 18);
    gtk_widget_set_valign (content, GTK_ALIGN_START);
    gtk_widget_set_hexpand (content, TRUE);

    self->content_box = GTK_BOX (content);

    self->this_computer_label = make_section_label (_("This Computer"));
    gtk_box_append (self->content_box, self->this_computer_label);

    self->this_computer_box = make_flow_box ();
    gtk_box_append (self->content_box, GTK_WIDGET (self->this_computer_box));

    self->network_label = make_section_label (_("Network"));
    gtk_box_append (self->content_box, self->network_label);

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

    /* Rebuild when the "show Network section" preference is toggled. */
    g_signal_connect_object (nautilus_preferences,
                             "changed::" NAUTILUS_PREFERENCES_OTHER_LOCATIONS_SHOW_NETWORK,
                             G_CALLBACK (schedule_rebuild), self,
                             G_CONNECT_SWAPPED);

    /* Previously connected servers, refreshed on load and on any change. */
    self->recent_servers = nautilus_recent_servers_new ();
    g_signal_connect_object (self->recent_servers, "notify::loading",
                             G_CALLBACK (schedule_rebuild), self, G_CONNECT_SWAPPED);
    g_signal_connect_object (self->recent_servers, "added",
                             G_CALLBACK (schedule_rebuild), self, G_CONNECT_SWAPPED);
    g_signal_connect_object (self->recent_servers, "changed",
                             G_CALLBACK (schedule_rebuild), self, G_CONNECT_SWAPPED);
    g_signal_connect_object (self->recent_servers, "removed",
                             G_CALLBACK (schedule_rebuild), self, G_CONNECT_SWAPPED);

    rebuild_tiles (self);

    nautilus_list_base_set_zoom_level (NAUTILUS_LIST_BASE (self),
                                       other_locations_view_info.zoom_level_standard);
}

NautilusOtherLocationsView *
nautilus_other_locations_view_new (void)
{
    return g_object_new (NAUTILUS_TYPE_OTHER_LOCATIONS_VIEW, NULL);
}
