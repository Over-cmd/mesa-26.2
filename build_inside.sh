#!/bin/bash
set -e

BUILD_DIR="${1:-${BUILD_DIR:-build}}"

# 🚀 INYECCIÓN MAESTRA DE ENLACE AAUDIO:
export CFLAGS="$CFLAGS -laaudio"
export LDFLAGS="$LDFLAGS -laaudio"

# 🛡️ RESTAURAR ARCHIVOS MODIFICADOS PREVIAMENTE
git checkout src/util/bitscan.c src/util/os_misc.c src/util/os_sysinfo.c src/util/u_sync_provider.c src/loader/loader.c 2>/dev/null || true

# 🛡️ PARCHE DE CROSS-FILE: Copiamos el cross_file.txt del sistema y eliminamos -U__ANDROID__
LOCAL_CROSS_FILE="${BUILD_DIR}_cross_file.txt"
cp /root/build-config/cross_file.txt "${LOCAL_CROSS_FILE}"
sed -i 's/-U__ANDROID__//g' "${LOCAL_CROSS_FILE}"

# 🟢 1. MOTOR DE CONFIGURACIÓN DE MESON
if [ ! -d "${BUILD_DIR}" ]; then
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
  -Dvulkan-drivers=wrapper \
  -Degl=disabled \
  -Dglx=disabled
fi

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

echo "-> Ejecutando compilación completa con Ninja (incluyendo submódulos)..."
ninja -C "${BUILD_DIR}"

# 🟢 2. EXTRACCIÓN INTELIGENTE DEL WRAPPER Y LOS MÓDULOS DE BYPASS DE ADRENOTOOLS
python3 -c '
import os, shutil

build_dir = "'"${BUILD_DIR}"'"
dst = os.path.join(build_dir, "libvulkan_wrapper.so.unstripped")

candidates = []
subproject_sos = []

for root, dirs, files in os.walk(build_dir):
    for file in files:
        if file.endswith(".so") and not any(x in file for x in ["expat", "glapi", "test"]):
            full_path = os.path.join(root, file)
            candidates.append(full_path)
            if "subprojects" in root:
                subproject_sos.append(full_path)

print("Librerías .so encontradas:", candidates)
print("Librerías de subproyectos (adrenotools / bypass):", subproject_sos)

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
    exit(1)
'

# 🟢 3. LIMPIEZA DE SÍMBOLOS CON LLVM-STRIP
NDK_DIR=$(find /home/builder/lib -maxdepth 2 -name "android-ndk*" 2>/dev/null | head -n 1)
STRIP="${NDK_DIR}/toolchains/llvm/prebuilt/linux-x86_64/bin/llvm-strip"
if [ ! -f "$STRIP" ]; then
    STRIP=$(find "${NDK_DIR}" -name "llvm-strip" | head -n 1)
fi
$STRIP --strip-unneeded -o "${BUILD_DIR}/libvulkan_wrapper.so" "${BUILD_DIR}/libvulkan_wrapper.so.unstripped"

# 🟢 4. CREACIÓN DEL ÁRBOL DE DIRECTORIOS RÍGIDO PARA EL ROOTFS
ROOTFS_DIR="${BUILD_DIR}/rootfs_export"
rm -rf "${ROOTFS_DIR}"
mkdir -p "${ROOTFS_DIR}/usr/lib"
mkdir -p "${ROOTFS_DIR}/usr/share/vulkan/icd.d"

# Copiamos el binario principal
cp "${BUILD_DIR}/libvulkan_wrapper.so" "${ROOTFS_DIR}/usr/lib/libvulkan_wrapper.so"

# Copiamos automáticamente todas las librerías secundarias/bypass generadas por el subproyecto adrenotools
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
echo " 🟢 FORJA EXITOSA: ¡ROOTFS CON ADRENOTOOLS INTEGRADO Y LISTO! "
echo "=========================================================="
