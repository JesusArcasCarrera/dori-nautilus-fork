# Track each generated UI file, not its parent directory: unrelated writes
# to the directory can otherwise hide Blueprint changes from Ninja.
subdir('ui')

gresource_file = configure_file(
  input: 'nautilus.gresource.xml.in',
  output: 'nautilus.gresource.xml',
  configuration: file_conf
)

resources = gnome.compile_resources(
  'nautilus-resources',
  gresource_file,
  dependencies: [appdata, blueprints, gresource_file],
  source_dir: 'resources',
  c_name: 'nautilus',
  extra_args: '--manual-register'
)
