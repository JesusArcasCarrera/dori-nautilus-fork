/*
 * Copyright (C) 2026 The Nemo project contributors
 *
 * Based on nautilus-network-directory.c (Copyright 2024 António Fernandes).
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "nautilus-other-locations-directory.h"

#include <gio/gio.h>

#include "nautilus-directory-private.h"
#include "nautilus-file-private.h"
#include "nautilus-file-utilities.h"
#include "nautilus-internal-place-file.h"
#include "nautilus-recent-servers.h"
#include "nautilus-scheme.h"


struct _NautilusOtherLocationsDirectory
{
    NautilusDirectory parent_slot;

    /* Backed by computer:/// — exposes drives, volumes and mounts as files. */
    NautilusDirectory *computer_backend_directory;
    gboolean computer_backend_done_loading;

    /* Backed by network:/// — exposes peers currently reachable on the LAN. */
    NautilusDirectory *network_backend_directory;
    gboolean network_backend_done_loading;

    NautilusRecentServers *recent_servers;
    GList *recent_server_files;
    gboolean recent_servers_done_loading;

    GList /*<owned OtherLocationsCallback>*/ *callback_list;
};

G_DEFINE_TYPE_WITH_CODE (NautilusOtherLocationsDirectory, nautilus_other_locations_directory, NAUTILUS_TYPE_DIRECTORY,
                         nautilus_ensure_extension_points ();
                         g_io_extension_point_implement (NAUTILUS_DIRECTORY_PROVIDER_EXTENSION_POINT_NAME,
                                                         g_define_type_id,
                                                         NAUTILUS_OTHER_LOCATIONS_DIRECTORY_PROVIDER_NAME,
                                                         0));

typedef struct
{
    NautilusOtherLocationsDirectory *self;

    NautilusDirectoryCallback callback;
    gpointer callback_data;

    gboolean computer_backend_ready;
    gboolean network_backend_ready;
} OtherLocationsCallback;

static gboolean
real_are_all_files_seen (NautilusDirectory *directory)
{
    NautilusOtherLocationsDirectory *self = NAUTILUS_OTHER_LOCATIONS_DIRECTORY (directory);

    return (nautilus_directory_are_all_files_seen (self->computer_backend_directory) &&
            nautilus_directory_are_all_files_seen (self->network_backend_directory) &&
            self->recent_servers_done_loading);
}

static gboolean
real_contains_file (NautilusDirectory *directory,
                    NautilusFile      *file)
{
    NautilusOtherLocationsDirectory *self = NAUTILUS_OTHER_LOCATIONS_DIRECTORY (directory);

    if (nautilus_file_get_directory (file) == directory)
    {
        /* Recent server files are directly owned by us. */
        return TRUE;
    }
    if (nautilus_directory_contains_file (self->network_backend_directory, file))
    {
        return TRUE;
    }
    if (nautilus_directory_contains_file (self->computer_backend_directory, file))
    {
        /* Unlike NautilusNetworkDirectory, we accept every mountable (local
         * drives + remote shares). */
        return TRUE;
    }

    return FALSE;
}

static void
on_backend_directory_done_loading (NautilusDirectory *backend_directory,
                                   gpointer           callback_data)
{
    NautilusOtherLocationsDirectory *self = callback_data;

    if (backend_directory == self->computer_backend_directory)
    {
        self->computer_backend_done_loading = TRUE;
    }
    else if (backend_directory == self->network_backend_directory)
    {
        self->network_backend_done_loading = TRUE;
    }
    else
    {
        /* Called from on_recent_servers_loading_changed () */
        g_assert (backend_directory == (NautilusDirectory *) self &&
                  self->recent_servers_done_loading);
    }

    if (self->computer_backend_done_loading &&
        self->network_backend_done_loading &&
        self->recent_servers_done_loading)
    {
        nautilus_directory_emit_done_loading (NAUTILUS_DIRECTORY (self));
    }
}

static void
real_force_reload (NautilusDirectory *directory)
{
    NautilusOtherLocationsDirectory *self = NAUTILUS_OTHER_LOCATIONS_DIRECTORY (directory);

    self->computer_backend_done_loading = FALSE;
    nautilus_directory_force_reload (self->computer_backend_directory);

    self->network_backend_done_loading = FALSE;
    nautilus_directory_force_reload (self->network_backend_directory);

    self->recent_servers_done_loading = FALSE;
    nautilus_recent_servers_force_reload (self->recent_servers);
}

static void
on_backend_directory_ready (NautilusDirectory *backend_directory,
                            GList             *unused_parameter,
                            gpointer           callback_data)
{
    OtherLocationsCallback *cb = callback_data;
    NautilusOtherLocationsDirectory *self = cb->self;

    if (backend_directory == self->computer_backend_directory)
    {
        cb->computer_backend_ready = TRUE;
    }
    else if (backend_directory == self->network_backend_directory)
    {
        cb->network_backend_ready = TRUE;
    }
    else
    {
        g_assert (backend_directory == (NautilusDirectory *) self &&
                  cb->self->recent_servers_done_loading);
    }

    if (cb->computer_backend_ready &&
        cb->network_backend_ready &&
        cb->self->recent_servers_done_loading)
    {
        g_autolist (NautilusFile) files = nautilus_directory_get_file_list (NAUTILUS_DIRECTORY (self));

        (*cb->callback)(NAUTILUS_DIRECTORY (self), files, cb->callback_data);

        self->callback_list = g_list_remove (self->callback_list, cb);
        g_free (cb);
    }
}

static void
on_recent_servers_loading_changed (GObject    *object,
                                   GParamSpec *pspec,
                                   gpointer    user_data)
{
    NautilusOtherLocationsDirectory *self = NAUTILUS_OTHER_LOCATIONS_DIRECTORY (user_data);
    NautilusRecentServers *recent_servers = NAUTILUS_RECENT_SERVERS (object);
    gboolean is_loading = nautilus_recent_servers_get_loading (recent_servers);

    self->recent_servers_done_loading = !is_loading;
    if (self->recent_servers_done_loading)
    {
        NautilusDirectory *self_as_directory = NAUTILUS_DIRECTORY (self);

        on_backend_directory_done_loading (self_as_directory, self);

        for (GList *l = self->callback_list; l != NULL; l = l->next)
        {
            on_backend_directory_ready (self_as_directory, NULL, l->data);
        }
    }
}

static NautilusFile *
get_recent_server_file (NautilusDirectory *directory,
                        GFileInfo         *server_info)
{
    /* Recent servers keep the SCHEME_NETWORK_VIEW URI scheme so the rest of
     * the codebase (handlers in files-view, recent-servers manager, etc.)
     * keeps recognising them transparently. */
    g_autofree char *uri = g_strconcat (SCHEME_NETWORK_VIEW ":///",
                                        g_file_info_get_name (server_info),
                                        NULL);
    g_autoptr (NautilusFile) file = nautilus_file_get_by_uri (uri);

    nautilus_file_update_info (file, server_info);

    return g_steal_pointer (&file);
}

static void
on_recent_servers_added (NautilusOtherLocationsDirectory *self,
                         GList                           *servers)
{
    NautilusDirectory *dir = NAUTILUS_DIRECTORY (self);
    g_autolist (NautilusFile) added_files = NULL;

    for (GList *l = servers; l != NULL; l = l->next)
    {
        GFileInfo *server_info = l->data;
        g_autoptr (NautilusFile) file = get_recent_server_file (dir, server_info);

        self->recent_server_files = g_list_prepend (self->recent_server_files,
                                                    g_object_ref (file));

        added_files = g_list_prepend (added_files, g_steal_pointer (&file));
    }

    nautilus_directory_emit_files_added (dir, added_files);
}

static void
on_recent_servers_changed (NautilusOtherLocationsDirectory *self,
                           GList                           *servers)
{
    g_autolist (NautilusFile) changed_files = NULL;

    for (GList *l = servers; l != NULL; l = l->next)
    {
        GFileInfo *server_info = l->data;
        NautilusFile *file = nautilus_directory_find_file_by_name (NAUTILUS_DIRECTORY (self),
                                                                   g_file_info_get_name (server_info));
        if (file == NULL)
        {
            g_critical ("Notified change on recent server whose random GUID name was not known yet");
            continue;
        }

        nautilus_file_update_info (file, server_info);

        changed_files = g_list_prepend (changed_files, g_object_ref (file));
    }

    nautilus_directory_emit_files_changed (NAUTILUS_DIRECTORY (self), changed_files);
}

static void
on_recent_servers_removed (NautilusOtherLocationsDirectory *self,
                           GList                           *servers)
{
    g_autolist (NautilusFile) removed_files = NULL;

    for (GList *l = servers; l != NULL; l = l->next)
    {
        GFileInfo *server_info = l->data;
        NautilusFile *file = nautilus_directory_find_file_by_name (NAUTILUS_DIRECTORY (self),
                                                                   g_file_info_get_name (server_info));
        if (file == NULL)
        {
            g_critical ("Notified removal of recent server whose random GUID name was not known yet");
            continue;
        }

        nautilus_file_mark_gone (file);

        /* Steal file from self->recent_server_files */
        GList *link = g_list_find (self->recent_server_files, file);
        self->recent_server_files = g_list_remove_link (self->recent_server_files, link);
        removed_files = g_list_concat (link, removed_files);
    }

    nautilus_directory_emit_files_changed (NAUTILUS_DIRECTORY (self), removed_files);
}

static OtherLocationsCallback *
callback_find (NautilusOtherLocationsDirectory *self,
               NautilusDirectoryCallback        callback,
               gpointer                         callback_data)
{
    for (GList *l = self->callback_list; l != NULL; l = l->next)
    {
        OtherLocationsCallback *cb = l->data;

        if (cb->callback == callback &&
            cb->callback_data == callback_data)
        {
            return cb;
        }
    }
    return NULL;
}

static void
real_call_when_ready (NautilusDirectory         *directory,
                      NautilusFileAttributes     file_attributes,
                      gboolean                   wait_for_file_list,
                      NautilusDirectoryCallback  callback,
                      gpointer                   callback_data)
{
    NautilusOtherLocationsDirectory *self = NAUTILUS_OTHER_LOCATIONS_DIRECTORY (directory);
    OtherLocationsCallback *cb;

    cb = callback_find (self, callback, callback_data);
    if (cb != NULL)
    {
        g_warning ("tried to add a new callback while an old one was pending");
        return;
    }

    cb = g_new0 (OtherLocationsCallback, 1);
    cb->self = self;
    cb->callback = callback;
    cb->callback_data = callback_data;

    self->callback_list = g_list_prepend (self->callback_list, cb);

    nautilus_directory_call_when_ready (self->computer_backend_directory,
                                        file_attributes,
                                        wait_for_file_list,
                                        on_backend_directory_ready, cb);
    nautilus_directory_call_when_ready (self->network_backend_directory,
                                        file_attributes,
                                        wait_for_file_list,
                                        on_backend_directory_ready, cb);
}

static void
real_cancel_callback (NautilusDirectory         *directory,
                      NautilusDirectoryCallback  callback,
                      gpointer                   callback_data)
{
    NautilusOtherLocationsDirectory *self = NAUTILUS_OTHER_LOCATIONS_DIRECTORY (directory);
    OtherLocationsCallback *cb;

    cb = callback_find (self, callback, callback_data);
    if (cb == NULL)
    {
        return;
    }

    if (!cb->computer_backend_ready)
    {
        nautilus_directory_cancel_callback (self->computer_backend_directory, on_backend_directory_ready, cb);
    }
    if (!cb->network_backend_ready)
    {
        nautilus_directory_cancel_callback (self->network_backend_directory, on_backend_directory_ready, cb);
    }

    self->callback_list = g_list_remove (self->callback_list, cb);
    g_free (cb);
}

static void
real_file_monitor_add (NautilusDirectory         *directory,
                       gconstpointer              client,
                       gboolean                   monitor_hidden_files,
                       NautilusFileAttributes     file_attributes,
                       NautilusDirectoryCallback  callback,
                       gpointer                   callback_data)
{
    NautilusOtherLocationsDirectory *self = NAUTILUS_OTHER_LOCATIONS_DIRECTORY (directory);

    nautilus_directory_file_monitor_add (self->computer_backend_directory,
                                         client,
                                         monitor_hidden_files,
                                         file_attributes,
                                         NULL, NULL);
    nautilus_directory_file_monitor_add (self->network_backend_directory,
                                         client,
                                         monitor_hidden_files,
                                         file_attributes,
                                         NULL, NULL);

    if (callback != NULL)
    {
        g_autolist (NautilusFile) files = nautilus_directory_get_file_list (directory);

        (*callback)(directory, files, callback_data);
    }
}

static void
real_file_monitor_remove (NautilusDirectory *directory,
                          gconstpointer      client)
{
    NautilusOtherLocationsDirectory *self = NAUTILUS_OTHER_LOCATIONS_DIRECTORY (directory);

    nautilus_directory_file_monitor_remove (self->computer_backend_directory, client);
    nautilus_directory_file_monitor_remove (self->network_backend_directory, client);
}

static GList *
real_get_file_list (NautilusDirectory *directory)
{
    NautilusOtherLocationsDirectory *self = NAUTILUS_OTHER_LOCATIONS_DIRECTORY (directory);
    g_autolist (NautilusFile) computer_list = nautilus_directory_get_file_list (self->computer_backend_directory);
    g_autolist (NautilusFile) network_list = nautilus_directory_get_file_list (self->network_backend_directory);
    g_autolist (NautilusFile) recent_servers = nautilus_file_list_copy (self->recent_server_files);

    /* All computer:/// mountables (local drives, optical, remote mounts) plus
     * network peers and previously connected servers. */
    return g_list_concat (g_steal_pointer (&computer_list),
                          g_list_concat (g_steal_pointer (&recent_servers),
                                         g_steal_pointer (&network_list)));
}

static gboolean
real_is_not_empty (NautilusDirectory *directory)
{
    NautilusOtherLocationsDirectory *self = NAUTILUS_OTHER_LOCATIONS_DIRECTORY (directory);

    return (self->recent_server_files != NULL ||
            nautilus_directory_is_not_empty (self->network_backend_directory) ||
            nautilus_directory_is_not_empty (self->computer_backend_directory));
}

static gboolean
real_is_editable (NautilusDirectory *directory)
{
    return FALSE;
}

static gboolean
real_handles_location (GFile *location)
{
    return g_file_has_uri_scheme (location, SCHEME_OTHER_LOCATIONS);
}

static NautilusFile *
real_new_as_file (NautilusDirectory *directory)
{
    return g_object_new (NAUTILUS_TYPE_INTERNAL_PLACE_FILE, "directory", directory, NULL);
}

static void
on_backend_directory_files_added (NautilusOtherLocationsDirectory *self,
                                  GList                           *added_files)
{
    nautilus_directory_emit_files_added (NAUTILUS_DIRECTORY (self), added_files);
}

static void
on_backend_directory_files_changed (NautilusOtherLocationsDirectory *self,
                                    GList                           *changed_files)
{
    nautilus_directory_emit_files_changed (NAUTILUS_DIRECTORY (self), changed_files);
}

static void
nautilus_other_locations_directory_dispose (GObject *object)
{
    NautilusOtherLocationsDirectory *self = NAUTILUS_OTHER_LOCATIONS_DIRECTORY (object);

    g_clear_list (&self->recent_server_files, g_object_unref);
    g_clear_list (&self->callback_list, g_free);

    G_OBJECT_CLASS (nautilus_other_locations_directory_parent_class)->dispose (object);
}

static void
nautilus_other_locations_directory_finalize (GObject *object)
{
    NautilusOtherLocationsDirectory *self = NAUTILUS_OTHER_LOCATIONS_DIRECTORY (object);

    g_clear_object (&self->computer_backend_directory);
    g_clear_object (&self->network_backend_directory);
    g_clear_object (&self->recent_servers);

    G_OBJECT_CLASS (nautilus_other_locations_directory_parent_class)->finalize (object);
}

static void
nautilus_other_locations_directory_init (NautilusOtherLocationsDirectory *self)
{
    self->computer_backend_directory = nautilus_directory_get_by_uri (SCHEME_COMPUTER ":///");
    g_signal_connect_object (self->computer_backend_directory, "files-added",
                             G_CALLBACK (on_backend_directory_files_added), self, G_CONNECT_SWAPPED);
    g_signal_connect_object (self->computer_backend_directory, "files-changed",
                             G_CALLBACK (on_backend_directory_files_changed), self, G_CONNECT_SWAPPED);
    g_signal_connect_object (self->computer_backend_directory, "done-loading",
                             G_CALLBACK (on_backend_directory_done_loading), self, G_CONNECT_DEFAULT);
    g_signal_connect_object (self->computer_backend_directory, "load-error",
                             G_CALLBACK (nautilus_directory_emit_load_error), self, G_CONNECT_SWAPPED);

    self->network_backend_directory = nautilus_directory_get_by_uri (SCHEME_NETWORK ":///");
    g_signal_connect_object (self->network_backend_directory, "files-added",
                             G_CALLBACK (nautilus_directory_emit_files_added), self, G_CONNECT_SWAPPED);
    g_signal_connect_object (self->network_backend_directory, "files-changed",
                             G_CALLBACK (nautilus_directory_emit_files_changed), self, G_CONNECT_SWAPPED);
    g_signal_connect_object (self->network_backend_directory, "done-loading",
                             G_CALLBACK (on_backend_directory_done_loading), self, G_CONNECT_DEFAULT);
    g_signal_connect_object (self->network_backend_directory, "load-error",
                             G_CALLBACK (nautilus_directory_emit_load_error), self, G_CONNECT_SWAPPED);

    self->recent_servers = nautilus_recent_servers_new ();
    g_signal_connect_object (self->recent_servers, "notify::loading",
                             G_CALLBACK (on_recent_servers_loading_changed), self, G_CONNECT_DEFAULT);
    g_signal_connect_object (self->recent_servers, "added",
                             G_CALLBACK (on_recent_servers_added), self, G_CONNECT_SWAPPED);
    g_signal_connect_object (self->recent_servers, "changed",
                             G_CALLBACK (on_recent_servers_changed), self, G_CONNECT_SWAPPED);
    g_signal_connect_object (self->recent_servers, "removed",
                             G_CALLBACK (on_recent_servers_removed), self, G_CONNECT_SWAPPED);
}

static void
nautilus_other_locations_directory_class_init (NautilusOtherLocationsDirectoryClass *klass)
{
    GObjectClass *object_class = G_OBJECT_CLASS (klass);
    NautilusDirectoryClass *directory_class = NAUTILUS_DIRECTORY_CLASS (klass);

    object_class->finalize = nautilus_other_locations_directory_finalize;
    object_class->dispose = nautilus_other_locations_directory_dispose;

    directory_class->are_all_files_seen = real_are_all_files_seen;
    directory_class->contains_file = real_contains_file;
    directory_class->force_reload = real_force_reload;
    directory_class->call_when_ready = real_call_when_ready;
    directory_class->cancel_callback = real_cancel_callback;
    directory_class->file_monitor_add = real_file_monitor_add;
    directory_class->file_monitor_remove = real_file_monitor_remove;
    directory_class->get_file_list = real_get_file_list;
    directory_class->is_not_empty = real_is_not_empty;
    directory_class->is_editable = real_is_editable;
    directory_class->handles_location = real_handles_location;
    directory_class->new_as_file = real_new_as_file;
}

NautilusOtherLocationsDirectory *
nautilus_other_locations_directory_new (void)
{
    g_autoptr (GFile) location = g_file_new_for_uri (SCHEME_OTHER_LOCATIONS ":///");

    return g_object_new (NAUTILUS_TYPE_OTHER_LOCATIONS_DIRECTORY,
                         "location", location,
                         NULL);
}
