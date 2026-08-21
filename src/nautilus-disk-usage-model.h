/*
 * Copyright © 2026 The Dori authors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <gio/gio.h>

G_BEGIN_DECLS

typedef struct _NautilusDiskUsageNode NautilusDiskUsageNode;

typedef struct
{
    double x;
    double y;
    double width;
    double height;
} NautilusDiskUsageRect;

typedef void (*NautilusDiskUsageProgressFunc) (guint64  files_scanned,
                                               guint64  directories_scanned,
                                               guint64  bytes_scanned,
                                               gpointer user_data);

NautilusDiskUsageNode *nautilus_disk_usage_scan          (GFile                         *location,
                                                          GCancellable                  *cancellable,
                                                          NautilusDiskUsageProgressFunc  progress_callback,
                                                          gpointer                       user_data,
                                                          GError                       **error);
void                   nautilus_disk_usage_node_free     (NautilusDiskUsageNode         *node);

const char            *nautilus_disk_usage_node_get_name (NautilusDiskUsageNode         *node);
GFile                 *nautilus_disk_usage_node_get_location
                                                         (NautilusDiskUsageNode         *node);
NautilusDiskUsageNode *nautilus_disk_usage_node_get_parent
                                                         (NautilusDiskUsageNode         *node);
guint                  nautilus_disk_usage_node_get_n_children
                                                         (NautilusDiskUsageNode         *node);
NautilusDiskUsageNode *nautilus_disk_usage_node_get_child
                                                         (NautilusDiskUsageNode         *node,
                                                          guint                          index);
guint64                nautilus_disk_usage_node_get_size (NautilusDiskUsageNode         *node);
guint64                nautilus_disk_usage_node_get_direct_size
                                                         (NautilusDiskUsageNode         *node);
guint64                nautilus_disk_usage_node_get_file_count
                                                         (NautilusDiskUsageNode         *node);
guint64                nautilus_disk_usage_node_get_direct_file_count
                                                         (NautilusDiskUsageNode         *node);
guint64                nautilus_disk_usage_node_get_directory_count
                                                         (NautilusDiskUsageNode         *node);
guint64                nautilus_disk_usage_node_get_unreadable_count
                                                         (NautilusDiskUsageNode         *node);
guint64                nautilus_disk_usage_node_get_excluded_filesystem_count
                                                         (NautilusDiskUsageNode         *node);

void                   nautilus_disk_usage_layout        (const guint64                 *sizes,
                                                          guint                          n_sizes,
                                                          NautilusDiskUsageRect           bounds,
                                                          NautilusDiskUsageRect          *rectangles);

G_DEFINE_AUTOPTR_CLEANUP_FUNC (NautilusDiskUsageNode, nautilus_disk_usage_node_free)

G_END_DECLS
