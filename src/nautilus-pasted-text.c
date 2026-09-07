/* nautilus-pasted-text.c
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "nautilus-pasted-text.h"

#include <gio/gio.h>
#include <string.h>

/* Only the first part of the text is inspected: the format is decided by the
 * head of a document, and clipboard pastes can be huge. */
#define SNIFF_LIMIT 16384

typedef struct
{
    const char *mime;
    const char *extension;
} MimeExtension;

/* Content types g_content_type_guess() may return for plain data. */
static const MimeExtension MIME_EXTENSIONS[] =
{
    { "text/x-python", "py" },
    { "text/x-python3", "py" },
    { "text/x-script.python", "py" },
    { "application/x-shellscript", "sh" },
    { "text/x-shellscript", "sh" },
    { "application/x-perl", "pl" },
    { "application/x-ruby", "rb" },
    { "application/javascript", "js" },
    { "text/javascript", "js" },
    { "text/html", "html" },
    { "application/xhtml+xml", "html" },
    { "image/svg+xml", "svg" },
    { "application/xml", "xml" },
    { "text/xml", "xml" },
    { "application/json", "json" },
    { "text/markdown", "md" },
    { "text/x-tex", "tex" },
    { "text/x-csrc", "c" },
    { "text/x-chdr", "h" },
    { "text/x-c++src", "cpp" },
    { "text/x-java", "java" },
    { "text/x-go", "go" },
    { "text/rust", "rs" },
    { "text/css", "css" },
    { "text/csv", "csv" },
    { "text/x-patch", "patch" },
    { "text/x-diff", "patch" },
    { "application/x-yaml", "yaml" },
    { "application/yaml", "yaml" },
    { "application/toml", "toml" },
    { "application/sql", "sql" },
    { "application/x-desktop", "desktop" },
    { "application/pgp-keys", "asc" },
};

typedef struct
{
    const char *extension;
    /* Regexes whose matches count towards this language, and how many hits
     * (lines) are needed before the guess is trusted. */
    const char *const patterns[8];
    int         threshold;
} Heuristic;

/* Order matters: the first heuristic to reach its threshold wins. More
 * specific syntaxes (LaTeX, diff, JSON) go before permissive ones
 * (Markdown, YAML). All patterns run in multiline mode against the head. */
static const Heuristic HEURISTICS[] =
{
    { "tex", { "^\\\\(documentclass|usepackage|begin\\{document\\}|section\\*?\\{|chapter\\{|item\\b|begin\\{equation)" }, 2 },
    { "patch", { "^diff --git |^--- [^\\s]|^\\+\\+\\+ [^\\s]|^@@ -\\d+,\\d+ \\+\\d+" }, 2 },
    { "html", { "^\\s*<!DOCTYPE html|<html\\b|<head\\b|<body\\b|</(div|p|span|table|ul|li|h[1-6])>" }, 2 },
    { "svg", { "<svg\\b" }, 1 },
    { "xml", { "^\\s*<\\?xml\\b" }, 1 },
    { "py", { "^\\s*(def|class) \\w+.*:\\s*$", "^\\s*(import \\w+|from \\w+(\\.\\w+)* import )", "^\\s*if __name__ == ['\"]__main__['\"]:", "^\\s*(elif|else|except|finally|with|for|while)\\b.*:\\s*$", "^\\s*(print|self)\\b" }, 2 },
    { "rs", { "^\\s*(pub )?fn \\w+\\s*[<(]", "^\\s*(let mut|let) \\w+", "^\\s*use std::|^\\s*use \\w+::", "^\\s*(impl|struct|enum|trait|mod) \\w+" }, 2 },
    { "go", { "^package \\w+", "^\\s*func (\\(\\w+ \\*?\\w+\\) )?\\w+\\(", "^import \\(|^import \"", ":= " }, 2 },
    { "sh", { "^\\s*(if|then|fi|else|elif|for|while|do|done|case|esac|export|local|echo|set -[eux]|source|function \\w+)\\b", "\\$\\(|\\$\\{\\w+\\}|\\|\\s*(grep|sed|awk|xargs|sort)\\b" }, 3 },
    { "ts", { "^\\s*(interface|type) \\w+.*[={]", ":\\s*(string|number|boolean|void|any)\\b", "^\\s*(export )?(const|let|function|class|async) \\w+.*[=(:{]", "=>" }, 3 },
    { "js", { "^\\s*(export )?(const|let|var|function|class|async function) \\w+", "^\\s*import .* from ['\"]|^\\s*(module\\.exports|require\\()", "console\\.\\w+\\(|=>|document\\.\\w+" }, 2 },
    { "css", { "^\\s*[.#@]?[\\w-]+(\\s*[,>~+]\\s*[.#]?[\\w-]+)*\\s*\\{\\s*$", "^\\s*[\\w-]+\\s*:\\s*[^;]+;\\s*$" }, 3 },
    { "c", { "^\\s*#include\\s*[<\"]", "^\\s*(int|void|char|static|struct|unsigned)\\b.*[;{(]", "^\\s*#define \\w+", "^\\s*return\\b.*;\\s*$" }, 2 },
    { "sql", { "^\\s*(SELECT|INSERT INTO|UPDATE|DELETE FROM|CREATE (TABLE|INDEX|VIEW)|ALTER TABLE|DROP TABLE|WITH \\w+ AS)\\b", "^\\s*(FROM|WHERE|JOIN|GROUP BY|ORDER BY|VALUES)\\b" }, 2 },
    { "json", { "^\\s*[\\[{]\\s*\"[^\"]+\"\\s*:.*[}\\]]\\s*$" }, 1 },
    { "json", { "^\\s*[\\[{]\\s*$", "^\\s*\\{\\s*\"[^\"]+\"\\s*:", "^\\s*\"[^\"]+\"\\s*:\\s*", "^\\s*[}\\]],?\\s*$" }, 3 },
    { "md", { "^#{1,6} \\S", "^\\s*[-*+] \\S", "^\\s*\\d+\\. \\S", "^```", "\\[[^\\]]+\\]\\([^)]+\\)", "\\*\\*[^*]+\\*\\*", "^> \\S", "^\\|.*\\|\\s*$" }, 2 },
    { "yaml", { "^---\\s*$", "^[\\w.-]+:\\s*(\\S.*)?$", "^\\s+[\\w.-]+:\\s+\\S", "^\\s*- [\\w.-]+:" }, 3 },
    { "ini", { "^\\[[\\w .-]+\\]\\s*$", "^\\s*[\\w.-]+\\s*=\\s*\\S" }, 3 },
};

static gboolean
starts_with_markdown_heading (const char *text)
{
    const char *eol;

    while (*text == '\n' || *text == '\r' || *text == ' ' || *text == '\t')
    {
        text++;
    }
    if (text[0] != '#')
    {
        return FALSE;
    }
    while (*text == '#')
    {
        text++;
    }
    if (*text != ' ')
    {
        return FALSE;
    }

    /* Markdown convention: the heading is followed by a blank line. A code
     * comment is usually followed directly by more code or comments. */
    eol = strchr (text, '\n');
    if (eol == NULL)
    {
        return TRUE;
    }
    eol++;
    while (*eol == ' ' || *eol == '\t' || *eol == '\r')
    {
        eol++;
    }
    return (*eol == '\n' || *eol == '\0');
}

static const char *
guess_from_content_type (const char *text,
                         gsize       length)
{
    gboolean uncertain = TRUE;
    g_autofree char *type = g_content_type_guess (NULL, (const guchar *) text, length, &uncertain);
    g_autofree char *mime = NULL;

    if (type == NULL || uncertain)
    {
        return NULL;
    }

    mime = g_content_type_get_mime_type (type);
    if (mime == NULL)
    {
        return NULL;
    }

    for (gsize i = 0; i < G_N_ELEMENTS (MIME_EXTENSIONS); i++)
    {
        if (g_ascii_strcasecmp (mime, MIME_EXTENSIONS[i].mime) == 0)
        {
            return MIME_EXTENSIONS[i].extension;
        }
    }

    return NULL;
}

static int
count_matches (const char *pattern,
               const char *text)
{
    g_autoptr (GRegex) regex = NULL;
    g_autoptr (GMatchInfo) info = NULL;
    int hits = 0;

    regex = g_regex_new (pattern, G_REGEX_MULTILINE, 0, NULL);
    if (regex == NULL)
    {
        g_warning ("nautilus-pasted-text: bad regex %s", pattern);
        return 0;
    }

    g_regex_match (regex, text, 0, &info);
    while (g_match_info_matches (info))
    {
        hits++;
        if (hits >= 8)
        {
            break;
        }
        g_match_info_next (info, NULL);
    }

    return hits;
}

static const char *
guess_from_heuristics (const char *text)
{
    /* SQL keywords are case-insensitive; everything else is case-sensitive to
     * avoid, e.g., prose starting with "For" counting as shell. */
    for (gsize i = 0; i < G_N_ELEMENTS (HEURISTICS); i++)
    {
        const Heuristic *h = &HEURISTICS[i];
        int hits = 0;

        for (gsize p = 0; p < G_N_ELEMENTS (h->patterns) && h->patterns[p] != NULL; p++)
        {
            if (g_strcmp0 (h->extension, "sql") == 0)
            {
                g_autofree char *pattern = g_strconcat ("(?i)", h->patterns[p], NULL);
                hits += count_matches (pattern, text);
            }
            else
            {
                hits += count_matches (h->patterns[p], text);
            }
        }

        if (hits >= h->threshold)
        {
            return h->extension;
        }
    }

    return NULL;
}

const char *
nautilus_pasted_text_guess_extension (const char *text,
                                      gssize      length)
{
    g_autofree char *head = NULL;
    const char *guess;

    if (text == NULL)
    {
        return "txt";
    }
    if (length < 0)
    {
        length = strlen (text);
    }

    /* Work on a NUL-terminated, valid-UTF-8 head so the regexes are safe. */
    head = g_strndup (text, MIN ((gsize) length, SNIFF_LIMIT));
    if (!g_utf8_validate (head, -1, NULL))
    {
        g_autofree char *clean = g_utf8_make_valid (head, -1);
        g_free (head);
        head = g_steal_pointer (&clean);
    }

    /* Shebangs and magic-based types first: the shared-mime database knows
     * "#!/usr/bin/env python", "<?xml", "%PDF-", "-----BEGIN PGP", ... */
    guess = guess_from_content_type (head, strlen (head));
    if (guess != NULL)
    {
        return guess;
    }

    guess = guess_from_heuristics (head);
    if (guess != NULL)
    {
        return guess;
    }

    /* Only once no language matched: a leading "# Title" line followed by a
     * blank line reads as a Markdown heading. Checked last on purpose, since
     * Python, shell, YAML, ... scripts often start with a "# comment" too. */
    if (starts_with_markdown_heading (head))
    {
        return "md";
    }

    return "txt";
}
