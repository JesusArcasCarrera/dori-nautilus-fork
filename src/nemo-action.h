/* nemo-action.h
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

#define NEMO_TYPE_ACTION (nemo_action_get_type ())

G_DECLARE_FINAL_TYPE (NemoAction, nemo_action, NEMO, ACTION, GObject)

NemoAction  *nemo_action_new           (GFile      *file);

const char  *nemo_action_get_id        (NemoAction *self);
const char  *nemo_action_get_name      (NemoAction *self);
const char  *nemo_action_get_comment   (NemoAction *self);
const char  *nemo_action_get_icon_name (NemoAction *self);
const char  *nemo_action_get_group     (NemoAction *self);
int          nemo_action_get_position  (NemoAction *self);

/* Whether the action should be shown for the given selection (a GList of
 * NautilusFile). An empty selection is the background-menu case. */
gboolean     nemo_action_is_visible    (NemoAction *self,
                                        GList      *selection);

/* Run the action against the given selection. parent_location is the folder
 * currently shown; widget is any widget in the view (used for clipboard
 * access). */
void         nemo_action_activate      (NemoAction *self,
                                        GList      *selection,
                                        GFile      *parent_location,
                                        GtkWidget  *widget);

G_END_DECLS
