/* dori-action.c
 *
 * A single user-defined context-menu action loaded from a ".nemo_action"
 * GKeyFile (group [Nemo Action]), compatible with the Cinnamon Nemo format.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <config.h>

#include "dori-action.h"
#include "nautilus-global-preferences.h"
#include "nautilus-pasted-text.h"
#include "nautilus-progress-info.h"

#include <errno.h>
#include <math.h>
#include <signal.h>
#include <string.h>
#include <unistd.h>
#include <glib/gi18n.h>
#include <glib/gstdio.h>
#include <adwaita.h>

#include "nautilus-file.h"
#include "nautilus-file-changes-queue.h"

#define ACTION_GROUP "Nemo Action"

typedef enum
{
    DORI_ACTION_TYPE_COMMAND,
    DORI_ACTION_TYPE_CREATE_FROM_CLIPBOARD,
    DORI_ACTION_TYPE_OVERWRITE_FROM_CLIPBOARD,
} DoriActionType;

typedef enum
{
    SELECTION_NONE,       /* exactly 0 selected files   */
    SELECTION_SINGLE,     /* exactly 1                  */
    SELECTION_MULTIPLE,   /* 2 or more                  */
    SELECTION_ANY,        /* 1 or more                  */
    SELECTION_COUNT,      /* exactly self->selection_count */
} SelectionKind;

struct _DoriAction
{
    GObject parent_instance;

    char *id;
    char *name;
    char *comment;
    char *icon_name;
    char *exec;
    char *group;
    char *section;
    int   position;
    DoriActionPlacement placement;

    /* Optional one-line prompt shown before running a command. The answer is
     * available as %p in Exec and as $DORI_PROMPT in the environment. */
    char *prompt;
    char *prompt_default;
    char *prompt_display_format;
    DoriActionPromptMode prompt_mode;

    DoriActionType type;

    SelectionKind selection_kind;
    int           selection_count;

    GStrv extensions;     /* NULL == match any */
    GStrv mimetypes;      /* NULL == match any */
    GStrv dependencies;   /* NULL == no required programs */
};

G_DEFINE_FINAL_TYPE (DoriAction, dori_action, G_TYPE_OBJECT)

static void
dori_action_finalize (GObject *object)
{
    DoriAction *self = DORI_ACTION (object);

    g_free (self->id);
    g_free (self->name);
    g_free (self->comment);
    g_free (self->icon_name);
    g_free (self->exec);
    g_free (self->group);
    g_free (self->section);
    g_free (self->prompt);
    g_free (self->prompt_default);
    g_free (self->prompt_display_format);
    g_strfreev (self->extensions);
    g_strfreev (self->mimetypes);
    g_strfreev (self->dependencies);

    G_OBJECT_CLASS (dori_action_parent_class)->finalize (object);
}

static void
dori_action_class_init (DoriActionClass *klass)
{
    G_OBJECT_CLASS (klass)->finalize = dori_action_finalize;
}

static void
dori_action_init (DoriAction *self)
{
    self->selection_kind = SELECTION_ANY;
    self->selection_count = -1;
    self->position = 0;
    self->prompt_mode = DORI_ACTION_PROMPT_ALWAYS;
}

static gboolean
prompt_display_format_is_valid (const char *format)
{
    guint substitutions = 0;

    if (format == NULL || *format == '\0')
    {
        return FALSE;
    }

    for (const char *character = format; *character != '\0'; character++)
    {
        if (*character != '%')
        {
            continue;
        }

        character++;
        if (*character == '%')
        {
            continue;
        }
        if (*character == 's')
        {
            substitutions++;
            continue;
        }

        return FALSE;
    }

    return substitutions == 1;
}

static char *
format_prompt_display_value (const char *format,
                             const char *value)
{
    GString *result = g_string_new (NULL);

    for (const char *character = format; *character != '\0'; character++)
    {
        if (*character != '%')
        {
            g_string_append_c (result, *character);
            continue;
        }

        character++;
        if (*character == '%')
        {
            g_string_append_c (result, '%');
        }
        else if (*character == 's')
        {
            g_string_append (result, value);
        }
    }

    return g_string_free (result, FALSE);
}

static void
parse_selection (DoriAction *self,
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

DoriAction *
dori_action_new (GFile *file)
{
    g_autoptr (GKeyFile) key_file = g_key_file_new ();
    g_autofree char *path = g_file_get_path (file);
    g_autoptr (GError) error = NULL;
    g_autofree char *exec = NULL;
    g_autofree char *name = NULL;
    g_autofree char *type_str = NULL;
    g_autofree char *selection = NULL;
    DoriActionType type = DORI_ACTION_TYPE_COMMAND;
    DoriAction *self;

    if (path == NULL ||
        !g_key_file_load_from_file (key_file, path, G_KEY_FILE_NONE, &error))
    {
        g_warning ("Dori action: cannot load %s: %s",
                   path != NULL ? path : "(non-local file)",
                   error != NULL ? error->message : "unknown error");
        return NULL;
    }

    if (!g_key_file_has_group (key_file, ACTION_GROUP))
    {
        g_warning ("Dori action: %s has no [%s] group", path, ACTION_GROUP);
        return NULL;
    }

    name = g_key_file_get_locale_string (key_file, ACTION_GROUP, "Name", NULL, NULL);
    exec = g_key_file_get_string (key_file, ACTION_GROUP, "Exec", NULL);
    type_str = g_key_file_get_string (key_file, ACTION_GROUP, "Type", NULL);

    if (g_strcmp0 (type_str, "create-from-clipboard") == 0)
    {
        type = DORI_ACTION_TYPE_CREATE_FROM_CLIPBOARD;
    }
    else if (g_strcmp0 (type_str, "overwrite-from-clipboard") == 0)
    {
        type = DORI_ACTION_TYPE_OVERWRITE_FROM_CLIPBOARD;
    }

    if (name == NULL)
    {
        g_warning ("Dori action: %s has no Name, ignoring", path);
        return NULL;
    }
    if (type == DORI_ACTION_TYPE_COMMAND && exec == NULL)
    {
        g_warning ("Dori action: %s has no Exec, ignoring", path);
        return NULL;
    }

    self = g_object_new (DORI_TYPE_ACTION, NULL);

    self->id = g_file_get_basename (file);
    self->name = g_steal_pointer (&name);
    self->exec = g_steal_pointer (&exec);
    self->type = type;
    self->comment = g_key_file_get_locale_string (key_file, ACTION_GROUP, "Comment", NULL, NULL);
    self->icon_name = g_key_file_get_string (key_file, ACTION_GROUP, "Icon-Name", NULL);
    self->group = g_key_file_get_locale_string (key_file, ACTION_GROUP, "Group", NULL, NULL);
    self->section = g_key_file_get_string (key_file, ACTION_GROUP, "Section", NULL);
    self->position = g_key_file_get_integer (key_file, ACTION_GROUP, "Position", NULL);
    {
        g_autofree char *placement = g_key_file_get_string (key_file, ACTION_GROUP, "Placement", NULL);

        self->placement = (placement != NULL && g_ascii_strcasecmp (placement, "open") == 0)
                          ? DORI_ACTION_PLACEMENT_OPEN
                          : DORI_ACTION_PLACEMENT_DEFAULT;
    }
    self->prompt = g_key_file_get_locale_string (key_file, ACTION_GROUP, "Prompt", NULL, NULL);
    self->prompt_default = g_key_file_get_locale_string (key_file, ACTION_GROUP, "Prompt-Default", NULL, NULL);
    self->prompt_display_format = g_key_file_get_locale_string (key_file, ACTION_GROUP,
                                                                 "Prompt-Display-Format", NULL, NULL);
    {
        g_autofree char *prompt_mode = g_key_file_get_string (key_file, ACTION_GROUP,
                                                              "Prompt-Mode", NULL);

        if (prompt_mode != NULL && g_ascii_strcasecmp (prompt_mode, "split") == 0)
        {
            self->prompt_mode = DORI_ACTION_PROMPT_SPLIT;
        }
    }
    if (self->prompt_display_format != NULL &&
        !prompt_display_format_is_valid (self->prompt_display_format))
    {
        g_warning ("Dori action: %s has an invalid Prompt-Display-Format; expected one %%s",
                   path);
        g_clear_pointer (&self->prompt_display_format, g_free);
    }
    if (self->prompt_mode == DORI_ACTION_PROMPT_SPLIT &&
        (self->prompt == NULL || *self->prompt == '\0' ||
         self->prompt_default == NULL || *self->prompt_default == '\0'))
    {
        g_warning ("Dori action: %s requests Prompt-Mode=split without Prompt and Prompt-Default; using always",
                   path);
        self->prompt_mode = DORI_ACTION_PROMPT_ALWAYS;
    }
    self->extensions = g_key_file_get_string_list (key_file, ACTION_GROUP, "Extensions", NULL, NULL);
    self->mimetypes = g_key_file_get_string_list (key_file, ACTION_GROUP, "Mimetypes", NULL, NULL);
    self->dependencies = g_key_file_get_string_list (key_file, ACTION_GROUP, "Dependencies", NULL, NULL);

    selection = g_key_file_get_string (key_file, ACTION_GROUP, "Selection", NULL);
    parse_selection (self, selection);

    return self;
}

const char *
dori_action_get_id (DoriAction *self)
{
    return self->id;
}

const char *
dori_action_get_name (DoriAction *self)
{
    return self->name;
}

const char *
dori_action_get_comment (DoriAction *self)
{
    return self->comment;
}

const char *
dori_action_get_icon_name (DoriAction *self)
{
    return self->icon_name;
}

const char *
dori_action_get_group (DoriAction *self)
{
    return self->group;
}

const char *
dori_action_get_section (DoriAction *self)
{
    return self->section;
}

int
dori_action_get_position (DoriAction *self)
{
    return self->position;
}

DoriActionPlacement
dori_action_get_placement (DoriAction *self)
{
    return self->placement;
}

const char *
dori_action_get_placement_string (DoriAction *self)
{
    return self->placement == DORI_ACTION_PLACEMENT_OPEN ? "open" : "default";
}

const char *
dori_action_get_exec (DoriAction *self)
{
    return self->exec;
}

const char *
dori_action_get_prompt (DoriAction *self)
{
    return self->prompt;
}

const char *
dori_action_get_prompt_default (DoriAction *self)
{
    return self->prompt_default;
}

const char *
dori_action_get_prompt_display_format (DoriAction *self)
{
    return self->prompt_display_format;
}

DoriActionPromptMode
dori_action_get_prompt_mode (DoriAction *self)
{
    return self->prompt_mode;
}

const char *
dori_action_get_prompt_mode_string (DoriAction *self)
{
    return self->prompt_mode == DORI_ACTION_PROMPT_SPLIT ? "split" : "always";
}

gboolean
dori_action_has_split_prompt (DoriAction *self)
{
    return self->type == DORI_ACTION_TYPE_COMMAND &&
           self->prompt_mode == DORI_ACTION_PROMPT_SPLIT;
}

char *
dori_action_dup_effective_prompt_default (DoriAction *self)
{
    g_autoptr (GVariant) overrides = NULL;
    const char *override = NULL;

    g_return_val_if_fail (DORI_IS_ACTION (self), NULL);

    if (nautilus_preferences != NULL)
    {
        overrides = g_settings_get_value (nautilus_preferences,
                                          NAUTILUS_PREFERENCES_ACTION_PROMPT_DEFAULTS);
        if (g_variant_lookup (overrides, self->id, "&s", &override))
        {
            return g_strdup (override);
        }
    }

    return g_strdup (self->prompt_default);
}

char *
dori_action_dup_display_name (DoriAction *self)
{
    g_autofree char *value = NULL;
    g_autofree char *display_value = NULL;

    g_return_val_if_fail (DORI_IS_ACTION (self), NULL);

    if (!dori_action_has_split_prompt (self))
    {
        return g_strdup (self->name);
    }

    value = dori_action_dup_effective_prompt_default (self);
    if (self->prompt_display_format != NULL)
    {
        display_value = format_prompt_display_value (self->prompt_display_format, value);
    }
    else
    {
        display_value = g_strdup (value);
    }

    return g_strdup_printf ("%s (%s)", self->name, display_value);
}

const char *
dori_action_get_type_string (DoriAction *self)
{
    switch (self->type)
    {
        case DORI_ACTION_TYPE_CREATE_FROM_CLIPBOARD:
            return "create-from-clipboard";
        case DORI_ACTION_TYPE_OVERWRITE_FROM_CLIPBOARD:
            return "overwrite-from-clipboard";
        case DORI_ACTION_TYPE_COMMAND:
        default:
            return "command";
    }
}

const char *
dori_action_get_selection_string (DoriAction *self)
{
    switch (self->selection_kind)
    {
        case SELECTION_NONE:
            return "None";
        case SELECTION_SINGLE:
            return "Single";
        case SELECTION_MULTIPLE:
            return "Multiple";
        case SELECTION_COUNT:
        case SELECTION_ANY:
        default:
            return "Any";
    }
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

/* Text-like files: any text subtype, anything the MIME database derives from
 * text/plain, and the usual structured formats that live under application/. */
static gboolean
file_is_text (NautilusFile *file)
{
    static const char *const text_like[] =
    {
        "application/json", "application/xml", "application/x-shellscript",
        "application/javascript", "application/x-yaml", "application/yaml",
        "application/toml", "application/x-desktop", "application/sql",
        "application/x-php", "application/x-perl", "application/x-ruby",
        "application/x-httpd-php", "application/xhtml+xml", "application/x-csh",
        NULL
    };
    const char *mime;

    if (file == NULL || nautilus_file_is_directory (file))
    {
        return FALSE;
    }

    mime = nautilus_file_get_mime_type (file);
    if (mime == NULL)
    {
        return FALSE;
    }
    if (g_str_has_prefix (mime, "text/") || g_content_type_is_a (mime, "text/plain"))
    {
        return TRUE;
    }
    for (guint i = 0; text_like[i] != NULL; i++)
    {
        if (g_content_type_is_a (mime, text_like[i]))
        {
            return TRUE;
        }
    }
    return FALSE;
}

gboolean
dori_action_is_visible (DoriAction *self,
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

    /* Overwriting only makes sense for text over a text file: never a folder,
     * an image or an archive. */
    if (self->type == DORI_ACTION_TYPE_OVERWRITE_FROM_CLIPBOARD &&
        (count != 1 || !file_is_text (selection->data)))
    {
        return FALSE;
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

/* Expand the Exec string, substituting the action placeholders and shell-quoting
 * every interpolated path so the result is safe to hand to "sh -c". */
static char *
build_command (DoriAction *self,
               GList      *selection,
               GFile      *parent_location,
               const char *prompt_value)
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

            case 'p':
            {
                g_autofree char *quoted = g_shell_quote (prompt_value != NULL ? prompt_value : "");
                g_string_append (cmd, quoted);
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

typedef struct
{
    DoriAction          *action;
    GSubprocess         *process;
    NautilusProgressInfo *progress;
    GCancellable        *cancellable;
    GFile               *destination;
    char                *progress_path;
    char                *last_progress_line;
    char                *progress_detail;
    GPid                 process_group;
    guint                tick_id;
    guint                force_cancel_id;
    gulong               pause_changed_id;
    gulong               cancelled_id;
    double               fraction;
    double               remaining_time;
    gboolean             has_fraction;
    gboolean             paused;
    gboolean             finished;
    gint                 refs;
} CommandRun;

static CommandRun *
command_run_ref (CommandRun *run)
{
    g_atomic_int_inc (&run->refs);
    return run;
}

static void
command_run_unref (CommandRun *run)
{
    if (!g_atomic_int_dec_and_test (&run->refs))
    {
        return;
    }

    if (run->pause_changed_id != 0)
    {
        g_signal_handler_disconnect (run->progress, run->pause_changed_id);
    }
    if (run->cancelled_id != 0)
    {
        g_cancellable_disconnect (run->cancellable, run->cancelled_id);
    }
    if (run->progress_path != NULL)
    {
        g_unlink (run->progress_path);
    }

    g_clear_object (&run->action);
    g_clear_object (&run->process);
    g_clear_object (&run->progress);
    g_clear_object (&run->cancellable);
    g_clear_object (&run->destination);
    g_free (run->progress_path);
    g_free (run->last_progress_line);
    g_free (run->progress_detail);
    g_free (run);
}

static void
create_process_group (gpointer user_data)
{
    setpgid (0, 0);
}

static void
signal_process_group (CommandRun *run,
                      int         signal_number)
{
    if (run->process_group <= 0)
    {
        return;
    }

    if (kill (-run->process_group, signal_number) != 0 && errno != ESRCH)
    {
        g_warning ("Dori action “%s”: could not signal process group: %s",
                   run->action->name, g_strerror (errno));
    }
}

static char *
format_action_time (guint seconds)
{
    if (seconds < 60)
    {
        return g_strdup_printf (ngettext ("%u second", "%u seconds", seconds), seconds);
    }

    return g_strdup_printf (_("%u min %02u s"), seconds / 60, seconds % 60);
}

/* Optional progress protocol for commands. If DORI_PROGRESS_FILE is present,
 * replace that file with one UTF-8 line containing:
 *
 *   current<TAB>total<TAB>remaining-seconds<TAB>details
 *
 * Invalid or partially-written updates are ignored. Commands which do not
 * implement the protocol retain an honest indeterminate activity bar. */
static void
read_command_progress (CommandRun *run)
{
    g_autofree char *contents = NULL;
    g_auto (GStrv) fields = NULL;
    char *end = NULL;
    double current;
    double total;
    double remaining = -1;

    if (run->progress_path == NULL ||
        !g_file_get_contents (run->progress_path, &contents, NULL, NULL) ||
        contents[0] == '\0')
    {
        return;
    }

    g_strchomp (contents);
    if (g_strcmp0 (contents, run->last_progress_line) == 0)
    {
        return;
    }

    fields = g_strsplit (contents, "\t", 4);
    if (g_strv_length (fields) < 2)
    {
        return;
    }

    current = g_ascii_strtod (fields[0], &end);
    if (end == fields[0] || *end != '\0' || !isfinite (current))
    {
        return;
    }
    total = g_ascii_strtod (fields[1], &end);
    if (end == fields[1] || *end != '\0' || !isfinite (total) || total <= 0)
    {
        return;
    }
    if (fields[2] != NULL && fields[2][0] != '\0')
    {
        remaining = g_ascii_strtod (fields[2], &end);
        if (end == fields[2] || *end != '\0' || !isfinite (remaining) || remaining < 0)
        {
            remaining = -1;
        }
    }

    g_free (run->last_progress_line);
    run->last_progress_line = g_strdup (contents);
    g_free (run->progress_detail);
    run->progress_detail = g_strdup (fields[3]);
    run->fraction = CLAMP (current / total, 0.0, 1.0);
    run->remaining_time = remaining;
    run->has_fraction = TRUE;

    nautilus_progress_info_set_progress (run->progress, run->fraction, 1.0);
    if (remaining >= 0)
    {
        nautilus_progress_info_set_remaining_time (run->progress, remaining);
    }
}

static gboolean
update_command_progress (gpointer user_data)
{
    CommandRun *run = user_data;
    guint elapsed;
    g_autofree char *elapsed_text = NULL;
    g_autofree char *remaining_text = NULL;
    g_autofree char *details = NULL;

    if (run->finished)
    {
        run->tick_id = 0;
        return G_SOURCE_REMOVE;
    }

    read_command_progress (run);
    elapsed = (guint) nautilus_progress_info_get_total_elapsed_time (run->progress);
    nautilus_progress_info_set_elapsed_time (run->progress, elapsed);
    elapsed_text = format_action_time (elapsed);

    if (!run->has_fraction)
    {
        nautilus_progress_info_pulse_progress (run->progress);
        details = g_strdup_printf (_("Running for %s"), elapsed_text);
    }
    else if (run->remaining_time >= 0)
    {
        remaining_text = format_action_time ((guint) ceil (run->remaining_time));
        details = g_strdup_printf (_("%d%% · about %s remaining%s%s"),
                                   (int) round (run->fraction * 100.0),
                                   remaining_text,
                                   run->progress_detail != NULL ? " · " : "",
                                   run->progress_detail != NULL ? run->progress_detail : "");
    }
    else
    {
        details = g_strdup_printf (_("%d%% · running for %s%s%s"),
                                   (int) round (run->fraction * 100.0),
                                   elapsed_text,
                                   run->progress_detail != NULL ? " · " : "",
                                   run->progress_detail != NULL ? run->progress_detail : "");
    }

    nautilus_progress_info_set_details (run->progress, details);

    return G_SOURCE_CONTINUE;
}

static gboolean
force_cancelled_command (gpointer user_data)
{
    CommandRun *run = user_data;

    run->force_cancel_id = 0;
    if (!run->finished)
    {
        signal_process_group (run, SIGKILL);
        g_subprocess_force_exit (run->process);
    }

    return G_SOURCE_REMOVE;
}

static void
on_command_cancelled (GCancellable *cancellable,
                      gpointer      user_data)
{
    CommandRun *run = user_data;

    if (run->finished || run->force_cancel_id != 0)
    {
        return;
    }

    if (run->paused)
    {
        signal_process_group (run, SIGCONT);
    }
    signal_process_group (run, SIGTERM);
    run->force_cancel_id = g_timeout_add_seconds_full (G_PRIORITY_DEFAULT, 2,
                                                       force_cancelled_command,
                                                       command_run_ref (run),
                                                       (GDestroyNotify) command_run_unref);
}

static void
on_command_progress_changed (NautilusProgressInfo *progress,
                             gpointer              user_data)
{
    CommandRun *run = user_data;
    gboolean paused;

    if (run->finished || g_cancellable_is_cancelled (run->cancellable))
    {
        return;
    }

    paused = nautilus_progress_info_get_is_user_paused (progress);
    if (paused == run->paused)
    {
        return;
    }

    signal_process_group (run, paused ? SIGSTOP : SIGCONT);
    run->paused = paused;
}

static void
on_command_finished (GObject      *source,
                     GAsyncResult *result,
                     gpointer      user_data)
{
    CommandRun *run = user_data;
    g_autoptr (GError) error = NULL;
    gboolean success;

    success = g_subprocess_wait_check_finish (G_SUBPROCESS (source), result, &error);
    run->finished = TRUE;

    if (run->tick_id != 0)
    {
        g_source_remove (run->tick_id);
        run->tick_id = 0;
    }
    if (run->force_cancel_id != 0)
    {
        g_source_remove (run->force_cancel_id);
        run->force_cancel_id = 0;
    }

    if (g_cancellable_is_cancelled (run->cancellable))
    {
        /* NautilusProgressInfo already changed its details to “Cancelled”. */
    }
    else if (success)
    {
        nautilus_progress_info_set_progress (run->progress, 1.0, 1.0);
        nautilus_progress_info_take_status (run->progress,
                                            g_strdup_printf (_("“%s” completed"), run->action->name),
                                            g_strdup (run->action->name));
        nautilus_progress_info_set_details (run->progress, _("Completed"));
    }
    else
    {
        nautilus_progress_info_take_status (run->progress,
                                            g_strdup_printf (_("“%s” failed"), run->action->name),
                                            g_strdup (run->action->name));
        nautilus_progress_info_set_details (run->progress,
                                            error != NULL ? error->message : _("Unknown error"));
        g_warning ("Dori action “%s” failed: %s",
                   run->action->name,
                   error != NULL ? error->message : "unknown error");
    }

    nautilus_progress_info_finish (run->progress);
    command_run_unref (run);
}

static void
run_command (DoriAction *self,
             GList      *selection,
             GFile      *parent_location,
             const char *prompt_value)
{
    g_autofree char *command = build_command (self, selection, parent_location, prompt_value);
    g_autofree char *workdir = (parent_location != NULL) ?
                               g_file_get_path (parent_location) : NULL;
    const char *argv[] = { "/bin/sh", "-c", command, NULL };
    g_auto (GStrv) envp = g_get_environ ();
    g_autoptr (GSubprocessLauncher) launcher = NULL;
    g_autoptr (GSubprocess) process = NULL;
    g_autoptr (GError) error = NULL;
    g_autofree char *progress_path = NULL;
    int progress_fd;
    CommandRun *run;
    const char *identifier;
    char *end = NULL;
    gint64 pid;

    if (self->prompt != NULL)
    {
        envp = g_environ_setenv (envp, "DORI_PROMPT",
                                 prompt_value != NULL ? prompt_value : "", TRUE);
    }

    progress_fd = g_file_open_tmp ("dori-action-progress-XXXXXX", &progress_path, &error);
    if (progress_fd >= 0)
    {
        close (progress_fd);
        envp = g_environ_setenv (envp, "DORI_PROGRESS_FILE", progress_path, TRUE);
    }
    else
    {
        g_warning ("Dori action “%s”: could not create progress channel: %s",
                   self->name, error->message);
        g_clear_error (&error);
    }

    launcher = g_subprocess_launcher_new (G_SUBPROCESS_FLAGS_STDOUT_SILENCE |
                                          G_SUBPROCESS_FLAGS_STDERR_SILENCE);
    if (workdir != NULL)
    {
        g_subprocess_launcher_set_cwd (launcher, workdir);
    }
    g_subprocess_launcher_set_environ (launcher, envp);
    g_subprocess_launcher_set_child_setup (launcher, create_process_group, NULL, NULL);

    process = g_subprocess_launcher_spawnv (launcher, argv, &error);
    if (process == NULL)
    {
        if (progress_path != NULL)
        {
            g_unlink (progress_path);
        }
        g_warning ("Dori action “%s”: could not run command: %s",
                   self->name, error->message);
        return;
    }

    run = g_new0 (CommandRun, 1);
    run->refs = 1;
    run->action = g_object_ref (self);
    run->process = g_steal_pointer (&process);
    run->progress = nautilus_progress_info_new ();
    run->cancellable = nautilus_progress_info_get_cancellable (run->progress);
    run->destination = parent_location != NULL ? g_object_ref (parent_location) : NULL;
    run->progress_path = g_steal_pointer (&progress_path);
    run->remaining_time = -1;

    identifier = g_subprocess_get_identifier (run->process);
    pid = identifier != NULL ? g_ascii_strtoll (identifier, &end, 10) : 0;
    if (identifier != NULL && end != identifier && *end == '\0' && pid > 0 && pid <= G_MAXINT)
    {
        run->process_group = (GPid) pid;
    }

    nautilus_progress_info_take_status (run->progress,
                                        g_strdup_printf (_("Running “%s”"), self->name),
                                        g_strdup (self->name));
    nautilus_progress_info_set_details (run->progress, _("Starting…"));
    nautilus_progress_info_pulse_progress (run->progress);
    if (run->destination != NULL)
    {
        nautilus_progress_info_set_destination (run->progress, run->destination);
    }

    run->pause_changed_id = g_signal_connect (run->progress, "changed",
                                              G_CALLBACK (on_command_progress_changed), run);
    run->cancelled_id = g_cancellable_connect (run->cancellable,
                                               G_CALLBACK (on_command_cancelled), run, NULL);
    nautilus_progress_info_start (run->progress);
    run->tick_id = g_timeout_add (500, update_command_progress, run);
    g_subprocess_wait_check_async (run->process, NULL, on_command_finished, run);
}

static void
save_prompt_default_override (DoriAction *self,
                              const char *value)
{
    g_autoptr (GVariant) current = NULL;
    g_autoptr (GVariant) updated = NULL;
    GVariantDict overrides;

    if (nautilus_preferences == NULL)
    {
        return;
    }

    current = g_settings_get_value (nautilus_preferences,
                                    NAUTILUS_PREFERENCES_ACTION_PROMPT_DEFAULTS);
    g_variant_dict_init (&overrides, current);
    if (g_strcmp0 (value, self->prompt_default) == 0)
    {
        g_variant_dict_remove (&overrides, self->id);
    }
    else
    {
        g_variant_dict_insert (&overrides, self->id, "s", value);
    }
    updated = g_variant_ref_sink (g_variant_dict_end (&overrides));

    if (!g_settings_set_value (nautilus_preferences,
                               NAUTILUS_PREFERENCES_ACTION_PROMPT_DEFAULTS,
                               updated))
    {
        g_warning ("Dori action “%s”: could not save the prompt default", self->name);
    }
}

typedef struct
{
    DoriAction *action;
    GList      *selection;        /* NautilusFile, owned refs */
    GFile      *parent_location;
    GtkWidget  *entry;
} PromptData;

static void
prompt_data_free (PromptData *data)
{
    g_clear_object (&data->action);
    nautilus_file_list_free (data->selection);
    g_clear_object (&data->parent_location);
    g_free (data);
}

static void
on_prompt_response (AdwAlertDialog *dialog,
                    const char     *response,
                    gpointer        user_data)
{
    PromptData *data = user_data;

    if (g_strcmp0 (response, "run") == 0 ||
        g_strcmp0 (response, "save-run") == 0)
    {
        const char *value = gtk_editable_get_text (GTK_EDITABLE (data->entry));

        if (g_strcmp0 (response, "save-run") == 0)
        {
            save_prompt_default_override (data->action, value);
        }
        run_command (data->action, data->selection, data->parent_location, value);
    }

    prompt_data_free (data);
}

/* Ask the user for the Prompt value in a small dialog, then run the command.
 * The selection list is owned by the caller, so it is copied for the async
 * round-trip. */
static void
run_command_with_prompt (DoriAction *self,
                         GList      *selection,
                         GFile      *parent_location,
                         GtkWidget  *widget)
{
    g_autofree char *body = NULL;
    AdwDialog *dialog;
    GtkWidget *entry = gtk_entry_new ();
    PromptData *data = g_new0 (PromptData, 1);
    g_autofree char *effective_default = NULL;

    if (dori_action_has_split_prompt (self))
    {
        body = g_strdup_printf ("%s\n%s", self->prompt,
                                _("Run uses the value once; Save makes it the new default and runs."));
    }
    else
    {
        body = g_strdup (self->prompt);
    }
    dialog = adw_alert_dialog_new (self->name, body);

    data->action = g_object_ref (self);
    data->selection = nautilus_file_list_copy (selection);
    data->parent_location = (parent_location != NULL) ? g_object_ref (parent_location) : NULL;
    data->entry = entry;

    effective_default = dori_action_dup_effective_prompt_default (self);
    if (effective_default != NULL)
    {
        gtk_editable_set_text (GTK_EDITABLE (entry), effective_default);
    }
    gtk_entry_set_activates_default (GTK_ENTRY (entry), TRUE);
    gtk_accessible_update_property (GTK_ACCESSIBLE (entry),
                                    GTK_ACCESSIBLE_PROPERTY_LABEL, self->prompt,
                                    -1);

    adw_alert_dialog_set_extra_child (ADW_ALERT_DIALOG (dialog), entry);
    if (dori_action_has_split_prompt (self))
    {
        adw_alert_dialog_add_responses (ADW_ALERT_DIALOG (dialog),
                                        "run", _("_Run"),
                                        "save-run", _("_Save"),
                                        NULL);
        adw_alert_dialog_set_prefer_wide_layout (ADW_ALERT_DIALOG (dialog), TRUE);
    }
    else
    {
        adw_alert_dialog_add_responses (ADW_ALERT_DIALOG (dialog),
                                        "cancel", _("_Cancel"),
                                        "run", _("_Run"),
                                        NULL);
    }
    adw_alert_dialog_set_response_appearance (ADW_ALERT_DIALOG (dialog), "run",
                                              ADW_RESPONSE_SUGGESTED);
    adw_alert_dialog_set_default_response (ADW_ALERT_DIALOG (dialog), "run");
    adw_alert_dialog_set_close_response (ADW_ALERT_DIALOG (dialog), "cancel");

    g_signal_connect (dialog, "response", G_CALLBACK (on_prompt_response), data);

    adw_dialog_present (dialog, widget);
    gtk_widget_grab_focus (entry);
}

typedef struct
{
    DoriActionType type;
    GFile         *target;      /* file to overwrite, or folder to create into */
    GdkClipboard  *clipboard;   /* kept so the confirmation can outlive the menu */
} ClipboardData;

static void
clipboard_data_free (ClipboardData *data)
{
    g_clear_object (&data->target);
    g_clear_object (&data->clipboard);
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
        g_warning ("Dori action: clipboard holds no text: %s",
                   error != NULL ? error->message : "(empty)");
        clipboard_data_free (data);
        return;
    }

    if (data->type == DORI_ACTION_TYPE_OVERWRITE_FROM_CLIPBOARD)
    {
        /* An overwrite cannot be undone, so the previous version goes to the
         * trash and a fresh file with the same name takes its place. Where
         * there is no trash (some removable media), fall back to a `name~`
         * backup so nothing is lost silently. */
        gboolean trashed = g_file_trash (data->target, NULL, &error);

        if (!trashed)
        {
            g_debug ("Dori action: could not trash before overwriting, keeping a backup: %s",
                     error->message);
            g_clear_error (&error);
        }
        if (!g_file_replace_contents (data->target, text, strlen (text), NULL,
                                      !trashed, G_FILE_CREATE_NONE, NULL, NULL, &error))
        {
            g_warning ("Dori action: could not overwrite file: %s", error->message);
        }
        nautilus_file_changes_queue_file_changed (data->target);
        nautilus_file_changes_consume_changes ();
    }
    else
    {
        /* Create a new, uniquely-named text file holding the clipboard text.
         * The base name is intentionally NOT translatable: file names should
         * not depend on the user's current locale. */
        const char *extension = nautilus_pasted_text_guess_extension (text, -1);

        for (int i = 1; i < 1000; i++)
        {
            g_autofree char *name = (i == 1) ?
                g_strdup_printf ("Pasted Text.%s", extension) :
                g_strdup_printf ("Pasted Text %d.%s", i, extension);
            g_autoptr (GFile) child = g_file_get_child (data->target, name);
            g_autoptr (GFileOutputStream) stream =
                g_file_create (child, G_FILE_CREATE_NONE, NULL, &error);

            if (stream != NULL)
            {
                if (!g_output_stream_write_all (G_OUTPUT_STREAM (stream),
                                                text, strlen (text),
                                                NULL, NULL, &error))
                {
                    g_warning ("Dori action: could not write file: %s",
                               error->message);
                }
                g_output_stream_close (G_OUTPUT_STREAM (stream), NULL, NULL);
                break;
            }
            if (!g_error_matches (error, G_IO_ERROR, G_IO_ERROR_EXISTS))
            {
                g_warning ("Dori action: could not create file: %s", error->message);
                break;
            }
            g_clear_error (&error);
        }
    }

    clipboard_data_free (data);
}

static void
on_overwrite_response (AdwAlertDialog *dialog,
                       const char     *response,
                       gpointer        user_data)
{
    ClipboardData *data = user_data;

    if (g_strcmp0 (response, "overwrite") != 0)
    {
        clipboard_data_free (data);
        return;
    }

    gdk_clipboard_read_text_async (data->clipboard, NULL, on_clipboard_text, data);
}

/* Destructive and not undoable in the strict sense: always ask first. */
static void
confirm_overwrite (ClipboardData *data,
                   NautilusFile  *file,
                   GtkWidget     *widget)
{
    const char *name = nautilus_file_get_display_name (file);
    g_autofree char *heading = g_strdup_printf (_("Overwrite “%s”?"), name);
    AdwAlertDialog *dialog;

    dialog = ADW_ALERT_DIALOG (adw_alert_dialog_new (heading, NULL));
    adw_alert_dialog_set_body (dialog,
                               _("Its content will be replaced with the text in the clipboard. "
                                 "The previous version is moved to the trash."));
    adw_alert_dialog_add_responses (dialog,
                                    "cancel", _("_Cancel"),
                                    "overwrite", _("_Overwrite"),
                                    NULL);
    adw_alert_dialog_set_response_appearance (dialog, "overwrite", ADW_RESPONSE_DESTRUCTIVE);
    adw_alert_dialog_set_default_response (dialog, "cancel");
    adw_alert_dialog_set_close_response (dialog, "cancel");
    g_signal_connect (dialog, "response", G_CALLBACK (on_overwrite_response), data);
    adw_dialog_present (ADW_DIALOG (dialog), widget);
}

void
dori_action_activate (DoriAction *self,
                      GList      *selection,
                      GFile      *parent_location,
                      GtkWidget  *widget)
{
    GdkClipboard *clipboard;
    ClipboardData *data;

    g_return_if_fail (DORI_IS_ACTION (self));

    if (self->type == DORI_ACTION_TYPE_COMMAND)
    {
        if (self->prompt != NULL)
        {
            run_command_with_prompt (self, selection, parent_location, widget);
        }
        else
        {
            run_command (self, selection, parent_location, NULL);
        }
        return;
    }

    /* Built-in clipboard actions: read the text asynchronously, then act. The
     * target must be captured now, since the selection list is owned by the
     * caller and may be freed before the async read completes. */
    data = g_new0 (ClipboardData, 1);
    data->type = self->type;

    if (self->type == DORI_ACTION_TYPE_OVERWRITE_FROM_CLIPBOARD)
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
    data->clipboard = g_object_ref (clipboard);

    if (self->type == DORI_ACTION_TYPE_OVERWRITE_FROM_CLIPBOARD)
    {
        confirm_overwrite (data, selection->data, widget);
        return;
    }

    gdk_clipboard_read_text_async (clipboard, NULL, on_clipboard_text, data);
}

void
dori_action_activate_default (DoriAction *self,
                              GList      *selection,
                              GFile      *parent_location,
                              GtkWidget  *widget)
{
    g_autofree char *value = NULL;

    g_return_if_fail (DORI_IS_ACTION (self));

    if (!dori_action_has_split_prompt (self))
    {
        dori_action_activate (self, selection, parent_location, widget);
        return;
    }

    value = dori_action_dup_effective_prompt_default (self);
    run_command (self, selection, parent_location, value);
}
