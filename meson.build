project('cpprom', 'cpp', default_options : ['warning_level=3', 'cpp_std=c++17'])

cpprom = library('cpprom', 'src/cpprom.cpp')

executable('cpprom', 'src/main.cpp', link_with : cpprom)
