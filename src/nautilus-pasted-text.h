/* nautilus-pasted-text.h
 *
 * Guess a sensible file extension for a chunk of text pasted from the
 * clipboard, so "Paste" on the folder background can create "Pasted text.py"
 * instead of a generic .txt.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <glib.h>

G_BEGIN_DECLS

/* Returns a static, lowercase extension without the dot ("py", "md", "txt",
 * ...). Never returns NULL; falls back to "txt". */
const char *nautilus_pasted_text_guess_extension (const char *text,
                                                  gssize      length);

G_END_DECLS
