/*
 * Copyright © 2026 The Dori authors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "nautilus-disk-usage-model.h"

#include <glib/gi18n.h>
#include <math.h>

#define FILE_ATTRIBUTES \
    G_FILE_ATTRIBUTE_STANDARD_NAME "," \
    G_FILE_ATTRIBUTE_STANDARD_DISPLAY_NAME "," \
    G_FILE_ATTRIBUTE_STANDARD_TYPE "," \
    G_FILE_ATTRIBUTE_STANDARD_SIZE "," \
    G_FILE_ATTRIBUTE_STANDARD_ALLOCATED_SIZE "," \
    G_FILE_ATTRIBUTE_ID_FILESYSTEM "," \
    G_FILE_ATTRIBUTE_UNIX_DEVICE "," \
    G_FILE_ATTRIBUTE_UNIX_INODE

#define PROGRESS_INTERVAL 256

struct _NautilusDiskUsageNode
{
    char *name;
    GFile *location;
    NautilusDiskUsageNode *parent;
    GPtrArray *children;

    guint64 size;
    guint64 direct_size;
    guint64 file_count;
    guint64 direct_file_count;
    guint64 directory_count;
    guint64 unreadable_count;
    guint64 excluded_filesystem_count;
};

typedef struct
{
    guint64 device;
    guint64 inode;
} FileIdentity;

typedef struct
{
    NautilusDiskUsageNode *node;
    GFileEnumerator *enumerator;
    gboolean initialized;
} ScanFrame;

typedef struct
{
    GCancellable *cancellable;
    NautilusDiskUsageProgressFunc progress_callback;
    gpointer progress_data;
    GHashTable *seen_files;
    const char *root_filesystem;
    guint64 files_scanned;
    guint64 directories_scanned;
    guint64 bytes_scanned;
    guint64 entries_since_progress;
} ScanState;

static NautilusDiskUsageNode *
disk_usage_node_new (GFile                 *location,
                     const char            *name,
                     NautilusDiskUsageNode *parent)
{
    NautilusDiskUsageNode *node = g_new0 (NautilusDiskUsageNode, 1);

    node->name = g_strdup (name);
    node->location = g_object_ref (location);
    node->parent = parent;
    node->children = g_ptr_array_new_with_free_func ((GDestroyNotify) nautilus_disk_usage_node_free);

    return node;
}

void
nautilus_disk_usage_node_free (NautilusDiskUsageNode *node)
{
    if (node == NULL)
    {
        return;
    }

    g_clear_pointer (&node->children, g_ptr_array_unref);
    g_clear_object (&node->location);
    g_free (node->name);
    g_free (node);
}

static guint
file_identity_hash (gconstpointer data)
{
    const FileIdentity *identity = data;

    return g_int64_hash (&identity->device) ^ g_int64_hash (&identity->inode);
}

static gboolean
file_identity_equal (gconstpointer a,
                     gconstpointer b)
{
    const FileIdentity *identity_a = a;
    const FileIdentity *identity_b = b;

    return identity_a->device == identity_b->device &&
           identity_a->inode == identity_b->inode;
}

static gboolean
mark_file_seen (ScanState *state,
                GFileInfo *info)
{
    FileIdentity lookup = {
        .device = g_file_info_get_attribute_uint32 (info, G_FILE_ATTRIBUTE_UNIX_DEVICE),
        .inode = g_file_info_get_attribute_uint64 (info, G_FILE_ATTRIBUTE_UNIX_INODE),
    };
    FileIdentity *identity;

    if (lookup.inode == 0 || lookup.device == 0)
    {
        return FALSE;
    }

    if (g_hash_table_contains (state->seen_files, &lookup))
    {
        return TRUE;
    }

    identity = g_memdup2 (&lookup, sizeof (lookup));
    g_hash_table_add (state->seen_files, identity);

    return FALSE;
}

static guint64
get_file_size (GFileInfo *info)
{
    if (g_file_info_has_attribute (info, G_FILE_ATTRIBUTE_STANDARD_ALLOCATED_SIZE))
    {
        return g_file_info_get_attribute_uint64 (info,
                                                 G_FILE_ATTRIBUTE_STANDARD_ALLOCATED_SIZE);
    }

    return MAX ((goffset) 0, g_file_info_get_size (info));
}

static void
report_progress (ScanState *state,
                 gboolean   force)
{
    if (state->progress_callback == NULL)
    {
        return;
    }

    state->entries_since_progress++;
    if (!force && state->entries_since_progress < PROGRESS_INTERVAL)
    {
        return;
    }

    state->entries_since_progress = 0;
    state->progress_callback (state->files_scanned,
                              state->directories_scanned,
                              state->bytes_scanned,
                              state->progress_data);
}

static void
scan_frame_free (ScanFrame *frame)
{
    g_clear_object (&frame->enumerator);
    g_free (frame);
}

static gint
compare_nodes_by_size (gconstpointer a,
                       gconstpointer b)
{
    const NautilusDiskUsageNode *node_a = *(NautilusDiskUsageNode * const *) a;
    const NautilusDiskUsageNode *node_b = *(NautilusDiskUsageNode * const *) b;

    if (node_a->size != node_b->size)
    {
        return node_a->size < node_b->size ? 1 : -1;
    }

    return g_utf8_collate (node_a->name, node_b->name);
}

static void
finish_frame (GPtrArray *stack,
              ScanState *state)
{
    ScanFrame *frame = g_ptr_array_index (stack, stack->len - 1);
    NautilusDiskUsageNode *node = frame->node;

    g_ptr_array_sort (node->children, compare_nodes_by_size);
    g_ptr_array_remove_index (stack, stack->len - 1);

    if (node->parent != NULL)
    {
        node->parent->size += node->size;
        node->parent->file_count += node->file_count;
        node->parent->directory_count += node->directory_count + 1;
        node->parent->unreadable_count += node->unreadable_count;
        node->parent->excluded_filesystem_count += node->excluded_filesystem_count;
    }

    report_progress (state, TRUE);
}

static gboolean
scan_tree (NautilusDiskUsageNode *root,
           ScanState             *state,
           GError               **error)
{
    g_autoptr (GPtrArray) stack = g_ptr_array_new_with_free_func ((GDestroyNotify) scan_frame_free);
    ScanFrame *root_frame = g_new0 (ScanFrame, 1);

    root_frame->node = root;
    g_ptr_array_add (stack, root_frame);

    while (stack->len > 0)
    {
        ScanFrame *frame = g_ptr_array_index (stack, stack->len - 1);
        g_autoptr (GFileInfo) info = NULL;
        g_autoptr (GError) local_error = NULL;

        if (g_cancellable_set_error_if_cancelled (state->cancellable, error))
        {
            return FALSE;
        }

        if (!frame->initialized)
        {
            frame->initialized = TRUE;
            frame->enumerator = g_file_enumerate_children (frame->node->location,
                                                           FILE_ATTRIBUTES,
                                                           G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS,
                                                           state->cancellable,
                                                           &local_error);
            if (frame->enumerator == NULL)
            {
                if (frame->node == root)
                {
                    g_propagate_error (error, g_steal_pointer (&local_error));
                    return FALSE;
                }

                frame->node->unreadable_count++;
                finish_frame (stack, state);
                continue;
            }

            state->directories_scanned++;
            report_progress (state, FALSE);
        }

        info = g_file_enumerator_next_file (frame->enumerator,
                                            state->cancellable,
                                            &local_error);
        if (info == NULL)
        {
            if (local_error != NULL)
            {
                if (g_error_matches (local_error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
                {
                    g_propagate_error (error, g_steal_pointer (&local_error));
                    return FALSE;
                }

                frame->node->unreadable_count++;
            }

            finish_frame (stack, state);
            continue;
        }

        if (g_file_info_get_file_type (info) == G_FILE_TYPE_DIRECTORY)
        {
            const char *filesystem = g_file_info_get_attribute_string (info,
                                                                        G_FILE_ATTRIBUTE_ID_FILESYSTEM);

            if ((state->root_filesystem != NULL &&
                 g_strcmp0 (filesystem, state->root_filesystem) != 0) ||
                mark_file_seen (state, info))
            {
                frame->node->excluded_filesystem_count++;
                report_progress (state, FALSE);
                continue;
            }

            g_autoptr (GFile) child_location = g_file_get_child (frame->node->location,
                                                                 g_file_info_get_name (info));
            const char *display_name = g_file_info_get_display_name (info);
            NautilusDiskUsageNode *child = disk_usage_node_new (child_location,
                                                                 display_name,
                                                                 frame->node);
            ScanFrame *child_frame = g_new0 (ScanFrame, 1);

            g_ptr_array_add (frame->node->children, child);
            child_frame->node = child;
            g_ptr_array_add (stack, child_frame);
            continue;
        }

        frame->node->direct_file_count++;
        frame->node->file_count++;
        state->files_scanned++;

        if (!mark_file_seen (state, info))
        {
            guint64 size = get_file_size (info);

            frame->node->direct_size += size;
            frame->node->size += size;
            state->bytes_scanned += size;
        }

        report_progress (state, FALSE);
    }

    return TRUE;
}

NautilusDiskUsageNode *
nautilus_disk_usage_scan (GFile                         *location,
                          GCancellable                  *cancellable,
                          NautilusDiskUsageProgressFunc  progress_callback,
                          gpointer                       user_data,
                          GError                       **error)
{
    g_autoptr (GFileInfo) root_info = NULL;
    g_autoptr (NautilusDiskUsageNode) root = NULL;
    g_autoptr (GCancellable) owned_cancellable = NULL;
    g_autoptr (GHashTable) seen_files = NULL;
    g_autofree char *fallback_name = NULL;
    ScanState state = { 0 };
    const char *display_name;

    g_return_val_if_fail (G_IS_FILE (location), NULL);
    g_return_val_if_fail (cancellable == NULL || G_IS_CANCELLABLE (cancellable), NULL);

    if (cancellable == NULL)
    {
        owned_cancellable = g_cancellable_new ();
        cancellable = owned_cancellable;
    }

    root_info = g_file_query_info (location,
                                   G_FILE_ATTRIBUTE_STANDARD_DISPLAY_NAME ","
                                   G_FILE_ATTRIBUTE_STANDARD_TYPE ","
                                   G_FILE_ATTRIBUTE_ID_FILESYSTEM ","
                                   G_FILE_ATTRIBUTE_UNIX_DEVICE ","
                                   G_FILE_ATTRIBUTE_UNIX_INODE,
                                   G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS,
                                   cancellable,
                                   error);
    if (root_info == NULL)
    {
        return NULL;
    }

    if (g_file_info_get_file_type (root_info) != G_FILE_TYPE_DIRECTORY)
    {
        g_set_error_literal (error,
                             G_IO_ERROR,
                             G_IO_ERROR_NOT_DIRECTORY,
                             _("The selected item is not a folder"));
        return NULL;
    }

    display_name = g_file_info_get_display_name (root_info);
    if (display_name == NULL || *display_name == '\0')
    {
        fallback_name = g_file_get_basename (location);
        display_name = fallback_name != NULL ? fallback_name : "/";
    }

    root = disk_usage_node_new (location, display_name, NULL);
    seen_files = g_hash_table_new_full (file_identity_hash,
                                        file_identity_equal,
                                        g_free,
                                        NULL);

    state.cancellable = cancellable;
    state.progress_callback = progress_callback;
    state.progress_data = user_data;
    state.seen_files = seen_files;
    state.root_filesystem = g_file_info_get_attribute_string (root_info,
                                                               G_FILE_ATTRIBUTE_ID_FILESYSTEM);
    mark_file_seen (&state, root_info);

    if (!scan_tree (root, &state, error))
    {
        return NULL;
    }

    report_progress (&state, TRUE);
    return g_steal_pointer (&root);
}

const char *
nautilus_disk_usage_node_get_name (NautilusDiskUsageNode *node)
{
    g_return_val_if_fail (node != NULL, NULL);
    return node->name;
}

GFile *
nautilus_disk_usage_node_get_location (NautilusDiskUsageNode *node)
{
    g_return_val_if_fail (node != NULL, NULL);
    return node->location;
}

NautilusDiskUsageNode *
nautilus_disk_usage_node_get_parent (NautilusDiskUsageNode *node)
{
    g_return_val_if_fail (node != NULL, NULL);
    return node->parent;
}

guint
nautilus_disk_usage_node_get_n_children (NautilusDiskUsageNode *node)
{
    g_return_val_if_fail (node != NULL, 0);
    return node->children->len;
}

NautilusDiskUsageNode *
nautilus_disk_usage_node_get_child (NautilusDiskUsageNode *node,
                                    guint                  index)
{
    g_return_val_if_fail (node != NULL, NULL);
    g_return_val_if_fail (index < node->children->len, NULL);
    return g_ptr_array_index (node->children, index);
}

guint64
nautilus_disk_usage_node_get_size (NautilusDiskUsageNode *node)
{
    g_return_val_if_fail (node != NULL, 0);
    return node->size;
}

guint64
nautilus_disk_usage_node_get_direct_size (NautilusDiskUsageNode *node)
{
    g_return_val_if_fail (node != NULL, 0);
    return node->direct_size;
}

guint64
nautilus_disk_usage_node_get_file_count (NautilusDiskUsageNode *node)
{
    g_return_val_if_fail (node != NULL, 0);
    return node->file_count;
}

guint64
nautilus_disk_usage_node_get_direct_file_count (NautilusDiskUsageNode *node)
{
    g_return_val_if_fail (node != NULL, 0);
    return node->direct_file_count;
}

guint64
nautilus_disk_usage_node_get_directory_count (NautilusDiskUsageNode *node)
{
    g_return_val_if_fail (node != NULL, 0);
    return node->directory_count;
}

guint64
nautilus_disk_usage_node_get_unreadable_count (NautilusDiskUsageNode *node)
{
    g_return_val_if_fail (node != NULL, 0);
    return node->unreadable_count;
}

guint64
nautilus_disk_usage_node_get_excluded_filesystem_count (NautilusDiskUsageNode *node)
{
    g_return_val_if_fail (node != NULL, 0);
    return node->excluded_filesystem_count;
}

static guint
find_partition (const guint64 *sizes,
                guint          start,
                guint          end,
                long double    total)
{
    long double before = 0;
    long double best_distance = G_MAXDOUBLE;
    guint best = start + 1;

    for (guint i = start; i < end - 1; i++)
    {
        long double distance;

        before += sizes[i];
        distance = fabsl ((total / 2.0L) - before);
        if (distance < best_distance)
        {
            best_distance = distance;
            best = i + 1;
        }
    }

    return best;
}

static void
layout_partition (const guint64          *sizes,
                  guint                   start,
                  guint                   end,
                  NautilusDiskUsageRect   bounds,
                  NautilusDiskUsageRect  *rectangles)
{
    long double total = 0;

    if (start >= end)
    {
        return;
    }

    if (end - start == 1)
    {
        rectangles[start] = bounds;
        return;
    }

    for (guint i = start; i < end; i++)
    {
        total += sizes[i];
    }

    if (total <= 0)
    {
        double item_width = bounds.width / (end - start);

        for (guint i = start; i < end; i++)
        {
            rectangles[i] = (NautilusDiskUsageRect) {
                bounds.x + item_width * (i - start),
                bounds.y,
                item_width,
                bounds.height,
            };
        }
        return;
    }

    guint split = find_partition (sizes, start, end, total);
    long double first_total = 0;
    NautilusDiskUsageRect first = bounds;
    NautilusDiskUsageRect second = bounds;

    for (guint i = start; i < split; i++)
    {
        first_total += sizes[i];
    }

    if (bounds.width >= bounds.height)
    {
        first.width = bounds.width * (double) (first_total / total);
        second.x = first.x + first.width;
        second.width = MAX (0.0, bounds.width - first.width);
    }
    else
    {
        first.height = bounds.height * (double) (first_total / total);
        second.y = first.y + first.height;
        second.height = MAX (0.0, bounds.height - first.height);
    }

    layout_partition (sizes, start, split, first, rectangles);
    layout_partition (sizes, split, end, second, rectangles);
}

void
nautilus_disk_usage_layout (const guint64          *sizes,
                            guint                   n_sizes,
                            NautilusDiskUsageRect   bounds,
                            NautilusDiskUsageRect  *rectangles)
{
    g_return_if_fail (sizes != NULL || n_sizes == 0);
    g_return_if_fail (rectangles != NULL || n_sizes == 0);

    layout_partition (sizes, 0, n_sizes, bounds, rectangles);
}
