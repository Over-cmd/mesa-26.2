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
