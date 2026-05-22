/* nemo-action-manager.c
 *
 * Loads and watches the user's custom context-menu actions.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <config.h>

#include "nemo-action-manager.h"

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

struct _NemoActionManager
{
    GObject parent_instance;

    char         *actions_dir;
    GFile        *actions_location;
    GFileMonitor *monitor;
    GList        *actions;        /* NemoAction, owned */
    guint         reload_idle_id;
};

G_DEFINE_FINAL_TYPE (NemoActionManager, nemo_action_manager, G_TYPE_OBJECT)

static int
compare_actions (gconstpointer a,
                 gconstpointer b)
{
    NemoAction *action_a = (NemoAction *) a;
    NemoAction *action_b = (NemoAction *) b;
    const char *group_a = nemo_action_get_group (action_a);
    const char *group_b = nemo_action_get_group (action_b);
    int cmp;

    cmp = g_strcmp0 (group_a != NULL ? group_a : "",
                     group_b != NULL ? group_b : "");
    if (cmp != 0)
    {
        return cmp;
    }

    if (nemo_action_get_position (action_a) != nemo_action_get_position (action_b))
    {
        return nemo_action_get_position (action_a) - nemo_action_get_position (action_b);
    }

    return g_strcmp0 (nemo_action_get_name (action_a),
                      nemo_action_get_name (action_b));
}

static void
load_actions (NemoActionManager *self)
{
    g_autoptr (GFileEnumerator) enumerator = NULL;

    g_list_free_full (self->actions, g_object_unref);
    self->actions = NULL;

    enumerator = g_file_enumerate_children (self->actions_location,
                                            G_FILE_ATTRIBUTE_STANDARD_NAME,
                                            G_FILE_QUERY_INFO_NONE, NULL, NULL);
    if (enumerator == NULL)
    {
        /* A missing directory simply means there are no actions yet. */
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
        if (name != NULL && g_str_has_suffix (name, ACTION_SUFFIX))
        {
            g_autoptr (GFile) file = g_file_get_child (self->actions_location, name);
            NemoAction *action = nemo_action_new (file);

            if (action != NULL)
            {
                self->actions = g_list_prepend (self->actions, action);
            }
        }

        g_object_unref (info);
    }

    self->actions = g_list_sort (self->actions, compare_actions);
}

static gboolean
reload_idle (gpointer data)
{
    NemoActionManager *self = data;

    self->reload_idle_id = 0;
    load_actions (self);
    g_signal_emit (self, signals[CHANGED], 0);

    return G_SOURCE_REMOVE;
}

static void
on_directory_changed (NemoActionManager *self,
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
nemo_action_manager_constructed (GObject *object)
{
    NemoActionManager *self = NEMO_ACTION_MANAGER (object);

    G_OBJECT_CLASS (nemo_action_manager_parent_class)->constructed (object);

    self->actions_dir = g_build_filename (g_get_user_data_dir (),
                                          "nemo", "actions", NULL);
    g_mkdir_with_parents (self->actions_dir, 0755);
    self->actions_location = g_file_new_for_path (self->actions_dir);

    load_actions (self);

    self->monitor = g_file_monitor_directory (self->actions_location,
                                              G_FILE_MONITOR_NONE, NULL, NULL);
    if (self->monitor != NULL)
    {
        g_signal_connect_swapped (self->monitor, "changed",
                                  G_CALLBACK (on_directory_changed), self);
    }
}

static void
nemo_action_manager_finalize (GObject *object)
{
    NemoActionManager *self = NEMO_ACTION_MANAGER (object);

    g_clear_handle_id (&self->reload_idle_id, g_source_remove);
    g_clear_object (&self->monitor);
    g_clear_object (&self->actions_location);
    g_clear_pointer (&self->actions_dir, g_free);
    g_list_free_full (self->actions, g_object_unref);

    G_OBJECT_CLASS (nemo_action_manager_parent_class)->finalize (object);
}

static void
nemo_action_manager_class_init (NemoActionManagerClass *klass)
{
    GObjectClass *object_class = G_OBJECT_CLASS (klass);

    object_class->constructed = nemo_action_manager_constructed;
    object_class->finalize = nemo_action_manager_finalize;

    signals[CHANGED] = g_signal_new ("changed",
                                     NEMO_TYPE_ACTION_MANAGER,
                                     G_SIGNAL_RUN_LAST,
                                     0, NULL, NULL, NULL,
                                     G_TYPE_NONE, 0);
}

static void
nemo_action_manager_init (NemoActionManager *self)
{
}

NemoActionManager *
nemo_action_manager_dup_singleton (void)
{
    static NemoActionManager *singleton = NULL;

    if (singleton == NULL)
    {
        singleton = g_object_new (NEMO_TYPE_ACTION_MANAGER, NULL);
        g_object_add_weak_pointer (G_OBJECT (singleton), (gpointer *) &singleton);

        return singleton;
    }

    return g_object_ref (singleton);
}

GList *
nemo_action_manager_get_actions (NemoActionManager *self)
{
    return self->actions;
}

NemoAction *
nemo_action_manager_get_action (NemoActionManager *self,
                                const char        *id)
{
    for (GList *l = self->actions; l != NULL; l = l->next)
    {
        if (g_strcmp0 (nemo_action_get_id (l->data), id) == 0)
        {
            return l->data;
        }
    }

    return NULL;
}
