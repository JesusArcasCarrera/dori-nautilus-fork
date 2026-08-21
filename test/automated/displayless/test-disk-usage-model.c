/*
 * Copyright © 2026 The Dori authors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <glib.h>
#include <glib/gstdio.h>
#include <unistd.h>

#include "nautilus-disk-usage-model.h"

static double
rectangle_area (NautilusDiskUsageRect *rectangle)
{
    return rectangle->width * rectangle->height;
}

static void
test_layout_is_proportional (void)
{
    const guint64 sizes[] = { 60, 30, 10 };
    NautilusDiskUsageRect bounds = { 20, 10, 1000, 500 };
    NautilusDiskUsageRect rectangles[G_N_ELEMENTS (sizes)];
    double total_area = rectangle_area (&bounds);

    nautilus_disk_usage_layout (sizes, G_N_ELEMENTS (sizes), bounds, rectangles);

    for (guint i = 0; i < G_N_ELEMENTS (sizes); i++)
    {
        g_assert_cmpfloat_with_epsilon (rectangle_area (&rectangles[i]),
                                        total_area * sizes[i] / 100.0,
                                        0.01);
        g_assert_cmpfloat (rectangles[i].x, >=, bounds.x);
        g_assert_cmpfloat (rectangles[i].y, >=, bounds.y);
        g_assert_cmpfloat (rectangles[i].x + rectangles[i].width,
                           <=,
                           bounds.x + bounds.width + 0.001);
        g_assert_cmpfloat (rectangles[i].y + rectangles[i].height,
                           <=,
                           bounds.y + bounds.height + 0.001);
    }
}

static void
test_layout_handles_zero_sizes (void)
{
    const guint64 sizes[] = { 0, 0, 0, 0 };
    NautilusDiskUsageRect bounds = { 0, 0, 400, 200 };
    NautilusDiskUsageRect rectangles[G_N_ELEMENTS (sizes)];

    nautilus_disk_usage_layout (sizes, G_N_ELEMENTS (sizes), bounds, rectangles);

    for (guint i = 0; i < G_N_ELEMENTS (sizes); i++)
    {
        g_assert_cmpfloat_with_epsilon (rectangles[i].width, 100, 0.001);
        g_assert_cmpfloat_with_epsilon (rectangles[i].height, 200, 0.001);
    }
}

static void
test_scan_aggregates_directories (void)
{
    g_autofree char *tmp_directory = g_dir_make_tmp ("nautilus-disk-usage-XXXXXX", NULL);
    g_autofree char *alpha_path = g_build_filename (tmp_directory, "alpha", NULL);
    g_autofree char *nested_path = g_build_filename (alpha_path, "nested", NULL);
    g_autofree char *root_file = g_build_filename (tmp_directory, "root.bin", NULL);
    g_autofree char *alpha_file = g_build_filename (alpha_path, "one.bin", NULL);
    g_autofree char *nested_file = g_build_filename (nested_path, "two.bin", NULL);
    g_autofree char *loop_path = g_build_filename (tmp_directory, "loop", NULL);
    g_autoptr (GFile) location = NULL;
    g_autoptr (NautilusDiskUsageNode) root = NULL;
    g_autoptr (GError) error = NULL;
    NautilusDiskUsageNode *alpha;

    g_assert_nonnull (tmp_directory);
    g_assert_cmpint (g_mkdir (alpha_path, 0700), ==, 0);
    g_assert_cmpint (g_mkdir (nested_path, 0700), ==, 0);
    g_assert_true (g_file_set_contents (root_file, "abc", 3, NULL));
    g_assert_true (g_file_set_contents (alpha_file, "12345", 5, NULL));
    g_assert_true (g_file_set_contents (nested_file, "123456789", 9, NULL));
    g_assert_cmpint (symlink (".", loop_path), ==, 0);

    location = g_file_new_for_path (tmp_directory);
    root = nautilus_disk_usage_scan (location, NULL, NULL, NULL, &error);
    g_assert_no_error (error);
    g_assert_nonnull (root);

    g_assert_cmpuint (nautilus_disk_usage_node_get_n_children (root), ==, 1);
    g_assert_cmpuint (nautilus_disk_usage_node_get_direct_file_count (root), ==, 2);
    g_assert_cmpuint (nautilus_disk_usage_node_get_file_count (root), ==, 4);
    g_assert_cmpuint (nautilus_disk_usage_node_get_directory_count (root), ==, 2);

    alpha = nautilus_disk_usage_node_get_child (root, 0);
    g_assert_cmpstr (nautilus_disk_usage_node_get_name (alpha), ==, "alpha");
    g_assert_cmpuint (nautilus_disk_usage_node_get_file_count (alpha), ==, 2);
    g_assert_cmpuint (nautilus_disk_usage_node_get_directory_count (alpha), ==, 1);
    g_assert_cmpuint (nautilus_disk_usage_node_get_size (root),
                      ==,
                      nautilus_disk_usage_node_get_direct_size (root) +
                      nautilus_disk_usage_node_get_size (alpha));

    g_assert_cmpint (g_remove (loop_path), ==, 0);
    g_assert_cmpint (g_remove (nested_file), ==, 0);
    g_assert_cmpint (g_remove (alpha_file), ==, 0);
    g_assert_cmpint (g_remove (root_file), ==, 0);
    g_assert_cmpint (g_rmdir (nested_path), ==, 0);
    g_assert_cmpint (g_rmdir (alpha_path), ==, 0);
    g_assert_cmpint (g_rmdir (tmp_directory), ==, 0);
}

static void
test_scan_honors_cancellation (void)
{
    g_autofree char *tmp_directory = g_dir_make_tmp ("nautilus-disk-usage-XXXXXX", NULL);
    g_autoptr (GFile) location = g_file_new_for_path (tmp_directory);
    g_autoptr (GCancellable) cancellable = g_cancellable_new ();
    g_autoptr (NautilusDiskUsageNode) root = NULL;
    g_autoptr (GError) error = NULL;

    g_cancellable_cancel (cancellable);
    root = nautilus_disk_usage_scan (location, cancellable, NULL, NULL, &error);

    g_assert_null (root);
    g_assert_error (error, G_IO_ERROR, G_IO_ERROR_CANCELLED);
    g_assert_cmpint (g_rmdir (tmp_directory), ==, 0);
}

int
main (int   argc,
      char *argv[])
{
    g_test_init (&argc, &argv, NULL);

    g_test_add_func ("/disk-usage/layout/proportional", test_layout_is_proportional);
    g_test_add_func ("/disk-usage/layout/zero-sizes", test_layout_handles_zero_sizes);
    g_test_add_func ("/disk-usage/scan/aggregates-directories", test_scan_aggregates_directories);
    g_test_add_func ("/disk-usage/scan/cancellation", test_scan_honors_cancellation);

    return g_test_run ();
}
