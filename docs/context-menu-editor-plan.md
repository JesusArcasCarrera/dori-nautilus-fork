# Editor completo del menú contextual (Ajustes)

Estado: plan aprobado el 2026-09-06; tickets en el roadmap `menu-contextual`
de MicroGit. Sustituye y amplía la sección «Plan: página Menú contextual» de
`docs/action-editor-roadmap.md`, que se queda como referencia del editor de
acciones.

## Objetivo

Que cualquier persona pueda decidir qué aparece en el menú contextual y en
qué orden, sin tocar ficheros. Tres contextos, uno por pestaña:

| Pestaña | Cuándo se muestra | Hoy lo construye |
|---|---|---|
| Archivo | clic derecho sobre uno o varios ficheros (ninguna carpeta) | `update_selection_menu` |
| Carpeta | clic derecho sobre una o varias carpetas (o mezcla) | `update_selection_menu` con las entradas de carpeta |
| Fondo | clic derecho en el área vacía de la vista | `update_background_menu` |

En cada pestaña, dos columnas: **En el menú**, la lista ordenada de lo que se
ve, con separadores y submenús; y **Disponibles**, el catálogo de todo lo que
podría estar, agrupado por origen y con búsqueda. Se arrastra de una a otra y
dentro de la primera para reordenar. Alternativa de respaldo, más floja: un
acordeón por sección con botón «Añadir»; sirve como primera versión funcional
antes del arrastrar y soltar, y como camino accesible por teclado.

## Modelo

### Catálogo (`DoriMenuCatalog`)

Cada entrada posible del menú recibe un **id estable** y metadatos:

- **Integradas** (las de `nautilus-files-view-context-menus.ui`): atributo
  nuevo `x-dori-id` en cada `item` y `submenu` del `.ui` (`open`, `open-with`,
  `cut`, `copy`, `paste`, `rename`, `move-to-trash`, `compress`, `extract-here`,
  `properties`, `new-folder`, `select-all`, `open-in-terminal`…). Etiqueta e
  icono se leen del propio `GMenuModel`; el contexto se deduce de la sección
  en que viven. Las que Nautilus oculta por estado (p. ej. `paste` sin
  portapapeles) siguen ocultándose: el editor decide presencia y orden, no
  habilitación.
- **Acciones personalizadas** (`DoriAction`): id `custom:<id>`, con su
  `Group` como submenú sugerido y su `Placement`.
- **Barra de acciones rápidas**: id `quick:<nombre>`; es una fila especial
  que se coloca como cualquier otra (ver M6).
- **Extensiones** (Ghostty, Syncthing, futuras capacidades de Marea):
  id `ext:<proveedor>` como bloque; dentro, lo que la extensión aporte en
  cada momento. Se reordena y oculta por proveedor, no por entrada, porque
  las entradas son dinámicas.
- **Scripts y Plantillas**: submenús integrados, se tratan como una entrada.

### Disposición (`DoriMenuLayout`)

Una clave GSettings `org.gnome.nautilus.preferences context-menu-layout` de
tipo `s` con un documento JSON, más cómodo de evolucionar, exportar e
importar que tres claves `as`:

```json
{
  "version": 1,
  "file":       [ "open", "open-with", "---", "cut", "copy", "paste", "---",
                  { "group": "PDF", "items": ["custom:combine-pdf", "custom:ocr-pdf"] },
                  "ext:ghostty", "---", "properties" ],
  "folder":     [ "..." ],
  "background": [ "..." ],
  "hidden":     { "file": ["star"], "folder": [], "background": ["select-all"] }
}
```

Reglas de aplicación, en `update_selection_menu` / `update_background_menu`
justo antes de mostrar el popover:

1. Se construye el menú como hasta ahora (así el estado habilitado/oculto de
   cada acción sigue siendo el de Nautilus).
2. Se recorren las secciones y se **reubican** los items según la lista del
   contexto: mismo orden, mismos separadores, mismos grupos. Lo que esté en
   `hidden` se quita.
3. Lo que exista en el menú pero no en la lista (una entrada nueva tras
   actualizar el fork, una extensión recién instalada) se añade al final de
   su sección de origen, para que nada desaparezca en silencio.
4. Sin clave o con JSON inválido, el menú queda como el de fábrica.

El coste es lineal en el número de items y se paga al abrir el menú, que ya
se reconstruye en cada apertura.

## Interfaz

Página «Menú contextual» en el diálogo de Ajustes, con `AdwViewSwitcher`
para las tres pestañas. Cada pestaña:

```
┌─ En el menú ───────────────────────┐  ┌─ Disponibles ──────── [buscar] ─┐
│ ⋮⋮ 📂 Abrir                    👁  │  │ ▸ Integradas                    │
│ ⋮⋮    Abrir con…               👁  │  │ ▸ Acciones personalizadas       │
│ ────────────────────────────── ─   │  │ ▸ Extensiones                   │
│ ⋮⋮ ✂ Cortar                    👁  │  │ ▸ Barra rápida                  │
│ ⋮⋮ 📁 PDF ▾                        │  │                                 │
│    ⋮⋮  Combinar PDFs           👁  │  │ (arrastra a la izquierda o «+») │
│ …                                  │  └─────────────────────────────────┘
└────────────────────────────────────┘
[Vista previa]  [Restablecer esta pestaña]  [Exportar…] [Importar…]
```

- Filas con asa de arrastre, icono, etiqueta y ojo para ocultar sin quitar.
- Separadores y grupos son filas también; se arrastran enteros.
- Arrastrar de «Disponibles» a «En el menú» inserta donde se suelta; arrastrar
  fuera de la lista quita. El catálogo nunca pierde entradas.
- Teclado: subir/bajar con Alt+↑/↓, añadir con Intro sobre el catálogo,
  quitar con Supr. Es la vía accesible y la que cubre el «acordeón».
- Vista previa: un `GtkPopoverMenu` real construido con el layout actual
  sobre una selección ficticia, para ver el resultado sin cerrar Ajustes.

Implementación GTK4: `GtkListBox` con `GtkDragSource`/`GtkDropTarget` por
fila y un indicador de inserción por CSS (`.m-drop-before` como en Marea
DnD); ~120 líneas reutilizables entre las dos columnas. No hay listbox
reordenable nativo en GTK4.

## Fases

| Fase | Contenido | Estimación |
|---|---|---|
| M0 | Catálogo con ids estables, clave `context-menu-layout`, aplicación del orden y ocultos en los tres contextos. Criterio: editar la clave con `gsettings` reordena y oculta sin editor. | 2 días |
| M1 | Página de Ajustes: tres pestañas, dos columnas, añadir/quitar/subir/bajar con botones y teclado, ojo para ocultar, búsqueda. Funcional sin arrastrar. | 2 días |
| M2 | Arrastrar y soltar entre columnas y dentro de la lista, indicador de inserción, arrastrar grupos enteros. | 1,5 días |
| M3 | Vista previa en vivo, restablecer por pestaña, exportar/importar JSON. | 1 día |
| M4 | Grupos y submenús editables: crear, renombrar, mover entradas dentro y fuera; reflejar en `Group` de las acciones. | 2 días |
| M5 | Extensiones como bloques por proveedor; capacidades de Marea cuando existan. | 1 día |
| M6 | Barra de acciones rápidas gobernada por el mismo modelo (`quick:*`), sustituyendo la tabla estática. | 1 día |
| M7 | «Añadir acción aquí…» con asistente de comando (plan previo, apartado b). | 2 días |

Riesgos: (1) al sincronizar con Nautilus upstream, los `x-dori-id` del `.ui`
hay que mantenerlos a mano; una prueba que compruebe que cada `item` tiene id
lo hace mecánico. (2) Las entradas de extensiones cambian de una apertura a
otra; por eso se manejan por bloque. (3) Ocultar «Propiedades» o «Abrir» es
legítimo pero peligroso: «Restablecer» siempre visible y confirmación al
ocultar las esenciales.
