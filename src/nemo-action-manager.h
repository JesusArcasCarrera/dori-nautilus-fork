/* nemo-action-manager.h
 *
 * Loads bundled context-menu actions and watches the user's custom actions in
 * ~/.local/share/nemo/actions (".nemo_action" files).
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <glib-object.h>

#include "nemo-action.h"

G_BEGIN_DECLS

#define NEMO_TYPE_ACTION_MANAGER (nemo_action_manager_get_type ())

G_DECLARE_FINAL_TYPE (NemoActionManager, nemo_action_manager, NEMO, ACTION_MANAGER, GObject)

NemoActionManager *nemo_action_manager_dup_singleton (void);

/* The loaded actions, sorted by group then position. Owned by the manager;
 * the list itself must not be modified or freed by the caller. */
GList             *nemo_action_manager_get_actions   (NemoActionManager *self);

NemoAction        *nemo_action_manager_get_action    (NemoActionManager *self,
                                                      const char        *id);

/* Whether the effective action with this ID comes from the user's actions
 * directory. Bundled actions overridden there count as user actions. */
gboolean           nemo_action_manager_is_user_action (NemoActionManager *self,
                                                       const char        *id);

/* The folder where the manager looks for .nemo_action files. The returned
 * GFile is owned by the manager; don't free. */
GFile             *nemo_action_manager_get_actions_dir (NemoActionManager *self);

/* Remove an action by id (the .nemo_action filename). Returns TRUE on success;
 * on failure, error is set. The "changed" signal fires asynchronously when
 * the file monitor notices the deletion. */
gboolean           nemo_action_manager_delete_action   (NemoActionManager *self,
                                                        const char        *id,
                                                        GError           **error);

/* Emits "changed" whenever the actions on disk are added, removed or edited. */

G_END_DECLS
