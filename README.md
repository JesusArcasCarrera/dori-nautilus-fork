# Dori — a personal Nautilus fork

> **Dori (Nautilus fork)** is my personal set of changes on top of
> [GNOME Files (Nautilus)](https://apps.gnome.org/Nautilus/), to make it behave
> the way I want on the desktop.

This is **not** a separate file manager or a rebrand. It keeps Nautilus' own
identity (binary `nautilus`, app id `org.gnome.Nautilus`) and is meant to be
**installed over GNOME Files**, so nothing in GNOME (Shell, file handlers,
search provider) breaks. "Dori" is just the name of this fork/repository.

## ⚠️ Personal project — no warranty, no support

This is a **personal fork for my own use**. It is **not affiliated with,
endorsed by, or supported by GNOME**. "GNOME", "Nautilus" and "GNOME Files"
are trademarks of their respective owners; this project reuses the
GPL-licensed *code*, not the branding.

Most of the code in this fork was **written by Claude (Anthropic's AI)** under
my direction. I provide **no warranty, no support, and no guarantee of
security or reliability** — use it entirely at your own risk. As with all GPL
software, it is distributed "as is" (see sections 15–16 of the GPL).

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
in. The fork can also install bundled actions under
`$XDG_DATA_DIRS/nautilus/actions/`; a personal action with the same filename
always takes precedence and only personal actions appear in the preferences
editor. The implementation is named `DoriAction` internally; Nemo remains in
the external filename, section and directory names solely for format
compatibility.

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

### Direct PDF, image and video operations

The fork ships type-aware submenus for common operations that should not
require opening a separate editor:

  - **PDF → Combine PDFs** joins two or more selected PDFs with `qpdf`. GTK
    does not retain the chronological order of selection clicks, so pages are
    combined in the stable visible order of the current view.
  - **Image** can rotate copies 90 degrees left/right or convert copies to PNG
    and JPEG with ImageMagick.
  - **Video → Convert to MKV without re-encoding** remuxes every selected video
    with FFmpeg, preserving its streams and quality.

Every operation writes to a temporary file in the destination folder and
renames it only after success. Originals are never overwritten; existing
output names receive a numeric suffix. An action is hidden automatically when
its dependency (`qpdf`, `magick` or `ffmpeg`) is unavailable.

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

### Other Locations as a Windows-style drive & network hub

Upstream removed the classic *Other Locations* page. This fork brings it back
behind `other-locations:///`, redesigned as a grid of Windows
"This PC"-style tiles. Each tile shows, next to its icon, the name and the
**real mount path**, and — for mounted local volumes — a thin usage bar with
*used / total* capacity (the bar/capacity layout is still being polished).
Unmounted volumes show their device node and no capacity. Tiles are grouped
into **On This Computer** and **Networks**, ordered mounted-first then
alphabetically; clicking one navigates to it. Local drives, volumes and mounts
come straight from `GVolumeMonitor`, so eject and usage stay live.

It is reachable from a sidebar entry with **its own visibility toggle** in
*Preferences → Sidebar → Built-in Items*, independent from the Network entry.
A *Preferences → Sidebar → Other Locations → **Show Network*** switch controls
whether the Networks section appears inside Other Locations — so you can hide
the standalone *Network* sidebar item and still reach networks here. When on,
the Networks section lists previously-connected servers (`NautilusRecentServers`),
`network:///` peers, and active remote mounts, and the window's *connect to
server* address bar is shown; when off, all of that is hidden.

### Editable XDG/places sidebar section

The user-folders block in the sidebar (Documents, Downloads, …) is an editable
list backed by the `sidebar-places` GSettings key: drag folders onto the
**Pin folder** affordance to add them, drag rows to **reorder** within the
section, and remove entries from the context menu. The *Desktop* built-in
entry is now controlled solely by its own toggle (instead of being gated
behind the shell's `gtk-shell-shows-desktop` hint, which is false on GNOME and
made the toggle do nothing).

### Interactive folder usage map

Right-click a single local folder — or the background of the current folder —
and choose **Disk Usage Map…** to open a proportional treemap of its contents.
Each rectangle represents the space used by a direct subfolder; files stored
directly in the current level are grouped into their own rectangle. Click a
folder to explore its children, or use the arrow keys and Enter; Backspace and
the header's back button return to the parent level.

Scanning runs outside the UI thread, reports live progress and can be cancelled
by closing the window. It measures allocated disk space, does not follow
symbolic links or cross into another filesystem, deduplicates hard links, and
shows a partial-results notice when some folders cannot be read.

## Building

```bash
git clone https://github.com/JesusArcasCarrera/dori-nautilus-fork.git
cd dori-nautilus-fork

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

`-Dextensions=false` skips the stock extensions that need `gexiv2-0.16` (not
yet packaged on Fedora 43 and not needed by anything in this fork).

Required system packages on Fedora 43:

```
sudo dnf builddep nautilus
sudo dnf install glycin-gtk4-devel tinysparql-devel
```

## Installing (replaces GNOME Files)

The fork keeps Nautilus' identity, so installing it **replaces** the system
GNOME Files. The simplest way is the bundled script — run it **without** sudo;
it builds as your user and asks for the password once for the system steps:

```bash
./install.sh
```

It builds into `_build_install`, installs into `/usr`, refreshes the
GLib/icon/desktop caches, pins the distro `nautilus` package so a `dnf upgrade`
won't revert it (`exclude=nautilus` in `/etc/dnf/dnf.conf`), and restarts the
running instance.

To undo: remove `exclude=nautilus` from `/etc/dnf/dnf.conf` and run
`sudo dnf reinstall nautilus`.

### Running uninstalled (for development)

Because the fork ships its own GSettings schema with new keys
(`sidebar-visible`, `type-to-action`, `sidebar-show-other-locations`,
`other-locations-show-network`, …), point the runtime at the built schema:

```bash
nautilus -q   # quit the system instance first (shared org.gnome.Nautilus id)
GSETTINGS_SCHEMA_DIR=$PWD/_build/data ./_build/src/nautilus
```

## Status / roadmap

Implemented:

  - Properties non-modal
  - Sidebar toggle on desktop + persistence
  - File operations: pause + dedicated window
  - Cut affordance overlay
  - Custom `.nemo_action` context-menu actions (core)
  - Configurable type-to-action
  - Other Locations Windows-style drive & network hub
  - Editable + reorderable XDG/places sidebar section
  - Interactive folder usage treemap

Planned:

  - **View cache** — when you re-enter a folder that hasn't changed, reuse
    the already-built view (scroll, selection) instead of rebuilding it
    from scratch.
  - **Async I/O hardening** — a sleeping HDD must not freeze navigation in
    tabs/views pointing at other devices.
  - **`.nemo_action` GUI editor** — create, edit, reorder and group
    actions without editing files by hand.
  - **Usage-bar polish** in the Other Locations tiles.

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
