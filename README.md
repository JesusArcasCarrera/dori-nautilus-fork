# Nemo

> Personal fork of [GNOME Files (Nautilus)](https://apps.gnome.org/Nautilus/)
> with desktop-oriented UX changes for users who don't want a mobile-first
> file manager.

## ⚠️ Name disclaimer

This is **not** the [Cinnamon Nemo](https://github.com/linuxmint/nemo) file
manager from Linux Mint. Both projects happen to share the name. They're
unrelated and have different goals.

## What's different from upstream Nautilus

Each item below is a single atomic commit on the `feat/desktop-ux-50` branch.

### Properties open as a non-modal, independent window

Right-click → *Properties* no longer opens a modal `AdwDialog` that blocks
the rest of the file manager. It opens as a regular top-level window: you can
move it, keep navigating, and open the properties of several items at once.

### Sidebar can actually be hidden in desktop layout

Upstream binds the `toggle-sidebar` action `enabled` ↔ `split-view:collapsed`,
so **F9 only worked in the narrow (mobile) layout**. Now F9 toggles the
sidebar in the desktop layout too, a sidebar toggle button is exposed in the
header bar, and the visibility persists across sessions through a new
`sidebar-visible` GSettings key under `org.gnome.nautilus.window-state`.

### File operations: pause/resume, expandable to a dedicated window

`NautilusProgressInfo` already exposed a `paused` flag but the copy/move
engine ignored it. A `GCond`-backed wait (`nautilus_progress_info_wait_if_paused`)
is now checked from `report_copy_progress()`, so pausing takes effect promptly
both between files and mid-file, and wakes on resume or cancel. Each
operation row gets a pause/resume button.

The default progress surface is still the compact popover (the upstream one),
but the popover has an **Open in Window** button — and double-clicking an
operation also opens it — that expands into a dedicated, resizable
**File Operations** window listing every running operation. Two surfaces, one
viewer-state lifecycle so finished operations aren't removed under the user.

### Cut affordance overlays the thumbnail instead of replacing it

When you cut a file, upstream hides the thumbnail entirely and paints a
scissors glyph in its place. This fork keeps the thumbnail visible (dimmed
via the icon widget's own opacity, so the filename label stays at full
opacity and readable) and overlays the dashed border and scissors on top.
You still see what you cut.

### Custom context-menu actions (`.nemo_action`)

A native system for user-defined entries in the context menu. Drop files into
`~/.local/share/nemo/actions/` and they appear in the right-click menu, live
(a `GFileMonitor` reloads on edit). The format is **compatible with Cinnamon
Nemo's `.nemo_action`** so existing actions from that ecosystem mostly drop
in.

Three action types are supported:

  - `command` (default) — runs `Exec` through `sh -c` in the current
    folder, with placeholders substituted and shell-quoted:
    `%F` (all paths), `%U` (all URIs), `%f` / `%u` (first), `%P` (parent
    folder), `%N` (basenames), `%%`.
  - `create-from-clipboard` — built-in, no external tools needed. Creates
    a new text file in the folder with the current clipboard text.
  - `overwrite-from-clipboard` — built-in. Replaces the selected file's
    contents with the clipboard text.

Visibility is filtered by `Selection`
(`None`/`Single`/`Multiple`/`Any` or a number), `Extensions` (incl.
`dir`/`nodirs`/`any`), `Mimetypes` (with `prefix/*` wildcards) and
`Dependencies` (programs that must be on `$PATH`, otherwise the entry hides).
Actions can be placed in `Group` (which becomes a submenu) and reordered
with `Position`. A GUI editor is planned (see Roadmap).

Example:

```ini
[Nemo Action]
Name=Open in VS Code
Exec=code %F
Selection=Any
Dependencies=code
Group=Editors
Position=10
Icon-Name=visual-studio-code
```

### Configurable type-to-action behaviour

Upstream forces typing in a folder to start a recursive search. This fork
adds a `type-to-action` GSettings enum with four modes, selectable from
**Preferences → General → When you Start Typing**:

  - **Filter This Folder** (default) — hides items in the current folder
    that don't start with the typed text (case-insensitive). `Backspace`
    pops a character, `Esc` clears.
  - **Jump to First Match** — typeahead-select: jumps to the first item
    whose name starts with the typed prefix. The prefix resets after 1 s
    of inactivity.
  - **Search Recursively** — the upstream GNOME behaviour. Subject to
    `recursive-search`.
  - **Do Nothing** — typing isn't intercepted. Use the search button or
    `Ctrl+F` / `Ctrl+Shift+F` to start a search explicitly.

In `filter` and `locate` modes the typed text is shown in the top banner
(`Filter: foo` / `Locate: foo`), and is cleared when you press `Esc` or
empty it via `Backspace`.

`Ctrl+F` (local search) and `Ctrl+Shift+F` (global search) keep working in
every mode.

## Building

```bash
git clone <this fork>
cd nautilus

# A meson wrap for blueprint-compiler ≥ 0.19 lives in
# subprojects/blueprint-compiler.wrap and is fetched automatically;
# the system one can stay at 0.18.

meson setup _build \
  -Dprefix=/usr \
  -Dtests=none \
  -Dintrospection=false \
  -Ddocs=false \
  -Dextensions=false      # avoids the gexiv2 >= 0.16 requirement
ninja -C _build
```

`-Dprefix=/usr` makes the binary use `/usr/share/locale` so it picks up the
system's translations (so the UI is localised even when running uninstalled).
`-Dextensions=false` skips the stock extensions that need `gexiv2-0.16` (not
yet packaged on Fedora 43 and not needed by anything in this fork).

Required system packages on Fedora 43:

```
sudo dnf builddep nautilus
sudo dnf install glycin-gtk4-devel tinysparql-devel
```

## Running

The binary is `_build/src/nemo`. Because this fork ships its own GSettings
schema with new keys (`sidebar-visible`, `type-to-action`), the runtime needs
to know where to find it. Either:

```bash
GSETTINGS_SCHEMA_DIR=$PWD/_build/data ./_build/src/nemo
```

or use `meson devenv -C _build src/nemo`, which sets up the same env.

**Don't `ninja install`** unless you understand the conflicts with your
distro's Nautilus package — the GResource paths and several internal IDs
still live under `org.gnome.nautilus`.

## Status / roadmap

Implemented:

  - Properties non-modal
  - Sidebar toggle on desktop + persistence
  - File operations: pause + dedicated window
  - Cut affordance overlay
  - Custom `.nemo_action` context-menu actions (core)
  - Configurable type-to-action
  - Fork rebrand (app ID `org.nemo.Files`, binary `nemo`)

Planned:

  - **View cache** — when you re-enter a folder that hasn't changed, reuse
    the already-built view (scroll, selection) instead of rebuilding it
    from scratch.
  - **Async I/O hardening** — a sleeping HDD must not freeze navigation in
    tabs/views pointing at other devices.
  - **`.nemo_action` GUI editor** — create, edit, reorder and group
    actions without editing files by hand.

## License

Inherits **GPL-3.0-or-later** from upstream Nautilus. See `LICENSE`.

Original copyright headers (Red Hat, Eazel, The GNOME project contributors,
and many others) are preserved in every file. Files modified by this fork
carry the original headers; the full history of changes is in the git log
of the `feat/desktop-ux-50` branch.

## Credits

This fork would not exist without the years of work poured into GNOME Files
by Alexander Larsson, the GNOME design team, and hundreds of upstream
contributors. Source: <https://gitlab.gnome.org/GNOME/nautilus>.

## Reporting issues

Issues should be reported to **this fork's tracker**, not to upstream
GNOME — the changes here are downstream-only and the upstream maintainers
shouldn't have to triage them. If you can reproduce the issue on the
official Files Flatpak, do report it upstream instead.
