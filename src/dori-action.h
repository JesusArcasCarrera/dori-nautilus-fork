/* dori-action.h
 *
 * A single user-defined context-menu action, loaded from a GKeyFile in the
 * Cinnamon Nemo ".nemo_action" format (group [Nemo Action]).
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <gio/gio.h>
#include <gtk/gtk.h>

G_BEGIN_DECLS

#define DORI_TYPE_ACTION (dori_action_get_type ())

G_DECLARE_FINAL_TYPE (DoriAction, dori_action, DORI, ACTION, GObject)

/* Optional "Placement" key: which block of the context menu hosts the entry.
 *   default  the custom-actions block (own section, grouped into submenus)
 *   open     the "open with" block, next to "Open in Terminal" and other
 *            extension-provided openers */
typedef enum
{
    DORI_ACTION_PLACEMENT_DEFAULT,
    DORI_ACTION_PLACEMENT_OPEN,
} DoriActionPlacement;

typedef enum
{
    DORI_ACTION_PROMPT_ALWAYS,
    DORI_ACTION_PROMPT_SPLIT,
} DoriActionPromptMode;

DoriAction  *dori_action_new           (GFile      *file);

const char  *dori_action_get_id        (DoriAction *self);
const char  *dori_action_get_name      (DoriAction *self);
const char  *dori_action_get_comment   (DoriAction *self);
const char  *dori_action_get_icon_name (DoriAction *self);
const char  *dori_action_get_group     (DoriAction *self);
const char  *dori_action_get_section   (DoriAction *self);
int          dori_action_get_position  (DoriAction *self);
DoriActionPlacement dori_action_get_placement (DoriAction *self);
const char  *dori_action_get_exec      (DoriAction *self);

/* Optional "Prompt" key: a question shown in a small dialog before running the
 * command. The answer replaces %p in Exec (shell-quoted) and is exported as
 * $DORI_PROMPT. "Prompt-Default" pre-fills the entry. */
const char  *dori_action_get_prompt         (DoriAction *self);
const char  *dori_action_get_prompt_default (DoriAction *self);
const char  *dori_action_get_prompt_display_format (DoriAction *self);
DoriActionPromptMode dori_action_get_prompt_mode (DoriAction *self);
gboolean     dori_action_has_split_prompt   (DoriAction *self);
char        *dori_action_dup_effective_prompt_default (DoriAction *self);
char        *dori_action_dup_display_name   (DoriAction *self);

/* Round-trip strings used in the .nemo_action file format. Useful to pre-fill
 * an editor without re-parsing the file. */
const char  *dori_action_get_type_string      (DoriAction *self);
const char  *dori_action_get_selection_string (DoriAction *self);
const char  *dori_action_get_placement_string (DoriAction *self);
const char  *dori_action_get_prompt_mode_string (DoriAction *self);

/* Whether the action should be shown for the given selection (a GList of
 * NautilusFile). An empty selection is the background-menu case. */
gboolean     dori_action_is_visible    (DoriAction *self,
                                        GList      *selection);

/* Run the action against the given selection. parent_location is the folder
 * currently shown; widget is any widget in the view (used for clipboard
 * access). */
void         dori_action_activate      (DoriAction *self,
                                        GList      *selection,
                                        GFile      *parent_location,
                                        GtkWidget  *widget);
void         dori_action_activate_default (DoriAction *self,
                                           GList      *selection,
                                           GFile      *parent_location,
                                           GtkWidget  *widget);

G_END_DECLS
