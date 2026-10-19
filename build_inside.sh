#!/bin/bash
set -e

# Uso: build_inside.sh [BUILD_DIR]      ARCH=aarch64 (64 bits) | arm (32 bits)
BUILD_DIR="${1:-${BUILD_DIR:-build}}"
ARCH="${ARCH:-aarch64}"

export PYTHONPATH="/workspace/src/compiler:/workspace/src/compiler/nir:$PYTHONPATH"

# ---------- NDK ----------
NDK_DIR=$(find /home/builder/lib -maxdepth 2 -name "android-ndk*" 2>/dev/null | head -n 1)
[ -z "$NDK_DIR" ] && NDK_DIR="/home/builder/lib/android-ndk-r30"
NDK_BIN="${NDK_DIR}/toolchains/llvm/prebuilt/linux-x86_64/bin"
NDK_SYSROOT="${NDK_DIR}/toolchains/llvm/prebuilt/linux-x86_64/sysroot"

if [ "$ARCH" = "arm" ]; then
    TRIPLE="armv7a-linux-androideabi30"
    SYSLIB="arm-linux-androideabi"
    CPU_FAMILY="arm"; CPU="armv7"
    LIBDIR_TAG="armhf"
else
    TRIPLE="aarch64-linux-android30"
    SYSLIB="aarch64-linux-android"
    CPU_FAMILY="aarch64"; CPU="armv8-a"
    LIBDIR_TAG="aarch64"
fi

# ---------- PARCHES ----------
# El #error dentro de os_misc.c dice "os_sysinfo.c" (por eso tu sed no coincidía).
# Reemplazo por regex cualquier "#error unexpected platform..." por un fallback.
python3 - <<'PY'
import re
for f in ("src/util/os_misc.c", "src/util/os_sysinfo.c"):
    try:
        s = open(f).read()
    except FileNotFoundError:
        continue
    n = re.sub(r'^[ \t]*#[ \t]*error[ \t]+unexpected platform.*$',
               '   /* patched: unsupported platform fallback */\n   return false;',
               s, flags=re.M)
    if n != s:
        open(f, "w").write(n)
        print(f"-> Parcheado {f}")
PY

# u_gralloc: los backends imapper4/5 necesitan cabeceras AIDL del AOSP que el NDK no trae.
# Se vacían y se deja un stub que devuelve NULL (Mesa usa el gralloc fallback).
python3 - <<'PY'
import glob
d = "src/util/u_gralloc/"
for f in glob.glob(d + "*imapper*api*.cpp"):
    open(f, "w").write("/* vaciado: sin cabeceras AIDL en NDK */\n")
    print(f"-> Vaciado {f}")
try:
    open(d + "u_gralloc_imapper5_api.cpp", "w").write(
        '#include "u_gralloc_internal.h"\n'
        'extern "C" struct u_gralloc *u_gralloc_imapper_api_create(void) { return NULL; }\n')
except FileNotFoundError:
    pass
PY

# vk_wrapper_features_gen.py: el vk.xml nuevo tiene features duplicadas entre KHR/EXT/NV
# (micromap, deviceFault, rayTracingInvocationReorder). Se convierte el assert en aviso.
python3 - <<'PY'
import re
f = "src/vulkan/wrapper/vk_wrapper_features_gen.py"
try:
    s = open(f).read()
except FileNotFoundError:
    s = None
if s:
    n = re.sub(r"^([ \t]*)assert len\(diagnostics\) == 0,.*$",
               r"\1if diagnostics:\n\1    import sys; print('WARN (ignorado):\\n' + '\\n'.join(diagnostics), file=sys.stderr)",
               s, flags=re.M)
    if n != s:
        open(f, "w").write(n)
        print(f"-> Parcheado {f}")
PY

# wrapper_physical_device.c: usa API que Mesa 26 ya no tiene.
# - engine_name: solo se elimina la asignación si tu wsi_common.h NO tiene el campo (con tu header actual sí existe y se conserva).
# - VK_QUEUE_PRESENT_BIT no es flag real de Vulkan -> bit libre 0x80 (NO 0: el wrapper lo usa como marca de "puede presentar").
python3 - <<'PY'
import re
f = "src/vulkan/wrapper/wrapper_physical_device.c"
try:
    s = open(f).read()
except FileNotFoundError:
    s = None
try:
    hdr = open("src/vulkan/wsi/wsi_common.h").read()
except FileNotFoundError:
    hdr = ""
if s and "engine_name" not in hdr:
    n = re.sub(r"^[ \t]*pdevice->wsi_device\.engine_name\s*=.*$",
               "   /* patched: wsi_device.engine_name removido en Mesa 26 */",
               s, flags=re.M)
    if n != s:
        open(f, "w").write(n)
        print(f"-> Parcheado {f}")
PY

# wsi_common.c: diagnóstico + soporte de presentación robusto.
# - Si iface->get_support falla (p.ej. DRI3 ausente en Termux:X11) no se pierde la cola gráfica.
# - Si queue_supports_blit queda en 0, se marcan todas las colas.
# - Imprime [WSI] support: ... en stderr para diagnosticar.
python3 - <<'PY'
import re
f = "src/vulkan/wsi/wsi_common.c"
try:
    s = open(f).read()
except FileNotFoundError:
    s = None
if s and "[WSI] support:" not in s:
    new_fn = r'''VKAPI_ATTR VkResult VKAPI_CALL
wsi_GetPhysicalDeviceSurfaceSupportKHR(VkPhysicalDevice physicalDevice,
                                       uint32_t queueFamilyIndex,
                                       VkSurfaceKHR _surface,
                                       VkBool32 *pSupported)
{
   VK_FROM_HANDLE(vk_physical_device, device, physicalDevice);
   ICD_FROM_HANDLE(VkIcdSurfaceBase, surface, _surface);
   struct wsi_device *wsi_device = device->wsi_device;
   struct wsi_interface *iface = wsi_device->wsi[surface->platform];

   bool has_graphics =
      (wsi_device->queue_supports_blit & BITFIELD64_BIT(queueFamilyIndex)) != 0;

   VkResult res = iface ? iface->get_support(surface, wsi_device,
                                             queueFamilyIndex, pSupported)
                        : VK_ERROR_SURFACE_LOST_KHR;

   fprintf(stderr, "[WSI] support: platform=%d qfi=%u iface=%p res=%d "
           "native=%u blit_mask=0x%llx qcount=%u\n",
           (int)surface->platform, queueFamilyIndex, (void *)iface, (int)res,
           res == VK_SUCCESS ? (unsigned)*pSupported : 0u,
           (unsigned long long)wsi_device->queue_supports_blit,
           wsi_device->queue_family_count);

   if (res != VK_SUCCESS) {
      *pSupported = has_graphics;
      return has_graphics ? VK_SUCCESS : res;
   }
   *pSupported = *pSupported || has_graphics;
   return VK_SUCCESS;
}
'''
    s, n1 = re.subn(
        r'VKAPI_ATTR VkResult VKAPI_CALL\s*\nwsi_GetPhysicalDeviceSurfaceSupportKHR\(.*?\n}\n',
        lambda m: new_fn, s, count=1, flags=re.S)
    marker = "   for (VkExternalSemaphoreHandleTypeFlags handle_type = 1;"
    fb = ("   if (!wsi->queue_supports_blit && wsi->queue_family_count)\n"
          "      wsi->queue_supports_blit = BITFIELD64_MASK(wsi->queue_family_count);\n\n")
    n2 = 0
    if marker in s:
        s = s.replace(marker, fb + marker, 1); n2 = 1
    open(f, "w").write(s)
    print(f"-> wsi_common.c parcheado (SurfaceSupport={n1}, fallback blit={n2})")
PY

# Migas de pan [WSI] (stderr -> archivo de diagnóstico) para ver hasta dónde llega la presentación.
python3 - <<'PY'
def patch(path, pairs):
    try:
        t = open(path).read()
    except FileNotFoundError:
        return
    o = t
    for old, new in pairs:
        if old in t and new not in t:
            t = t.replace(old, new, 1)
    if t != o:
        open(path, "w").write(t)
        print("-> trazas [WSI] añadidas en " + path)

patch("src/vulkan/wsi/wsi_common.c", [
(r'''   ICD_FROM_HANDLE(VkIcdSurfaceBase, surface, pCreateInfo->surface);
   struct wsi_device *wsi_device = device->physical->wsi_device;
''', r'''   ICD_FROM_HANDLE(VkIcdSurfaceBase, surface, pCreateInfo->surface);
   struct wsi_device *wsi_device = device->physical->wsi_device;
   fprintf(stderr, "[WSI] CreateSwapchain platform=%d fmt=%d %ux%u minImages=%u mode=%d iface=%p\n",
           (int)surface->platform, (int)pCreateInfo->imageFormat,
           pCreateInfo->imageExtent.width, pCreateInfo->imageExtent.height,
           pCreateInfo->minImageCount, (int)pCreateInfo->presentMode,
           (void *)wsi_device->wsi[surface->platform]);
'''),
(r'''                                             &info, alloc,
                                             &swapchain);
   if (result != VK_SUCCESS)
      return result;
''', r'''                                             &info, alloc,
                                             &swapchain);
   fprintf(stderr, "[WSI] create_swapchain -> %d\n", (int)result);
   if (result != VK_SUCCESS)
      return result;
'''),
])

patch("src/vulkan/wsi/wsi_common_x11.c", [
(r'''   assert(pCreateInfo->sType == VK_STRUCTURE_TYPE_XCB_SURFACE_CREATE_INFO_KHR);
''', r'''   assert(pCreateInfo->sType == VK_STRUCTURE_TYPE_XCB_SURFACE_CREATE_INFO_KHR);
   fprintf(stderr, "[WSI] CreateXcbSurface conn=%p window=%u\n",
           (void *)pCreateInfo->connection, (unsigned)pCreateInfo->window);
'''),
(r'''   assert(pCreateInfo->sType == VK_STRUCTURE_TYPE_XLIB_SURFACE_CREATE_INFO_KHR);
''', r'''   assert(pCreateInfo->sType == VK_STRUCTURE_TYPE_XLIB_SURFACE_CREATE_INFO_KHR);
   fprintf(stderr, "[WSI] CreateXlibSurface dpy=%p window=%lu\n",
           (void *)pCreateInfo->dpy, (unsigned long)pCreateInfo->window);
'''),
])
PY

# Superficie X11: trazas de por qué get_visualtype_for_window falla y no abortar la creación de la superficie.
python3 - <<'PY'
f = "src/vulkan/wsi/wsi_common_x11.c"
try:
    t = open(f).read()
except FileNotFoundError:
    t = None
if t and "[WSI] visual:" not in t:
    o = t
    t = t.replace(r'''   if (attrib == NULL || tree == NULL) {
      free(attrib);''', r'''   fprintf(stderr, "[WSI] visual: conn=%p conn_err=%d window=%u tree=%p attrib=%p\n",
           (void *)conn, xcb_connection_has_error(conn), (unsigned)window,
           (void *)tree, (void *)attrib);
   if (attrib == NULL || tree == NULL) {
      free(attrib);''', 1)
    t = t.replace(r'''   xcb_screen_t *screen = get_screen_for_root(conn, root);
   if (screen == NULL)
      return NULL;
''', r'''   xcb_screen_t *screen = get_screen_for_root(conn, root);
   fprintf(stderr, "[WSI] visual: root=%u screen=%p visual_id=%u\n",
           (unsigned)root, (void *)screen, (unsigned)visual_id);
   if (screen == NULL)
      return NULL;
''', 1)
    # no abortar si no se encuentra el visual: crear la superficie igualmente (sin alpha)
    t = t.replace(r'''   if (!visual)
      return VK_ERROR_OUT_OF_HOST_MEMORY;
''', r'''   if (!visual)
      fprintf(stderr, "[WSI] WARNING: visual no encontrado, se crea la superficie sin alpha\n");
''')
    t = t.replace("surface->has_alpha = visual_has_alpha(visual, visual_depth);",
                  "surface->has_alpha = visual ? visual_has_alpha(visual, visual_depth) : false;")
    if t != o:
        open(f, "w").write(t)
        print("-> wsi_common_x11.c: trazas de visual + superficie tolerante")
PY

# Más migas [WSI] (entrada de cada consulta de superficie y resultado de crear la superficie)
python3 - <<'PY'
def entry_trace(t, name):
    i = t.find(name + "(")
    if i < 0:
        return t
    j = t.find("\n{\n", i)
    if j < 0:
        return t
    ins = '\n   fprintf(stderr, "[WSI] -> %s\\n");' % name
    if ins in t:
        return t
    return t[:j + 3] + ins[1:] + "\n" + t[j + 3:]

f = "src/vulkan/wsi/wsi_common.c"
try:
    t = open(f).read()
except FileNotFoundError:
    t = None
if t and "[WSI] -> wsi_GetPhysicalDeviceSurfaceFormatsKHR" not in t:
    for n in ("wsi_GetPhysicalDeviceSurfaceCapabilitiesKHR",
              "wsi_GetPhysicalDeviceSurfaceCapabilities2KHR",
              "wsi_GetPhysicalDeviceSurfaceFormatsKHR",
              "wsi_GetPhysicalDeviceSurfaceFormats2KHR",
              "wsi_GetPhysicalDeviceSurfacePresentModesKHR"):
        t = entry_trace(t, n)
    open(f, "w").write(t)
    print("-> wsi_common.c: trazas de entrada")

f = "src/vulkan/wsi/wsi_common_x11.c"
try:
    t = open(f).read()
except FileNotFoundError:
    t = None
if t and "[WSI] surface creada" not in t:
    for k in ("xlib", "xcb"):
        old = "   *pSurface = VkIcdSurfaceBase_to_handle(&surface->%s.base);\n   return VK_SUCCESS;" % k
        new = ("   *pSurface = VkIcdSurfaceBase_to_handle(&surface->%s.base);\n"
               "   fprintf(stderr, \"[WSI] surface creada (%s) has_alpha=%%d\\n\", (int)surface->has_alpha);\n"
               "   return VK_SUCCESS;") % (k, k)
        t = t.replace(old, new, 1)
    for n in ("wsi_GetPhysicalDeviceXcbPresentationSupportKHR",
              "wsi_GetPhysicalDeviceXlibPresentationSupportKHR"):
        i = t.find(n + "(")
        j = t.find("\n{\n", i) if i >= 0 else -1
        if j >= 0:
            t = t[:j + 3] + '   fprintf(stderr, "[WSI] -> %s\\n");\n' % n + t[j + 3:]
    open(f, "w").write(t)
    print("-> wsi_common_x11.c: trazas de creación y soporte de presentación")
PY

if [ -f src/vulkan/wsi/wsi_common.c ] && ! grep -q "\[CRASH\]" src/vulkan/wsi/wsi_common.c; then
cat >> src/vulkan/wsi/wsi_common.c <<'CEOF'

/* ---- [CRASH] backtrace en SIGABRT (solo si WRAPPER_DIAG está activo) ---- */
#include <signal.h>
#include <dlfcn.h>
#include <unwind.h>
#include <string.h>

static _Unwind_Reason_Code
wsi_crash_cb(struct _Unwind_Context *ctx, void *arg)
{
   int *n = (int *)arg;
   uintptr_t pc = (uintptr_t)_Unwind_GetIP(ctx);
   if (!pc)
      return _URC_END_OF_STACK;
   Dl_info info;
   if (dladdr((void *)pc, &info) && info.dli_fname) {
      const char *base = strrchr(info.dli_fname, '/');
      fprintf(stderr, "[CRASH] #%d pc=%p %s+0x%lx %s\n", *n, (void *)pc,
              base ? base + 1 : info.dli_fname,
              (unsigned long)(pc - (uintptr_t)info.dli_fbase),
              info.dli_sname ? info.dli_sname : "");
   } else {
      fprintf(stderr, "[CRASH] #%d pc=%p\n", *n, (void *)pc);
   }
   (*n)++;
   return *n >= 40 ? _URC_END_OF_STACK : _URC_NO_REASON;
}

static void
wsi_crash_handler(int sig)
{
   int n = 0;
   fprintf(stderr, "[CRASH] signal %d, backtrace:\n", sig);
   _Unwind_Backtrace(wsi_crash_cb, &n);
   signal(sig, SIG_DFL);
   raise(sig);
}

__attribute__((constructor)) static void
wsi_crash_install(void)
{
   const char *d = getenv("WRAPPER_DIAG");
   if (d && atoi(d))
      signal(SIGABRT, wsi_crash_handler);   /* solo SIGABRT: Wine usa SIGSEGV */
}
CEOF
echo "-> wsi_common.c: handler [CRASH] en SIGABRT añadido"
fi

# wsi_common.h: asegurar que struct wsi_device tiene el campo que usa wsi_common_drm.c
# (si tu copia local del header no está en el repo, el CI compila con otra versión).
python3 - <<'PY'
import re
f = "src/vulkan/wsi/wsi_common.h"
try:
    t = open(f).read()
except FileNotFoundError:
    t = None
if t and "cached_sync_file_import_export_result" not in t:
    n = re.sub(r"(struct wsi_device \{.*?)(\n\};)",
               lambda m: m.group(1) + "\n   uint32_t cached_sync_file_import_export_result;" + m.group(2),
               t, count=1, flags=re.S)
    if n != t:
        open(f, "w").write(n)
        print("-> wsi_common.h: añadido cached_sync_file_import_export_result")
PY

# wsi_common_x11.c: ver_reply puede ser NULL si el servidor X (Termux:X11/Bannerlator) no responde la versión.
python3 - <<'PY'
f = "src/vulkan/wsi/wsi_common_x11.c"
try:
    t = open(f).read()
except FileNotFoundError:
    t = None
if t:
    o = t
    t = t.replace("has_present_v1_2 =\n        (ver_reply->major_version", "has_present_v1_2 = ver_reply != NULL &&\n        (ver_reply->major_version")
    t = t.replace("has_present_v1_4 =\n        (ver_reply->major_version", "has_present_v1_4 = ver_reply != NULL &&\n        (ver_reply->major_version")
    t = t.replace("wsi_conn->has_xfixes = (ver_reply->major_version >= 2);", "wsi_conn->has_xfixes = (ver_reply != NULL && ver_reply->major_version >= 2);")
    if t != o:
        open(f, "w").write(t)
        print("-> wsi_common_x11.c parcheado (NULL checks)")
PY

# ---------- LIBS X11/XCB/DRM (el meson.build las pone como null_dep, hay que enlazarlas a mano) ----------
TERMUX_LIB="/data/data/com.termux/files/usr/lib"
XLINK=""
for f in "${TERMUX_LIB}"/libxcb*.so "${TERMUX_LIB}"/libX11.so "${TERMUX_LIB}"/libX11-xcb.so "${TERMUX_LIB}"/libxshmfence.so "${TERMUX_LIB}"/libdrm.so; do
    [ -f "$f" ] || continue
    b=$(basename "$f" .so); XLINK="${XLINK}, '-l${b#lib}'"
done
echo "-> Libs X11/XCB enlazadas: ${XLINK}"

# Mesa define HAVE_LIBDRM / HAVE_X11_DRM en su meson.build original; el tuyo no.
# Sin ellos, wsi_common_x11.c cae en UNREACHABLE("X11 DRM support missing!") al crear
# el swapchain (y wsi_common.c no compila el camino DRM). Por eso Vulkan no presenta.
DRM_DEFS=""
if [ -f "/data/data/com.termux/files/usr/include/libdrm/xf86drm.h" ] || [ -f "/data/data/com.termux/files/usr/include/xf86drm.h" ]; then
    DRM_DEFS=", '-DHAVE_LIBDRM=1', '-DHAVE_X11_DRM=1'"
    echo "-> libdrm encontrado: se definen HAVE_LIBDRM y HAVE_X11_DRM"
else
    echo "-> [AVISO] no hay xf86drm.h de libdrm; el WSI X11 no podrá presentar"
fi

# ---------- CROSS FILE ----------
rm -rf "${BUILD_DIR}"
CROSS="${BUILD_DIR}_cross_file.txt"
rm -f "${CROSS}"

COMMON_ARGS="'-include', 'sys/sysmacros.h', '-include', 'fcntl.h', '-include', 'unistd.h', '-D_GNU_SOURCE', '-DVK_QUEUE_PRESENT_BIT=0x80', '-DHAVE_FFSLL=0'${DRM_DEFS}, '-DDETECT_OS_LINUX=1', '-DDETECT_OS_UNIX=1', '-DDETECT_OS_ANDROID=1', '-DVK_USE_PLATFORM_ANDROID_KHR=1', '-I/workspace/workspace_headers', '-I/data/data/com.termux/files/usr/include', '-I/data/data/com.termux/files/usr/include/libdrm'"

cat << EOF > "${CROSS}"
[binaries]
c = '${NDK_BIN}/${TRIPLE}-clang'
cpp = '${NDK_BIN}/${TRIPLE}-clang++'
ar = '${NDK_BIN}/llvm-ar'
strip = '${NDK_BIN}/llvm-strip'
pkg-config = 'pkg-config'

[built-in options]
c_args = [${COMMON_ARGS}]
cpp_args = [${COMMON_ARGS}]
c_link_args = ['-L/data/data/com.termux/files/usr/lib', '-L${NDK_SYSROOT}/usr/lib/${SYSLIB}/30', '-Wl,--no-as-needed', '-ldl', '-lm', '-llog', '-landroid'${XLINK}]
cpp_link_args = ['-L/data/data/com.termux/files/usr/lib', '-L${NDK_SYSROOT}/usr/lib/${SYSLIB}/30', '-Wl,--no-as-needed', '-ldl', '-lm', '-llog', '-landroid'${XLINK}, '-static-libstdc++']

[host_machine]
system = 'android'
cpu_family = '${CPU_FAMILY}'
cpu = '${CPU}'
endian = 'little'

[properties]
pkg_config_libdir = ['/data/data/com.termux/files/usr/lib/pkgconfig', '/data/data/com.termux/files/usr/share/pkgconfig']
EOF

# ---------- MESON ----------
meson subprojects download || true

meson setup "${BUILD_DIR}" --cross-file "${CROSS}" \
  -Db_sanitize=none \
  -Db_lundef=false \
  -Dcpp_rtti=false \
  -Dllvm=disabled \
  -Dshared-llvm=disabled \
  -Dplatforms=x11,android \
  -Dandroid-stub=true \
  -Dplatform-sdk-version=30 \
  -Dgallium-drivers= \
  -Dvulkan-drivers=wrapper \
  -Dopengl=false -Dgles1=disabled -Dgles2=disabled \
  -Degl=disabled -Dgbm=disabled -Dglx=disabled \
  -Dxmlconfig=disabled \
  -Dzlib=disabled -Dzstd=disabled

echo "-> Compilando con Ninja..."
ninja -C "${BUILD_DIR}"

echo "=== WSI: objetos compilados (wsi_common_drm debe aparecer) ==="
find "${BUILD_DIR}" -name "wsi_common*.o" | sed 's|.*/||' | sort
echo "=== Unidades con HAVE_X11_DRM en compile_commands ==="
grep -c "HAVE_X11_DRM" "${BUILD_DIR}/compile_commands.json" || true

# ---------- LOCALIZAR WRAPPER ----------
SRC="${BUILD_DIR}/src/vulkan/wrapper/libvulkan_wrapper.so"
if [ ! -f "$SRC" ]; then
    SRC=$(find "${BUILD_DIR}" -name "libvulkan_wrapper*.so" | head -n 1)
fi
[ -f "$SRC" ] || { echo "-> [ERROR] No se encontró libvulkan_wrapper.so"; exit 1; }
echo "-> Wrapper: $SRC"
cp "$SRC" "${BUILD_DIR}/libvulkan_wrapper.so.unstripped"
"${NDK_BIN}/llvm-strip" --strip-unneeded -o "${BUILD_DIR}/libvulkan_wrapper.so" "${BUILD_DIR}/libvulkan_wrapper.so.unstripped"

# ---------- DIAGNÓSTICO (por qué no carga en Bannerlator) ----------
OUT="${BUILD_DIR}/libvulkan_wrapper.so"
echo "=== NEEDED (deben existir en el rootfs/dispositivo) ==="
"${NDK_BIN}/llvm-readelf" -d "$OUT" | grep NEEDED || true
echo "=== Símbolos ICD exportados (deben aparecer los 3) ==="
"${NDK_BIN}/llvm-nm" -D --defined-only "$OUT" | grep -E "vk_icd|vkGetInstanceProcAddr" || echo "!! NO exporta vk_icdGetInstanceProcAddr"
echo "=== Símbolos sin resolver (sin @LIBC ni AAudio/AHardwareBuffer/ANativeWindow) ==="
"${NDK_BIN}/llvm-nm" -D --undefined-only "$OUT" | awk '{print $2}' \
  | grep -vE "@|^AAudio|^AHardwareBuffer|^ANativeWindow|^ASurface|^__android|^android_" | sort | head -300 || true
echo "=== Resumen NEEDED final ==="
"${NDK_BIN}/llvm-readelf" -d "$OUT" | grep -E "NEEDED|SONAME" || true

# Tabla de símbolos para resolver los offsets [CRASH] (súbela junto al artefacto)
"${NDK_BIN}/llvm-nm" -n -C --defined-only "${BUILD_DIR}/libvulkan_wrapper.so.unstripped" > ./symbols.txt 2>/dev/null || true
echo "-> symbols.txt: $(wc -l < ./symbols.txt 2>/dev/null) símbolos"

# ---------- ROOTFS ----------
ROOTFS_DIR="${BUILD_DIR}/rootfs_export"
rm -rf "${ROOTFS_DIR}"
mkdir -p "${ROOTFS_DIR}/usr/lib" "${ROOTFS_DIR}/usr/share/vulkan/icd.d"
cp "${BUILD_DIR}/libvulkan_wrapper.so" "${ROOTFS_DIR}/usr/lib/"
find "${BUILD_DIR}/subprojects" -name "*.so" -exec cp -f {} "${ROOTFS_DIR}/usr/lib/" \; 2>/dev/null || true

cat << EOF > "${ROOTFS_DIR}/usr/share/vulkan/icd.d/wrapper_icd.${LIBDIR_TAG}.json"
{
    "file_format_version": "1.0.0",
    "ICD": {
        "library_path": "libvulkan_wrapper.so",
        "api_version": "1.3.255"
    }
}
EOF

echo "-> Empaquetando tar.zst..."
( cd "${ROOTFS_DIR}" && tar -cf - usr | zstd -19 -o "../gamenative_driver_rootfs_${ARCH}.tar.zst" )
cp "${BUILD_DIR}/gamenative_driver_rootfs_${ARCH}.tar.zst" "./gamenative_driver_rootfs_${ARCH}.tar.zst"

echo "=== LISTO (${ARCH}) ==="
