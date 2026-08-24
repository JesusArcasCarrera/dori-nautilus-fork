/*
 * Copyright © 2026 The Dori authors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "nautilus-folder-tools.h"

#include <string.h>

struct _NautilusFolderToolsResult
{
    guint changed;
    guint failed;
    char *first_error;
};

typedef NautilusFolderToolsResult *(*FolderToolFunc) (GFile         *location,
                                                      GCancellable *cancellable,
                                                      GError      **error);

typedef struct
{
    FolderToolFunc func;
} FolderToolTaskData;

static void
result_record_error (NautilusFolderToolsResult *result,
                     const GError              *error)
{
    result->failed++;

    if (result->first_error == NULL)
    {
        result->first_error = g_strdup (error->message);
    }
}

void
nautilus_folder_tools_result_free (NautilusFolderToolsResult *result)
{
    if (result == NULL)
    {
        return;
    }

    g_free (result->first_error);
    g_free (result);
}

guint
nautilus_folder_tools_result_get_changed (NautilusFolderToolsResult *result)
{
    g_return_val_if_fail (result != NULL, 0);

    return result->changed;
}

guint
nautilus_folder_tools_result_get_failed (NautilusFolderToolsResult *result)
{
    g_return_val_if_fail (result != NULL, 0);

    return result->failed;
}

const char *
nautilus_folder_tools_result_get_first_error (NautilusFolderToolsResult *result)
{
    g_return_val_if_fail (result != NULL, NULL);

    return result->first_error;
}

const char *
nautilus_folder_tools_get_category_for_name (const char *name)
{
    static const char *image_extensions[] = { "jpg", "jpeg", "png", "gif", "bmp", "tiff", "webp", "svg" };
    static const char *video_extensions[] = { "mp4", "avi", "mkv", "mov", "wmv", "flv", "webm", "m4v" };
    static const char *audio_extensions[] = { "mp3", "wav", "flac", "aac", "ogg", "m4a", "wma" };
    static const char *document_extensions[] = { "pdf", "doc", "docx", "txt", "rtf" };
    static const char *archive_extensions[] = { "zip", "rar", "7z", "tar", "gz" };
    const char *dot;
    g_autofree char *extension = NULL;

    g_return_val_if_fail (name != NULL, NULL);

    dot = strrchr (name, '.');
    if (dot == NULL || dot == name || dot[1] == '\0' || strlen (dot + 1) > 10)
    {
        return NULL;
    }

    extension = g_ascii_strdown (dot + 1, -1);

    for (guint i = 0; i < G_N_ELEMENTS (image_extensions); i++)
    {
        if (g_str_equal (extension, image_extensions[i]))
        {
            return "imagenes";
        }
    }
    for (guint i = 0; i < G_N_ELEMENTS (video_extensions); i++)
    {
        if (g_str_equal (extension, video_extensions[i]))
        {
            return "videos";
        }
    }
    for (guint i = 0; i < G_N_ELEMENTS (audio_extensions); i++)
    {
        if (g_str_equal (extension, audio_extensions[i]))
        {
            return "audio";
        }
    }
    for (guint i = 0; i < G_N_ELEMENTS (document_extensions); i++)
    {
        if (g_str_equal (extension, document_extensions[i]))
        {
            return "documentos";
        }
    }
    for (guint i = 0; i < G_N_ELEMENTS (archive_extensions); i++)
    {
        if (g_str_equal (extension, archive_extensions[i]))
        {
            return "archivos";
        }
    }

    return "otros";
}

static gboolean
check_cancelled (GCancellable  *cancellable,
                 GError       **error)
{
    return cancellable != NULL && g_cancellable_set_error_if_cancelled (cancellable, error);
}

static GPtrArray *
enumerate_child_names (GFile         *directory,
                       GFileType      file_type,
                       GCancellable  *cancellable,
                       GError       **error)
{
    g_autoptr (GFileEnumerator) enumerator = NULL;
    g_autoptr (GPtrArray) names = g_ptr_array_new_with_free_func (g_free);

    enumerator = g_file_enumerate_children (directory,
                                            G_FILE_ATTRIBUTE_STANDARD_NAME ","
                                            G_FILE_ATTRIBUTE_STANDARD_TYPE,
                                            G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS,
                                            cancellable,
                                            error);
    if (enumerator == NULL)
    {
        return NULL;
    }

    while (TRUE)
    {
        g_autoptr (GFileInfo) info = NULL;

        info = g_file_enumerator_next_file (enumerator, cancellable, error);
        if (info == NULL)
        {
            if (error != NULL && *error != NULL)
            {
                return NULL;
            }

            break;
        }

        if (g_file_info_get_file_type (info) == file_type)
        {
            g_ptr_array_add (names, g_strdup (g_file_info_get_name (info)));
        }
    }

    if (!g_file_enumerator_close (enumerator, cancellable, error))
    {
        return NULL;
    }

    return g_steal_pointer (&names);
}

static gboolean
clean_empty_children (GFile                      *directory,
                      GCancellable                *cancellable,
                      NautilusFolderToolsResult   *result,
                      GError                     **error)
{
    g_autoptr (GPtrArray) child_names = NULL;
    g_autoptr (GError) local_error = NULL;
    gboolean fully_scanned = TRUE;

    if (check_cancelled (cancellable, error))
    {
        return FALSE;
    }

    child_names = enumerate_child_names (directory,
                                         G_FILE_TYPE_DIRECTORY,
                                         cancellable,
                                         &local_error);
    if (child_names == NULL)
    {
        if (g_error_matches (local_error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
        {
            g_propagate_error (error, g_steal_pointer (&local_error));
        }
        else
        {
            result_record_error (result, local_error);
        }

        return FALSE;
    }

    for (guint i = 0; i < child_names->len; i++)
    {
        g_autoptr (GFile) child = NULL;

        child = g_file_get_child (directory, g_ptr_array_index (child_names, i));
        if (!clean_empty_children (child, cancellable, result, error))
        {
            if (error != NULL && *error != NULL)
            {
                return FALSE;
            }

            fully_scanned = FALSE;
            continue;
        }

        g_clear_error (&local_error);
        if (g_file_delete (child, cancellable, &local_error))
        {
            result->changed++;
        }
        else if (!g_error_matches (local_error, G_IO_ERROR, G_IO_ERROR_NOT_EMPTY))
        {
            if (g_error_matches (local_error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
            {
                g_propagate_error (error, g_steal_pointer (&local_error));
                return FALSE;
            }

            result_record_error (result, local_error);
            fully_scanned = FALSE;
        }

        g_clear_error (&local_error);
    }

    return fully_scanned;
}

NautilusFolderToolsResult *
nautilus_folder_tools_clean_empty (GFile         *location,
                                   GCancellable *cancellable,
                                   GError      **error)
{
    g_autoptr (NautilusFolderToolsResult) result = NULL;

    g_return_val_if_fail (G_IS_FILE (location), NULL);
    g_return_val_if_fail (error == NULL || *error == NULL, NULL);

    result = g_new0 (NautilusFolderToolsResult, 1);
    clean_empty_children (location, cancellable, result, error);

    if (error != NULL && *error != NULL)
    {
        return NULL;
    }

    return g_steal_pointer (&result);
}

static GFile *
get_unique_destination (GFile       *category_directory,
                        const char  *name,
                        guint        suffix)
{
    g_autofree char *candidate = NULL;

    if (suffix == 0)
    {
        return g_file_get_child (category_directory, name);
    }

    const char *dot = strrchr (name, '.');
    g_autofree char *base = g_strndup (name, dot - name);
    candidate = g_strdup_printf ("%s_%u%s", base, suffix, dot);

    return g_file_get_child (category_directory, candidate);
}

NautilusFolderToolsResult *
nautilus_folder_tools_group_media (GFile         *location,
                                   GCancellable *cancellable,
                                   GError      **error)
{
    g_autoptr (NautilusFolderToolsResult) result = NULL;
    g_autoptr (GPtrArray) file_names = NULL;
    g_autoptr (GError) local_error = NULL;

    g_return_val_if_fail (G_IS_FILE (location), NULL);
    g_return_val_if_fail (error == NULL || *error == NULL, NULL);

    result = g_new0 (NautilusFolderToolsResult, 1);
    file_names = enumerate_child_names (location,
                                        G_FILE_TYPE_REGULAR,
                                        cancellable,
                                        error);
    if (file_names == NULL)
    {
        return NULL;
    }

    for (guint i = 0; i < file_names->len; i++)
    {
        g_autoptr (GFile) source = NULL;
        g_autoptr (GFile) category_directory = NULL;
        const char *name = g_ptr_array_index (file_names, i);
        const char *category;

        category = nautilus_folder_tools_get_category_for_name (name);
        if (category == NULL)
        {
            continue;
        }

        source = g_file_get_child (location, name);
        category_directory = g_file_get_child (location, category);

        g_clear_error (&local_error);
        if (!g_file_make_directory (category_directory, cancellable, &local_error) &&
            !g_error_matches (local_error, G_IO_ERROR, G_IO_ERROR_EXISTS))
        {
            if (g_error_matches (local_error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
            {
                g_propagate_error (error, g_steal_pointer (&local_error));
                return NULL;
            }

            result_record_error (result, local_error);
            continue;
        }

        for (guint suffix = 0; ; suffix++)
        {
            g_autoptr (GFile) destination = get_unique_destination (category_directory,
                                                                    name,
                                                                    suffix);

            g_clear_error (&local_error);
            if (g_file_move (source,
                             destination,
                             G_FILE_COPY_NOFOLLOW_SYMLINKS,
                             cancellable,
                             NULL,
                             NULL,
                             &local_error))
            {
                result->changed++;
                break;
            }

            if (g_error_matches (local_error, G_IO_ERROR, G_IO_ERROR_EXISTS))
            {
                continue;
            }
            if (g_error_matches (local_error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
            {
                g_propagate_error (error, g_steal_pointer (&local_error));
                return NULL;
            }

            result_record_error (result, local_error);
            break;
        }
    }

    return g_steal_pointer (&result);
}

static void
folder_tool_thread (GTask        *task,
                    gpointer      source_object,
                    gpointer      task_data,
                    GCancellable *cancellable)
{
    FolderToolTaskData *data = task_data;
    g_autoptr (GError) error = NULL;
    NautilusFolderToolsResult *result;

    result = data->func (G_FILE (source_object), cancellable, &error);
    if (result == NULL)
    {
        g_task_return_error (task, g_steal_pointer (&error));
    }
    else
    {
        g_task_return_pointer (task, result, (GDestroyNotify) nautilus_folder_tools_result_free);
    }
}

static void
run_async (GFile               *location,
           FolderToolFunc       func,
           GCancellable        *cancellable,
           GAsyncReadyCallback  callback,
           gpointer             user_data)
{
    g_autoptr (GTask) task = NULL;
    FolderToolTaskData *data;

    g_return_if_fail (G_IS_FILE (location));

    task = g_task_new (location, cancellable, callback, user_data);
    data = g_new0 (FolderToolTaskData, 1);
    data->func = func;
    g_task_set_task_data (task, data, g_free);
    g_task_run_in_thread (task, folder_tool_thread);
}

void
nautilus_folder_tools_clean_empty_async (GFile               *location,
                                         GCancellable         *cancellable,
                                         GAsyncReadyCallback   callback,
                                         gpointer              user_data)
{
    run_async (location, nautilus_folder_tools_clean_empty, cancellable, callback, user_data);
}

void
nautilus_folder_tools_group_media_async (GFile               *location,
                                         GCancellable         *cancellable,
                                         GAsyncReadyCallback   callback,
                                         gpointer              user_data)
{
    run_async (location, nautilus_folder_tools_group_media, cancellable, callback, user_data);
}

NautilusFolderToolsResult *
nautilus_folder_tools_finish (GAsyncResult  *result,
                              GError       **error)
{
    g_return_val_if_fail (G_IS_TASK (result), NULL);

    return g_task_propagate_pointer (G_TASK (result), error);
}
