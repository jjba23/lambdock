# lambdock

# Copyright © Josep Bigorra <jjbigorra@gmail.com>

# lambdock is free software: you can redistribute it and/or modify
# it under the terms of the GNU General Public License as published by
# the Free Software Foundation, either version 3 of the License, or
# (at your option) any later version.

# lambdock is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
# GNU General Public License for more details.

# You should have received a copy of the GNU General Public License
# along with lambdock.  If not, see <https://www.gnu.org/licenses/>.

project('lambdock', 'c',
  version: '0.1.0',
  default_options: ['warning_level=2', 'c_std=c11'])

cc = meson.get_compiler('c')

gtk_dep = dependency('gtk4')
guile_dep = dependency('guile-3.0')
layer_shell_dep = dependency('gtk4-layer-shell-0')

wayland_client_dep = dependency('wayland-client')

# Locate wlr-protocols XML directory via pkg-config
wlr_protocols_dep = dependency('wlr-protocols')
wlr_pkgdatadir = wlr_protocols_dep.get_variable(pkgconfig: 'pkgdatadir')
toplevel_xml = wlr_pkgdatadir / 'unstable/wlr-foreign-toplevel-management-unstable-v1.xml'

# Locate wayland-scanner executable
wayland_scanner = find_program('wayland-scanner')

# Generate C protocol source and header files
wl_proto_src = custom_target(
  'wlr-foreign-toplevel-protocol-c',
  input: toplevel_xml,
  output: 'wlr-foreign-toplevel-management-unstable-v1-protocol.c',
  command: [wayland_scanner, 'private-code', '@INPUT@', '@OUTPUT@'],
)

wl_proto_h = custom_target(
  'wlr-foreign-toplevel-protocol-h',
  input: toplevel_xml,
  output: 'wlr-foreign-toplevel-management-unstable-v1-protocol.h',
  command: [wayland_scanner, 'client-header', '@INPUT@', '@OUTPUT@'],
)

# Define paths
datadir = get_option('datadir')
pkgdatadir = get_option('prefix') / datadir / 'lambdock'
scheme_install_path = get_option('prefix') / datadir / 'guile/site/3.0'

# Pass defines to C
add_project_arguments('-DLAMBDOCK_SCHEME_DIR="' + scheme_install_path + '"', language: 'c')
add_project_arguments('-DDOCK_CSS_PATH="' + pkgdatadir / 'css/dock.css' + '"', language: 'c')
add_project_arguments('-DDEFAULT_GSK_RENDERER="gl"', language: 'c')

# Install scheme files and CSS file
install_subdir('scheme/lambdock', install_dir: datadir / 'guile/site/3.0')
install_data('resources/css/dock.css', install_dir: pkgdatadir / 'css')

sources = files(
  'src/main.c',
  'src/ui_helpers.c',
  'src/guile_bridge.c',
  'src/dock_window.c',
)

executable('lambdock',
  sources,
  wl_proto_src,
  wl_proto_h,
  dependencies: [gtk_dep, guile_dep, layer_shell_dep, wayland_client_dep],
  install: true)
