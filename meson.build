project('lambdock', 'c',
  version: '0.1.0',
  default_options: ['warning_level=2', 'c_std=c11'])

gtk_dep = dependency('gtk4')
guile_dep = dependency('guile-3.0')
layer_shell_dep = dependency('gtk4-layer-shell-0')

# Define Scheme target directory ($prefix/share/guile/site/3.0)
datadir = get_option('datadir')
scheme_install_path = get_option('prefix') / datadir / 'guile/site/3.0'

# Pass the installed path macro to C compiler
add_project_arguments('-DLAMBDOCK_SCHEME_DIR="' + scheme_install_path + '"', language: 'c')

# 3. Tell Meson to copy the scheme files into the store upon installation
install_subdir('scheme/lambdock',
  install_dir: datadir / 'guile/site/3.0')

executable('lambdock',
  'src/main.c',
  dependencies: [gtk_dep, guile_dep, layer_shell_dep],
  install: true)
