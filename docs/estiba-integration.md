# Integración con Estiba (marea-archive)

El plan del archivador del ecosistema, el inventario de funciones de PeaZip,
la matriz de motores y la decisión de arquitectura viven en
`marea/docs/estiba/plan.md` (repo `marea`, roadmap `estiba` en MicroGit).

Lo que toca a este fork:

- Ticket local: «Integrar marea-archive en comprimir y extraer (E4)».
- Puntos de cambio: `compress_task_thread_func` y `extract_task_thread_func`
  en `src/nautilus-file-operations.c`, hoy sobre gnome-autoar (monohilo:
  144 s frente a 24 s de `7z` en un `.7z` de 325 MB con 20 núcleos; benchmark
  en `_tmp/bench/autoar-bench.c`).
- Opción de meson `marea_archive`: enlaza `libmarea_archive` por pkg-config y
  cae a gnome-autoar si no está instalada.
- El diálogo «Comprimir» gana nivel, contraseña, volúmenes y «cada elemento
  por separado»; el menú gana «Probar archivo» y «Abrir con Estiba».
