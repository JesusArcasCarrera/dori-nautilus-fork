# Action editor — roadmap y guía de continuación

Documento vivo para el editor gráfico de `.nemo_action` (feature 6b).
Cuando llegue el momento de mejorarlo, leer esto primero — evita
redescubrir la arquitectura y los puntos sensibles.

## Estado actual (commit `preferences: add a graphical editor for custom .nemo_action files`)

Lo que ya funciona:

- Página **Custom Actions** en `Preferencias` (`src/resources/ui/nautilus-preferences-dialog.blp`, `custom_actions_page`).
- Botón **+** en la cabecera del grupo abre el editor en blanco; se
  crea un fichero con nombre slugificado en `~/.local/share/nemo/actions/`.
- Por cada acción cargada: fila con título (Name), subtítulo (Comment),
  icono prefix (Icon-Name), y sufijos **lápiz** (Edit) + **papelera**
  (Delete, con confirmación destructiva).
- La página se reconstruye sola al disparar la señal `changed` del
  `NemoActionManager` (vía `GFileMonitor` de 200 ms de debounce).
- Editor (`src/nemo-action-editor.{c,h}`): `AdwDialog` programático
  con `AdwToolbarView` + `AdwHeaderBar` (Cancel / Save) y cuatro
  `AdwPreferencesGroup`:
  - **Name** — `Name` (entry, requerido).
  - **Action** — `Type` (combo de 3), `Exec` (entry, requerido si
    Type=Command). Descripción del grupo lista los placeholders.
  - **Visibility** — `Selection` (combo: Any / None / Single / Multiple).
  - **Placement** — `Group` (entry, opcional → submenú).
- Validación mínima: si falta Name, se hace `grab_focus` sobre el
  campo; lo mismo con Exec cuando Type=Command. Sin mensaje visible.
- Slugify simple en `nemo_action_editor.c::slugify` (lower-case,
  alfanuméricos, runs de no-alfanum → `-`).
- Si el slug está ocupado, se prueba `-2`, `-3`, ... hasta 999
  (`pick_filename`).

Lo que **falta** o se quedó simple a propósito.

## Mejoras que merecen la pena (en orden de coste / valor)

### Campos no expuestos en el editor

El parser (`nemo-action.c::nemo_action_new`) ya entiende estos
campos `.nemo_action` pero el editor los ignora — solo se pueden
poner editando el fichero a mano:

| Clave        | Para qué                                         | UI sugerida           |
|--------------|--------------------------------------------------|-----------------------|
| `Comment`    | Tooltip de la entrada de menú.                   | `AdwEntryRow`         |
| `Icon-Name`  | Nombre del icono freedesktop.                    | `AdwEntryRow` + previsualización pequeña usando `gtk_image_new_from_icon_name` actualizada en `changed` del entry. Bonus: `gtk_icon_theme_has_icon` para validar live. |
| `Extensions` | Filtro por extensión (lista `;` separada o palabras clave `any`/`dir`/`nodirs`). | `AdwEntryRow` con placeholder de ejemplo, o un grupo de tags. |
| `Mimetypes`  | Lista de mimetypes (admite `prefix/*`).          | `AdwEntryRow` con autocompletado opcional.   |
| `Dependencies` | Programas que deben estar en `$PATH` para que la entrada se vea. | `AdwEntryRow` con check live (`g_find_program_in_path`). |
| `Position`   | Orden dentro de un grupo.                        | `AdwSpinRow` (entero ≥ 0). |
| `Selection` exacto | Número exacto de ficheros seleccionados.   | Sustituir la combo por una combo con opción **Exactly N** y un spin asociado. |

### Validación y feedback

Hoy `on_save_clicked` solo enfoca el campo vacío. Falta:

- Botón Save **deshabilitado** mientras Name (y Exec si Command) están
  vacíos. Conectar `notify::text` de cada entry y a `notify::selected`
  de la combo, recalcular `valid` y `gtk_widget_set_sensitive`.
- Marcar entries inválidos con la clase CSS `error`
  (`gtk_widget_add_css_class (entry, "error")`).
- Mostrar el motivo bajo el entry con `AdwEntryRow.set_subtitle`
  cuando falla (raro en AdwEntryRow; alternativa: poner una
  `AdwPreferencesGroup.set_description` dinámica).
- Cuando `Icon-Name` no existe en el tema, advertir en pequeño.

### Plantillas al crear (Add)

El botón **+** abre el editor en blanco. Mejor: ofrecer una pequeña
elección **antes**:

- Run a command (default).
- Open in a code editor (template `Exec=code %F`, `Dependencies=code`).
- Open in a terminal (`Exec=gnome-terminal --working-directory=%P`).
- Create file from clipboard (`Type=create-from-clipboard`,
  `Selection=None`).
- Overwrite file with clipboard (`Type=overwrite-from-clipboard`,
  `Selection=Single`).
- Custom script (`Exec=~/.local/bin/myscript.sh %F`).

Implementación: pequeña `AdwAlertDialog` con 5-6 botones, o un
`AdwNavigationView` dentro del propio editor con una primera page
de elección. Pre-rellenar los campos según la opción elegida.

### Reordenación (`Position`)

`Position` se respeta al renderizar el menú contextual y al ordenar
en la página (`nemo-action-manager.c::compare_actions`). Para
editarlo:

- A corto: añadir el campo `Position` como `AdwSpinRow` en el editor.
- A largo: drag & drop entre filas de la `AdwPreferencesGroup`. GTK4
  expone `GtkDragSource` + `GtkDropTarget` por widget. Después del
  drop, recalcular `Position` de todas las filas afectadas y reescribir
  los `.nemo_action` (cuidado con escrituras múltiples seguidas — usar
  un solo `g_idle_add` que batch'ee).

### Preview / Test

Botón "Test" en el header del editor que ejecuta la acción
contra una selección sintética (p.ej. el fichero `~/.local/share/nemo/actions/<id>.nemo_action`
mismo) o contra la selección actual de la ventana padre.

### Vista por grupos como sub-listas

Hoy el agrupado se hace con una fila "header" deshabilitada (`— Group —`)
intercalada. Más bonito: crear una `AdwPreferencesGroup` por cada
`Group` (esto requiere construirlas dinámicamente y montarlas dentro
del `AdwPreferencesPage`, no del fixed `custom_actions_group` del
`.blp`). El widget que sirve de "container vivo" del page sería el
`AdwPreferencesPage` mismo.

### Edición en lote / import / export

- **Exportar** una acción a un fichero ad-hoc (Save As) para compartirla.
- **Importar** desde una URL o fichero (validar formato antes de copiar
  al directorio del manager).
- **Duplicar** una acción (botón ya esbozado en planning previo).

### Compatibilidad con Cinnamon Nemo

Cinnamon Nemo soporta más claves que las que parseamos:

- `Quote` — cómo quotar las rutas en `%F`/`%f` (single, double,
  backtick).
- `EscapeSpaces` — escapar espacios en vez de quotar.
- `Conditions` — condiciones dinámicas (`gsettings <schema> <key>`,
  `exec <prog>`, `desktop`, `removable`, etc.).
- `Separator` — separador para el nombre cuando son varios.
- `URI-Scheme` — restringir por esquema (`file`, `smb`, ...).

Si se decide ampliar la compatibilidad, parsearlas en
`nemo-action.c::nemo_action_new`, evaluarlas en
`nemo_action_is_visible` / `build_command`, y exponer las que tengan
sentido en el editor.

## Bugs conocidos del editor

- **Sin notificación de save**: el dialog se cierra sin AdwToast. Si
  guardar falla (p.ej. permisos), solo aparece un `g_warning` en
  stderr — el usuario no se entera. Añadir `AdwToast` "Action saved"
  / "Could not save: …".
- **Renombrar implica un nuevo fichero**: al editar una acción y
  cambiar `Name`, el slug NO se regenera (se reusa `existing_id`). Si
  el nombre es ahora muy distinto, el filename queda incoherente.
  Decisión a tomar: mantener filename estable (actual) o
  ofrecer "Rename file too?". Lo conservador es mantener estable.
- **`Selection=Count` exacta** se carga como "Any" en el editor (no
  hay opción en la combo). Editar y guardar una acción con
  `Selection=3` la convierte en `Any`. Bug latente.
- **Texto de los `g_warning`**: en inglés. Pasarlos a `g_critical` o
  promoverlos a UI cuando se añada el toast.

## Mapa de ficheros

| Fichero                                                       | Qué tiene                                       |
|---------------------------------------------------------------|-------------------------------------------------|
| `src/nemo-action.{c,h}`                                       | Parser, condiciones, sustitución, ejecución.    |
| `src/nemo-action-manager.{c,h}`                               | Singleton, `GFileMonitor`, `get_actions_dir`, `delete_action`. |
| `src/nemo-action-editor.{c,h}`                                | `AdwDialog`, `slugify`, `pick_filename`, `on_save_clicked`. |
| `src/nautilus-preferences-dialog.c::setup_custom_actions_page` | Botón +, conexión "changed", lifetime del manager (atado a la group via `g_object_set_data_full`). |
| `src/nautilus-preferences-dialog.c::rebuild_custom_actions_rows` | Construcción de las filas con botones suffix y tracking de las rows en `ROWS_KEY`. |
| `src/resources/ui/nautilus-preferences-dialog.blp`            | Declaración del `custom_actions_page` + `custom_actions_group`. Si en algún momento se descubre que hace falta más widget templated, declararlo aquí. |
| `src/nautilus-files-view.c::build_custom_actions_menu`        | Cómo se traducen las acciones al menú contextual real (referencia, no editar para el editor). |
| `~/.local/share/nemo/actions/`                                | El directorio donde viven los `.nemo_action` editados por el usuario. El editor escribe ahí. |

## Cosas a recordar

- El `NemoActionManager` se accede vía `nemo_action_manager_dup_singleton`. El
  patrón en preferences-dialog ata la vida del manager al
  `custom_actions_group` (g_object_set_data_full con destroy_notify
  `g_object_unref`) para que la conexión a `"changed"` siga válida
  mientras el diálogo esté abierto.
- Al borrar una acción, el manager NO emite `changed` instantáneo:
  espera al evento del `GFileMonitor` con su debounce de 200 ms. Si
  alguna vez se quiere UI más reactiva, llamar también a
  `load_actions + emit_changed` síncronamente desde `delete_action`.
- Reglas de quoting de `Exec`: ver `nemo-action.c::quote_path_or_uri`
  y `build_command`. Cualquier nuevo placeholder debe seguir el mismo
  patrón (shell-quote siempre lo interpolado).
- Pendientes de la review (no específicos del editor pero relacionados):
  typeahead O(N) sin cancelación, clipboard `g_file_replace_contents`
  síncrono — ver `desktop-ux-features` memory.
