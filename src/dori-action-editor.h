/* dori-action-editor.h
 *
 * Modal dialog to create or edit a .nemo_action file with a form.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <adwaita.h>
#include <gio/gio.h>

#include "dori-action.h"

G_BEGIN_DECLS

#define DORI_TYPE_ACTION_EDITOR (dori_action_editor_get_type ())

G_DECLARE_FINAL_TYPE (DoriActionEditor, dori_action_editor, DORI, ACTION_EDITOR, AdwDialog)

/* Present the editor dialog. When @action is non-NULL, it is pre-loaded and
 * the file is saved back to the same .nemo_action; when NULL, a new action
 * is created in @actions_dir with a slugified filename derived from Name. */
void dori_action_editor_present (DoriAction *action,
                                 GFile      *actions_dir,
                                 GtkWidget  *parent);

G_END_DECLS
