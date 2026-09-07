/* SPDX-License-Identifier: GPL-3.0-or-later */

#include <config.h>

#include "dori-action.h"
#include "nautilus-global-preferences.h"

#include <glib/gstdio.h>

static DoriAction *
load_action (const char *contents)
{
    g_autofree char *directory = g_dir_make_tmp ("dori-action-test-XXXXXX", NULL);
    g_autofree char *path = g_build_filename (directory, "test.nemo_action", NULL);
    g_autoptr (GFile) file = NULL;
    DoriAction *action;

    g_assert_nonnull (directory);
    g_assert_true (g_file_set_contents (path, contents, -1, NULL));
    file = g_file_new_for_path (path);
    action = dori_action_new (file);

    g_assert_cmpint (g_unlink (path), ==, 0);
    g_assert_cmpint (g_rmdir (directory), ==, 0);

    return action;
}

static void
clear_prompt_defaults (void)
{
    g_settings_set_value (nautilus_preferences,
                          NAUTILUS_PREFERENCES_ACTION_PROMPT_DEFAULTS,
                          g_variant_new_array (G_VARIANT_TYPE ("{ss}"), NULL, 0));
}

static void
test_split_prompt_and_user_default (void)
{
    g_autoptr (DoriAction) action = NULL;
    g_autofree char *display_name = NULL;
    g_autofree char *effective_default = NULL;
    GVariantBuilder builder;

    clear_prompt_defaults ();
    action = load_action ("[Nemo Action]\n"
                          "Name=Extract frames\n"
                          "Exec=tool --value %p %F\n"
                          "Prompt=Frames per second\n"
                          "Prompt-Default=5\n"
                          "Prompt-Mode=split\n"
                          "Prompt-Display-Format=%s fps\n");

    g_assert_true (dori_action_has_split_prompt (action));
    g_assert_cmpstr (dori_action_get_prompt_mode_string (action), ==, "split");
    display_name = dori_action_dup_display_name (action);
    g_assert_cmpstr (display_name, ==, "Extract frames (5 fps)");
    g_clear_pointer (&display_name, g_free);

    g_variant_builder_init (&builder, G_VARIANT_TYPE ("a{ss}"));
    g_variant_builder_add (&builder, "{ss}", "test.nemo_action", "2.5");
    g_settings_set_value (nautilus_preferences,
                          NAUTILUS_PREFERENCES_ACTION_PROMPT_DEFAULTS,
                          g_variant_builder_end (&builder));

    effective_default = dori_action_dup_effective_prompt_default (action);
    g_assert_cmpstr (effective_default, ==, "2.5");
    display_name = dori_action_dup_display_name (action);
    g_assert_cmpstr (display_name, ==, "Extract frames (2.5 fps)");
}

static void
test_legacy_prompt_still_asks (void)
{
    g_autoptr (DoriAction) action = NULL;
    g_autofree char *display_name = NULL;

    clear_prompt_defaults ();
    action = load_action ("[Nemo Action]\n"
                          "Name=Rename output\n"
                          "Exec=tool --value %p %F\n"
                          "Prompt=Name\n"
                          "Prompt-Default=Output\n");

    g_assert_false (dori_action_has_split_prompt (action));
    g_assert_cmpstr (dori_action_get_prompt_mode_string (action), ==, "always");
    display_name = dori_action_dup_display_name (action);
    g_assert_cmpstr (display_name, ==, "Rename output");
}

static void
test_invalid_split_falls_back (void)
{
    g_autoptr (DoriAction) action = NULL;

    clear_prompt_defaults ();
    g_test_expect_message (G_LOG_DOMAIN, G_LOG_LEVEL_WARNING,
                           "*requests Prompt-Mode=split without Prompt and Prompt-Default*");
    action = load_action ("[Nemo Action]\n"
                          "Name=Broken\n"
                          "Exec=tool %F\n"
                          "Prompt=Value\n"
                          "Prompt-Mode=split\n");
    g_test_assert_expected_messages ();

    g_assert_false (dori_action_has_split_prompt (action));
}

static void
test_invalid_display_format_is_ignored (void)
{
    g_autoptr (DoriAction) action = NULL;
    g_autofree char *display_name = NULL;

    clear_prompt_defaults ();
    g_test_expect_message (G_LOG_DOMAIN, G_LOG_LEVEL_WARNING,
                           "*invalid Prompt-Display-Format*");
    action = load_action ("[Nemo Action]\n"
                          "Name=Action\n"
                          "Exec=tool %p %F\n"
                          "Prompt=Value\n"
                          "Prompt-Default=5\n"
                          "Prompt-Mode=split\n"
                          "Prompt-Display-Format=%n\n");
    g_test_assert_expected_messages ();

    display_name = dori_action_dup_display_name (action);
    g_assert_cmpstr (display_name, ==, "Action (5)");
}

static void
test_split_primary_runs_effective_default (void)
{
    g_autofree char *directory = g_dir_make_tmp ("dori-action-run-test-XXXXXX", NULL);
    g_autofree char *output_path = NULL;
    g_autofree char *quoted_output_path = NULL;
    g_autofree char *contents = NULL;
    g_autofree char *result = NULL;
    g_autoptr (DoriAction) action = NULL;
    g_autoptr (GFile) parent_location = NULL;
    gint64 deadline;

    g_assert_nonnull (directory);
    output_path = g_build_filename (directory, "prompt-value", NULL);
    quoted_output_path = g_shell_quote (output_path);
    contents = g_strdup_printf ("[Nemo Action]\n"
                                "Name=Extract frames\n"
                                "Exec=printf '%%s' %%p > %s\n"
                                "Prompt=Frames per second\n"
                                "Prompt-Default=5\n"
                                "Prompt-Mode=split\n",
                                quoted_output_path);

    clear_prompt_defaults ();
    action = load_action (contents);
    parent_location = g_file_new_for_path (directory);
    dori_action_activate_default (action, NULL, parent_location, NULL);

    deadline = g_get_monotonic_time () + (2 * G_TIME_SPAN_SECOND);
    while (!g_file_test (output_path, G_FILE_TEST_EXISTS) &&
           g_get_monotonic_time () < deadline)
    {
        g_main_context_iteration (NULL, FALSE);
        g_usleep (10 * 1000);
    }
    while (g_main_context_iteration (NULL, FALSE))
    {
    }

    g_assert_true (g_file_get_contents (output_path, &result, NULL, NULL));
    g_assert_cmpstr (result, ==, "5");
    g_assert_cmpint (g_unlink (output_path), ==, 0);
    g_assert_cmpint (g_rmdir (directory), ==, 0);
}

int
main (int   argc,
      char *argv[])
{
    g_test_init (&argc, &argv, NULL);
    nautilus_global_preferences_init ();

    g_test_add_func ("/dori-action/split-prompt/user-default",
                     test_split_prompt_and_user_default);
    g_test_add_func ("/dori-action/split-prompt/legacy",
                     test_legacy_prompt_still_asks);
    g_test_add_func ("/dori-action/split-prompt/invalid-mode",
                     test_invalid_split_falls_back);
    g_test_add_func ("/dori-action/split-prompt/invalid-format",
                     test_invalid_display_format_is_ignored);
    g_test_add_func ("/dori-action/split-prompt/primary-runs-default",
                     test_split_primary_runs_effective_default);

    return g_test_run ();
}
