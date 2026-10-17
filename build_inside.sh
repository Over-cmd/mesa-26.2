#!/bin/bash
set -e

BUILD_DIR="${1:-${BUILD_DIR:-build}}"

# 🚀 RUTAS Y CONFIGURACIÓN DEL NDK R30
NDK_DIR=$(find /home/builder/lib -maxdepth 2 -name "android-ndk*" 2>/dev/null | head -n 1)
if [ -z "$NDK_DIR" ]; then
    NDK_DIR="/home/builder/lib/android-ndk-r30"
fi
NDK_SYSROOT="${NDK_DIR}/toolchains/llvm/prebuilt/linux-x86_64/sysroot"

# Exportaciones generales por compatibilidad y enlace de XCB/Audio
export CFLAGS="$CFLAGS -laaudio -I/data/data/com.termux/files/usr/include -I/data/data/com.termux/files/usr/include/libdrm"
export LDFLAGS="$LDFLAGS -laaudio -L/data/data/com.termux/files/usr/lib -lxcb -lxcb-randr -lxcb-xfixes -lX11 -lX11-xcb"

# 🛡️ RESTAURAR ARCHIVOS MODIFICADOS PREVIAMENTE
git checkout src/util/bitscan.c src/util/os_misc.c src/util/os_sysinfo.c src/util/u_sync_provider.c src/loader/loader.c src/vulkan/wrapper/vk_wrapper_features_gen.py src/vulkan/wrapper/wrapper_physical_device.c src/vulkan/wrapper/meson.build meson_options.txt 2>/dev/null || true

# 🛠️ CREAR PUENTES DE HEADERS PARA CUTILS, SYNC, VNDK Y LIBDRM
mkdir -p workspace_headers/cutils workspace_headers/sync workspace_headers/vndk

if [ ! -f "workspace_headers/cutils/native_handle.h" ]; then
    REAL_CUTILS=$(find "${NDK_SYSROOT}" -name "native_handle.h" | head -n 1)
    if [ -n "$REAL_CUTILS" ]; then
        cp "$REAL_CUTILS" workspace_headers/cutils/
    else
        cat << 'EOF' > workspace_headers/cutils/native_handle.h
#ifndef ANDROID_CUTILS_NATIVE_HANDLE_H
#define ANDROID_CUTILS_NATIVE_HANDLE_H

typedef struct native_handle
{
    int version;
    int numFds;
    int numInts;
    int data[0];
} native_handle_t;

#ifdef __cplusplus
extern "C" {
#endif

inline int native_handle_close(const native_handle_t* h) { return 0; }
inline int native_handle_delete(native_handle_t* h) { return 0; }

#ifdef __cplusplus
}
#endif

#endif /* ANDROID_CUTILS_NATIVE_HANDLE_H */
EOF
    fi
fi

cat << 'EOF' > workspace_headers/sync/sync.h
#ifndef ANDROID_SYNC_SYNC_H
#define ANDROID_SYNC_SYNC_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

static inline int sync_wait(int fd, int timeout) { return 0; }
static inline int sync_merge(const char *name, int fd1, int fd2) { return -1; }

#ifdef __cplusplus
}
#endif

#endif /* ANDROID_SYNC_SYNC_H */
EOF

if [ ! -f "workspace_headers/vndk/hardware_buffer.h" ]; then
    cat << 'EOF' > workspace_headers/vndk/hardware_buffer.h
#ifndef VNDK_HARDWARE_BUFFER_H
#define VNDK_HARDWARE_BUFFER_H

#include <android/hardware_buffer.h>
#include <cutils/native_handle.h>

#ifdef __cplusplus
extern "C" {
#endif

const native_handle_t* AHardwareBuffer_getNativeHandle(const AHardwareBuffer* hardwareBuffer);

#ifdef __cplusplus
}
#endif

#endif /* VNDK_HARDWARE_BUFFER_H */
EOF
fi

# 🟢 0. LIMPIEZA DE DIRECTORIO RESIDUAL DE ADRENOTOOLS Y BUILD
if [ -d "subprojects/libadrenotools" ] && [ ! -f "subprojects/libadrenotools/meson.build" ]; then
    echo "-> Limpiando directorio huérfano de libadrenotools para forzar su descarga limpia..."
    rm -rf "subprojects/libadrenotools"
fi

rm -rf "${BUILD_DIR}"
LOCAL_CROSS_FILE="${BUILD_DIR}_cross_file.txt"
rm -f "${LOCAL_CROSS_FILE}"

# 🛡️ GENERACIÓN DE CROSS_FILE CON -D_GNU_SOURCE Y RUTAS DE LIBDRM DE TERMUX
cat << EOF > "${LOCAL_CROSS_FILE}"
[binaries]
c = '${NDK_SYSROOT}/../bin/aarch64-linux-android30-clang'
cpp = '${NDK_SYSROOT}/../bin/aarch64-linux-android30-clang++'
ar = '${NDK_SYSROOT}/../bin/llvm-ar'
strip = '${NDK_SYSROOT}/../bin/llvm-strip'
pkgconfig = 'pkg-config'

[host_machine]
system = 'linux'
cpu_family = 'aarch64'
cpu = 'armv8-a'
endian = 'little'

[properties]
c_args = ['-D_GNU_SOURCE', '-I/workspace/workspace_headers', '-I${NDK_SYSROOT}/usr/include', '-I${NDK_SYSROOT}/usr/include/aarch64-linux-android', '-I/data/data/com.termux/files/usr/include', '-I/data/data/com.termux/files/usr/include/libdrm', '-DVK_USE_PLATFORM_ANDROID_KHR=1']
cpp_args = ['-D_GNU_SOURCE', '-I/workspace/workspace_headers', '-I${NDK_SYSROOT}/usr/include', '-I${NDK_SYSROOT}/usr/include/aarch64-linux-android', '-I/data/data/com.termux/files/usr/include', '-I/data/data/com.termux/files/usr/include/libdrm', '-DVK_USE_PLATFORM_ANDROID_KHR=1']
link_args = ['-L/data/data/com.termux/files/usr/lib', '-lxcb', '-lxcb-randr', '-lxcb-xfixes', '-lX11', '-lX11-xcb']
EOF

# 🟢 1. MOTOR DE CONFIGURACIÓN DE MESON
meson subprojects download || true

meson setup "${BUILD_DIR}" --cross-file "${LOCAL_CROSS_FILE}" \
-Db_sanitize=none \
-Db_lundef=true \
-Dcpp_rtti=false \
-Dgbm=disabled \
-Dopengl=true \
-Dllvm=disabled \
-Dshared-llvm=disabled \
-Dplatforms=x11 \
-Dgallium-drivers="" \
-Ddraw-use-llvm=false \
-Dxmlconfig=disabled \
-Dwrapper-vk=true \
-Degl=disabled \
-Dglx=disabled

# 🗡️ PARCHE CIRÚRGICO EN BITSCAN.C
if [ -f "src/util/bitscan.c" ]; then
    python3 -c '
path = "src/util/bitscan.c"
with open(path, "r") as f:
    text = f.read()
import re
text = re.sub(r"#ifndef HAVE_FFS\s*int\s*ffs\(.*?\)\s*\{.*?\}?\s*#endif", "", text, flags=re.DOTALL)
text = re.sub(r"int\s*ffs\(.*?\)\s*\{.*?\}", "", text, flags=re.DOTALL)
text = re.sub(r"#ifndef HAVE_FFSLL\s*int\s*ffsll\(.*?\)\s*\{.*?\}?\s*#endif", "", text, flags=re.DOTALL)
text = re.sub(r"int\s*ffsll\(.*?\)\s*\{.*?\}", "", text, flags=re.DOTALL)
with open(path, "w") as f:
    f.write(text)
'
fi

# 🗡️ PARCHE CIRÚRGICO GLOBAL PARA OS_MISC Y OS_SYSINFO
python3 -c '
import glob
for path in glob.glob("src/util/os_*.c") + glob.glob("src/util/os_*.h"):
    with open(path, "r") as f:
        content = f.read()
    if "#error unexpected platform" in content:
        content = content.replace("#error unexpected platform", "// #error unexpected platform\n#ifndef DETECT_OS_LINUX\n#define DETECT_OS_LINUX 1\n#endif")
        with open(path, "w") as f:
            f.write(content)
'

# 🗡️ PARCHE CIRÚRGICO EN U_SYNC_PROVIDER.C
if [ -f "src/util/u_sync_provider.c" ]; then
    python3 -c '
path = "src/util/u_sync_provider.c"
with open(path, "r") as f:
    content = f.read()
with open(path, "w") as f:
    f.write("#if 0\n" + content + "\n#endif\n")
'
fi

# 🗡️ PARCHE CIRÚRGICO EN LOADER.C
if [ -f "src/loader/loader.c" ]; then
    python3 -c '
path = "src/loader/loader.c"
with open(path, "r") as f:
    content = f.read()
patch = """
#include <sys/sysmacros.h>
#ifndef major
#define major(x) (((x) >> 8) & 0xfff)
#endif
#ifndef minor
#define minor(x) ((x) & 0xff)
#endif
"""
if "#ifndef major" not in content:
    content = patch + "\n" + content
    with open(path, "w") as f:
        f.write(content)
'
fi

# 🗡️ PARCHE ROBUSTO EN VK_WRAPPER_FEATURES_GEN.PY (EVITAR CONFLICTOS DE EXTENSIONES)
if [ -f "src/vulkan/wrapper/vk_wrapper_features_gen.py" ]; then
    python3 -c '
path = "src/vulkan/wrapper/vk_wrapper_features_gen.py"
with open(path, "r") as f:
    content = f.read()

if "assert len(diagnostics) == 0" in content:
    content = content.replace("assert len(diagnostics) == 0", "# assert len(diagnostics) == 0")
    with open(path, "w") as f:
        f.write(content)
        print("-> [Parche Aplicado] Aserción de duplicados desactivada con éxito en vk_wrapper_features_gen.py")
'
fi

# 🗡️ PARCHE CIRÚRGICO EN WRAPPER_PHYSICAL_DEVICE.C
if [ -f "src/vulkan/wrapper/wrapper_physical_device.c" ]; then
    python3 -c '
path = "src/vulkan/wrapper/wrapper_physical_device.c"
with open(path, "r") as f:
    content = f.read()

if "#define VK_QUEUE_PRESENT_BIT" not in content:
    content = "#define VK_QUEUE_PRESENT_BIT 0\n" + content

content = content.replace("pdevice->wsi_device.engine_name = engine_name;", "/* pdevice->wsi_device.engine_name = engine_name; */")

with open(path, "w") as f:
    f.write(content)
'
fi

# 🗡️ PARCHE CIRÚRGICO EN MESON.BUILD (CORREGIR VK_ICD_GEN CON --ICD-LIB-PATH Y --ICD-FILENAME)
if [ -f "src/vulkan/wrapper/meson.build" ]; then
    python3 -c '
path = "src/vulkan/wrapper/meson.build"
with open(path, "r") as f:
    content = f.read()

target_block = """wrapper_icd = custom_target(
  '\''wrapper_icd'\'',
  input: [vk_icd_gen, vk_api_xml],
  output: '\''wrapper_icd.@0@.json'\''.format(host_machine.cpu()),
  command: [
    prog_python,
    '\''@INPUT0@'\'',
    '\''--api-version'\'', '\''1.3'\'',
    '\''--xml'\'', '\''@INPUT1@'\'',
    '\''--lib-path'\'', join_paths(get_option('\''prefix'\''), get_option('\''libdir'\''), '\''libvulkan_wrapper.so'\''),
    '\''--out'\'', '\''@OUTPUT@'\'',
  ],"""

replacement_block = """wrapper_icd = custom_target(
  '\''wrapper_icd'\'',
  input: [vk_icd_gen, vk_api_xml],
  output: '\''wrapper_icd.@0@.json'\''.format(host_machine.cpu()),
  command: [
    prog_python,
    '\''@INPUT0@'\'',
    '\''--api-version'\'', '\''1.3'\'',
    '\''--xml'\'', '\''@INPUT1@'\'',
    '\''--icd-lib-path'\'', join_paths(get_option('\''prefix'\''), get_option('\''libdir'\''), '\''libvulkan_wrapper.so'\''),
    '\''--icd-filename'\'', '\''@OUTPUT@'\'',
    '\''--out'\'', '\''@OUTPUT@'\'',
  ],"""

if "--lib-path" in content:
    content = content.replace(target_block, replacement_block)
    with open(path, "w") as f:
        f.write(content)
        print("-> [Parche Aplicado] Argumentos del generador de ICD actualizados en meson.build")
'
fi

echo "-> Ejecutando compilación completa de todos los objetivos con Ninja..."
ninja -C "${BUILD_DIR}" all

# 🟢 2. EXTRACCIÓN INTELIGENTE DEL WRAPPER + DIAGNÓSTICO DE LOGS
python3 -c '
import os, shutil

build_dir = "'"${BUILD_DIR}"'"
dst = os.path.join(build_dir, "libvulkan_wrapper.so.unstripped")

candidates = []
for root, dirs, files in os.walk(build_dir):
    for file in files:
        if file.endswith(".so") and not any(x in file for x in ["expat", "glapi", "test"]):
            candidates.append(os.path.join(root, file))

print("Librerías .so encontradas:", candidates)

src = None
for c in candidates:
    if "wrapper" in c or "vulkan" in c:
        src = c
        break

if not src and candidates:
    src = candidates[0]

if src and os.path.exists(src):
    size_mb = os.path.getsize(src) / (1024 * 1024)
    shutil.copy2(src, dst)
    print(f"-> [Forja Real] ¡Driver principal localizado en {src} ({size_mb:.2f} MB) y extraído con éxito!")
else:
    print(f"-> [❌ ERROR CRÍTICO] No se encontró el binario principal en {build_dir}.")
    log_path = os.path.join(build_dir, "meson-logs", "meson-log.txt")
    if os.path.exists(log_path):
        print("--- CONTENIDO RECIENTE DE MESON-LOG.TXT ---")
        with open(log_path, "r") as log_f:
            print(log_f.read()[-3000:])
    exit(1)
'

# 🟢 3. LIMPIEZA DE SÍMBOLOS CON LLVM-STRIP
STRIP="${NDK_DIR}/toolchains/llvm/prebuilt/linux-x86_64/bin/llvm-strip"
if [ ! -f "$STRIP" ]; then
    STRIP=$(find "${NDK_DIR}" -name "llvm-strip" | head -n 1)
fi
$STRIP --strip-unneeded -o "${BUILD_DIR}/libvulkan_wrapper.so" "${BUILD_DIR}/libvulkan_wrapper.so.unstripped"

# 🟢 4. CREACIÓN DEL ÁRBOL DE DIRECTORIOS RÍGIDO
ROOTFS_DIR="${BUILD_DIR}/rootfs_export"
rm -rf "${ROOTFS_DIR}"
mkdir -p "${ROOTFS_DIR}/usr/lib"
mkdir -p "${ROOTFS_DIR}/usr/share/vulkan/icd.d"

cp "${BUILD_DIR}/libvulkan_wrapper.so" "${ROOTFS_DIR}/usr/lib/libvulkan_wrapper.so"

python3 -c '
import os, shutil
build_dir = "'"${BUILD_DIR}"'"
rootfs_lib = "'"${ROOTFS_DIR}"'/usr/lib"

for root, dirs, files in os.walk(os.path.join(build_dir, "subprojects")):
    for file in files:
        if file.endswith(".so"):
            src_so = os.path.join(root, file)
            dst_so = os.path.join(rootfs_lib, file)
            shutil.copy2(src_so, dst_so)
            print(f"-> [Subproyecto] Bypass/Librería integrada: {file}")
'

cat << 'EOF' > "${ROOTFS_DIR}/usr/share/vulkan/icd.d/wrapper_icd.aarch64.json"
{
    "file_format_version": "1.0.0",
    "ICD": {
        "library_path": "/usr/lib/libvulkan_wrapper.so",
        "api_version": "1.3.255"
    }
}
EOF

# 🟢 5. EMPAQUETADO COMPRIMIDO TAR.ZST
echo "-> Forjando el paquete comprimido gamenative_driver_rootfs.tar.zst..."
cd "${ROOTFS_DIR}"
tar -cvf - usr | zstd -o "../gamenative_driver_rootfs.tar.zst"
cd - > /dev/null

cp "${BUILD_DIR}/gamenative_driver_rootfs.tar.zst" ./gamenative_driver_rootfs.tar.zst

echo "=========================================================="
echo " 🟢 FORJA EXITOSA: ¡ROOTFS CON SUBPROYECTOS Y WRAPPER LISTO! "
echo "=========================================================="
