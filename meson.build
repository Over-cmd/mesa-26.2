# Copyright © 2017-2020 Intel Corporation
# SPDX-License-Identifier: MIT

project(
  'mesa',
  ['c', 'cpp'],
  version : files('VERSION'),
  license : 'MIT',
  meson_version : '>= 1.4.0',
  default_options : [
    'buildtype=debugoptimized',
    'b_ndebug=if-release',
    'c_std=c11',
    'cpp_std=c++17',
    'rust_std=2021',
    'build.rust_std=2021',
  ],
)

project_version = meson.project_version()

if host_machine.system() == 'darwin'
  add_languages('objc', native : false)
  libname_prefix = 'lib'
  libname_suffix = 'dylib'
elif host_machine.system() == 'windows'
  libname_prefix = ''
  libname_suffix = 'dll'
else
  libname_prefix = 'lib'
  libname_suffix = 'so'
endif

cc = meson.get_compiler('c')
cpp = meson.get_compiler('cpp')
fs = import('fs')

sizeof_pointer = cc.sizeof('void*').to_string()

null_dep = dependency('', required : false)
prog_bison = find_program('bison', required : false)
prog_flex = find_program('flex', required : false)
prog_flex_cpp = find_program('flex', required : false)

if get_option('layout') != 'mirror'
  error('`mirror` is the only build directory layout supported')
endif

if get_option('b_lto')
  project_version += '-lto'
  if not get_option('allow-broken-lto')
     error('Building Mesa with LTO is not supported. Please disable LTO for building Mesa.')
  endif
endif

with_llvm = get_option('llvm')
amd_with_llvm = with_llvm.allowed() and get_option('amd-use-llvm')

with_mesa_debug = get_option('buildtype') == 'debug'
with_mesa_ndebug = get_option('b_ndebug') == 'true' or (get_option('buildtype') == 'release' and get_option('b_ndebug') == 'if-release')

relative_dir = fs.relative_to(meson.project_source_root(), meson.project_build_root())
dir_source_root = meson.project_source_root()

cl_args = [
  '-fmacro-prefix-map=@0@/='.format(relative_dir),
  '-fmacro-prefix-map=@0@/='.format(meson.project_source_root()),
  '-fmacro-prefix-map=@0@/='.format(meson.project_build_root()),
  '-cl-std=cl2.0',
  '-D__OPENCL_VERSION__=200',
  '-DHAVE___BUILTIN_FFS',
  '-DHAVE___BUILTIN_CLZ',
]

if with_mesa_ndebug
  cl_args += ['-DNDEBUG']
endif

pre_args = [
  '-D__STDC_CONSTANT_MACROS',
  '-D__STDC_FORMAT_MACROS',
  '-D__STDC_LIMIT_MACROS',
  '-DPACKAGE_VERSION="@0@"'.format(project_version),
  '-DPACKAGE_BUGREPORT="https://gitlab.freedesktop.org/mesa/mesa/-/issues"',
]

c_cpp_args = []
c_args = []
cpp_args = []

c_msvc_compat_args = []
cpp_msvc_compat_args = []
soong_compat_c_args = []
sse41_args = []
avx_args = []
avx2_args = []
no_override_init_args = []
with_gfx_compute = false
with_driver_using_cl = false
yacc_is_bison = false
with_panfrost_rust = false
with_poly = false
with_wayland_bind_display = false
with_ld_version_script = false
with_xcb_keysyms = false

with_moltenvk_dir = get_option('moltenvk-dir')
with_vulkan_loader_rpath = get_option('vulkan-loader-rpath')
with_vulkan_icd_dir = get_option('vulkan-icd-dir')
with_tests = get_option('build-tests')
with_glcpp_tests = get_option('enable-glcpp-tests')
with_radv_tests = get_option('build-radv-tests')
with_aco_tests = get_option('build-aco-tests')
with_glx_read_only_text = get_option('glx-read-only-text')
with_glx_direct = get_option('glx-direct')
with_vulkan_overlay_layer = get_option('vulkan-layers').contains('overlay')
with_vulkan_device_select_layer = get_option('vulkan-layers').contains('device-select')
with_vulkan_screenshot_layer = get_option('vulkan-layers').contains('screenshot')
with_vulkan_vram_report_limit_layer = get_option('vulkan-layers').contains('vram-report-limit')
with_vulkan_anti_lag_layer = get_option('vulkan-layers').contains('anti-lag')

with_tools = get_option('tools')
if with_tools.contains('all')
  with_tools = [
    'amd', 'asahi', 'dlclose-skip', 'drm-shim', 'etnaviv',
    'freedreno', 'glsl', 'imagination', 'intel', 'intel-ui',
    'lima', 'nir', 'nouveau', 'panfrost', 'gamma', 'zink',
  ]
endif

with_any_vulkan_layers = get_option('vulkan-layers').length() != 0
with_intel_tools = with_tools.contains('intel') or with_tools.contains('intel-ui')
with_imgui = with_intel_tools or with_vulkan_overlay_layer or with_tools.contains('gamma')

dri_drivers_path = get_option('dri-drivers-path')
if dri_drivers_path == ''
  dri_drivers_path = join_paths(get_option('prefix'), get_option('libdir'), 'dri')
endif

gbm_backends_path = get_option('gbm-backends-path')
if gbm_backends_path == ''
  gbm_backends_path = join_paths(get_option('prefix'), get_option('libdir'), 'gbm')
endif

with_opengl = get_option('opengl')
with_gles1 = get_option('gles1').allowed()
with_gles2 = get_option('gles2').allowed()

pre_args += '-DHAVE_OPENGL=@0@'.format(with_opengl.to_int())
pre_args += '-DHAVE_OPENGL_ES_1=@0@'.format(with_gles1.to_int())
pre_args += '-DHAVE_OPENGL_ES_2=@0@'.format(with_gles2.to_int())

with_any_opengl = with_opengl or with_gles1 or with_gles2

system_has_kms_drm = ['openbsd', 'netbsd', 'freebsd', 'gnu/kfreebsd', 'dragonfly', 'linux', 'sunos', 'android', 'managarm'].contains(host_machine.system())

gallium_drivers = get_option('gallium-drivers')
if gallium_drivers.contains('auto')
  if system_has_kms_drm
    if ['x86', 'x86_64'].contains(host_machine.cpu_family())
      gallium_drivers = [
        'r300', 'r600', 'radeonsi', 'nouveau', 'virgl', 'svga', 'llvmpipe', 'softpipe',
        'iris', 'crocus', 'i915', 'zink'
      ]
    elif ['aarch64'].contains(host_machine.cpu_family())
      gallium_drivers = [
        'v3d', 'vc4', 'freedreno', 'etnaviv', 'nouveau', 'svga',
        'tegra', 'virgl', 'lima', 'panfrost', 'llvmpipe', 'softpipe', 'iris',
        'zink', 'asahi'
      ]
    elif ['arm'].contains(host_machine.cpu_family())
      gallium_drivers = [
        'v3d', 'vc4', 'freedreno', 'etnaviv', 'nouveau', 'svga',
        'tegra', 'virgl', 'lima', 'panfrost', 'llvmpipe', 'softpipe', 'iris',
        'zink'
      ]
    else
      gallium_drivers = ['amd', 'swrast']
    endif
  else
    gallium_drivers = ['llvmpipe', 'softpipe']
  endif
elif gallium_drivers.contains('all')
   gallium_drivers = [
     'r300', 'r600', 'radeonsi', 'crocus', 'v3d', 'vc4', 'freedreno', 'etnaviv', 'i915',
     'nouveau', 'svga', 'tegra', 'virgl', 'lima', 'panfrost', 'llvmpipe', 'softpipe', 'iris',
     'zink', 'd3d12', 'asahi', 'rocket', 'ethosu'
   ]
endif

with_gallium_radeonsi = gallium_drivers.contains('radeonsi')
with_gallium_r300 = gallium_drivers.contains('r300')
with_gallium_r600 = gallium_drivers.contains('r600')
with_gallium_nouveau = gallium_drivers.contains('nouveau')
with_gallium_freedreno = gallium_drivers.contains('freedreno')
with_gallium_softpipe = gallium_drivers.contains('softpipe')
with_gallium_llvmpipe = gallium_drivers.contains('llvmpipe')
with_gallium_vc4 = gallium_drivers.contains('vc4')
with_gallium_v3d = gallium_drivers.contains('v3d')
with_gallium_panfrost = gallium_drivers.contains('panfrost')
with_gallium_etnaviv = gallium_drivers.contains('etnaviv')
with_gallium_tegra = gallium_drivers.contains('tegra')
with_gallium_crocus = gallium_drivers.contains('crocus')
with_gallium_iris = gallium_drivers.contains('iris')
with_gallium_i915 = gallium_drivers.contains('i915')
with_gallium_svga = gallium_drivers.contains('svga')
with_gallium_virgl = gallium_drivers.contains('virgl')
with_gallium_lima = gallium_drivers.contains('lima')
with_gallium_zink = gallium_drivers.contains('zink')
with_gallium_d3d12 = gallium_drivers.contains('d3d12')
with_gallium_asahi = gallium_drivers.contains('asahi')

foreach gallium_driver : gallium_drivers
  pre_args += '-DHAVE_@0@'.format(gallium_driver.to_upper())
endforeach

with_gallium_swrast = with_gallium_softpipe or with_gallium_llvmpipe
if with_gallium_swrast
  pre_args += '-DHAVE_SWRAST'
endif

with_gallium = gallium_drivers.length() != 0
with_gallium_drm = system_has_kms_drm and [
  with_gallium_asahi,
  with_gallium_etnaviv,
  with_gallium_freedreno,
  with_gallium_lima,
  with_gallium_panfrost,
  with_gallium_v3d,
  with_gallium_vc4,
].contains(true)

# Variable combinada requerida por src/meson.build
with_gallium_or_lvp = with_gallium or with_gallium_swrast

_vulkan_drivers = get_option('vulkan-drivers')
if _vulkan_drivers.contains('auto')
  if system_has_kms_drm
    if ['x86', 'x86_64'].contains(host_machine.cpu_family())
      _vulkan_drivers = ['amd', 'intel', 'intel_hasvk', 'nouveau', 'swrast']
    elif ['aarch64'].contains(host_machine.cpu_family())
      _vulkan_drivers = ['swrast', 'intel', 'panfrost', 'freedreno', 'asahi']
    else
      _vulkan_drivers = ['amd', 'swrast']
    endif
  else
    _vulkan_drivers = []
  endif
elif _vulkan_drivers.contains('all')
   _vulkan_drivers = ['amd', 'intel', 'intel_hasvk', 'swrast',
                      'freedreno', 'panfrost', 'virtio', 'broadcom',
                      'imagination', 'microsoft-experimental',
                      'nouveau', 'asahi', 'gfxstream', 'kosmickrisp']
endif

with_intel_vk = _vulkan_drivers.contains('intel')
with_intel_hasvk = _vulkan_drivers.contains('intel_hasvk')
with_amd_vk = _vulkan_drivers.contains('amd')
with_freedreno_vk = _vulkan_drivers.contains('freedreno')
with_panfrost_vk = _vulkan_drivers.contains('panfrost')
with_swrast_vk = _vulkan_drivers.contains('swrast')
with_virtio_vk = _vulkan_drivers.contains('virtio')
with_broadcom_vk = _vulkan_drivers.contains('broadcom')
with_imagination_vk = _vulkan_drivers.contains('imagination')
with_microsoft_vk = _vulkan_drivers.contains('microsoft-experimental')
with_nouveau_vk = _vulkan_drivers.contains('nouveau')
with_asahi_vk = _vulkan_drivers.contains('asahi')
with_gfxstream_vk = _vulkan_drivers.contains('gfxstream')
with_wrapper_vk = _vulkan_drivers.contains('wrapper')
with_any_vk = _vulkan_drivers.length() != 0

with_dri = false
if with_gallium and (system_has_kms_drm or host_machine.system() == 'darwin')
  _glx = get_option('glx')
  _egl = get_option('egl')
  if _glx == 'dri' or _egl.enabled() or (_glx == 'disabled' and _egl.allowed())
    with_dri = true
  endif
endif

with_vulkan_beta = get_option('vulkan-beta')
if with_vulkan_beta
  pre_args += '-DVK_ENABLE_BETA_EXTENSIONS'
endif

_codecs = get_option('video-codecs')
patent_codecs = ['vc1dec', 'h264dec', 'h264enc', 'h265dec', 'h265enc']
free_codecs = ['av1dec', 'av1enc', 'vp9dec', 'mpeg12dec', 'jpegdec']
all_codecs = patent_codecs + free_codecs

if _codecs.contains('all')
  _codecs = all_codecs
elif _codecs.contains('all_free')
  _codecs += free_codecs
endif

foreach c : all_codecs
   pre_args += '-DVIDEO_CODEC_@0@=@1@'.format(c.to_upper(), _codecs.contains(c).to_int())
endforeach

_platforms = get_option('platforms')
if _platforms.contains('auto')
  if system_has_kms_drm
    _platforms = ['x11', 'wayland']
  else
    _platforms = ['x11']
  endif
endif

with_platform_android = _platforms.contains('android')
with_platform_x11 = _platforms.contains('x11')
with_platform_wayland = _platforms.contains('wayland')
with_platform_haiku = _platforms.contains('haiku')
with_platform_windows = _platforms.contains('windows')
with_platform_macos = _platforms.contains('macos')

# --- VARIABLES REQUERIDAS POR LOS SUBDIRECTORIOS ---
with_android_stub = get_option('android-stub')
with_glvnd = false
with_shared_glapi = false
with_gbm = false
with_egl = false
with_perfetto = false
with_gpuvis = false
with_sysprof = false
use_xmlconfig = false
with_dri_platform = ''

prog_python = import('python').find_installation('python3')
devenv = environment()

if host_machine.system() == 'windows'
  dep_thread = null_dep
else
  dep_thread = dependency('threads')
endif

dep_clock = null_dep
dep_atomic = null_dep
dep_m = cc.find_library('m', required : false)
dep_valgrind = null_dep
dep_unwind = null_dep
dep_futex = null_dep
dep_regex = null_dep
dep_llvm = null_dep

# Dependencias XCB/X11 y externas requeridas por submódulos de Mesa
dep_xcb = null_dep
dep_xcb_dri3 = null_dep
dep_xcb_glx = null_dep
dep_x11_xcb = null_dep
dep_xcb_randr = null_dep
dep_xcb_xrandr = null_dep
dep_xcb_present = null_dep
dep_xcb_sync = null_dep
dep_xcb_xfixes = null_dep
dep_xcb_shm = null_dep
dep_xshmfence = null_dep
dep_x11 = null_dep
dep_spirv_tools = null_dep

with_c11_threads = false
if cc.has_function('thrd_create', prefix: '#include <threads.h>')
  if with_platform_android
    pre_args += '-DHAVE_THRD_CREATE'
    with_c11_threads = true
  endif
endif

with_glx = get_option('glx')
if with_glx == 'auto'
  if not with_opengl or with_platform_android
    with_glx = 'disabled'
  elif with_dri or with_gallium
    with_glx = 'dri'
  else
    with_glx = 'disabled'
  endif
endif

with_gbm = get_option('gbm') \
  .require(system_has_kms_drm, error_message : 'GBM only supports DRM/KMS platforms') \
  .disable_auto_if(not with_dri) \
  .allowed()

with_egl = get_option('egl') \
  .require(with_platform_windows or with_platform_haiku or with_dri or with_platform_android, error_message : 'EGL requires DRI, Windows or Android') \
  .allowed()

if with_gbm
  pre_args += '-DHAVE_LIBGBM'
endif

if with_egl
  _platforms += 'surfaceless'
  if with_gbm and not with_platform_android
    _platforms += 'drm'
  endif
  egl_native_platform = get_option('egl-native-platform')
  if egl_native_platform.contains('auto')
    egl_native_platform = 'surfaceless'
  endif
endif

if 'x11' in _platforms
  _platforms += 'xcb'
endif

foreach platform : _platforms
  pre_args += '-DHAVE_@0@_PLATFORM'.format(platform.to_upper())
endforeach

if with_platform_android and get_option('platform-sdk-version') >= 29
  c_cpp_args += '-fno-emulated-tls'
  add_project_link_arguments('-Wl,-plugin-opt=-emulated-tls=0', language: ['c', 'cpp'])
endif

# Desactivación estricta y limpia de CLC y LLVM
with_clc = false

with_llvm = with_llvm \
  .enable_if(with_clc, error_message : 'CLC requires LLVM')

dep_clc = null_dep

dep_dl = null_dep
if host_machine.system() != 'windows'
  if not cc.has_function('dlopen')
    dep_dl = cc.find_library('dl', required : false)
  endif
endif

dep_zlib = dependency('zlib', required : get_option('zlib'), allow_fallback : true)
dep_zstd = dependency('libzstd', required : get_option('zstd'))

dep_libdrm = dependency('libdrm', required : false)
dep_expat = dependency('expat', required : get_option('expat'), allow_fallback : true)

add_project_arguments(pre_args, language : ['c', 'cpp'])
add_project_arguments(c_cpp_args, language : ['c', 'cpp'])
add_project_arguments(c_args, language : ['c'])
add_project_arguments(cpp_args, language : ['cpp'])

subdir('include')
subdir('bin')
subdir('src')

summary({'Prefix': get_option('prefix')}, section: 'Directories')
