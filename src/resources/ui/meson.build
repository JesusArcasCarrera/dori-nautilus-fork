blueprint_files = files(
    'nautilus-app-chooser.blp',
    'nautilus-batch-rename-dialog.blp',
    'nautilus-column-chooser.blp',
    'nautilus-compress-dialog.blp',
    'nautilus-date-entry-row.blp',
    'nautilus-date-range-dialog.blp',
    'nautilus-file-chooser.blp',
    'nautilus-file-conflict-dialog.blp',
    'nautilus-file-properties-change-permissions.blp',
    'nautilus-files-view-select-items.blp',
    'nautilus-files-view.blp',
    'nautilus-grid-cell.blp',
    'nautilus-grid-view-captions-dialog.blp',
    'nautilus-history-controls.blp',
    'nautilus-name-cell.blp',
    'nautilus-network-address-bar.blp',
    'nautilus-network-cell.blp',
    'nautilus-new-folder-dialog.blp',
    'nautilus-preferences-dialog.blp',
    'nautilus-operations-ui-manager-request-passphrase.blp',
    'nautilus-progress-indicator.blp',
    'nautilus-progress-info-widget.blp',
    'nautilus-properties-widget.blp',
    'nautilus-properties-window.blp',
    'nautilus-rename-file-popover.blp',
    'nautilus-search-popover.blp',
    'nautilus-search-types-dialog.blp',
    'nautilus-sidebar-row.blp',
    'nautilus-toolbar.blp',
    'nautilus-view-controls.blp',
    'nautilus-window.blp',
    'shortcuts-dialog.blp',
)

blueprints = []
foreach blueprint : blueprint_files
  blueprints += custom_target(
    blueprint.full_path().split('/')[-1].replace('.blp', '.ui'),
    input: blueprint,
    output: '@BASENAME@.ui',
    command: [blueprint_cmd, 'compile', '--output', '@OUTPUT@', '@INPUT@'],
  )
endforeach
