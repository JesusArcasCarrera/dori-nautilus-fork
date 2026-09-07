/* dori-action-manager.h
 *
 * Loads bundled context-menu actions and watches the user's custom actions in
 * ~/.local/share/nemo/actions (".nemo_action" files).
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <glib-object.h>

#include "dori-action.h"

G_BEGIN_DECLS

#define DORI_TYPE_ACTION_MANAGER (dori_action_manager_get_type ())

G_DECLARE_FINAL_TYPE (DoriActionManager, dori_action_manager, DORI, ACTION_MANAGER, GObject)

DoriActionManager *dori_action_manager_dup_singleton (void);

/* The loaded actions, sorted by group then position. Owned by the manager;
 * the list itself must not be modified or freed by the caller. */
GList             *dori_action_manager_get_actions   (DoriActionManager *self);

DoriAction        *dori_action_manager_get_action    (DoriActionManager *self,
                                                      const char        *id);

/* Whether the effective action with this ID comes from the user's actions
 * directory. Bundled actions overridden there count as user actions. */
gboolean           dori_action_manager_is_user_action (DoriActionManager *self,
                                                       const char        *id);

/* The folder where the manager looks for .nemo_action files. The returned
 * GFile is owned by the manager; don't free. */
GFile             *dori_action_manager_get_actions_dir (DoriActionManager *self);

/* Remove an action by id (the .nemo_action filename). Returns TRUE on success;
 * on failure, error is set. The "changed" signal fires asynchronously when
 * the file monitor notices the deletion. */
gboolean           dori_action_manager_delete_action   (DoriActionManager *self,
                                                        const char        *id,
                                                        GError           **error);

/* Emits "changed" whenever the actions on disk are added, removed or edited. */

G_END_DECLS
