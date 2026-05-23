/* nemo-action-editor.c
 *
 * Modal dialog to create or edit a .nemo_action file. Builds the form
 * programmatically with libadwaita rows so no extra resource is needed.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <config.h>

#include "nemo-action-editor.h"

#include <glib/gi18n.h>
#include <string.h>

#define ACTION_GROUP "Nemo Action"

struct _NemoActionEditor
{
    AdwDialog parent_instance;

    /* Where to write a freshly-created action. NULL when editing an existing
     * one — the existing file is reused. */
    GFile *actions_dir;
    /* Filename (basename) of the source file when editing. NULL when new. */
    char  *existing_id;

    AdwEntryRow *name_entry;
    AdwComboRow *type_combo;
    AdwEntryRow *exec_entry;
    AdwComboRow *selection_combo;
    AdwEntryRow *group_entry;
};

G_DEFINE_FINAL_TYPE (NemoActionEditor, nemo_action_editor, ADW_TYPE_DIALOG)

static const char * const TYPE_NICKS[] =
{
    "command",                  /* index 0 */
    "create-from-clipboard",    /* index 1 */
    "overwrite-from-clipboard", /* index 2 */
};

static const char * const SELECTION_NICKS[] =
{
    "Any",      /* index 0 */
    "None",     /* index 1 */
    "Single",   /* index 2 */
    "Multiple", /* index 3 */
};

static guint
type_nick_to_index (const char *nick)
{
    for (guint i = 0; i < G_N_ELEMENTS (TYPE_NICKS); i++)
    {
        if (g_strcmp0 (nick, TYPE_NICKS[i]) == 0)
        {
            return i;
        }
    }
    return 0;
}

static guint
selection_nick_to_index (const char *nick)
{
    for (guint i = 0; i < G_N_ELEMENTS (SELECTION_NICKS); i++)
    {
        if (g_ascii_strcasecmp (nick != NULL ? nick : "", SELECTION_NICKS[i]) == 0)
        {
            return i;
        }
    }
    return 0;
}

/* Turn an arbitrary user-supplied string into a safe lower-case file-name
 * stem. Non-alphanumeric runs collapse to a single dash; an empty result
 * falls back to "action". */
static char *
slugify (const char *text)
{
    g_autofree char *down = NULL;
    GString *out;
    gboolean last_dash;

    if (text == NULL || *text == '\0')
    {
        return g_strdup ("action");
    }

    down = g_utf8_strdown (text, -1);
    out = g_string_new (NULL);
    last_dash = TRUE; /* suppress leading dashes */

    for (const char *p = down; *p != '\0'; p = g_utf8_next_char (p))
    {
        gunichar c = g_utf8_get_char (p);

        if (g_unichar_isalnum (c))
        {
            char buf[8];
            int len = g_unichar_to_utf8 (c, buf);
            g_string_append_len (out, buf, len);
            last_dash = FALSE;
        }
        else if (!last_dash)
        {
            g_string_append_c (out, '-');
            last_dash = TRUE;
        }
    }

    if (out->len > 0 && out->str[out->len - 1] == '-')
    {
        g_string_truncate (out, out->len - 1);
    }
    if (out->len == 0)
    {
        g_string_append (out, "action");
    }

    return g_string_free (out, FALSE);
}

static char *
pick_filename (GFile      *actions_dir,
               const char *name)
{
    g_autofree char *slug = slugify (name);

    for (int i = 1; i < 1000; i++)
    {
        g_autofree char *candidate = (i == 1) ?
            g_strconcat (slug, ".nemo_action", NULL) :
            g_strdup_printf ("%s-%d.nemo_action", slug, i);
        g_autoptr (GFile) file = g_file_get_child (actions_dir, candidate);

        if (!g_file_query_exists (file, NULL))
        {
            return g_steal_pointer (&candidate);
        }
    }

    return g_strconcat (slug, ".nemo_action", NULL);
}

static void
on_save_clicked (GtkButton *button,
                 gpointer   user_data)
{
    NemoActionEditor *self = NEMO_ACTION_EDITOR (user_data);
    g_autoptr (GKeyFile) kf = g_key_file_new ();
    g_autofree char *filename = NULL;
    g_autoptr (GFile) target = NULL;
    g_autofree char *path = NULL;
    g_autoptr (GError) error = NULL;
    const char *name = gtk_editable_get_text (GTK_EDITABLE (self->name_entry));
    const char *exec = gtk_editable_get_text (GTK_EDITABLE (self->exec_entry));
    const char *group = gtk_editable_get_text (GTK_EDITABLE (self->group_entry));
    guint type_idx = adw_combo_row_get_selected (self->type_combo);
    guint sel_idx = adw_combo_row_get_selected (self->selection_combo);

    if (name == NULL || *name == '\0')
    {
        gtk_widget_grab_focus (GTK_WIDGET (self->name_entry));
        return;
    }
    if (type_idx == 0 && (exec == NULL || *exec == '\0'))
    {
        gtk_widget_grab_focus (GTK_WIDGET (self->exec_entry));
        return;
    }

    g_key_file_set_string (kf, ACTION_GROUP, "Name", name);
    if (type_idx != 0)
    {
        g_key_file_set_string (kf, ACTION_GROUP, "Type", TYPE_NICKS[type_idx]);
    }
    if (type_idx == 0)
    {
        g_key_file_set_string (kf, ACTION_GROUP, "Exec", exec);
    }
    g_key_file_set_string (kf, ACTION_GROUP, "Selection", SELECTION_NICKS[sel_idx]);
    if (group != NULL && *group != '\0')
    {
        g_key_file_set_string (kf, ACTION_GROUP, "Group", group);
    }

    if (self->existing_id != NULL)
    {
        filename = g_strdup (self->existing_id);
    }
    else
    {
        filename = pick_filename (self->actions_dir, name);
    }

    target = g_file_get_child (self->actions_dir, filename);
    path = g_file_get_path (target);
    if (path == NULL ||
        !g_key_file_save_to_file (kf, path, &error))
    {
        g_warning ("Nemo action editor: could not save “%s”: %s",
                   filename,
                   error != NULL ? error->message : "non-local actions directory");
        return;
    }

    adw_dialog_close (ADW_DIALOG (self));
}

static void
nemo_action_editor_init (NemoActionEditor *self)
{
    GtkWidget *toolbar;
    GtkWidget *header;
    GtkWidget *cancel_btn;
    GtkWidget *save_btn;
    GtkWidget *page;
    GtkWidget *basics_group;
    GtkWidget *action_group_widget;
    GtkWidget *visibility_group;
    GtkWidget *placement_group;
    g_autoptr (GtkStringList) type_model = NULL;
    g_autoptr (GtkStringList) selection_model = NULL;

    adw_dialog_set_title (ADW_DIALOG (self), _("Action"));
    adw_dialog_set_content_width (ADW_DIALOG (self), 520);
    adw_dialog_set_content_height (ADW_DIALOG (self), 640);

    toolbar = adw_toolbar_view_new ();

    header = adw_header_bar_new ();
    adw_header_bar_set_show_end_title_buttons (ADW_HEADER_BAR (header), FALSE);
    adw_header_bar_set_show_start_title_buttons (ADW_HEADER_BAR (header), FALSE);

    cancel_btn = gtk_button_new_with_mnemonic (_("_Cancel"));
    g_signal_connect_swapped (cancel_btn, "clicked",
                              G_CALLBACK (adw_dialog_close), self);
    adw_header_bar_pack_start (ADW_HEADER_BAR (header), cancel_btn);

    save_btn = gtk_button_new_with_mnemonic (_("_Save"));
    gtk_widget_add_css_class (save_btn, "suggested-action");
    g_signal_connect (save_btn, "clicked", G_CALLBACK (on_save_clicked), self);
    adw_header_bar_pack_end (ADW_HEADER_BAR (header), save_btn);

    adw_toolbar_view_add_top_bar (ADW_TOOLBAR_VIEW (toolbar), header);

    page = adw_preferences_page_new ();

    /* Basics ------------------------------------------------------------ */
    basics_group = adw_preferences_group_new ();
    adw_preferences_group_set_title (ADW_PREFERENCES_GROUP (basics_group), _("Name"));

    self->name_entry = ADW_ENTRY_ROW (adw_entry_row_new ());
    adw_preferences_row_set_title (ADW_PREFERENCES_ROW (self->name_entry),
                                   _("Menu label"));
    adw_preferences_group_add (ADW_PREFERENCES_GROUP (basics_group),
                               GTK_WIDGET (self->name_entry));
    adw_preferences_page_add (ADW_PREFERENCES_PAGE (page),
                              ADW_PREFERENCES_GROUP (basics_group));

    /* Action ------------------------------------------------------------ */
    action_group_widget = adw_preferences_group_new ();
    adw_preferences_group_set_title (ADW_PREFERENCES_GROUP (action_group_widget),
                                     _("Action"));
    adw_preferences_group_set_description (ADW_PREFERENCES_GROUP (action_group_widget),
        _("Exec placeholders: %F selected paths · %f first path · %U URIs · %P parent folder · %N basenames. Only used with Run Command."));

    type_model = gtk_string_list_new ((const char *[]) {
        _("Run Command"),
        _("Create file from clipboard"),
        _("Overwrite file with clipboard"),
        NULL,
    });
    self->type_combo = ADW_COMBO_ROW (adw_combo_row_new ());
    adw_preferences_row_set_title (ADW_PREFERENCES_ROW (self->type_combo), _("Type"));
    adw_combo_row_set_model (self->type_combo, G_LIST_MODEL (type_model));
    adw_preferences_group_add (ADW_PREFERENCES_GROUP (action_group_widget),
                               GTK_WIDGET (self->type_combo));

    self->exec_entry = ADW_ENTRY_ROW (adw_entry_row_new ());
    adw_preferences_row_set_title (ADW_PREFERENCES_ROW (self->exec_entry),
                                   _("Command"));
    adw_preferences_group_add (ADW_PREFERENCES_GROUP (action_group_widget),
                               GTK_WIDGET (self->exec_entry));
    adw_preferences_page_add (ADW_PREFERENCES_PAGE (page),
                              ADW_PREFERENCES_GROUP (action_group_widget));

    /* Visibility -------------------------------------------------------- */
    visibility_group = adw_preferences_group_new ();
    adw_preferences_group_set_title (ADW_PREFERENCES_GROUP (visibility_group),
                                     _("Visibility"));

    selection_model = gtk_string_list_new ((const char *[]) {
        _("Any selection (one or more files)"),
        _("Empty selection (background menu only)"),
        _("Exactly one file"),
        _("Two or more files"),
        NULL,
    });
    self->selection_combo = ADW_COMBO_ROW (adw_combo_row_new ());
    adw_preferences_row_set_title (ADW_PREFERENCES_ROW (self->selection_combo),
                                   _("Show when"));
    adw_combo_row_set_model (self->selection_combo, G_LIST_MODEL (selection_model));
    adw_preferences_group_add (ADW_PREFERENCES_GROUP (visibility_group),
                               GTK_WIDGET (self->selection_combo));
    adw_preferences_page_add (ADW_PREFERENCES_PAGE (page),
                              ADW_PREFERENCES_GROUP (visibility_group));

    /* Placement --------------------------------------------------------- */
    placement_group = adw_preferences_group_new ();
    adw_preferences_group_set_title (ADW_PREFERENCES_GROUP (placement_group),
                                     _("Placement"));

    self->group_entry = ADW_ENTRY_ROW (adw_entry_row_new ());
    adw_preferences_row_set_title (ADW_PREFERENCES_ROW (self->group_entry),
                                   _("Submenu / Group (optional)"));
    adw_preferences_group_add (ADW_PREFERENCES_GROUP (placement_group),
                               GTK_WIDGET (self->group_entry));
    adw_preferences_page_add (ADW_PREFERENCES_PAGE (page),
                              ADW_PREFERENCES_GROUP (placement_group));

    adw_toolbar_view_set_content (ADW_TOOLBAR_VIEW (toolbar), page);
    adw_dialog_set_child (ADW_DIALOG (self), toolbar);
}

static void
nemo_action_editor_finalize (GObject *object)
{
    NemoActionEditor *self = NEMO_ACTION_EDITOR (object);

    g_clear_object (&self->actions_dir);
    g_clear_pointer (&self->existing_id, g_free);

    G_OBJECT_CLASS (nemo_action_editor_parent_class)->finalize (object);
}

static void
nemo_action_editor_class_init (NemoActionEditorClass *klass)
{
    G_OBJECT_CLASS (klass)->finalize = nemo_action_editor_finalize;
}

void
nemo_action_editor_present (NemoAction *action,
                            GFile      *actions_dir,
                            GtkWidget  *parent)
{
    NemoActionEditor *self;

    g_return_if_fail (G_IS_FILE (actions_dir));

    self = g_object_new (NEMO_TYPE_ACTION_EDITOR, NULL);
    self->actions_dir = g_object_ref (actions_dir);

    if (action != NULL)
    {
        const char *name = nemo_action_get_name (action);
        const char *exec = nemo_action_get_exec (action);
        const char *group = nemo_action_get_group (action);

        self->existing_id = g_strdup (nemo_action_get_id (action));

        if (name != NULL)
        {
            gtk_editable_set_text (GTK_EDITABLE (self->name_entry), name);
        }
        if (exec != NULL)
        {
            gtk_editable_set_text (GTK_EDITABLE (self->exec_entry), exec);
        }
        if (group != NULL)
        {
            gtk_editable_set_text (GTK_EDITABLE (self->group_entry), group);
        }
        adw_combo_row_set_selected (self->type_combo,
            type_nick_to_index (nemo_action_get_type_string (action)));
        adw_combo_row_set_selected (self->selection_combo,
            selection_nick_to_index (nemo_action_get_selection_string (action)));
    }

    adw_dialog_present (ADW_DIALOG (self), parent);
}
