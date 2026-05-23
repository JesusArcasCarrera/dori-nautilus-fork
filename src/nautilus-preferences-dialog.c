/* nautilus-preferences-window.c - Functions to create and show the nautilus
 *  preference window.
 *
 *  Copyright (C) 2002 Jan Arne Petersen
 *  Copyright (C) 2016 Carlos Soriano <csoriano@gnome.com>
 *
 *  The Gnome Library is free software; you can redistribute it and/or
 *  modify it under the terms of the GNU Library General Public License as
 *  published by the Free Software Foundation; either version 2 of the
 *  License, or (at your option) any later version.
 *
 *  The Gnome Library is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 *  Library General Public License for more details.
 *
 *  You should have received a copy of the GNU Library General Public
 *  License along with the Gnome Library; see the file COPYING.LIB.  If not,
 *  see <http://www.gnu.org/licenses/>.
 *
 *  Authors: Jan Arne Petersen <jpetersen@uni-bonn.de>
 */

#include <config.h>

#include "nautilus-preferences-dialog.h"

#include <adwaita.h>
#include <gio/gio.h>
#include <glib/gi18n.h>
#include <nautilus-extension.h>

#include "nautilus-column-utilities.h"
#include "nautilus-date-utilities.h"
#include "nautilus-global-preferences.h"
#include "nemo-action-editor.h"
#include "nemo-action-manager.h"

/* bool preferences */
#define NAUTILUS_PREFERENCES_DIALOG_FOLDERS_FIRST_WIDGET                       \
        "sort_folders_first_row"
#define NAUTILUS_PREFERENCES_DIALOG_DELETE_PERMANENTLY_WIDGET                  \
        "show_delete_permanently_row"
#define NAUTILUS_PREFERENCES_DIALOG_CREATE_LINK_WIDGET                         \
        "show_create_link_row"
#define NAUTILUS_PREFERENCES_DIALOG_LIST_VIEW_USE_TREE_WIDGET                  \
        "use_tree_view_row"

/* combo preferences */
#define NAUTILUS_PREFERENCES_DIALOG_OPEN_ACTION_COMBO                          \
        "open_action_row"
#define NAUTILUS_PREFERENCES_DIALOG_SEARCH_RECURSIVE_ROW                       \
        "search_recursive_row"
#define NAUTILUS_PREFERENCES_DIALOG_THUMBNAILS_ROW                       \
        "thumbnails_row"
#define NAUTILUS_PREFERENCES_DIALOG_COUNT_ROW                       \
        "count_row"
#define NAUTILUS_PREFERENCES_DIALOG_TYPE_TO_ACTION_ROW                         \
        "type_to_action_row"

static const char * const speed_tradeoff_values[] =
{
    "local-only", "always", "never",
    NULL
};

static const char * const click_behavior_values[] = {"single", "double", NULL};

static const char * const type_to_action_values[] =
{
    "filter", "locate", "search", "none",
    NULL
};

static void
bind_builder_bool (GtkBuilder *builder,
                   GSettings  *settings,
                   const char *widget_name,
                   const char *prefs)
{
    g_settings_bind (settings, prefs, gtk_builder_get_object (builder, widget_name),
                     "active", G_SETTINGS_BIND_DEFAULT);
}

/* Translators: Both %s will be replaced with formatted timestamps. */
#define DATE_FORMAT_ROW_SUBTITLE _("Examples: “%s”, “%s”")

static void
setup_detailed_date (GtkBuilder *builder)
{
    AdwActionRow *simple_row = ADW_ACTION_ROW (gtk_builder_get_object (builder, "date_format_simple_row"));
    AdwActionRow *detailed_row = ADW_ACTION_ROW (gtk_builder_get_object (builder, "date_format_detailed_row"));

    g_autoptr (GDateTime) now = g_date_time_new_now_local ();
    g_autoptr (GDateTime) earlier = g_date_time_add_days (now, -3);

    g_autofree gchar *simple_date_now = nautilus_date_preview_detailed_format (now, FALSE);
    g_autofree gchar *simple_date_earlier = nautilus_date_preview_detailed_format (earlier, FALSE);
    g_autofree gchar *simple_row_subtitle = g_strdup_printf (DATE_FORMAT_ROW_SUBTITLE,
                                                             simple_date_now,
                                                             simple_date_earlier);
    adw_action_row_set_subtitle (simple_row, simple_row_subtitle);

    g_autofree gchar *detailed_date_now = nautilus_date_preview_detailed_format (now, TRUE);
    g_autofree gchar *detailed_date_earlier = nautilus_date_preview_detailed_format (earlier, TRUE);
    g_autofree gchar *detailed_row_subtitle = g_strdup_printf (DATE_FORMAT_ROW_SUBTITLE,
                                                               detailed_date_now,
                                                               detailed_date_earlier);
    adw_action_row_set_subtitle (detailed_row, detailed_row_subtitle);
}

static GVariant *
combo_row_mapping_set (const GValue       *gvalue,
                       const GVariantType *expected_type,
                       gpointer            user_data)
{
    const gchar **values = user_data;

    return g_variant_new_string (values[g_value_get_uint (gvalue)]);
}

static gboolean
combo_row_mapping_get (GValue   *gvalue,
                       GVariant *variant,
                       gpointer  user_data)
{
    const gchar **values = user_data;
    const gchar *value;

    value = g_variant_get_string (variant, NULL);

    for (int i = 0; values[i]; i++)
    {
        if (g_strcmp0 (value, values[i]) == 0)
        {
            g_value_set_uint (gvalue, i);

            return TRUE;
        }
    }

    return FALSE;
}

static void
bind_builder_combo_row (GtkBuilder  *builder,
                        GSettings   *settings,
                        const char  *widget_name,
                        const char  *prefs,
                        const char **values)
{
    g_settings_bind_with_mapping (settings, prefs, gtk_builder_get_object (builder, widget_name),
                                  "selected", G_SETTINGS_BIND_DEFAULT,
                                  combo_row_mapping_get, combo_row_mapping_set,
                                  (gpointer) values, NULL);
}

static void
setup_combo (GtkBuilder  *builder,
             const char  *widget_name,
             const char **strings)
{
    AdwComboRow *combo_row;
    g_autoptr (GtkStringList) list_store = NULL;

    combo_row = (AdwComboRow *) gtk_builder_get_object (builder, widget_name);
    g_assert (ADW_IS_COMBO_ROW (combo_row));

    list_store = gtk_string_list_new (strings);
    adw_combo_row_set_model (combo_row, G_LIST_MODEL (list_store));
}

/* The list of rows currently shown in the Custom Actions group; tracked so
 * we can drop them before repopulating when the action manager fires
 * "changed". Stored on the group widget via g_object_set_data. */
#define ROWS_KEY "nemo-custom-actions-rows"
#define MANAGER_KEY "nemo-custom-actions-manager"
#define ACTION_ID_KEY "nemo-action-id"

static void rebuild_custom_actions_rows (AdwPreferencesGroup *group,
                                         NemoActionManager   *manager);

static void
on_edit_action_clicked (GtkButton *button,
                        gpointer   user_data)
{
    const char *id = user_data;
    g_autoptr (NemoActionManager) manager = nemo_action_manager_dup_singleton ();
    NemoAction *action = nemo_action_manager_get_action (manager, id);
    GtkRoot *root = gtk_widget_get_root (GTK_WIDGET (button));

    if (action == NULL || !GTK_IS_WIDGET (root))
    {
        return;
    }
    nemo_action_editor_present (action,
                                nemo_action_manager_get_actions_dir (manager),
                                GTK_WIDGET (root));
}

static void
on_delete_response (AdwAlertDialog *dialog,
                    GAsyncResult   *result,
                    gpointer        user_data)
{
    g_autofree char *id = user_data;
    const char *response = adw_alert_dialog_choose_finish (dialog, result);

    if (g_strcmp0 (response, "delete") == 0)
    {
        g_autoptr (NemoActionManager) manager = nemo_action_manager_dup_singleton ();
        g_autoptr (GError) error = NULL;

        if (!nemo_action_manager_delete_action (manager, id, &error))
        {
            g_warning ("Could not delete action “%s”: %s", id, error->message);
        }
    }
}

static void
on_delete_action_clicked (GtkButton *button,
                          gpointer   user_data)
{
    const char *id = user_data;
    GtkRoot *root = gtk_widget_get_root (GTK_WIDGET (button));
    AdwAlertDialog *dialog;

    if (!GTK_IS_WIDGET (root))
    {
        return;
    }

    dialog = ADW_ALERT_DIALOG (adw_alert_dialog_new (_("Delete action?"),
        _("The corresponding .nemo_action file will be removed permanently.")));
    adw_alert_dialog_add_responses (dialog,
                                    "cancel", _("_Cancel"),
                                    "delete", _("_Delete"),
                                    NULL);
    adw_alert_dialog_set_response_appearance (dialog, "delete",
                                              ADW_RESPONSE_DESTRUCTIVE);
    adw_alert_dialog_set_default_response (dialog, "cancel");
    adw_alert_dialog_set_close_response (dialog, "cancel");

    adw_alert_dialog_choose (dialog, GTK_WIDGET (root), NULL,
                             (GAsyncReadyCallback) on_delete_response,
                             g_strdup (id));
}

static void
on_add_action_clicked (GtkButton *button,
                       gpointer   user_data)
{
    g_autoptr (NemoActionManager) manager = nemo_action_manager_dup_singleton ();
    GtkRoot *root = gtk_widget_get_root (GTK_WIDGET (button));

    if (!GTK_IS_WIDGET (root))
    {
        return;
    }
    nemo_action_editor_present (NULL,
                                nemo_action_manager_get_actions_dir (manager),
                                GTK_WIDGET (root));
}

static void
on_manager_changed (NemoActionManager   *manager,
                    AdwPreferencesGroup *group)
{
    rebuild_custom_actions_rows (group, manager);
}

static GtkWidget *
make_suffix_button (const char  *icon_name,
                    const char  *tooltip,
                    GCallback    handler,
                    const char  *action_id)
{
    GtkWidget *button = gtk_button_new_from_icon_name (icon_name);

    gtk_widget_set_tooltip_text (button, tooltip);
    gtk_widget_set_valign (button, GTK_ALIGN_CENTER);
    gtk_widget_add_css_class (button, "flat");
    g_signal_connect_data (button, "clicked", handler,
                           g_strdup (action_id),
                           (GClosureNotify) g_free, 0);

    return button;
}

static void
rebuild_custom_actions_rows (AdwPreferencesGroup *group,
                             NemoActionManager   *manager)
{
    GList *previous;
    GList *actions;
    GList *current = NULL;
    const char *last_group = NULL;

    /* Drop the rows we added last time. The group's title + description and
     * the header-suffix Add button are not in this list, so they stay. */
    previous = g_object_get_data (G_OBJECT (group), ROWS_KEY);
    for (GList *l = previous; l != NULL; l = l->next)
    {
        adw_preferences_group_remove (group, GTK_WIDGET (l->data));
    }
    g_list_free (previous);
    g_object_set_data (G_OBJECT (group), ROWS_KEY, NULL);

    actions = nemo_action_manager_get_actions (manager);

    if (actions == NULL)
    {
        AdwActionRow *empty = ADW_ACTION_ROW (adw_action_row_new ());

        adw_preferences_row_set_title (ADW_PREFERENCES_ROW (empty),
                                       _("No custom actions yet"));
        adw_action_row_set_subtitle (empty,
            _("Click the + button to create one, or drop a .nemo_action file into ~/.local/share/nemo/actions/."));
        adw_preferences_group_add (group, GTK_WIDGET (empty));
        current = g_list_prepend (current, empty);
        g_object_set_data (G_OBJECT (group), ROWS_KEY, current);
        return;
    }

    for (GList *l = actions; l != NULL; l = l->next)
    {
        NemoAction *action = l->data;
        const char *name = nemo_action_get_name (action);
        const char *comment = nemo_action_get_comment (action);
        const char *icon = nemo_action_get_icon_name (action);
        const char *group_name = nemo_action_get_group (action);
        const char *id = nemo_action_get_id (action);
        AdwActionRow *row;

        if (group_name == NULL)
        {
            group_name = "";
        }
        if (g_strcmp0 (group_name, last_group) != 0)
        {
            AdwActionRow *header = ADW_ACTION_ROW (adw_action_row_new ());
            g_autofree char *title = g_strdup_printf ("— %s —",
                                                      *group_name != '\0' ?
                                                      group_name : _("Ungrouped"));

            adw_preferences_row_set_title (ADW_PREFERENCES_ROW (header), title);
            gtk_widget_set_sensitive (GTK_WIDGET (header), FALSE);
            adw_preferences_group_add (group, GTK_WIDGET (header));
            current = g_list_prepend (current, header);
            last_group = group_name;
        }

        row = ADW_ACTION_ROW (adw_action_row_new ());
        adw_preferences_row_set_title (ADW_PREFERENCES_ROW (row), name);
        if (comment != NULL && *comment != '\0')
        {
            adw_action_row_set_subtitle (row, comment);
        }
        if (icon != NULL && *icon != '\0')
        {
            adw_action_row_add_prefix (row, gtk_image_new_from_icon_name (icon));
        }

        adw_action_row_add_suffix (row,
            make_suffix_button ("document-edit-symbolic", _("Edit"),
                                G_CALLBACK (on_edit_action_clicked), id));
        adw_action_row_add_suffix (row,
            make_suffix_button ("user-trash-symbolic", _("Delete"),
                                G_CALLBACK (on_delete_action_clicked), id));

        adw_preferences_group_add (group, GTK_WIDGET (row));
        current = g_list_prepend (current, row);
    }

    g_object_set_data (G_OBJECT (group), ROWS_KEY, current);
}

/* Set up the "Custom Actions" preferences page: an Add button in the group
 * header, one editable row per loaded .nemo_action, and a live reload hooked
 * to the action manager so external edits (and the editor's saves/deletes)
 * appear without reopening the dialog. */
static void
setup_custom_actions_page (GtkBuilder *builder)
{
    NemoActionManager *manager = nemo_action_manager_dup_singleton ();
    AdwPreferencesGroup *group;
    GtkWidget *add_button;

    group = ADW_PREFERENCES_GROUP (gtk_builder_get_object (builder,
                                                           "custom_actions_group"));

    /* Tie the manager's lifetime to the group so the signal stays valid as
     * long as the dialog is open, but is released when it closes. */
    g_object_set_data_full (G_OBJECT (group), MANAGER_KEY, manager,
                            g_object_unref);

    add_button = gtk_button_new_from_icon_name ("list-add-symbolic");
    gtk_widget_set_tooltip_text (add_button, _("Add an action"));
    gtk_widget_add_css_class (add_button, "flat");
    g_signal_connect (add_button, "clicked",
                      G_CALLBACK (on_add_action_clicked), NULL);
    adw_preferences_group_set_header_suffix (group, add_button);

    rebuild_custom_actions_rows (group, manager);

    g_signal_connect_object (manager, "changed",
                             G_CALLBACK (on_manager_changed), group,
                             G_CONNECT_DEFAULT);
}

/* ---------- Drives overview page ---------- */

/* Rows currently shown in the Drives group; we drop them before every
 * rebuild triggered by GVolumeMonitor signals. Stored on the group
 * widget so its lifetime matches the dialog. */
#define DRIVES_ROWS_KEY "nemo-drives-rows"
#define DRIVES_MONITOR_KEY "nemo-drives-monitor"
#define DRIVES_REBUILD_PENDING_KEY "nemo-drives-rebuild-pending"

static void rebuild_drives_rows (AdwPreferencesGroup *group);

/* Compose the size summary in the same shape the sidebar tooltip uses:
 * "<free> / <used>". Returns NULL when the filesystem doesn't report
 * sizes (e.g. virtual mounts) so callers can fall back to a dash. */
static char *
build_drive_size_summary (GFile    *root,
                          double   *out_fraction)
{
    g_autoptr (GFileInfo) info = NULL;
    guint64 total;
    guint64 free_bytes;
    guint64 used_bytes;
    g_autofree char *free_str = NULL;
    g_autofree char *used_str = NULL;

    if (out_fraction != NULL)
    {
        *out_fraction = -1.0;
    }

    if (root == NULL || !g_file_is_native (root))
    {
        return NULL;
    }

    info = g_file_query_filesystem_info (root,
                                         G_FILE_ATTRIBUTE_FILESYSTEM_SIZE ","
                                         G_FILE_ATTRIBUTE_FILESYSTEM_FREE,
                                         NULL, NULL);
    if (info == NULL ||
        !g_file_info_has_attribute (info, G_FILE_ATTRIBUTE_FILESYSTEM_SIZE) ||
        !g_file_info_has_attribute (info, G_FILE_ATTRIBUTE_FILESYSTEM_FREE))
    {
        return NULL;
    }

    total = g_file_info_get_attribute_uint64 (info, G_FILE_ATTRIBUTE_FILESYSTEM_SIZE);
    free_bytes = g_file_info_get_attribute_uint64 (info, G_FILE_ATTRIBUTE_FILESYSTEM_FREE);
    used_bytes = total > free_bytes ? total - free_bytes : 0;

    if (total > 0 && out_fraction != NULL)
    {
        *out_fraction = CLAMP ((double) used_bytes / (double) total, 0.0, 1.0);
    }

    free_str = g_format_size (free_bytes);
    used_str = g_format_size (used_bytes);

    return g_strdup_printf ("%s / %s", free_str, used_str);
}

/* Pull the best symbolic icon for a row: mount first (most specific),
 * then volume, then drive. Caller takes ownership. */
static GIcon *
pick_drive_icon (GMount  *mount,
                 GVolume *volume,
                 GDrive  *drive)
{
    GIcon *icon = NULL;

    if (mount != NULL)
    {
        icon = g_mount_get_symbolic_icon (mount);
    }
    if (icon == NULL && volume != NULL)
    {
        icon = g_volume_get_symbolic_icon (volume);
    }
    if (icon == NULL && drive != NULL)
    {
        icon = g_drive_get_symbolic_icon (drive);
    }
    if (icon == NULL)
    {
        icon = g_themed_icon_new ("drive-harddisk-symbolic");
    }

    return icon;
}

static char *
pick_drive_name (GMount  *mount,
                 GVolume *volume,
                 GDrive  *drive)
{
    if (mount != NULL)
    {
        return g_mount_get_name (mount);
    }
    if (volume != NULL)
    {
        return g_volume_get_name (volume);
    }
    if (drive != NULL)
    {
        return g_drive_get_name (drive);
    }
    return g_strdup (_("Unknown drive"));
}

/* Append one ActionRow describing a (drive, volume, mount) triple. Any
 * of them may be NULL — that's how we cover both real mounts and bare
 * volumes/drives. */
static void
append_drive_row (AdwPreferencesGroup  *group,
                  GList               **rows,
                  GDrive               *drive,
                  GVolume              *volume,
                  GMount               *mount)
{
    AdwActionRow *row = ADW_ACTION_ROW (adw_action_row_new ());
    g_autoptr (GIcon) icon = pick_drive_icon (mount, volume, drive);
    g_autofree char *name = pick_drive_name (mount, volume, drive);
    g_autoptr (GFile) root = NULL;
    g_autofree char *mount_path = NULL;
    g_autofree char *size_text = NULL;
    double fraction = -1.0;
    GtkWidget *suffix_box;
    GtkWidget *progress;
    GtkWidget *size_label;
    GtkWidget *icon_image;

    if (mount != NULL)
    {
        root = g_mount_get_default_location (mount);
        mount_path = g_file_get_parse_name (root);
        size_text = build_drive_size_summary (root, &fraction);
    }

    adw_preferences_row_set_title (ADW_PREFERENCES_ROW (row), name);
    adw_action_row_set_subtitle (row,
                                 mount_path != NULL ? mount_path : _("Not mounted"));

    icon_image = gtk_image_new_from_gicon (icon);
    gtk_image_set_icon_size (GTK_IMAGE (icon_image), GTK_ICON_SIZE_LARGE);
    adw_action_row_add_prefix (row, icon_image);

    suffix_box = gtk_box_new (GTK_ORIENTATION_VERTICAL, 2);
    gtk_widget_set_valign (suffix_box, GTK_ALIGN_CENTER);

    progress = gtk_progress_bar_new ();
    gtk_widget_set_size_request (progress, 140, -1);
    gtk_widget_set_valign (progress, GTK_ALIGN_CENTER);
    if (fraction >= 0.0)
    {
        gtk_progress_bar_set_fraction (GTK_PROGRESS_BAR (progress), fraction);
    }
    else
    {
        /* No data — fade out so it doesn't look like 0% used. */
        gtk_widget_set_opacity (progress, 0.35);
    }
    gtk_box_append (GTK_BOX (suffix_box), progress);

    size_label = gtk_label_new (size_text != NULL ? size_text : "—");
    gtk_widget_add_css_class (size_label, "dim-label");
    gtk_widget_add_css_class (size_label, "caption");
    gtk_label_set_xalign (GTK_LABEL (size_label), 1.0);
    gtk_box_append (GTK_BOX (suffix_box), size_label);

    adw_action_row_add_suffix (row, suffix_box);

    adw_preferences_group_add (group, GTK_WIDGET (row));
    *rows = g_list_prepend (*rows, row);
}

/* Volume monitor signals fire in bursts (e.g. a single mount yields
 * volume-added + mount-added). Coalesce them into one idle rebuild. */
static gboolean
rebuild_drives_idle_cb (gpointer user_data)
{
    AdwPreferencesGroup *group = ADW_PREFERENCES_GROUP (user_data);

    g_object_set_data (G_OBJECT (group), DRIVES_REBUILD_PENDING_KEY,
                       GINT_TO_POINTER (FALSE));
    rebuild_drives_rows (group);

    return G_SOURCE_REMOVE;
}

static void
schedule_drives_rebuild (AdwPreferencesGroup *group)
{
    gboolean pending = GPOINTER_TO_INT (g_object_get_data (G_OBJECT (group),
                                                           DRIVES_REBUILD_PENDING_KEY));

    if (pending)
    {
        return;
    }
    g_object_set_data (G_OBJECT (group), DRIVES_REBUILD_PENDING_KEY,
                       GINT_TO_POINTER (TRUE));
    g_idle_add_full (G_PRIORITY_DEFAULT_IDLE, rebuild_drives_idle_cb,
                     g_object_ref (group), g_object_unref);
}

static void
on_volume_monitor_changed (GVolumeMonitor      *monitor,
                           gpointer             changed,
                           AdwPreferencesGroup *group)
{
    schedule_drives_rebuild (group);
}

static void
rebuild_drives_rows (AdwPreferencesGroup *group)
{
    GList *previous;
    GList *current = NULL;
    GVolumeMonitor *monitor;
    g_autolist (GDrive) drives = NULL;
    g_autolist (GVolume) orphan_volumes = NULL;
    g_autolist (GMount) orphan_mounts = NULL;

    previous = g_object_get_data (G_OBJECT (group), DRIVES_ROWS_KEY);
    for (GList *l = previous; l != NULL; l = l->next)
    {
        adw_preferences_group_remove (group, GTK_WIDGET (l->data));
    }
    g_list_free (previous);
    g_object_set_data (G_OBJECT (group), DRIVES_ROWS_KEY, NULL);

    monitor = g_object_get_data (G_OBJECT (group), DRIVES_MONITOR_KEY);
    if (monitor == NULL)
    {
        return;
    }

    drives = g_volume_monitor_get_connected_drives (monitor);
    for (GList *d = drives; d != NULL; d = d->next)
    {
        GDrive *drive = d->data;
        g_autolist (GVolume) volumes = g_drive_get_volumes (drive);

        if (volumes == NULL)
        {
            /* Drive without volumes (e.g. empty card reader). */
            append_drive_row (group, &current, drive, NULL, NULL);
            continue;
        }

        for (GList *v = volumes; v != NULL; v = v->next)
        {
            GVolume *volume = v->data;
            g_autoptr (GMount) mount = g_volume_get_mount (volume);

            append_drive_row (group, &current, drive, volume, mount);
        }
    }

    /* Volumes that don't belong to any drive (e.g. some encrypted or
     * pseudo-block devices). Avoid listing them twice. */
    orphan_volumes = g_volume_monitor_get_volumes (monitor);
    for (GList *v = orphan_volumes; v != NULL; v = v->next)
    {
        GVolume *volume = v->data;
        g_autoptr (GDrive) drive = g_volume_get_drive (volume);
        g_autoptr (GMount) mount = NULL;

        if (drive != NULL)
        {
            continue;
        }
        mount = g_volume_get_mount (volume);
        append_drive_row (group, &current, NULL, volume, mount);
    }

    /* Mounts without an associated volume (e.g. network shares, GVFS
     * backends). */
    orphan_mounts = g_volume_monitor_get_mounts (monitor);
    for (GList *m = orphan_mounts; m != NULL; m = m->next)
    {
        GMount *mount = m->data;
        g_autoptr (GVolume) volume = g_mount_get_volume (mount);

        if (volume != NULL)
        {
            continue;
        }
        append_drive_row (group, &current, NULL, NULL, mount);
    }

    if (current == NULL)
    {
        AdwActionRow *empty = ADW_ACTION_ROW (adw_action_row_new ());

        adw_preferences_row_set_title (ADW_PREFERENCES_ROW (empty),
                                       _("No drives detected"));
        adw_action_row_set_subtitle (empty,
            _("Connect a drive or mount a network location to see it here."));
        adw_preferences_group_add (group, GTK_WIDGET (empty));
        current = g_list_prepend (current, empty);
    }

    g_object_set_data (G_OBJECT (group), DRIVES_ROWS_KEY, current);
}

/* Set up the Drives overview page: one row per drive/volume/mount with
 * an icon, a usage bar and a "<free> / <used>" caption. The volume
 * monitor's lifetime is tied to the group, and add/remove signals
 * trigger a coalesced rebuild. */
static void
setup_drives_page (GtkBuilder *builder)
{
    AdwPreferencesGroup *group;
    GVolumeMonitor *monitor;

    group = ADW_PREFERENCES_GROUP (gtk_builder_get_object (builder,
                                                           "drives_group"));
    monitor = g_volume_monitor_get ();
    g_object_set_data_full (G_OBJECT (group), DRIVES_MONITOR_KEY, monitor,
                            g_object_unref);

    g_signal_connect_object (monitor, "volume-added",
                             G_CALLBACK (on_volume_monitor_changed), group,
                             G_CONNECT_DEFAULT);
    g_signal_connect_object (monitor, "volume-removed",
                             G_CALLBACK (on_volume_monitor_changed), group,
                             G_CONNECT_DEFAULT);
    g_signal_connect_object (monitor, "mount-added",
                             G_CALLBACK (on_volume_monitor_changed), group,
                             G_CONNECT_DEFAULT);
    g_signal_connect_object (monitor, "mount-removed",
                             G_CALLBACK (on_volume_monitor_changed), group,
                             G_CONNECT_DEFAULT);
    g_signal_connect_object (monitor, "drive-connected",
                             G_CALLBACK (on_volume_monitor_changed), group,
                             G_CONNECT_DEFAULT);
    g_signal_connect_object (monitor, "drive-disconnected",
                             G_CALLBACK (on_volume_monitor_changed), group,
                             G_CONNECT_DEFAULT);

    rebuild_drives_rows (group);
}

static void
nautilus_preferences_dialog_setup (GtkBuilder *builder)
{
    setup_combo (builder, NAUTILUS_PREFERENCES_DIALOG_OPEN_ACTION_COMBO,
                 (const char *[]) { _("Single-Click"), _("Double-Click"), NULL });
    setup_combo (builder, NAUTILUS_PREFERENCES_DIALOG_SEARCH_RECURSIVE_ROW,
                 (const char *[]) { _("On This Device Only"), _("All Locations"), _("Never"), NULL });
    setup_combo (builder, NAUTILUS_PREFERENCES_DIALOG_THUMBNAILS_ROW,
                 (const char *[]) { _("On This Device Only"), _("All Files"), _("Never"), NULL });
    setup_combo (builder, NAUTILUS_PREFERENCES_DIALOG_COUNT_ROW,
                 (const char *[]) { _("On This Device Only"), _("All Folders"), _("Never"), NULL });
    setup_combo (builder, NAUTILUS_PREFERENCES_DIALOG_TYPE_TO_ACTION_ROW,
                 (const char *[]) {
                     _("Filter This Folder"),
                     _("Jump to First Match"),
                     _("Search Recursively"),
                     _("Do Nothing"),
                     NULL,
                 });

    /* setup preferences */
    bind_builder_bool (builder, gtk_filechooser_preferences,
                       NAUTILUS_PREFERENCES_DIALOG_FOLDERS_FIRST_WIDGET,
                       NAUTILUS_PREFERENCES_SORT_DIRECTORIES_FIRST);
    bind_builder_bool (builder, nautilus_list_view_preferences,
                       NAUTILUS_PREFERENCES_DIALOG_LIST_VIEW_USE_TREE_WIDGET,
                       NAUTILUS_PREFERENCES_LIST_VIEW_USE_TREE);
    bind_builder_bool (builder, nautilus_preferences,
                       NAUTILUS_PREFERENCES_DIALOG_CREATE_LINK_WIDGET,
                       NAUTILUS_PREFERENCES_SHOW_CREATE_LINK);
    bind_builder_bool (builder, nautilus_preferences,
                       NAUTILUS_PREFERENCES_DIALOG_DELETE_PERMANENTLY_WIDGET,
                       NAUTILUS_PREFERENCES_SHOW_DELETE_PERMANENTLY);

    /* Sidebar visibility toggles. */
    bind_builder_bool (builder, nautilus_preferences,
                       "sidebar_show_recent_row",
                       NAUTILUS_PREFERENCES_SIDEBAR_SHOW_RECENT);
    bind_builder_bool (builder, nautilus_preferences,
                       "sidebar_show_starred_row",
                       NAUTILUS_PREFERENCES_SIDEBAR_SHOW_STARRED);
    bind_builder_bool (builder, nautilus_preferences,
                       "sidebar_show_desktop_row",
                       NAUTILUS_PREFERENCES_SIDEBAR_SHOW_DESKTOP);
    bind_builder_bool (builder, nautilus_preferences,
                       "sidebar_show_network_row",
                       NAUTILUS_PREFERENCES_SIDEBAR_SHOW_NETWORK);
    bind_builder_bool (builder, nautilus_preferences,
                       "sidebar_show_trash_row",
                       NAUTILUS_PREFERENCES_SIDEBAR_SHOW_TRASH);
    bind_builder_bool (builder, nautilus_preferences,
                       "sidebar_show_xdg_section_row",
                       NAUTILUS_PREFERENCES_SIDEBAR_SHOW_XDG_SECTION);
    bind_builder_bool (builder, nautilus_preferences,
                       "sidebar_show_cloud_row",
                       NAUTILUS_PREFERENCES_SIDEBAR_SHOW_CLOUD);

    setup_detailed_date (builder);

    bind_builder_combo_row (builder, nautilus_preferences,
                            NAUTILUS_PREFERENCES_DIALOG_OPEN_ACTION_COMBO,
                            NAUTILUS_PREFERENCES_CLICK_POLICY,
                            (const char **) click_behavior_values);
    bind_builder_combo_row (builder, nautilus_preferences,
                            NAUTILUS_PREFERENCES_DIALOG_SEARCH_RECURSIVE_ROW,
                            NAUTILUS_PREFERENCES_RECURSIVE_SEARCH,
                            (const char **) speed_tradeoff_values);
    bind_builder_combo_row (builder, nautilus_preferences,
                            NAUTILUS_PREFERENCES_DIALOG_THUMBNAILS_ROW,
                            NAUTILUS_PREFERENCES_SHOW_FILE_THUMBNAILS,
                            (const char **) speed_tradeoff_values);
    bind_builder_combo_row (builder, nautilus_preferences,
                            NAUTILUS_PREFERENCES_DIALOG_COUNT_ROW,
                            NAUTILUS_PREFERENCES_SHOW_DIRECTORY_ITEM_COUNTS,
                            (const char **) speed_tradeoff_values);
    bind_builder_combo_row (builder, nautilus_preferences,
                            NAUTILUS_PREFERENCES_DIALOG_TYPE_TO_ACTION_ROW,
                            NAUTILUS_PREFERENCES_TYPE_TO_ACTION,
                            (const char **) type_to_action_values);

    setup_custom_actions_page (builder);
    setup_drives_page (builder);
}

void
nautilus_preferences_dialog_show (GtkWidget *parent)
{
    static AdwPreferencesDialog *preferences_dialog = NULL;
    g_autoptr (GtkBuilder) builder = NULL;
    g_autoptr (GSimpleActionGroup) action_group = g_simple_action_group_new ();
    g_autoptr (GAction) date_time_action = NULL;

    if (preferences_dialog != NULL)
    {
        /* Destroy existing window, which might be hidden behind other windows,
         * attached to another parent. */
        adw_dialog_force_close (ADW_DIALOG (preferences_dialog));
        g_clear_weak_pointer (&preferences_dialog);
    }

    builder = gtk_builder_new_from_resource ("/org/gnome/nautilus/ui/nautilus-preferences-dialog.ui");
    nautilus_preferences_dialog_setup (builder);

    preferences_dialog = ADW_PREFERENCES_DIALOG (gtk_builder_get_object (builder, "preferences_dialog"));
    g_object_add_weak_pointer (G_OBJECT (preferences_dialog), (gpointer *) &preferences_dialog);

    date_time_action = g_settings_create_action (nautilus_preferences,
                                                 NAUTILUS_PREFERENCES_DATE_TIME_FORMAT);
    g_action_map_add_action (G_ACTION_MAP (action_group), date_time_action);
    gtk_widget_insert_action_group (GTK_WIDGET (preferences_dialog),
                                    "preferences",
                                    G_ACTION_GROUP (action_group));

    adw_dialog_present (ADW_DIALOG (preferences_dialog), parent);
}
