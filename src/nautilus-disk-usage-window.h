/*
 * Copyright © 2026 The Dori authors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <adwaita.h>
#include <gio/gio.h>

G_BEGIN_DECLS

void nautilus_disk_usage_window_present (GFile     *location,
                                         GtkWindow *parent);

G_END_DECLS
