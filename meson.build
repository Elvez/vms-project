project('streamer', 'cpp',
  version: '0.1.0',
  default_options: ['cpp_std=c++17']
)

deps = [
  dependency('libavformat', required: true),
  dependency('libavcodec', required: true),
  dependency('libavutil', required: true),
  dependency('libswscale', required: true),
]

executable('streamer',
  'src/streamer.cpp',
  dependencies: deps,
  install: true
)
