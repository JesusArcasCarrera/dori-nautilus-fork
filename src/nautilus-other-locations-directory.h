/*
 * Copyright (C) 2026 The Nemo project contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */


#pragma once

#include "nautilus-directory.h"

G_BEGIN_DECLS

#define NAUTILUS_OTHER_LOCATIONS_DIRECTORY_PROVIDER_NAME "other-locations-directory-provider"

#define NAUTILUS_TYPE_OTHER_LOCATIONS_DIRECTORY (nautilus_other_locations_directory_get_type ())

G_DECLARE_FINAL_TYPE (NautilusOtherLocationsDirectory, nautilus_other_locations_directory, NAUTILUS, OTHER_LOCATIONS_DIRECTORY, NautilusDirectory);

NautilusOtherLocationsDirectory *nautilus_other_locations_directory_new (void);

G_END_DECLS
