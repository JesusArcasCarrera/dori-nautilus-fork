# Screenshots

Screenshots of the fork's own features, taken on a synthetic demo folder
(`Home / Demo`) with generated placeholder content — no real user data.
Nautilus ran inside a user+mount namespace with its own `/home/demo` and no
external drives visible, so every path and every sidebar entry in these images
is synthetic. All of them are 1400×900, dark style, English UI.

| File | What it shows |
| --- | --- |
| `overview-demo-folder.png` | The demo folder in icon view: sidebar, breadcrumb and thumbnails for images, videos and PDFs. |
| `type-to-action-filter.png` | `type-to-action` in **Filter This Folder** mode: typing `p` narrows the current folder to the matching items instead of starting a recursive search. |
| `preferences-type-to-action.png` | Preferences → General → **When you Start Typing**, with the four modes (Filter This Folder, Jump to First Match, Search Recursively, Do Nothing). |
| `preferences-sidebar.png` | Preferences → **Sidebar**: per-item toggles for the built-in entries (including the standalone *Other Locations*), section toggles and *Other Locations → Show Network*. |
| `custom-actions-editor.png` | Preferences → **Custom Actions**: the graphical editor for a `.nemo_action` (menu label, action type, command with placeholders, and the optional input question). |
| `context-menu-custom-actions.png` | Context menu of a symbolic link showing **Open Link Target Location** plus two user-defined actions contributed by `.nemo_action` files. |
| `context-menu-folder-tools.png` | Folder context menu → **Clean** submenu: Clean Empty Folders, Flatten Contents, Group Media, Group Duplicates (this level or including subfolders). |
| `context-menu-video-actions.png` | Video context menu → **Video** submenu: remux to MKV, re-encode, loop, frame extraction, contact sheet, GIF and *Transcribe to subtitles*. |
| `context-menu-image-actions.png` | Image context menu → **Image** submenu: rotate, convert to PNG/JPEG/PDF, image composition, OCR and copy without metadata. |
| `context-menu-pdf-actions.png` | Two PDFs selected → **PDF → Combine PDFs**, which joins them in selection order. |
| `properties-media-tracks.png` | Properties of a Matroska file: every audio and subtitle track (language, title, codec, channels, default flag) read with `ffprobe`, plus the on-demand **SHA-256** row already calculated with its copy button. |
| `properties-non-modal.png` | Two independent Properties windows open at once over the file manager window — they are ordinary top-level windows, not modal dialogs. |
| `file-operations-popover.png` | The compact progress popover with two tracked operations, determinate progress from `$DORI_PROGRESS_FILE`, ETA, pause/cancel buttons and **Open in Window**. |
| `file-operations-window.png` | The dedicated **File Operations** window, with the first operation paused (Resume button) and the second still running. |
| `cut-overlay.png` | A cut file: the thumbnail stays visible, dimmed, with the dashed border and scissors drawn on top; the filename label keeps full opacity. |
| `paste-text-as-file.png` | Text pasted on the folder background became `Pasted text.py` — the type is guessed from the content (a Python snippet) and the file is revealed and selected. |
| `sidebar-places-editable.png` | Dragging a folder onto the sidebar: the **Pin folder** and **New bookmark** affordances appear in the editable places section, and the drop target row is highlighted. |
| `disk-usage-treemap.png` | **Disk Usage Map…** of the demo folder: proportional treemap by subfolder, with files at the current level grouped into their own rectangle. |
| `compress-dialog-estiba.png` | The *Compress…* dialog (routed through Estiba/`libmarea_archive` in this build) with the archive name, the compression method list (ZIP, encrypted ZIP, TAR.XZ, 7Z) and the passphrase fields. |

Not included: the `other-locations:///` hub. It could not be captured here —
its model is backed by `computer:///` and `network:///`, which are not
available in the isolated X/D-Bus session used to take these screenshots, so
the page rendered empty ("No Locations").
