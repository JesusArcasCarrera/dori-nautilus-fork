/* nemo-action.c
 *
 * A single user-defined context-menu action loaded from a ".nemo_action"
 * GKeyFile (group [Nemo Action]), compatible with the Cinnamon Nemo format.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <config.h>

#include "nemo-action.h"

#include <string.h>
#include <glib/gi18n.h>

#include "nautilus-file.h"

#define ACTION_GROUP "Nemo Action"

typedef enum
{
    NEMO_ACTION_TYPE_COMMAND,
    NEMO_ACTION_TYPE_CREATE_FROM_CLIPBOARD,
    NEMO_ACTION_TYPE_OVERWRITE_FROM_CLIPBOARD,
} NemoActionType;

typedef enum
{
    SELECTION_NONE,       /* exactly 0 selected files   */
    SELECTION_SINGLE,     /* exactly 1                  */
    SELECTION_MULTIPLE,   /* 2 or more                  */
    SELECTION_ANY,        /* 1 or more                  */
    SELECTION_COUNT,      /* exactly self->selection_count */
} SelectionKind;

struct _NemoAction
{
    GObject parent_instance;

    char *id;
    char *name;
    char *comment;
    char *icon_name;
    char *exec;
    char *group;
    int   position;

    NemoActionType type;

    SelectionKind selection_kind;
    int           selection_count;

    GStrv extensions;     /* NULL == match any */
    GStrv mimetypes;      /* NULL == match any */
    GStrv dependencies;   /* NULL == no required programs */
};

G_DEFINE_FINAL_TYPE (NemoAction, nemo_action, G_TYPE_OBJECT)

static void
nemo_action_finalize (GObject *object)
{
    NemoAction *self = NEMO_ACTION (object);

    g_free (self->id);
    g_free (self->name);
    g_free (self->comment);
    g_free (self->icon_name);
    g_free (self->exec);
    g_free (self->group);
    g_strfreev (self->extensions);
    g_strfreev (self->mimetypes);
    g_strfreev (self->dependencies);

    G_OBJECT_CLASS (nemo_action_parent_class)->finalize (object);
}

static void
nemo_action_class_init (NemoActionClass *klass)
{
    G_OBJECT_CLASS (klass)->finalize = nemo_action_finalize;
}

static void
nemo_action_init (NemoAction *self)
{
    self->selection_kind = SELECTION_ANY;
    self->selection_count = -1;
    self->position = 0;
}

static void
parse_selection (NemoAction *self,
                 const char *value)
{
    char *end = NULL;
    gint64 number;

    if (value == NULL || *value == '\0' ||
        g_ascii_strcasecmp (value, "any") == 0 ||
        g_ascii_strcasecmp (value, "notnone") == 0)
    {
        self->selection_kind = SELECTION_ANY;
        return;
    }
    if (g_ascii_strcasecmp (value, "none") == 0)
    {
        self->selection_kind = SELECTION_NONE;
        return;
    }
    if (g_ascii_strcasecmp (value, "single") == 0 ||
        g_ascii_strcasecmp (value, "s") == 0)
    {
        self->selection_kind = SELECTION_SINGLE;
        return;
    }
    if (g_ascii_strcasecmp (value, "multiple") == 0 ||
        g_ascii_strcasecmp (value, "m") == 0)
    {
        self->selection_kind = SELECTION_MULTIPLE;
        return;
    }

    number = g_ascii_strtoll (value, &end, 10);
    if (end != value && number >= 0)
    {
        self->selection_kind = SELECTION_COUNT;
        self->selection_count = (int) number;
        return;
    }

    self->selection_kind = SELECTION_ANY;
}

NemoAction *
nemo_action_new (GFile *file)
{
    g_autoptr (GKeyFile) key_file = g_key_file_new ();
    g_autofree char *path = g_file_get_path (file);
    g_autoptr (GError) error = NULL;
    g_autofree char *exec = NULL;
    g_autofree char *name = NULL;
    g_autofree char *type_str = NULL;
    g_autofree char *selection = NULL;
    NemoActionType type = NEMO_ACTION_TYPE_COMMAND;
    NemoAction *self;

    if (path == NULL ||
        !g_key_file_load_from_file (key_file, path, G_KEY_FILE_NONE, &error))
    {
        g_warning ("Nemo action: cannot load %s: %s",
                   path != NULL ? path : "(non-local file)",
                   error != NULL ? error->message : "unknown error");
        return NULL;
    }

    if (!g_key_file_has_group (key_file, ACTION_GROUP))
    {
        g_warning ("Nemo action: %s has no [%s] group", path, ACTION_GROUP);
        return NULL;
    }

    name = g_key_file_get_locale_string (key_file, ACTION_GROUP, "Name", NULL, NULL);
    exec = g_key_file_get_string (key_file, ACTION_GROUP, "Exec", NULL);
    type_str = g_key_file_get_string (key_file, ACTION_GROUP, "Type", NULL);

    if (g_strcmp0 (type_str, "create-from-clipboard") == 0)
    {
        type = NEMO_ACTION_TYPE_CREATE_FROM_CLIPBOARD;
    }
    else if (g_strcmp0 (type_str, "overwrite-from-clipboard") == 0)
    {
        type = NEMO_ACTION_TYPE_OVERWRITE_FROM_CLIPBOARD;
    }

    if (name == NULL)
    {
        g_warning ("Nemo action: %s has no Name, ignoring", path);
        return NULL;
    }
    if (type == NEMO_ACTION_TYPE_COMMAND && exec == NULL)
    {
        g_warning ("Nemo action: %s has no Exec, ignoring", path);
        return NULL;
    }

    self = g_object_new (NEMO_TYPE_ACTION, NULL);

    self->id = g_file_get_basename (file);
    self->name = g_steal_pointer (&name);
    self->exec = g_steal_pointer (&exec);
    self->type = type;
    self->comment = g_key_file_get_locale_string (key_file, ACTION_GROUP, "Comment", NULL, NULL);
    self->icon_name = g_key_file_get_string (key_file, ACTION_GROUP, "Icon-Name", NULL);
    self->group = g_key_file_get_locale_string (key_file, ACTION_GROUP, "Group", NULL, NULL);
    self->position = g_key_file_get_integer (key_file, ACTION_GROUP, "Position", NULL);
    self->extensions = g_key_file_get_string_list (key_file, ACTION_GROUP, "Extensions", NULL, NULL);
    self->mimetypes = g_key_file_get_string_list (key_file, ACTION_GROUP, "Mimetypes", NULL, NULL);
    self->dependencies = g_key_file_get_string_list (key_file, ACTION_GROUP, "Dependencies", NULL, NULL);

    selection = g_key_file_get_string (key_file, ACTION_GROUP, "Selection", NULL);
    parse_selection (self, selection);

    return self;
}

const char *
nemo_action_get_id (NemoAction *self)
{
    return self->id;
}

const char *
nemo_action_get_name (NemoAction *self)
{
    return self->name;
}

const char *
nemo_action_get_comment (NemoAction *self)
{
    return self->comment;
}

const char *
nemo_action_get_icon_name (NemoAction *self)
{
    return self->icon_name;
}

const char *
nemo_action_get_group (NemoAction *self)
{
    return self->group;
}

int
nemo_action_get_position (NemoAction *self)
{
    return self->position;
}

static gboolean
file_matches_extensions (NautilusFile *file,
                         GStrv         extensions)
{
    if (extensions == NULL || extensions[0] == NULL)
    {
        return TRUE;
    }

    for (guint i = 0; extensions[i] != NULL; i++)
    {
        const char *ext = extensions[i];

        if (g_ascii_strcasecmp (ext, "any") == 0)
        {
            return TRUE;
        }
        if (g_ascii_strcasecmp (ext, "dir") == 0)
        {
            if (nautilus_file_is_directory (file))
            {
                return TRUE;
            }
            continue;
        }
        if (g_ascii_strcasecmp (ext, "nodirs") == 0 ||
            g_ascii_strcasecmp (ext, "none") == 0)
        {
            if (!nautilus_file_is_directory (file))
            {
                return TRUE;
            }
            continue;
        }

        g_autofree char *uri = nautilus_file_get_uri (file);
        g_autofree char *suffix = g_strconcat (".", ext, NULL);
        if (uri != NULL && g_str_has_suffix (uri, suffix))
        {
            return TRUE;
        }
    }

    return FALSE;
}

static gboolean
file_matches_mimetypes (NautilusFile *file,
                        GStrv         mimetypes)
{
    const char *mime;

    if (mimetypes == NULL || mimetypes[0] == NULL)
    {
        return TRUE;
    }

    mime = nautilus_file_get_mime_type (file);
    if (mime == NULL)
    {
        return FALSE;
    }

    for (guint i = 0; mimetypes[i] != NULL; i++)
    {
        const char *pattern = mimetypes[i];

        if (g_str_has_suffix (pattern, "/*"))
        {
            g_autofree char *prefix = g_strndup (pattern, strlen (pattern) - 1);
            if (g_str_has_prefix (mime, prefix))
            {
                return TRUE;
            }
        }
        else if (g_strcmp0 (pattern, mime) == 0)
        {
            return TRUE;
        }
    }

    return FALSE;
}

gboolean
nemo_action_is_visible (NemoAction *self,
                        GList      *selection)
{
    guint count = g_list_length (selection);

    switch (self->selection_kind)
    {
        case SELECTION_NONE:
            if (count != 0)
            {
                return FALSE;
            }
            break;

        case SELECTION_SINGLE:
            if (count != 1)
            {
                return FALSE;
            }
            break;

        case SELECTION_MULTIPLE:
            if (count < 2)
            {
                return FALSE;
            }
            break;

        case SELECTION_ANY:
            if (count == 0)
            {
                return FALSE;
            }
            break;

        case SELECTION_COUNT:
            if ((int) count != self->selection_count)
            {
                return FALSE;
            }
            break;
    }

    if (self->dependencies != NULL)
    {
        for (guint i = 0; self->dependencies[i] != NULL; i++)
        {
            g_autofree char *found = g_find_program_in_path (self->dependencies[i]);
            if (found == NULL)
            {
                return FALSE;
            }
        }
    }

    for (GList *l = selection; l != NULL; l = l->next)
    {
        if (!file_matches_extensions (l->data, self->extensions) ||
            !file_matches_mimetypes (l->data, self->mimetypes))
        {
            return FALSE;
        }
    }

    return TRUE;
}

static char *
quote_path_or_uri (NautilusFile *file,
                   gboolean      want_uri)
{
    g_autofree char *raw = NULL;

    if (want_uri)
    {
        raw = nautilus_file_get_uri (file);
    }
    else
    {
        g_autoptr (GFile) location = nautilus_file_get_location (file);
        raw = g_file_get_path (location);
    }

    return g_shell_quote (raw != NULL ? raw : "");
}

static void
append_quoted_list (GString  *out,
                    GList    *selection,
                    gboolean  want_uri)
{
    for (GList *l = selection; l != NULL; l = l->next)
    {
        g_autofree char *quoted = quote_path_or_uri (l->data, want_uri);

        if (l != selection)
        {
            g_string_append_c (out, ' ');
        }
        g_string_append (out, quoted);
    }
}

/* Expand the Exec string, substituting the Nemo placeholders and shell-quoting
 * every interpolated path so the result is safe to hand to "sh -c". */
static char *
build_command (NemoAction *self,
               GList      *selection,
               GFile      *parent_location)
{
    GString *cmd = g_string_new (NULL);

    for (const char *p = self->exec; *p != '\0'; p++)
    {
        if (*p != '%')
        {
            g_string_append_c (cmd, *p);
            continue;
        }

        p++;
        switch (*p)
        {
            case '\0':
                g_string_append_c (cmd, '%');
                p--;
                break;

            case '%':
                g_string_append_c (cmd, '%');
                break;

            case 'F':
                append_quoted_list (cmd, selection, FALSE);
                break;

            case 'U':
                append_quoted_list (cmd, selection, TRUE);
                break;

            case 'f':
                if (selection != NULL)
                {
                    g_autofree char *s = quote_path_or_uri (selection->data, FALSE);
                    g_string_append (cmd, s);
                }
                break;

            case 'u':
                if (selection != NULL)
                {
                    g_autofree char *s = quote_path_or_uri (selection->data, TRUE);
                    g_string_append (cmd, s);
                }
                break;

            case 'P':
            {
                g_autofree char *path = (parent_location != NULL) ?
                                        g_file_get_path (parent_location) : NULL;
                if (path != NULL)
                {
                    g_autofree char *quoted = g_shell_quote (path);
                    g_string_append (cmd, quoted);
                }
                break;
            }

            case 'N':
                for (GList *l = selection; l != NULL; l = l->next)
                {
                    g_autoptr (GFile) loc = nautilus_file_get_location (l->data);
                    g_autofree char *base = g_file_get_basename (loc);
                    g_autofree char *quoted = g_shell_quote (base != NULL ? base : "");

                    if (l != selection)
                    {
                        g_string_append_c (cmd, ' ');
                    }
                    g_string_append (cmd, quoted);
                }
                break;

            default:
                /* Unknown placeholder: leave it untouched. */
                g_string_append_c (cmd, '%');
                g_string_append_c (cmd, *p);
                break;
        }
    }

    return g_string_free (cmd, FALSE);
}

static void
run_command (NemoAction *self,
             GList      *selection,
             GFile      *parent_location)
{
    g_autofree char *command = build_command (self, selection, parent_location);
    g_autofree char *workdir = (parent_location != NULL) ?
                               g_file_get_path (parent_location) : NULL;
    const char *argv[] = { "/bin/sh", "-c", command, NULL };
    g_autoptr (GError) error = NULL;

    if (!g_spawn_async (workdir, (char **) argv, NULL,
                        G_SPAWN_SEARCH_PATH |
                        G_SPAWN_STDOUT_TO_DEV_NULL |
                        G_SPAWN_STDERR_TO_DEV_NULL,
                        NULL, NULL, NULL, &error))
    {
        g_warning ("Nemo action “%s”: could not run command: %s",
                   self->name, error->message);
    }
}

typedef struct
{
    NemoActionType type;
    GFile         *target;   /* file to overwrite, or folder to create into */
} ClipboardData;

static void
clipboard_data_free (ClipboardData *data)
{
    g_clear_object (&data->target);
    g_free (data);
}

static void
on_clipboard_text (GObject      *source,
                   GAsyncResult *result,
                   gpointer      user_data)
{
    GdkClipboard *clipboard = GDK_CLIPBOARD (source);
    ClipboardData *data = user_data;
    g_autoptr (GError) error = NULL;
    g_autofree char *text = gdk_clipboard_read_text_finish (clipboard, result, &error);

    if (text == NULL)
    {
        g_warning ("Nemo action: clipboard holds no text: %s",
                   error != NULL ? error->message : "(empty)");
        clipboard_data_free (data);
        return;
    }

    if (data->type == NEMO_ACTION_TYPE_OVERWRITE_FROM_CLIPBOARD)
    {
        if (!g_file_replace_contents (data->target, text, strlen (text), NULL,
                                      FALSE, G_FILE_CREATE_NONE, NULL, NULL, &error))
        {
            g_warning ("Nemo action: could not overwrite file: %s", error->message);
        }
    }
    else
    {
        /* Create a new, uniquely-named text file holding the clipboard text. */
        for (int i = 1; i < 1000; i++)
        {
            g_autofree char *name = (i == 1) ?
                g_strdup (_("Pasted Text.txt")) :
                g_strdup_printf (_("Pasted Text %d.txt"), i);
            g_autoptr (GFile) child = g_file_get_child (data->target, name);
            g_autoptr (GFileOutputStream) stream =
                g_file_create (child, G_FILE_CREATE_NONE, NULL, &error);

            if (stream != NULL)
            {
                if (!g_output_stream_write_all (G_OUTPUT_STREAM (stream),
                                                text, strlen (text),
                                                NULL, NULL, &error))
                {
                    g_warning ("Nemo action: could not write file: %s",
                               error->message);
                }
                g_output_stream_close (G_OUTPUT_STREAM (stream), NULL, NULL);
                break;
            }
            if (!g_error_matches (error, G_IO_ERROR, G_IO_ERROR_EXISTS))
            {
                g_warning ("Nemo action: could not create file: %s", error->message);
                break;
            }
            g_clear_error (&error);
        }
    }

    clipboard_data_free (data);
}

void
nemo_action_activate (NemoAction *self,
                      GList      *selection,
                      GFile      *parent_location,
                      GtkWidget  *widget)
{
    GdkClipboard *clipboard;
    ClipboardData *data;

    g_return_if_fail (NEMO_IS_ACTION (self));

    if (self->type == NEMO_ACTION_TYPE_COMMAND)
    {
        run_command (self, selection, parent_location);
        return;
    }

    /* Built-in clipboard actions: read the text asynchronously, then act. The
     * target must be captured now, since the selection list is owned by the
     * caller and may be freed before the async read completes. */
    data = g_new0 (ClipboardData, 1);
    data->type = self->type;

    if (self->type == NEMO_ACTION_TYPE_OVERWRITE_FROM_CLIPBOARD)
    {
        if (selection == NULL)
        {
            g_free (data);
            return;
        }
        data->target = nautilus_file_get_location (selection->data);
    }
    else
    {
        if (parent_location == NULL)
        {
            g_free (data);
            return;
        }
        data->target = g_object_ref (parent_location);
    }

    clipboard = gtk_widget_get_clipboard (widget);
    gdk_clipboard_read_text_async (clipboard, NULL, on_clipboard_text, data);
}
