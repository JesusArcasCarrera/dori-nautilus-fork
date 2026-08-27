media_tools_path = join_paths(prefix, get_option('libexecdir'), 'nautilus-media-tools')
action_configuration = configuration_data()
action_configuration.set('media_tools_path', media_tools_path)

media_actions = [
  'combine-pdf.nemo_action',
  'convert-image-jpeg.nemo_action',
  'convert-image-png.nemo_action',
  'rotate-image-left.nemo_action',
  'rotate-image-right.nemo_action',
  'video-to-mkv.nemo_action',
]

foreach action : media_actions
  configure_file(
    input: action + '.in',
    output: action,
    configuration: action_configuration,
    install: true,
    install_dir: join_paths(datadir, 'nautilus', 'actions'),
  )
endforeach

install_data(
  'nautilus-media-tools',
  install_dir: get_option('libexecdir'),
  install_mode: 'rwxr-xr-x',
)
