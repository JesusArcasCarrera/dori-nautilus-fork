/*
 * Copyright © 2026 The Dori authors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <gio/gio.h>

G_BEGIN_DECLS

typedef struct _NautilusFolderToolsResult NautilusFolderToolsResult;

const char *nautilus_folder_tools_get_category_for_name (const char *name);

NautilusFolderToolsResult *nautilus_folder_tools_clean_empty (GFile         *location,
                                                              GCancellable *cancellable,
                                                              GError      **error);
NautilusFolderToolsResult *nautilus_folder_tools_group_media (GFile         *location,
                                                              GCancellable *cancellable,
                                                              GError      **error);
NautilusFolderToolsResult *nautilus_folder_tools_group_duplicates (GFile         *location,
                                                                   gboolean       recursive,
                                                                   GCancellable *cancellable,
                                                                   GError      **error);

void nautilus_folder_tools_clean_empty_async (GFile               *location,
                                              GCancellable         *cancellable,
                                              GAsyncReadyCallback   callback,
                                              gpointer              user_data);
void nautilus_folder_tools_group_media_async (GFile               *location,
                                              GCancellable         *cancellable,
                                              GAsyncReadyCallback   callback,
                                              gpointer              user_data);
void nautilus_folder_tools_group_duplicates_async (GFile               *location,
                                                   gboolean             recursive,
                                                   GCancellable         *cancellable,
                                                   GAsyncReadyCallback   callback,
                                                   gpointer              user_data);

NautilusFolderToolsResult *nautilus_folder_tools_finish (GAsyncResult  *result,
                                                         GError       **error);

guint nautilus_folder_tools_result_get_changed (NautilusFolderToolsResult *result);
guint nautilus_folder_tools_result_get_failed (NautilusFolderToolsResult *result);
const char *nautilus_folder_tools_result_get_first_error (NautilusFolderToolsResult *result);
void nautilus_folder_tools_result_free (NautilusFolderToolsResult *result);

G_DEFINE_AUTOPTR_CLEANUP_FUNC (NautilusFolderToolsResult, nautilus_folder_tools_result_free)

G_END_DECLS
