/*
 * Copyright © 2026 The Dori authors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <glib.h>
#include <glib/gstdio.h>
#include <unistd.h>

#include "nautilus-folder-tools.h"

static void
remove_tree (const char *path)
{
    g_autoptr (GDir) directory = NULL;
    const char *name;

    if (g_file_test (path, G_FILE_TEST_IS_SYMLINK) ||
        !g_file_test (path, G_FILE_TEST_IS_DIR))
    {
        g_remove (path);
        return;
    }

    directory = g_dir_open (path, 0, NULL);
    while (directory != NULL && (name = g_dir_read_name (directory)) != NULL)
    {
        g_autofree char *child = g_build_filename (path, name, NULL);
        remove_tree (child);
    }

    g_rmdir (path);
}

static void
create_file (const char *path,
             const char *contents)
{
    g_assert_true (g_file_set_contents (path, contents, -1, NULL));
}

static void
test_categories_match_script (void)
{
    g_assert_cmpstr (nautilus_folder_tools_get_category_for_name ("photo.JPG"), ==, "imagenes");
    g_assert_cmpstr (nautilus_folder_tools_get_category_for_name ("clip.webm"), ==, "videos");
    g_assert_cmpstr (nautilus_folder_tools_get_category_for_name ("song.flac"), ==, "audio");
    g_assert_cmpstr (nautilus_folder_tools_get_category_for_name ("notes.txt"), ==, "documentos");
    g_assert_cmpstr (nautilus_folder_tools_get_category_for_name ("backup.7z"), ==, "archivos");
    g_assert_cmpstr (nautilus_folder_tools_get_category_for_name ("model.blend"), ==, "otros");
    g_assert_null (nautilus_folder_tools_get_category_for_name ("README"));
    g_assert_null (nautilus_folder_tools_get_category_for_name (".hidden"));
    g_assert_null (nautilus_folder_tools_get_category_for_name ("file.extensiontoolong"));
}

static void
test_group_media_only_moves_direct_files (void)
{
    g_autofree char *root = g_dir_make_tmp ("nautilus-folder-tools-XXXXXX", NULL);
    g_autofree char *nested = g_build_filename (root, "nested", NULL);
    g_autofree char *images = g_build_filename (root, "imagenes", NULL);
    g_autofree char *root_photo = g_build_filename (root, "photo.jpg", NULL);
    g_autofree char *existing_photo = g_build_filename (images, "photo.jpg", NULL);
    g_autofree char *unique_photo = g_build_filename (images, "photo_1.jpg", NULL);
    g_autofree char *root_video = g_build_filename (root, "clip.MP4", NULL);
    g_autofree char *moved_video = g_build_filename (root, "videos", "clip.MP4", NULL);
    g_autofree char *root_unknown = g_build_filename (root, "model.blend", NULL);
    g_autofree char *moved_unknown = g_build_filename (root, "otros", "model.blend", NULL);
    g_autofree char *root_no_extension = g_build_filename (root, "README", NULL);
    g_autofree char *nested_photo = g_build_filename (nested, "nested.jpg", NULL);
    g_autofree char *link_path = g_build_filename (root, "linked.jpg", NULL);
    g_autoptr (GFile) location = g_file_new_for_path (root);
    g_autoptr (NautilusFolderToolsResult) result = NULL;
    g_autoptr (GError) error = NULL;

    g_assert_nonnull (root);
    g_assert_cmpint (g_mkdir (nested, 0700), ==, 0);
    g_assert_cmpint (g_mkdir (images, 0700), ==, 0);
    create_file (root_photo, "new");
    create_file (existing_photo, "old");
    create_file (root_video, "video");
    create_file (root_unknown, "other");
    create_file (root_no_extension, "plain");
    create_file (nested_photo, "nested");
    g_assert_cmpint (symlink (nested_photo, link_path), ==, 0);
    for (guint i = 0; i < 64; i++)
    {
        g_autofree char *name = g_strdup_printf ("bulk-%u.jpg", i);
        g_autofree char *path = g_build_filename (root, name, NULL);
        create_file (path, "bulk");
    }

    result = nautilus_folder_tools_group_media (location, NULL, &error);

    g_assert_no_error (error);
    g_assert_nonnull (result);
    g_assert_cmpuint (nautilus_folder_tools_result_get_changed (result), ==, 67);
    g_assert_cmpuint (nautilus_folder_tools_result_get_failed (result), ==, 0);
    g_assert_true (g_file_test (unique_photo, G_FILE_TEST_IS_REGULAR));
    g_assert_true (g_file_test (existing_photo, G_FILE_TEST_IS_REGULAR));
    g_assert_true (g_file_test (moved_video, G_FILE_TEST_IS_REGULAR));
    g_assert_true (g_file_test (moved_unknown, G_FILE_TEST_IS_REGULAR));
    g_assert_true (g_file_test (root_no_extension, G_FILE_TEST_IS_REGULAR));
    g_assert_true (g_file_test (nested_photo, G_FILE_TEST_IS_REGULAR));
    g_assert_true (g_file_test (link_path, G_FILE_TEST_IS_SYMLINK));
    g_assert_false (g_file_test (root_photo, G_FILE_TEST_EXISTS));

    remove_tree (root);
}

static void
test_clean_empty_is_recursive_and_safe (void)
{
    g_autofree char *root = g_dir_make_tmp ("nautilus-folder-tools-XXXXXX", NULL);
    g_autofree char *empty_parent = g_build_filename (root, "empty-parent", NULL);
    g_autofree char *empty_nested = g_build_filename (empty_parent, "empty-nested", NULL);
    g_autofree char *empty_sibling = g_build_filename (root, "empty-sibling", NULL);
    g_autofree char *nonempty = g_build_filename (root, "nonempty", NULL);
    g_autofree char *nonempty_file = g_build_filename (nonempty, "keep.txt", NULL);
    g_autofree char *external = g_dir_make_tmp ("nautilus-folder-tools-external-XXXXXX", NULL);
    g_autofree char *link_path = g_build_filename (root, "linked-directory", NULL);
    g_autoptr (GFile) location = g_file_new_for_path (root);
    g_autoptr (NautilusFolderToolsResult) result = NULL;
    g_autoptr (GError) error = NULL;

    g_assert_nonnull (root);
    g_assert_nonnull (external);
    g_assert_cmpint (g_mkdir (empty_parent, 0700), ==, 0);
    g_assert_cmpint (g_mkdir (empty_nested, 0700), ==, 0);
    g_assert_cmpint (g_mkdir (empty_sibling, 0700), ==, 0);
    g_assert_cmpint (g_mkdir (nonempty, 0700), ==, 0);
    create_file (nonempty_file, "keep");
    g_assert_cmpint (symlink (external, link_path), ==, 0);
    for (guint i = 0; i < 64; i++)
    {
        g_autofree char *name = g_strdup_printf ("empty-bulk-%u", i);
        g_autofree char *path = g_build_filename (root, name, NULL);
        g_assert_cmpint (g_mkdir (path, 0700), ==, 0);
    }

    result = nautilus_folder_tools_clean_empty (location, NULL, &error);

    g_assert_no_error (error);
    g_assert_nonnull (result);
    g_assert_cmpuint (nautilus_folder_tools_result_get_changed (result), ==, 67);
    g_assert_cmpuint (nautilus_folder_tools_result_get_failed (result), ==, 0);
    g_assert_true (g_file_test (root, G_FILE_TEST_IS_DIR));
    g_assert_false (g_file_test (empty_parent, G_FILE_TEST_EXISTS));
    g_assert_false (g_file_test (empty_sibling, G_FILE_TEST_EXISTS));
    g_assert_true (g_file_test (nonempty_file, G_FILE_TEST_IS_REGULAR));
    g_assert_true (g_file_test (link_path, G_FILE_TEST_IS_SYMLINK));
    g_assert_true (g_file_test (external, G_FILE_TEST_IS_DIR));

    remove_tree (root);
    remove_tree (external);
}

static void
test_group_duplicates_only_compares_direct_files (void)
{
    g_autofree char *root = g_dir_make_tmp ("nautilus-folder-tools-XXXXXX", NULL);
    g_autofree char *nested = g_build_filename (root, "nested", NULL);
    g_autofree char *duplicates = g_build_filename (root, "duplicados", NULL);
    g_autofree char *original = g_build_filename (root, "a.bin", NULL);
    g_autofree char *duplicate = g_build_filename (root, "b.bin", NULL);
    g_autofree char *unique = g_build_filename (root, "unique.bin", NULL);
    g_autofree char *nested_copy = g_build_filename (nested, "nested-copy.bin", NULL);
    g_autofree char *existing_destination = g_build_filename (duplicates, "b.bin", NULL);
    g_autofree char *moved_duplicate = g_build_filename (duplicates, "b.bin_1", NULL);
    g_autofree char *link_path = g_build_filename (root, "linked.bin", NULL);
    g_autoptr (GFile) location = g_file_new_for_path (root);
    g_autoptr (NautilusFolderToolsResult) result = NULL;
    g_autoptr (GError) error = NULL;

    g_assert_nonnull (root);
    g_assert_cmpint (g_mkdir (nested, 0700), ==, 0);
    g_assert_cmpint (g_mkdir (duplicates, 0700), ==, 0);
    create_file (original, "same contents");
    create_file (duplicate, "same contents");
    create_file (unique, "different contents");
    create_file (nested_copy, "same contents");
    create_file (existing_destination, "occupied name");
    g_assert_cmpint (symlink (original, link_path), ==, 0);

    result = nautilus_folder_tools_group_duplicates (location, FALSE, NULL, &error);

    g_assert_no_error (error);
    g_assert_nonnull (result);
    g_assert_cmpuint (nautilus_folder_tools_result_get_changed (result), ==, 1);
    g_assert_cmpuint (nautilus_folder_tools_result_get_failed (result), ==, 0);
    g_assert_true (g_file_test (original, G_FILE_TEST_IS_REGULAR));
    g_assert_false (g_file_test (duplicate, G_FILE_TEST_EXISTS));
    g_assert_true (g_file_test (moved_duplicate, G_FILE_TEST_IS_REGULAR));
    g_assert_true (g_file_test (existing_destination, G_FILE_TEST_IS_REGULAR));
    g_assert_true (g_file_test (nested_copy, G_FILE_TEST_IS_REGULAR));
    g_assert_true (g_file_test (unique, G_FILE_TEST_IS_REGULAR));
    g_assert_true (g_file_test (link_path, G_FILE_TEST_IS_SYMLINK));

    remove_tree (root);
}

static void
test_group_duplicates_deep_scans_subfolders (void)
{
    g_autofree char *root = g_dir_make_tmp ("nautilus-folder-tools-XXXXXX", NULL);
    g_autofree char *nested = g_build_filename (root, "nested", NULL);
    g_autofree char *deeper = g_build_filename (nested, "deeper", NULL);
    g_autofree char *duplicates = g_build_filename (root, "duplicados", NULL);
    g_autofree char *original = g_build_filename (root, "original.bin", NULL);
    g_autofree char *nested_copy = g_build_filename (nested, "copy.bin", NULL);
    g_autofree char *deep_copy = g_build_filename (deeper, "deep-copy.bin", NULL);
    g_autofree char *ignored_copy = g_build_filename (duplicates, "ignored.bin", NULL);
    g_autofree char *moved_nested = g_build_filename (duplicates, "copy.bin", NULL);
    g_autofree char *moved_deep = g_build_filename (duplicates, "deep-copy.bin", NULL);
    g_autofree char *link_path = g_build_filename (nested, "linked.bin", NULL);
    g_autoptr (GFile) location = g_file_new_for_path (root);
    g_autoptr (NautilusFolderToolsResult) result = NULL;
    g_autoptr (GError) error = NULL;

    g_assert_nonnull (root);
    g_assert_cmpint (g_mkdir (nested, 0700), ==, 0);
    g_assert_cmpint (g_mkdir (deeper, 0700), ==, 0);
    g_assert_cmpint (g_mkdir (duplicates, 0700), ==, 0);
    create_file (original, "same contents");
    create_file (nested_copy, "same contents");
    create_file (deep_copy, "same contents");
    create_file (ignored_copy, "same contents");
    g_assert_cmpint (symlink (original, link_path), ==, 0);

    result = nautilus_folder_tools_group_duplicates (location, TRUE, NULL, &error);

    g_assert_no_error (error);
    g_assert_nonnull (result);
    g_assert_cmpuint (nautilus_folder_tools_result_get_changed (result), ==, 2);
    g_assert_cmpuint (nautilus_folder_tools_result_get_failed (result), ==, 0);
    g_assert_true (g_file_test (original, G_FILE_TEST_IS_REGULAR));
    g_assert_false (g_file_test (nested_copy, G_FILE_TEST_EXISTS));
    g_assert_false (g_file_test (deep_copy, G_FILE_TEST_EXISTS));
    g_assert_true (g_file_test (moved_nested, G_FILE_TEST_IS_REGULAR));
    g_assert_true (g_file_test (moved_deep, G_FILE_TEST_IS_REGULAR));
    g_assert_true (g_file_test (ignored_copy, G_FILE_TEST_IS_REGULAR));
    g_assert_true (g_file_test (link_path, G_FILE_TEST_IS_SYMLINK));

    remove_tree (root);
}

static void
test_operations_honor_cancellation (void)
{
    g_autofree char *root = g_dir_make_tmp ("nautilus-folder-tools-XXXXXX", NULL);
    g_autoptr (GFile) location = g_file_new_for_path (root);
    g_autoptr (GCancellable) cancellable = g_cancellable_new ();
    g_autoptr (NautilusFolderToolsResult) result = NULL;
    g_autoptr (GError) error = NULL;

    g_cancellable_cancel (cancellable);
    result = nautilus_folder_tools_clean_empty (location, cancellable, &error);

    g_assert_null (result);
    g_assert_error (error, G_IO_ERROR, G_IO_ERROR_CANCELLED);
    remove_tree (root);
}

typedef struct
{
    GMainLoop *loop;
    NautilusFolderToolsResult *result;
    GError *error;
} AsyncResultData;

static void
group_media_done (GObject      *source_object,
                  GAsyncResult *result,
                  gpointer      user_data)
{
    AsyncResultData *data = user_data;

    data->result = nautilus_folder_tools_finish (result, &data->error);
    g_main_loop_quit (data->loop);
}

static void
test_async_wrapper_returns_result (void)
{
    g_autofree char *root = g_dir_make_tmp ("nautilus-folder-tools-XXXXXX", NULL);
    g_autofree char *source = g_build_filename (root, "photo.png", NULL);
    g_autofree char *destination = g_build_filename (root, "imagenes", "photo.png", NULL);
    g_autoptr (GFile) location = g_file_new_for_path (root);
    g_autoptr (GMainLoop) loop = g_main_loop_new (NULL, FALSE);
    g_autoptr (NautilusFolderToolsResult) result = NULL;
    g_autoptr (GError) error = NULL;
    AsyncResultData data = { .loop = loop };

    create_file (source, "image");
    nautilus_folder_tools_group_media_async (location, NULL, group_media_done, &data);
    g_main_loop_run (loop);

    result = g_steal_pointer (&data.result);
    error = g_steal_pointer (&data.error);
    g_assert_no_error (error);
    g_assert_nonnull (result);
    g_assert_cmpuint (nautilus_folder_tools_result_get_changed (result), ==, 1);
    g_assert_true (g_file_test (destination, G_FILE_TEST_IS_REGULAR));
    remove_tree (root);
}

static void
test_group_duplicates_async_uses_recursive_scope (void)
{
    g_autofree char *root = g_dir_make_tmp ("nautilus-folder-tools-XXXXXX", NULL);
    g_autofree char *nested = g_build_filename (root, "nested", NULL);
    g_autofree char *original = g_build_filename (root, "original.bin", NULL);
    g_autofree char *copy = g_build_filename (nested, "copy.bin", NULL);
    g_autofree char *destination = g_build_filename (root, "duplicados", "copy.bin", NULL);
    g_autoptr (GFile) location = g_file_new_for_path (root);
    g_autoptr (GMainLoop) loop = g_main_loop_new (NULL, FALSE);
    g_autoptr (NautilusFolderToolsResult) result = NULL;
    g_autoptr (GError) error = NULL;
    AsyncResultData data = { .loop = loop };

    g_assert_nonnull (root);
    g_assert_cmpint (g_mkdir (nested, 0700), ==, 0);
    create_file (original, "same contents");
    create_file (copy, "same contents");

    nautilus_folder_tools_group_duplicates_async (location,
                                                   TRUE,
                                                   NULL,
                                                   group_media_done,
                                                   &data);
    g_main_loop_run (loop);

    result = g_steal_pointer (&data.result);
    error = g_steal_pointer (&data.error);
    g_assert_no_error (error);
    g_assert_nonnull (result);
    g_assert_cmpuint (nautilus_folder_tools_result_get_changed (result), ==, 1);
    g_assert_true (g_file_test (destination, G_FILE_TEST_IS_REGULAR));
    remove_tree (root);
}

int
main (int   argc,
      char *argv[])
{
    g_test_init (&argc, &argv, NULL);

    g_test_add_func ("/folder-tools/categories/match-script", test_categories_match_script);
    g_test_add_func ("/folder-tools/group/direct-files", test_group_media_only_moves_direct_files);
    g_test_add_func ("/folder-tools/clean/recursive-safe", test_clean_empty_is_recursive_and_safe);
    g_test_add_func ("/folder-tools/duplicates/direct-files", test_group_duplicates_only_compares_direct_files);
    g_test_add_func ("/folder-tools/duplicates/deep", test_group_duplicates_deep_scans_subfolders);
    g_test_add_func ("/folder-tools/cancellation", test_operations_honor_cancellation);
    g_test_add_func ("/folder-tools/async/result", test_async_wrapper_returns_result);
    g_test_add_func ("/folder-tools/async/duplicates-deep", test_group_duplicates_async_uses_recursive_scope);

    return g_test_run ();
}
