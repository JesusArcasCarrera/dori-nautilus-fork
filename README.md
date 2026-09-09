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

### Paste text or images as new files

"Paste" on the folder background (and Ctrl+V) is never greyed out just because
the clipboard holds no files. Text becomes a new file named after what it looks
like — `Pasted text.py`, `.md`, `.tex`, `.json`, `.sh`, `.html`, `.sql`, … and
plain `.txt` when nothing matches — using the shared-mime magic first
(shebangs, XML, PDF…) and a small set of syntax heuristics
(`src/nautilus-pasted-text.c`). Images are saved as `Pasted image.png`. The
file is created through the regular file-operations job, so it gets a unique
name on collision, undo support, and is revealed in the view.

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
    a new file in the folder with the current clipboard text (same type
    detection as the default Paste; kept for Nemo compatibility).
  - `overwrite-from-clipboard` — built-in. Replaces the selected file's
    content with the clipboard text. Only offered for a single text-like file
    (text/*, JSON, XML, shell, YAML…) when the clipboard holds text; always
    asks for confirmation, and the previous version goes to the trash (or to a
    `name~` backup where there is no trash) so it can be recovered.

An action can ask a question first with `Prompt` (and `Prompt-Default`):
Nautilus shows a small dialog with an entry before running, and the answer is
available as `%p` in `Exec` (shell-quoted) and as `$DORI_PROMPT`.

Parameterized actions can opt into a split menu row with
`Prompt-Mode=split`. Its main zone runs immediately with the effective default;
the smaller arrow opens the input dialog, where the value can be used once or
saved as the new per-user default. Menu labels show only the action name; the
current default is shown in the input dialog. Its header close button and Escape
dismiss it without saving or running. `Prompt-Display-Format` remains accepted
for compatibility, but does not append parameters to menu labels.
Missing or invalid split parameters fall back to the legacy ask-every-time
behavior. User overrides live in GSettings and never rewrite installed action
files.

Every command action is tracked by Nautilus' native operations indicator. A
command with no progress integration gets an indeterminate activity bar plus
elapsed time; cancelling or pausing the operation controls its complete
process group. Commands which can measure their work may publish determinate
progress by atomically replacing the file named by `$DORI_PROGRESS_FILE` with
one UTF-8 TSV line:

```text
current<TAB>total<TAB>remaining-seconds<TAB>details
```

The remaining-time and details fields may be empty. Invalid or partially
written updates are ignored, so a command always falls back safely to the
indeterminate display.

Visibility is filtered by `Selection`
(`None`/`Single`/`Multiple`/`Any` or a number), `Extensions` (incl.
`dir`/`nodirs`/`any`), `Mimetypes` (with `prefix/*` wildcards) and
`Dependencies` (programs that must be on `$PATH`, otherwise the entry hides).
Actions can be placed in `Group` (which becomes a submenu) and reordered
with `Position`. The optional, invisible `Section` identifier inserts a native
separator whenever it changes between consecutive visible actions in the same
group, so dynamic menus remain grouped without relying on position heuristics.
`Placement=open` moves an action (or its whole `Group`
submenu) out of the custom-actions block and into the "open with" block, next
to "Open in Terminal" and other openers contributed by extensions — handy for
"Open in VS Code"-style entries. Actions can be created and edited from
Preferences → Custom Actions.

Example:

```ini
[Nemo Action]
Name=Open in VS Code
Exec=code %F
Selection=Any
Dependencies=code
Group=Editors
Section=open
Position=10
Icon-Name=visual-studio-code
```

### Direct PDF, image and video operations

Architecture decision (2026-09-07): `.nemo_action` files remain Dori's
connector to commands. Dori keeps its loader, editor, parameter dialogs,
selection order and operation tracking, and ships the action definitions and
`nautilus-media-tools` helper below. Commands may also come from independent
projects; using them does not require a central execution service.

The separate `acciones` project is deferred pending a focused design for an
application-queryable index of capabilities. Do not move this catalog or its
implementations there as part of Dori work. If that service is adopted later,
an action can invoke it through `Exec` while retaining its `.nemo_action`
identity and Dori integration. The former `nautilus-acciones.py` extension is
not the integration path.

The fork ships type-aware submenus for common operations that should not
require opening a separate editor:

  - **PDF → Combine PDFs** joins two or more selected PDFs with `qpdf`, in
    the order the files were selected. GTK's selection model forgets click
    order, so the view tracks it itself (`selection_order`) and every custom
    action receives `%F` in that order; a range or Select All adds its files
    in visible order.
  - **Image** can rotate copies 90 degrees left/right, convert copies to PNG
    and JPEG, or arrange two or more selected images in horizontal columns
    inside a 1920×1080 JPEG with ImageMagick.
  - **Video** can remux to MKV, join compatible videos in selection order into
    a chaptered MKV without re-encoding, re-encode to a compatible MP4 (H.264
    + AAC, NVENC when available), extract five PNG frames per second, save a
    single frame (the 100th by default, asked through `Prompt`), build a 5×4
    contact sheet, create an animated GIF, create a ten-play MKV loop without
    re-encoding, or arrange two or more selected videos in horizontal columns
    inside a 1920×1080 MP4. Video compositions normalize dimensions, aspect
    ratio, timestamps and frame rate, and repeat shorter clips until the
    longest one ends. Frame extraction, GIF and looping require exactly one
    selected video.
  - **Image** also converts one or more images to a PDF (one page each, in
    selection order), recognises text with tesseract into a `.txt` and the
    clipboard, and copies images without EXIF/GPS metadata (exiftool).
  - **PDF → Make searchable (OCR)** adds a text layer with `ocrmypdf`
    (`pip install ocrmypdf`; the entry hides until it is installed).
  - **Documents** converts office files (ODF, OOXML, legacy MS Office, RTF) to
    PDF with headless LibreOffice.

The fork deliberately does **not** reimplement heavy processing. Where a local
CLI already exists it is simply wired into the menu:

  - **Video → Transcribe to subtitles (subs)** runs the `subs` command
    (faster-whisper on GPU plus NLLB translation) and reports the SRT files it
    left next to the media.
  - **Documents → Narrate to MP3 (voz)** runs `voz leer` (the local TTS) on a
    PDF, TXT or Markdown file.

Both entries appear only when the corresponding command is on `$PATH`.

Every operation writes to a temporary file in the destination folder and
renames it only after success. Originals are never overwritten; existing
output names receive a numeric suffix. An action is hidden automatically when
its dependency (`qpdf`, `magick`, `ffmpeg`, `tesseract`, `exiftool`,
`soffice`, `ocrmypdf`, `subs`, `voz`) is unavailable.

For a single local video, **Properties** reads the container asynchronously
with `ffprobe` and lists every audio and subtitle track, including language,
track title, codec, channel layout and default/forced disposition when present.
The dialog remains responsive while this information loads, and the section is
simply omitted when `ffprobe` is unavailable.

### Compress and extract with Estiba when available

A new `estiba` meson feature (auto-detected through the `estiba-0.1`
pkg-config module) routes *Compress…* and *Extract Here* through
`libmarea_archive` — 7z multithreaded, libarchive, unrar, zstd, brotli —
instead of gnome-autoar, which is single-threaded (144 s vs 24 s on a
325 MB `.7z` with 20 cores). The existing dialog, progress texts, undo,
passphrase prompt and destination rules are kept; without the library the
autoar path is untouched. Estiba lives in the
[marea](https://github.com/JesusArcasCarrera/marea) repository.

### Folder tools: flatten, clean, group

Right-click a folder → **Clean** submenu:

  - **Flatten Folder Contents** moves regular files from all subfolders into
    the folder. Existing files are never overwritten, folders and symlinks
    are kept, and folders with version-control metadata are refused.
  - **Clean Empty Folders** removes nested empty folders, keeping the
    selected one.
  - **Group Files** moves the files directly inside the folder into
    category folders (images, videos, audio, documents, archives, other).
  - **Group Duplicates** (this level or including subfolders) compares files
    by SHA-256 and moves extra copies to a `duplicados` folder, keeping one.

Each tool confirms first, runs as a tracked, cancellable operation and
reports moved/failed counts. Symbolic links also gain **Open Link Target**
navigation.

### Properties: media tracks and on-demand SHA-256

Properties of a video lists every audio and subtitle track (language, title,
codec, channels, default/forced) read asynchronously with `ffprobe`. Any file
gets a *SHA-256* row: click to calculate, click again to copy.

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
  - Folder tools: flatten, clean empty, group by type, group duplicates
  - Estiba (libmarea_archive) compression and extraction
  - Properties: media tracks, on-demand SHA-256
  - Graphical `.nemo_action` editor in Preferences → Custom Actions
  - Complete Spanish translation of every fork feature

Planned:

  - **View cache** — when you re-enter a folder that hasn't changed, reuse
    the already-built view (scroll, selection) instead of rebuilding it
    from scratch.
  - **Async I/O hardening** — a sleeping HDD must not freeze navigation in
    tabs/views pointing at other devices.
  - **`.nemo_action` editor polish** — reorder and group actions from the
    editor (creating and editing already works).
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
