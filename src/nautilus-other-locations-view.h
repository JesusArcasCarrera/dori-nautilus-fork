/*
 * Copyright (C) 2026 The Nemo project contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include "nautilus-list-base.h"

G_BEGIN_DECLS

#define NAUTILUS_TYPE_OTHER_LOCATIONS_VIEW (nautilus_other_locations_view_get_type())

G_DECLARE_FINAL_TYPE (NautilusOtherLocationsView, nautilus_other_locations_view, NAUTILUS, OTHER_LOCATIONS_VIEW, NautilusListBase)

NautilusOtherLocationsView *nautilus_other_locations_view_new (void);

G_END_DECLS
