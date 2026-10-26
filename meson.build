project('geany-favorites', 'c',
  version : '0.1',
  license : 'GPL-2.0-or-later',
  meson_version : '>= 0.56',
  default_options : ['warning_level=2', 'buildtype=debugoptimized'],
)

add_project_arguments('-Wno-unused-parameter', language : 'c')

geany_dep = dependency('geany', version : '>= 1.38')
# gtk_menu_popup_at_pointer()
gtk_dep = dependency('gtk+-3.0', version : '>= 3.22')

plugindir = get_option('plugindir')
if plugindir == ''
  # Where Geany itself looks for system-wide plugins.
  plugindir = geany_dep.get_variable(pkgconfig : 'libdir') / 'geany'
endif

shared_module('favorites', 'favorites.c',
  name_prefix : '',
  dependencies : [geany_dep, gtk_dep],
  install : true,
  install_dir : plugindir,
)
