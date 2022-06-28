project('cpprom', 'cpp', default_options : ['warning_level=3', 'cpp_std=c++17'])

src = ['src/cpprom.cpp']
if host_machine.system() == 'linux'
  src += 'src/processmetrics.cpp'
endif

flags = []
if get_option('single_threaded')
  flags += '-DCPPROM_SINGLE_THREADED'
endif

cpprom_inc = include_directories('include')
cpprom_lib = library('cpprom', src, include_directories : cpprom_inc, cpp_args : flags)
cpprom_dep = declare_dependency(
  compile_args : flags,
  link_with : cpprom_lib,
  include_directories : cpprom_inc,
)

if not meson.is_subproject()
  executable('overview', 'examples/overview.cpp', dependencies : cpprom_dep)
  executable('helpers', 'examples/helpers.cpp', dependencies : cpprom_dep)

  if host_machine.system() == 'linux'
    executable('server', 'examples/server.cpp', dependencies : cpprom_dep)
  endif
endif
