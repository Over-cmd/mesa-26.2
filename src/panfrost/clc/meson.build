# Copyright 2017 Intel Corporation
# Copyright 2024 Collabora
# SPDX-License-Identifier: MIT

# Si estamos haciendo compilación cruzada para Android, omitimos esta herramienta 
# interna de shaders de OpenCL ya que no se ejecuta en el dispositivo final.
if meson.is_cross_build()
  subdirdone()
endif

if get_option('precomp-compiler') == 'system'
  prog_panfrost_compile = find_program('panfrost_compile', native : true)
else
  prog_panfrost_compile = executable(
    'panfrost_compile',
    ['pan_compile.c'],
    link_with : [libpanfrost_compiler],
    include_directories : [inc_include, inc_src],
    c_args : [c_msvc_compat_args, no_override_init_args, soong_compat_c_args],
    link_args : [ld_args_build_id],
    dependencies : [idep_vtn, idep_nir, idep_mesautil, libpanfrost_dep],
    native : true,
    install : get_option('install-precomp-compiler'),
  )
endif
