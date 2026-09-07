/* dori-action-manager.c
 *
 * Loads bundled and user-defined context-menu actions, and watches the latter.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <config.h>

#include "dori-action-manager.h"

#include <errno.h>
#include <gio/gio.h>
#include <glib/gstdio.h>

#define ACTION_SUFFIX ".nemo_action"
#define RELOAD_COALESCE_MSEC 200

enum
{
    CHANGED,
    LAST_SIGNAL
};

static guint signals[LAST_SIGNAL] = { 0 };

struct _DoriActionManager
{
    GObject parent_instance;

    char         *actions_dir;
    GFile        *actions_location;
    GFileMonitor *monitor;
    GList        *actions;        /* DoriAction, owned */
    guint         reload_idle_id;
};

G_DEFINE_FINAL_TYPE (DoriActionManager, dori_action_manager, G_TYPE_OBJECT)

static int
compare_actions (gconstpointer a,
                 gconstpointer b)
{
    DoriAction *action_a = (DoriAction *) a;
    DoriAction *action_b = (DoriAction *) b;
    const char *group_a = dori_action_get_group (action_a);
    const char *group_b = dori_action_get_group (action_b);
    int cmp;

    cmp = g_strcmp0 (group_a != NULL ? group_a : "",
                     group_b != NULL ? group_b : "");
    if (cmp != 0)
    {
        return cmp;
    }

    if (dori_action_get_position (action_a) != dori_action_get_position (action_b))
    {
        return dori_action_get_position (action_a) - dori_action_get_position (action_b);
    }

    return g_strcmp0 (dori_action_get_name (action_a),
                      dori_action_get_name (action_b));
}

static void
load_actions_from_directory (DoriActionManager *self,
                             GFile             *directory,
                             GHashTable        *loaded_ids)
{
    g_autoptr (GFileEnumerator) enumerator = NULL;
    enumerator = g_file_enumerate_children (directory,
                                            G_FILE_ATTRIBUTE_STANDARD_NAME,
                                            G_FILE_QUERY_INFO_NONE, NULL, NULL);
    if (enumerator == NULL)
    {
        /* Missing XDG data directories simply contain no actions. */
        return;
    }

    while (TRUE)
    {
        GFileInfo *info = g_file_enumerator_next_file (enumerator, NULL, NULL);
        const char *name;

        if (info == NULL)
        {
            break;
        }

        name = g_file_info_get_name (info);
        if (name != NULL &&
            g_str_has_suffix (name, ACTION_SUFFIX) &&
            !g_hash_table_contains (loaded_ids, name))
        {
            g_autoptr (GFile) file = g_file_get_child (directory, name);
            DoriAction *action = dori_action_new (file);

            if (action != NULL)
            {
                self->actions = g_list_prepend (self->actions, action);
                g_hash_table_add (loaded_ids, g_strdup (name));
            }
        }

        g_object_unref (info);
    }
}

static void
load_actions (DoriActionManager *self)
{
    g_autoptr (GHashTable) loaded_ids = NULL;
    const char * const *system_data_dirs;

    g_list_free_full (self->actions, g_object_unref);
    self->actions = NULL;

    loaded_ids = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, NULL);

    /* User actions have precedence over bundled actions with the same ID. */
    load_actions_from_directory (self, self->actions_location, loaded_ids);

    system_data_dirs = g_get_system_data_dirs ();
    for (guint i = 0; system_data_dirs[i] != NULL; i++)
    {
        g_autofree char *actions_dir = NULL;
        g_autoptr (GFile) actions_location = NULL;

        actions_dir = g_build_filename (system_data_dirs[i],
                                        "nautilus", "actions", NULL);
        actions_location = g_file_new_for_path (actions_dir);

        if (!g_file_equal (actions_location, self->actions_location))
        {
            load_actions_from_directory (self, actions_location, loaded_ids);
        }
    }

    self->actions = g_list_sort (self->actions, compare_actions);
}

static gboolean
reload_idle (gpointer data)
{
    DoriActionManager *self = data;

    self->reload_idle_id = 0;
    load_actions (self);
    g_signal_emit (self, signals[CHANGED], 0);

    return G_SOURCE_REMOVE;
}

static void
on_directory_changed (DoriActionManager *self,
                      GFile             *file,
                      GFile             *other_file,
                      GFileMonitorEvent  event_type)
{
    /* Coalesce the burst of events a single edit produces into one reload. */
    if (self->reload_idle_id == 0)
    {
        self->reload_idle_id = g_timeout_add (RELOAD_COALESCE_MSEC,
                                              reload_idle, self);
    }
}

static void
dori_action_manager_constructed (GObject *object)
{
    DoriActionManager *self = DORI_ACTION_MANAGER (object);

    G_OBJECT_CLASS (dori_action_manager_parent_class)->constructed (object);

    g_autoptr (GError) monitor_error = NULL;

    self->actions_dir = g_build_filename (g_get_user_data_dir (),
                                          "nemo", "actions", NULL);
    if (g_mkdir_with_parents (self->actions_dir, 0755) != 0)
    {
        g_warning ("Dori action manager: cannot create %s: %s",
                   self->actions_dir, g_strerror (errno));
    }
    self->actions_location = g_file_new_for_path (self->actions_dir);

    load_actions (self);

    self->monitor = g_file_monitor_directory (self->actions_location,
                                              G_FILE_MONITOR_NONE, NULL,
                                              &monitor_error);
    if (self->monitor == NULL)
    {
        g_warning ("Dori action manager: cannot watch %s for changes "
                   "(live reload of .nemo_action files will not work): %s",
                   self->actions_dir,
                   monitor_error != NULL ? monitor_error->message : "unknown error");
        return;
    }

    g_signal_connect_swapped (self->monitor, "changed",
                              G_CALLBACK (on_directory_changed), self);
}

static void
dori_action_manager_finalize (GObject *object)
{
    DoriActionManager *self = DORI_ACTION_MANAGER (object);

    g_clear_handle_id (&self->reload_idle_id, g_source_remove);
    g_clear_object (&self->monitor);
    g_clear_object (&self->actions_location);
    g_clear_pointer (&self->actions_dir, g_free);
    g_list_free_full (self->actions, g_object_unref);

    G_OBJECT_CLASS (dori_action_manager_parent_class)->finalize (object);
}

static void
dori_action_manager_class_init (DoriActionManagerClass *klass)
{
    GObjectClass *object_class = G_OBJECT_CLASS (klass);

    object_class->constructed = dori_action_manager_constructed;
    object_class->finalize = dori_action_manager_finalize;

    signals[CHANGED] = g_signal_new ("changed",
                                     DORI_TYPE_ACTION_MANAGER,
                                     G_SIGNAL_RUN_LAST,
                                     0, NULL, NULL, NULL,
                                     G_TYPE_NONE, 0);
}

static void
dori_action_manager_init (DoriActionManager *self)
{
}

DoriActionManager *
dori_action_manager_dup_singleton (void)
{
    static DoriActionManager *singleton = NULL;

    if (singleton == NULL)
    {
        singleton = g_object_new (DORI_TYPE_ACTION_MANAGER, NULL);
        g_object_add_weak_pointer (G_OBJECT (singleton), (gpointer *) &singleton);

        return singleton;
    }

    return g_object_ref (singleton);
}

GList *
dori_action_manager_get_actions (DoriActionManager *self)
{
    return self->actions;
}

DoriAction *
dori_action_manager_get_action (DoriActionManager *self,
                                const char        *id)
{
    for (GList *l = self->actions; l != NULL; l = l->next)
    {
        if (g_strcmp0 (dori_action_get_id (l->data), id) == 0)
        {
            return l->data;
        }
    }

    return NULL;
}

gboolean
dori_action_manager_is_user_action (DoriActionManager *self,
                                    const char        *id)
{
    g_autoptr (GFile) file = NULL;

    g_return_val_if_fail (DORI_IS_ACTION_MANAGER (self), FALSE);
    g_return_val_if_fail (id != NULL, FALSE);

    file = g_file_get_child (self->actions_location, id);

    return g_file_query_exists (file, NULL);
}

GFile *
dori_action_manager_get_actions_dir (DoriActionManager *self)
{
    return self->actions_location;
}

gboolean
dori_action_manager_delete_action (DoriActionManager *self,
                                   const char        *id,
                                   GError           **error)
{
    g_autoptr (GFile) file = NULL;

    g_return_val_if_fail (DORI_IS_ACTION_MANAGER (self), FALSE);
    g_return_val_if_fail (id != NULL, FALSE);

    file = g_file_get_child (self->actions_location, id);

    return g_file_delete (file, NULL, error);
}
