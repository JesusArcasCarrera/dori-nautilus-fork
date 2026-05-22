/* nemo-action-manager.h
 *
 * Loads and watches the user's custom context-menu actions from the
 * ~/.local/share/nemo/actions directory (".nemo_action" files).
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

/* Emits "changed" whenever the actions on disk are added, removed or edited. */

G_END_DECLS
