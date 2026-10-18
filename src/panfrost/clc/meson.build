# Copyright 2017 Intel Corporation
# Copyright 2024 Collabora
# SPDX-License-Identifier: MIT

if get_option('precomp-compiler') == 'system'
  prog_panfrost_compile = find_program('panfrost_compile', native : true)
else
  # Si estamos en compilación cruzada, construimos el binario de forma nativa
  # evitando mezclar dependencias del host (Android) en una herramienta de la máquina build (PC)
  is_cross = meson.is_cross_build()
  
  prog_panfrost_compile = executable(
    'panfrost_compile',
    ['pan_compile.c'],
    link_with : is_cross ? [] : [libpanfrost_compiler],
    include_directories : [inc_include, inc_src],
    c_args : [c_msvc_compat_args, no_override_init_args, soong_compat_c_args],
    link_args : [ld_args_build_id],
    dependencies : is_cross ? [idep_mesautil] : [idep_vtn, idep_nir, idep_mesautil, libpanfrost_dep],
    native : true,
    install : get_option('install-precomp-compiler'),
  )
endif
