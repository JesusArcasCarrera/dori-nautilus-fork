media_tools_path = join_paths(prefix, get_option('libexecdir'), 'nautilus-media-tools')
action_configuration = configuration_data()
action_configuration.set('media_tools_path', media_tools_path)

media_actions = [
  'contact-sheet.nemo_action',
  'extract-frame.nemo_action',
  'images-to-pdf.nemo_action',
  'join-videos.nemo_action',
  'narrate-document.nemo_action',
  'ocr-image.nemo_action',
  'ocr-pdf.nemo_action',
  'office-to-pdf.nemo_action',
  'strip-image-metadata.nemo_action',
  'transcribe-subtitles.nemo_action',
  'video-to-gif.nemo_action',
  'video-to-mp4.nemo_action',
  'combine-pdf.nemo_action',
  'compose-images.nemo_action',
  'compose-videos.nemo_action',
  'convert-image-jpeg.nemo_action',
  'convert-image-png.nemo_action',
  'extract-video-frames.nemo_action',
  'loop-video-x10.nemo_action',
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
