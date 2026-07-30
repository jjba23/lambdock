project('lambdock', 'c',
  version: '0.1.0',
  default_options: ['warning_level=2', 'c_std=c11'])

gtk_dep = dependency('gtk4')
guile_dep = dependency('guile-3.0')
layer_shell_dep = dependency('gtk4-layer-shell-0')

executable('lambdock',
  'src/main.c',
  dependencies: [gtk_dep, guile_dep, layer_shell_dep],
  install: true)