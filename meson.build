project('audavi', 'c',
  version: '0.1.0',
  meson_version: '>=1.0.0',
  default_options: ['c_std=c17', 'buildtype=release'],
)

gtk4_dep = dependency('gtk4', version: '>=4.10')
mpv_dep = dependency('mpv', version: '>=2.0')
epoxy_dep = dependency('epoxy')

executable('audavi',
  sources: ['src/main.c'],
  dependencies: [gtk4_dep, mpv_dep, epoxy_dep],
  install: true,
)
