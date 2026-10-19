#!/bin/bash
set -e

BUILD_DIR="${1:-${BUILD_DIR:-build}}"

export PYTHONPATH="/workspace/src/compiler:/workspace/src/compiler/nir:$PYTHONPATH"

# ---------- NDK ----------
NDK_DIR=$(find /home/builder/lib -maxdepth 2 -name "android-ndk*" 2>/dev/null | head -n 1)
[ -z "$NDK_DIR" ] && NDK_DIR="/home/builder/lib/android-ndk-r30"
NDK_BIN="${NDK_DIR}/toolchains/llvm/prebuilt/linux-x86_64/bin"
NDK_SYSROOT="${NDK_DIR}/toolchains/llvm/prebuilt/linux-x86_64/sysroot"
TRIPLE="aarch64-linux-android30"
SYSLIB="aarch64-linux-android"

# ---------- PARCHES ----------
# El #error de os_misc.c dice "os_sysinfo.c": se parchea por regex.
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

# ---------- CROSS FILE ----------
rm -rf "${BUILD_DIR}"
CROSS="${BUILD_DIR}_cross_file.txt"
rm -f "${CROSS}"

COMMON_ARGS="'-D_GNU_SOURCE', '-DHAVE_FFSLL=0', '-DDETECT_OS_LINUX=1', '-DDETECT_OS_UNIX=1', '-DDETECT_OS_ANDROID=1', '-DVK_USE_PLATFORM_ANDROID_KHR=1', '-I/workspace/workspace_headers', '-I/data/data/com.termux/files/usr/include', '-I/data/data/com.termux/files/usr/include/libdrm'"

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
c_link_args = ['-L/data/data/com.termux/files/usr/lib', '-L${NDK_SYSROOT}/usr/lib/${SYSLIB}/30', '-ldl', '-lm', '-llog', '-landroid']
cpp_link_args = ['-L/data/data/com.termux/files/usr/lib', '-L${NDK_SYSROOT}/usr/lib/${SYSLIB}/30', '-ldl', '-lm', '-llog', '-landroid', '-static-libstdc++']

[host_machine]
system = 'android'
cpu_family = 'aarch64'
cpu = 'armv8-a'
endian = 'little'
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

# ---------- LOCALIZAR WRAPPER ----------
SRC="${BUILD_DIR}/src/vulkan/wrapper/libvulkan_wrapper.so"
if [ ! -f "$SRC" ]; then
    SRC=$(find "${BUILD_DIR}" -name "libvulkan_wrapper*.so" | head -n 1)
fi
[ -f "$SRC" ] || { echo "-> [ERROR] No se encontró libvulkan_wrapper.so"; exit 1; }
echo "-> Wrapper: $SRC"
cp "$SRC" "${BUILD_DIR}/libvulkan_wrapper.so.unstripped"
"${NDK_BIN}/llvm-strip" --strip-unneeded -o "${BUILD_DIR}/libvulkan_wrapper.so" "${BUILD_DIR}/libvulkan_wrapper.so.unstripped"

# ---------- ROOTFS ----------
ROOTFS_DIR="${BUILD_DIR}/rootfs_export"
rm -rf "${ROOTFS_DIR}"
mkdir -p "${ROOTFS_DIR}/usr/lib" "${ROOTFS_DIR}/usr/share/vulkan/icd.d"
cp "${BUILD_DIR}/libvulkan_wrapper.so" "${ROOTFS_DIR}/usr/lib/"
find "${BUILD_DIR}/subprojects" -name "*.so" -exec cp -f {} "${ROOTFS_DIR}/usr/lib/" \; 2>/dev/null || true

cat << 'EOF' > "${ROOTFS_DIR}/usr/share/vulkan/icd.d/wrapper_icd.aarch64.json"
{
    "file_format_version": "1.0.0",
    "ICD": {
        "library_path": "libvulkan_wrapper.so",
        "api_version": "1.3.255"
    }
}
EOF

echo "-> Empaquetando tar.zst..."
( cd "${ROOTFS_DIR}" && tar -cf - usr | zstd -19 -o "../gamenative_driver_rootfs.tar.zst" )
cp "${BUILD_DIR}/gamenative_driver_rootfs.tar.zst" ./gamenative_driver_rootfs.tar.zst

echo "=== FORJA MESA 26 EXITOSA ==="
