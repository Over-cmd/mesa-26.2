#!/bin/bash
set -e

BUILD_DIR="${1:-${BUILD_DIR:-build}}"

# 🚀 EXPORTAR PYTHONPATH PARA QUE LOS SCRIPTS DE PYTHON ENCUENTREN LOS MÓDULOS DE NIR Y MESA
export PYTHONPATH="/workspace/src/compiler:/workspace/src/compiler/nir:$PYTHONPATH"

# 🚀 RUTAS Y CONFIGURACIÓN DEL NDK R30
NDK_DIR=$(find /home/builder/lib -maxdepth 2 -name "android-ndk*" 2>/dev/null | head -n 1)
if [ -z "$NDK_DIR" ]; then
    NDK_DIR="/home/builder/lib/android-ndk-r30"
fi
NDK_SYSROOT="${NDK_DIR}/toolchains/llvm/prebuilt/linux-x86_64/sysroot"

# Exportaciones generales de AAudio y enlaces
export CFLAGS="$CFLAGS -laaudio -I/data/data/com.termux/files/usr/include -I/data/data/com.termux/files/usr/include/libdrm"
export LDFLAGS="$LDFLAGS -laaudio -L/data/data/com.termux/files/usr/lib -ldrm -lxcb -lxcb-randr -lxcb-xfixes -lX11 -lX11-xcb"

# 🛠️ CREAR ARCHIVO DE COMPILACIÓN CRUZADA (CROSS FILE)
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
system = 'android'
cpu_family = 'aarch64'
cpu = 'armv8-a'
endian = 'little'

[properties]
c_args = ['-D_GNU_SOURCE', '-DPIPE_OS_ANDROID=1', '-DHAVE_FFSLL=0', '-I/workspace/workspace_headers', '-I${NDK_SYSROOT}/usr/include', '-I${NDK_SYSROOT}/usr/include/aarch64-linux-android', '-I/data/data/com.termux/files/usr/include', '-I/data/data/com.termux/files/usr/include/libdrm', '-DVK_USE_PLATFORM_ANDROID_KHR=1']
cpp_args = ['-D_GNU_SOURCE', '-DPIPE_OS_ANDROID=1', '-DHAVE_FFSLL=0', '-I/workspace/workspace_headers', '-I${NDK_SYSROOT}/usr/include', '-I${NDK_SYSROOT}/usr/include/aarch64-linux-android', '-I/data/data/com.termux/files/usr/include', '-I/data/data/com.termux/files/usr/include/libdrm', '-DVK_USE_PLATFORM_ANDROID_KHR=1']
link_args = ['-nodefaultlibs', '-lc++', '-lc++abi', '-lm', '-ldl', '-lc', '-lgcc', '-L/data/data/com.termux/files/usr/lib', '-L${NDK_SYSROOT}/usr/lib/aarch64-linux-android/30', '-L${NDK_SYSROOT}/usr/lib/aarch64-linux-android', '-Wl,--sysroot=${NDK_SYSROOT}', '-ldrm', '-lxcb', '-lxcb-randr', '-lxcb-xfixes', '-lX11', '-lX11-xcb', '-laaudio']
EOF

# 🟢 CONFIGURACIÓN CON MESON
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

# ⚙️ COMPILACIÓN CON NINJA
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
