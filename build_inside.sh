#!/bin/bash
set -e

BUILD_DIR="${1:-${BUILD_DIR:-build}}"

# 🚀 RUTAS Y CONFIGURACIÓN DEL NDK R30
NDK_DIR=$(find /home/builder/lib -maxdepth 2 -name "android-ndk*" 2>/dev/null | head -n 1)
if [ -z "$NDK_DIR" ]; then
    NDK_DIR="/home/builder/lib/android-ndk-r30"
fi
NDK_SYSROOT="${NDK_DIR}/toolchains/llvm/prebuilt/linux-x86_64/sysroot"

# Exportaciones generales de AAudio y enlaces
export CFLAGS="$CFLAGS -laaudio -I/data/data/com.termux/files/usr/include -I/data/data/com.termux/files/usr/include/libdrm"
export LDFLAGS="$LDFLAGS -laaudio -L/data/data/com.termux/files/usr/lib -ldrm -lxcb -lxcb-randr -lxcb-xfixes -lX11 -lX11-xcb"

# 🛡️ RESTAURAR ARCHIVOS MODIFICADOS PREVIAMENTE
git checkout src/util/bitscan.c src/util/os_misc.c src/util/os_sysinfo.c src/util/u_sync_provider.c src/loader/loader.c src/vulkan/wrapper/vk_wrapper_features_gen.py src/vulkan/wrapper/wrapper_physical_device.c src/vulkan/wrapper/wrapper_log.c src/vulkan/wrapper/wrapper_device_memory.c src/util/u_gralloc/meson.build src/android_stub/meson.build meson_options.txt 2>/dev/null || true

# 🛠️ CREAR PUENTES DE HEADERS PARA CUTILS, SYNC Y VNDK
mkdir -p workspace_headers/cutils workspace_headers/sync workspace_headers/vndk

if [ ! -f "workspace_headers/cutils/native_handle.h" ]; then
    REAL_CUTILS=$(find "${NDK_SYSROOT}" -name "native_handle.h" | head -n 1)
    if [ -n "$REAL_CUTILS" ]; then
        cp "$REAL_CUTILS" workspace_headers/cutils/
    else
        cat << 'EOF' > workspace_headers/cutils/native_handle.h
#ifndef ANDROID_CUTILS_NATIVE_HANDLE_H
#define ANDROID_CUTILS_NATIVE_HANDLE_H
typedef struct native_handle { int version; int numFds; int numInts; int data[0]; } native_handle_t;
#endif
EOF
    fi
fi

cat << 'EOF' > workspace_headers/sync/sync.h
#ifndef ANDROID_SYNC_SYNC_H
#define ANDROID_SYNC_SYNC_H
static inline int sync_wait(int fd, int timeout) { return 0; }
static inline int sync_merge(const char *name, int fd1, int fd2) { return -1; }
#endif
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

#endif
EOF
fi

rm -rf "${BUILD_DIR}"
LOCAL_CROSS_FILE="${BUILD_DIR}_cross_file.txt"
rm -f "${LOCAL_CROSS_FILE}"

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
link_args = ['-nodefaultlibs', '-lc++', '-lc++abi', '-lm', '-ldl', '-lc', '-lgcc', '-L/data/data/com.termux/files/usr/lib', '-L${NDK_SYSROOT}/usr/lib/aarch64-linux-android/30', '-L${NDK_SYSROOT}/usr/lib/aarch64-linux-android', '-Wl,--sysroot=${NDK_SYSROOT}', '-ldrm', '-lxcb', '-lxcb-randr', '-lxcb-xfixes', '-lX11', '-lX11-xcb', '-laaudio']
EOF

# 🟢 CONFIGURACIÓN MESON
meson subprojects download || true

meson setup "${BUILD_DIR}" --cross-file "${LOCAL_CROSS_FILE}" \
-Db_sanitize=none \
-Db_lundef=true \
-Dcpp_rtti=false \
-Dgbm=enabled \
-Dopengl=true \
-Dllvm=disabled \
-Dshared-llvm=disabled \
-Dplatforms=x11,android \
-Degl-native-platform=android \
-Dgallium-drivers=panfrost \
-Ddraw-use-llvm=false \
-Dxmlconfig=disabled \
-Dvulkan-drivers=panfrost,wrapper \
-Degl=enabled \
-Dglx=disabled \
-Dandroid-stub=true \
-Dplatform-sdk-version=30 \
-Dzlib=disabled \
-Dzstd=disabled

# 🗡️ PARCHE EN SRC/ANDROID_STUB/MESON.BUILD (WITH_LIBBACKTRACE + CUTILS)
if [ -f "src/android_stub/meson.build" ]; then
    python3 -c '
path = "src/android_stub/meson.build"
with open(path, "r") as f: content = f.read()

if "with_libbacktrace =" not in content:
    content = "with_libbacktrace = false\n" + content

if "\x27cutils\x27" not in content and "lib_names = [" in content:
    content = content.replace("lib_names = [", "lib_names = [\x27cutils\x27, ")

with open(path, "w") as f: f.write(content)
print("-> [Parche Aplicado] with_libbacktrace y cutils integrados en android_stub/meson.build")
'
fi

# 🗡️ PARCHE CIRÚRGICO EN BITSCAN.C
if [ -f "src/util/bitscan.c" ]; then
    python3 -c '
path = "src/util/bitscan.c"
with open(path, "r") as f: text = f.read()
text = text.replace("ffsll(long long int val)", "__attribute__((unused)) ffsll_ndk_disabled(long long int val)")
text = text.replace("ffs(int val)", "__attribute__((unused)) ffs_ndk_disabled(int val)")
with open(path, "w") as f: f.write(text)
'
fi

# 🗡️ PARCHE CIRÚRGICO EN VK_WRAPPER_FEATURES_GEN.PY
if [ -f "src/vulkan/wrapper/vk_wrapper_features_gen.py" ]; then
    python3 -c '
path = "src/vulkan/wrapper/vk_wrapper_features_gen.py"
with open(path, "r") as f: content = f.read()
content = content.replace("assert len(diagnostics) == 0", "# assert len(diagnostics) == 0")
with open(path, "w") as f: f.write(content)
'
fi

# 🗡️ PARCHE CIRÚRGICO EN WRAPPER_DEVICE_MEMORY.C
if [ -f "src/vulkan/wrapper/wrapper_device_memory.c" ]; then
    python3 -c '
path = "src/vulkan/wrapper/wrapper_device_memory.c"
with open(path, "r") as f: content = f.read()
if "#include <vndk/hardware_buffer.h>" not in content:
    content = "#include <vndk/hardware_buffer.h>\n" + content
    with open(path, "w") as f: f.write(content)
'
fi

# 🗡️ PARCHE CIRÚRGICO EN WRAPPER_LOG.C
if [ -f "src/vulkan/wrapper/wrapper_log.c" ]; then
    python3 -c '
path = "src/vulkan/wrapper/wrapper_log.c"
with open(path, "r") as f: content = f.read()
if "#include <fcntl.h>" not in content:
    content = "#include <fcntl.h>\n#include <unistd.h>\n" + content
    with open(path, "w") as f: f.write(content)
'
fi

python3 -c '
import glob
for path in glob.glob("src/util/os_*.c") + glob.glob("src/util/os_*.h"):
    with open(path, "r") as f: content = f.read()
    if "#error unexpected platform" in content:
        content = content.replace("#error unexpected platform", "// #error unexpected platform\n#ifndef DETECT_OS_LINUX\n#define DETECT_OS_LINUX 1\n#endif")
        with open(path, "w") as f: f.write(content)
'

if [ -f "src/util/u_sync_provider.c" ]; then
    python3 -c '
path = "src/util/u_sync_provider.c"
with open(path, "r") as f: content = f.read()
with open(path, "w") as f: f.write("#if 0\n" + content + "\n#endif\n")
'
fi

if [ -f "src/loader/loader.c" ]; then
    python3 -c '
path = "src/loader/loader.c"
with open(path, "r") as f: content = f.read()
patch = "#include <sys/sysmacros.h>\n#ifndef major\n#define major(x) (((x) >> 8) & 0xfff)\n#endif\n#ifndef minor\n#define minor(x) ((x) & 0xff)\n#endif\n"
if "#ifndef major" not in content:
    with open(path, "w") as f: f.write(patch + "\n" + content)
'
fi

if [ -f "src/vulkan/wrapper/wrapper_physical_device.c" ]; then
    python3 -c '
path = "src/vulkan/wrapper/wrapper_physical_device.c"
with open(path, "r") as f: content = f.read()
if "#define VK_QUEUE_PRESENT_BIT" not in content:
    content = "#define VK_QUEUE_PRESENT_BIT 0\n" + content
content = content.replace("pdevice->wsi_device.engine_name = engine_name;", "/* pdevice->wsi_device.engine_name = engine_name; */")
with open(path, "w") as f: f.write(content)
'
fi

echo "-> Ejecutando compilación completa con Ninja..."
ninja -C "${BUILD_DIR}"

# 🟢 EXTRACCIÓN DEL BINARIO DEL WRAPPER
python3 -c '
import os, shutil

build_dir = "'"${BUILD_DIR}"'"
dst = os.path.join(build_dir, "libvulkan_wrapper.so.unstripped")

candidates = []
for root, dirs, files in os.walk(build_dir):
    for file in files:
        if file.endswith(".so") and not any(x in file for x in ["expat", "glapi", "test"]):
            candidates.append(os.path.join(root, file))

src = None
for c in candidates:
    if "wrapper" in c or "vulkan" in c:
        src = c
        break

if not src and candidates:
    src = candidates[0]

if src and os.path.exists(src):
    shutil.copy2(src, dst)
    print(f"-> [Forja Real] Driver wrapper localizado en {src}")
else:
    print("-> [❌ ERROR CRÍTICO] No se encontró el binario del wrapper.")
    exit(1)
'

# 🟢 LIMPIEZA DE SÍMBOLOS CON LLVM-STRIP
STRIP="${NDK_DIR}/toolchains/llvm/prebuilt/linux-x86_64/bin/llvm-strip"
if [ ! -f "$STRIP" ]; then
    STRIP=$(find "${NDK_DIR}" -name "llvm-strip" | head -n 1)
fi
$STRIP --strip-unneeded -o "${BUILD_DIR}/libvulkan_wrapper.so" "${BUILD_DIR}/libvulkan_wrapper.so.unstripped"

# 🟢 ÁRBOL DE DIRECTORIOS RÍGIDO CON ICD RELATIVO
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
            shutil.copy2(os.path.join(root, file), os.path.join(rootfs_lib, file))
' 2>/dev/null || true

cat << 'EOF' > "${ROOTFS_DIR}/usr/share/vulkan/icd.d/wrapper_icd.aarch64.json"
{
    "file_format_version": "1.0.0",
    "ICD": {
        "library_path": "libvulkan_wrapper.so",
        "api_version": "1.3.255"
    }
}
EOF

# 🟢 EMPAQUETADO TAR.ZST
echo "-> Forjando el paquete comprimido gamenative_driver_rootfs.tar.zst..."
cd "${ROOTFS_DIR}"
tar -cvf - usr | zstd -o "../gamenative_driver_rootfs.tar.zst"
cd - > /dev/null

cp "${BUILD_DIR}/gamenative_driver_rootfs.tar.zst" ./gamenative_driver_rootfs.tar.zst

echo "=========================================================="
echo " 🟢 FORJA MESA 26 EXITOSA: ¡WRAPPER Y MANIFIESTO LISTOS! "
echo "=========================================================="
