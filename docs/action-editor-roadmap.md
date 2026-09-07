# Action editor — roadmap y guía de continuación

Documento vivo para el editor gráfico de `.nemo_action` (feature 6b).
Cuando llegue el momento de mejorarlo, leer esto primero — evita
redescubrir la arquitectura y los puntos sensibles.

## Acuerdo de integración (2026-09-07)

Nemo Actions sigue siendo el conector de Dori: define la entrada del menú,
sus condiciones y parámetros, e invoca el CLI correspondiente mediante `Exec`.
El catálogo y el auxiliar multimedia que distribuye Dori se mantienen aquí.
El proyecto `acciones` queda aplazado hasta estudiar su papel como índice de
capacidades consultable por aplicaciones; no es un requisito de ejecución
ni autoriza trasladar allí las operaciones de Dori. Una integración futura
puede cambiar el comando de una `.nemo_action` sin sustituir este soporte.
La extensión Python `nautilus-acciones.py` se descartó como integración del
menú; no debe reactivarse para este propósito.

## Estado actual (commit `preferences: add a graphical editor for custom .nemo_action files`)

Lo que ya funciona:

- Página **Custom Actions** en `Preferencias` (`src/resources/ui/nautilus-preferences-dialog.blp`, `custom_actions_page`).
- Botón **+** en la cabecera del grupo abre el editor en blanco; se
  crea un fichero con nombre slugificado en `~/.local/share/nemo/actions/`.
- Por cada acción cargada: fila con título (Name), subtítulo (Comment),
  icono prefix (Icon-Name), y sufijos **lápiz** (Edit) + **papelera**
  (Delete, con confirmación destructiva).
- La página se reconstruye sola al disparar la señal `changed` del
  `DoriActionManager` (vía `GFileMonitor` de 200 ms de debounce).
- Editor (`src/dori-action-editor.{c,h}`): `AdwDialog` programático
  con `AdwToolbarView` + `AdwHeaderBar` (Cancel / Save) y cuatro
  `AdwPreferencesGroup`:
  - **Name** — `Name` (entry, requerido).
  - **Action** — `Type` (combo de 3), `Exec` (entry, requerido si
    Type=Command). Descripción del grupo lista los placeholders.
  - **Visibility** — `Selection` (combo: Any / None / Single / Multiple).
  - **Placement** — `Group` (entry, opcional → submenú).
- Validación mínima: si falta Name, se hace `grab_focus` sobre el
  campo; lo mismo con Exec cuando Type=Command. Sin mensaje visible.
- Slugify simple en `dori_action_editor.c::slugify` (lower-case,
  alfanuméricos, runs de no-alfanum → `-`).
- Si el slug está ocupado, se prueba `-2`, `-3`, ... hasta 999
  (`pick_filename`).

Lo que **falta** o se quedó simple a propósito.

## Mejoras que merecen la pena (en orden de coste / valor)

### Campos no expuestos en el editor

El parser (`dori-action.c::dori_action_new`) ya entiende estos
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
| `Placement`  | `open` → la entrada va al bloque de «Abrir en terminal» (extensiones) en vez del bloque de acciones. **Expuesto** ya como combo «Menu block». | — |
| `Prompt` / `Prompt-Default` | Pregunta previa; la respuesta llega como `%p` y `$DORI_PROMPT`. **Expuestos** como pregunta y valor inicial. |
| `Prompt-Mode` | `always` abre siempre el diálogo; `split` crea una fila partida que ejecuta el predeterminado o permite configurarlo. **Expuesto** como combo «Activation». |
| `Prompt-Display-Format` | Clave heredada, todavía expuesta y validada en el editor por compatibilidad. El menú muestra solo el nombre; el valor efectivo se consulta en el diálogo de parámetros. |
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
en la página (`dori-action-manager.c::compare_actions`). Para
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
`dori-action.c::dori_action_new`, evaluarlas en
`dori_action_is_visible` / `build_command`, y exponer las que tengan
sentido en el editor.

## Bugs conocidos del editor

- ~~**Guardar pierde claves no expuestas**~~: corregido. `on_save_clicked`
  carga el fichero existente antes de escribir, así `Icon-Name`, `Mimetypes`,
  `Prompt`, comentarios y traducciones sobreviven a un round-trip.

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
| `src/dori-action.{c,h}`                                       | Parser, condiciones, sustitución, ejecución.    |
| `src/dori-action-manager.{c,h}`                               | Singleton, `GFileMonitor`, `get_actions_dir`, `delete_action`. |
| `src/dori-action-editor.{c,h}`                                | `AdwDialog`, `slugify`, `pick_filename`, `on_save_clicked`. |
| `src/nautilus-preferences-dialog.c::setup_custom_actions_page` | Botón +, conexión "changed", lifetime del manager (atado a la group via `g_object_set_data_full`). |
| `src/nautilus-preferences-dialog.c::rebuild_custom_actions_rows` | Construcción de las filas con botones suffix y tracking de las rows en `ROWS_KEY`. |
| `src/resources/ui/nautilus-preferences-dialog.blp`            | Declaración del `custom_actions_page` + `custom_actions_group`. Si en algún momento se descubre que hace falta más widget templated, declararlo aquí. |
| `src/nautilus-files-view.c::build_custom_actions_menu`        | Cómo se traducen las acciones al menú contextual real (referencia, no editar para el editor). |
| `~/.local/share/nemo/actions/`                                | El directorio donde viven los `.nemo_action` editados por el usuario. El editor escribe ahí. |

## Cosas a recordar

- El `DoriActionManager` se accede vía `dori_action_manager_dup_singleton`. El
  patrón en preferences-dialog ata la vida del manager al
  `custom_actions_group` (g_object_set_data_full con destroy_notify
  `g_object_unref`) para que la conexión a `"changed"` siga válida
  mientras el diálogo esté abierto.
- Al borrar una acción, el manager NO emite `changed` instantáneo:
  espera al evento del `GFileMonitor` con su debounce de 200 ms. Si
  alguna vez se quiere UI más reactiva, llamar también a
  `load_actions + emit_changed` síncronamente desde `delete_action`.
- Reglas de quoting de `Exec`: ver `dori-action.c::quote_path_or_uri`
  y `build_command`. Cualquier nuevo placeholder debe seguir el mismo
  patrón (shell-quote siempre lo interpolado).
- Pendientes de la review (no específicos del editor pero relacionados):
  typeahead O(N) sin cancelación, clipboard `g_file_replace_contents`
  síncrono — ver `desktop-ux-features` memory.

## Plan: página «Menú contextual» en Ajustes (pendiente)

Objetivo: que una persona sin perfil técnico pueda (a) reordenar la barra de
iconos rápidos y (b) crear una acción en dos pasos, sin tocar
`~/.local/share/nemo/actions/` ni saber qué es un `.nemo_action`.

### a) Barra de iconos rápidos reordenable

- **Modelo**: nueva clave GSettings `org.gnome.nautilus.preferences
  quick-actions` (tipo `as`) con los nombres de acción, p. ej.
  `['cut','copy','rename','move-to-trash']` para selección y otra
  `background-quick-actions` para el fondo. La tabla estática
  `ContextMenuQuickAction` de `nautilus-files-view.c` pasa a ser un
  catálogo (nombre → icono, etiqueta, hide_when_disabled) y el orden/visibles
  lo dicta la clave. Mantener ambas tablas como default de la clave.
- **Candidatos al catálogo** (todo lo que ya tiene `view.*` action):
  cut, copy, paste, rename, move-to-trash, delete-permanently, new-folder,
  select-all, console/open-in-terminal, properties, star, create-link,
  compress, open-with-other-application, y también acciones
  `.nemo_action` (id con prefijo `custom:`, icono de `Icon-Name`).
- **UI**: `AdwPreferencesGroup` con un `GtkListBox` de filas
  «icono + nombre + asa de arrastre + switch de visibilidad». Drag & drop
  con `GtkDragSource`/`GtkDropTarget` por fila (GTK4 no tiene listbox
  reorderable nativo; patrón conocido: ~80 líneas, ver
  gnome-control-center «Search» page). Al soltar se reescribe la clave;
  la vista escucha `changed::quick-actions` y reconstruye el popover en
  `update_context_menus`. Previsualización en vivo: una fila superior con
  los iconos en el orden actual.
- **Coste**: ~1 día. Sin riesgo para el resto del menú.

### b) Crear una acción «in situ» sin escribir scripts

- **Entrada en el menú contextual**: ítem «Añadir acción aquí…» al final
  del bloque de acciones (sección propia, oculto si la preferencia
  `show-add-action-item` está off). Abre `DoriActionEditor`
  pre-rellenado con el contexto del clic: si hay selección, `Selection`
  se ajusta (Single/Multiple) y `Extensions`/`Mimetypes` se rellenan con
  los de los ficheros seleccionados (p. ej. `Mimetypes=video/*`), para que
  la acción nueva salga «solo para cosas como esta».
- **Asistente de comando** (sustituye al entry `Exec` en blanco):
  1. «¿Qué programa?» entry con autocompletado sobre `$PATH`
     (`g_find_program_in_path` live) + botón «Elegir aplicación…»
     (`GtkAppChooserDialog`, saca el `Exec` del `.desktop`).
  2. «¿Qué le paso?» chips exclusivos: *el fichero* (`%F`), *los ficheros*
     (`%F`, `Selection=Multiple`), *la carpeta actual* (`%P`),
     *nada, solo abrir aquí* (`%P` como cwd), *un texto que me pregunte*
     (`%p` + `Prompt`). Cada chip rellena `Exec` visible debajo, editable
     para quien quiera afinar.
  3. Toggles: «Abrir en terminal» (envuelve con la terminal preferida),
     «Avisar al terminar» (`&& notify-send`), «Mostrar solo si el programa
     está instalado» (`Dependencies=`).
- **Plantillas** en el botón «+» (ya listadas arriba: editor, terminal,
  portapapeles) más «Convertir con ffmpeg/magick» que precarga
  `Mimetypes` y un `Exec` con `%p` para el formato de salida.
- **Probar sin guardar**: botón «Probar con la selección actual» que
  ejecuta `dori_action_activate` sobre un `DoriAction` temporal (construido
  desde un `GKeyFile` en memoria; hace falta un `dori_action_new_from_key_file`).
- **Coste**: asistente + entrada contextual ~1,5 días; plantillas y
  «probar» ~medio día más.

Orden sugerido: (b) entrada contextual con pre-relleno (barato, mucho
valor) → (a) barra reordenable → (b) asistente de comando → plantillas.
