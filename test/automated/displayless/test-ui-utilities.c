#include <glib.h>
#include <gtk/gtk.h>

#include <nautilus-ui-utilities.h>
#include <nautilus-resources.h>


static void
test_string_capitalization (void)
{
    char *capitalized;

    g_assert_null (nautilus_capitalize_str (NULL));

    capitalized = nautilus_capitalize_str ("");
    g_assert_cmpstr (capitalized, ==, "");
    g_free (capitalized);

    capitalized = nautilus_capitalize_str ("foo");
    g_assert_cmpstr (capitalized, ==, "Foo");
    g_free (capitalized);

    capitalized = nautilus_capitalize_str ("Foo");
    g_assert_cmpstr (capitalized, ==, "Foo");
    g_free (capitalized);
}

static guint
count_menu_action (GMenuModel *model,
                   const char *expected_action)
{
    guint count = 0;

    for (gint i = 0; i < g_menu_model_get_n_items (model); i++)
    {
        g_autofree char *action = NULL;

        if (g_menu_model_get_item_attribute (model, i,
                                             G_MENU_ATTRIBUTE_ACTION,
                                             "s", &action) &&
            g_str_equal (action, expected_action))
        {
            g_autofree char *label = NULL;
            g_autofree char *hidden_when = NULL;

            g_assert_true (g_menu_model_get_item_attribute (model, i,
                                                            G_MENU_ATTRIBUTE_LABEL,
                                                            "s", &label));
            g_assert_nonnull (label);
            g_assert_true (g_menu_model_get_item_attribute (model, i,
                                                            "hidden-when",
                                                            "s", &hidden_when));
            g_assert_cmpstr (hidden_when, ==, "action-disabled");
            count++;
        }

        g_autoptr (GMenuModel) section = g_menu_model_get_item_link (model, i,
                                                                     G_MENU_LINK_SECTION);
        g_autoptr (GMenuModel) submenu = g_menu_model_get_item_link (model, i,
                                                                     G_MENU_LINK_SUBMENU);

        if (section != NULL)
        {
            count += count_menu_action (section, expected_action);
        }
        if (submenu != NULL)
        {
            count += count_menu_action (submenu, expected_action);
        }
    }

    return count;
}

static GMenuModel *
find_submenu_by_label (GMenuModel *model,
                       const char *expected_label)
{
    for (gint i = 0; i < g_menu_model_get_n_items (model); i++)
    {
        g_autofree char *label = NULL;
        g_autoptr (GMenuModel) section = NULL;
        g_autoptr (GMenuModel) submenu = NULL;
        g_autoptr (GMenuModel) found = NULL;

        submenu = g_menu_model_get_item_link (model, i, G_MENU_LINK_SUBMENU);
        if (submenu != NULL &&
            g_menu_model_get_item_attribute (model, i,
                                             G_MENU_ATTRIBUTE_LABEL,
                                             "s", &label) &&
            g_str_equal (label, expected_label))
        {
            return g_steal_pointer (&submenu);
        }

        section = g_menu_model_get_item_link (model, i, G_MENU_LINK_SECTION);
        if (section != NULL)
        {
            found = find_submenu_by_label (section, expected_label);
        }
        if (found == NULL && submenu != NULL)
        {
            found = find_submenu_by_label (submenu, expected_label);
        }
        if (found != NULL)
        {
            return g_steal_pointer (&found);
        }
    }

    return NULL;
}

static void
test_link_target_location_menu_item (void)
{
    g_autoptr (GtkBuilder) builder = gtk_builder_new_from_resource (
        "/org/gnome/nautilus/menu/nautilus-files-view-context-menus.ui");
    GObject *selection_menu = gtk_builder_get_object (builder, "selection-menu");

    g_assert_true (G_IS_MENU_MODEL (selection_menu));
    g_assert_cmpuint (count_menu_action (G_MENU_MODEL (selection_menu),
                                        "view.open-link-target-location"),
                      ==, 1);
}

static void
test_flatten_items_are_in_clean_submenus (void)
{
    g_autoptr (GtkBuilder) builder = gtk_builder_new_from_resource (
        "/org/gnome/nautilus/menu/nautilus-files-view-context-menus.ui");
    GObject *background_menu = gtk_builder_get_object (builder, "background-menu");
    GObject *selection_menu = gtk_builder_get_object (builder, "selection-menu");
    g_autoptr (GMenuModel) background_clean = NULL;
    g_autoptr (GMenuModel) selection_clean = NULL;

    g_assert_true (G_IS_MENU_MODEL (background_menu));
    g_assert_true (G_IS_MENU_MODEL (selection_menu));

    background_clean = find_submenu_by_label (G_MENU_MODEL (background_menu), "_Clean");
    selection_clean = find_submenu_by_label (G_MENU_MODEL (selection_menu), "_Clean");
    g_assert_nonnull (background_clean);
    g_assert_nonnull (selection_clean);

    g_assert_cmpuint (count_menu_action (background_clean,
                                        "view.current-directory-flatten"),
                      ==, 1);
    g_assert_cmpuint (count_menu_action (G_MENU_MODEL (background_menu),
                                        "view.current-directory-flatten"),
                      ==, 1);
    g_assert_cmpuint (count_menu_action (selection_clean, "view.flatten"), ==, 1);
    g_assert_cmpuint (count_menu_action (G_MENU_MODEL (selection_menu),
                                        "view.flatten"),
                      ==, 1);
}

int
main (int   argc,
      char *argv[])
{
    g_test_init (&argc, &argv, NULL);
    g_test_set_nonfatal_assertions ();
    nautilus_register_resource ();

    g_test_add_func ("/string-capitalization",
                     test_string_capitalization);
    g_test_add_func ("/menu/link-target-location",
                     test_link_target_location_menu_item);
    g_test_add_func ("/menu/flatten-in-clean-submenus",
                     test_flatten_items_are_in_clean_submenus);

    int result = g_test_run ();

    nautilus_unregister_resource ();

    return result;
}
