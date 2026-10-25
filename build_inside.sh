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

   WSI_TRACE("[WSI] support: platform=%d qfi=%u iface=%p res=%d "
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

# WSI_TRACE: escribe las trazas directamente en WRAPPER_DIAG_FILE (el stderr solo se redirige al crear el
# dispositivo, así que lo anterior —superficie, cargador— se perdía). Solo actúa con WRAPPER_DIAG=1.
python3 - <<'PY'
MACRO = r'''#ifndef WSI_TRACE
#include <stdio.h>
#include <stdlib.h>
#define WSI_TRACE(...) do { \
   const char *_wd = getenv("WRAPPER_DIAG"); \
   if (_wd && atoi(_wd)) { \
      const char *_wp = getenv("WRAPPER_DIAG_FILE"); \
      FILE *_wf = _wp ? fopen(_wp, "a") : NULL; \
      if (_wf) { fprintf(_wf, __VA_ARGS__); fclose(_wf); } \
      else { fprintf(stderr, __VA_ARGS__); } \
   } \
} while (0)
#endif
'''
for f in ("src/vulkan/wsi/wsi_common.c", "src/vulkan/wsi/wsi_common_x11.c",
          "src/vulkan/runtime/vk_instance.c"):
    try:
        t = open(f).read()
    except FileNotFoundError:
        continue
    if "define WSI_TRACE" not in t:
        open(f, "w").write(MACRO + t)
        print("-> WSI_TRACE añadido a " + f)
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
   WSI_TRACE("[WSI] CreateSwapchain platform=%d fmt=%d %ux%u minImages=%u mode=%d iface=%p\n",
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
   WSI_TRACE("[WSI] create_swapchain -> %d\n", (int)result);
   if (result != VK_SUCCESS)
      return result;
'''),
])

patch("src/vulkan/wsi/wsi_common_x11.c", [
(r'''   assert(pCreateInfo->sType == VK_STRUCTURE_TYPE_XCB_SURFACE_CREATE_INFO_KHR);
''', r'''   assert(pCreateInfo->sType == VK_STRUCTURE_TYPE_XCB_SURFACE_CREATE_INFO_KHR);
   WSI_TRACE("[WSI] CreateXcbSurface conn=%p window=%u\n",
           (void *)pCreateInfo->connection, (unsigned)pCreateInfo->window);
'''),
(r'''   assert(pCreateInfo->sType == VK_STRUCTURE_TYPE_XLIB_SURFACE_CREATE_INFO_KHR);
''', r'''   assert(pCreateInfo->sType == VK_STRUCTURE_TYPE_XLIB_SURFACE_CREATE_INFO_KHR);
   WSI_TRACE("[WSI] CreateXlibSurface dpy=%p window=%lu\n",
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
      free(attrib);''', r'''   WSI_TRACE("[WSI] visual: conn=%p conn_err=%d window=%u tree=%p attrib=%p\n",
           (void *)conn, xcb_connection_has_error(conn), (unsigned)window,
           (void *)tree, (void *)attrib);
   if (attrib == NULL || tree == NULL) {
      free(attrib);''', 1)
    t = t.replace(r'''   xcb_screen_t *screen = get_screen_for_root(conn, root);
   if (screen == NULL)
      return NULL;
''', r'''   xcb_screen_t *screen = get_screen_for_root(conn, root);
   WSI_TRACE("[WSI] visual: root=%u screen=%p visual_id=%u\n",
           (unsigned)root, (void *)screen, (unsigned)visual_id);
   if (screen == NULL)
      return NULL;
''', 1)
    # no abortar si no se encuentra el visual: crear la superficie igualmente (sin alpha)
    t = t.replace(r'''   if (!visual)
      return VK_ERROR_OUT_OF_HOST_MEMORY;
''', r'''   if (!visual)
      WSI_TRACE("[WSI] WARNING: visual no encontrado, se crea la superficie sin alpha\n");
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
    ins = '\n   WSI_TRACE("[WSI] -> %s\\n");' % name
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
               "   WSI_TRACE(\"[WSI] surface creada (%s) has_alpha=%%d\\n\", (int)surface->has_alpha);\n"
               "   return VK_SUCCESS;") % (k, k)
        t = t.replace(old, new, 1)
    for n in ("wsi_GetPhysicalDeviceXcbPresentationSupportKHR",
              "wsi_GetPhysicalDeviceXlibPresentationSupportKHR"):
        i = t.find(n + "(")
        j = t.find("\n{\n", i) if i >= 0 else -1
        if j >= 0:
            t = t[:j + 3] + '   WSI_TRACE("[WSI] -> %s\\n");\n' % n + t[j + 3:]
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

#include <ucontext.h>

static struct sigaction wsi_old_segv;
static __thread int wsi_in_segv;

static void
wsi_segv_handler(int sig, siginfo_t *info, void *uctx)
{
   if (!wsi_in_segv) {
      uintptr_t pc = 0, lr = 0;
#if defined(__aarch64__)
      ucontext_t *uc = (ucontext_t *)uctx;
      pc = (uintptr_t)uc->uc_mcontext.pc;
      lr = (uintptr_t)uc->uc_mcontext.regs[30];
#elif defined(__x86_64__)
      ucontext_t *uc = (ucontext_t *)uctx;
      pc = (uintptr_t)uc->uc_mcontext.gregs[REG_RIP];
#endif
      Dl_info di;
      static volatile int wsi_segv_count;
      memset(&di, 0, sizeof(di));
      int have = pc && dladdr((void *)pc, &di) && di.dli_fname;
      int in_box = have && strstr(di.dli_fname, "box64");
      uintptr_t fa = info ? (uintptr_t)info->si_addr : 0;
      /* box64 usa SIGSEGV para su propia gestión de memoria: solo se informa de fallos en código nativo
         ajeno a box64, o de accesos casi-NULL (puntero de función/estructura nulo), con tope de 40 líneas */
      if (wsi_segv_count < 40 && (!in_box || fa < 0x100000)) {
         wsi_segv_count++;
         if (!have) { di.dli_fname = "?"; di.dli_fbase = NULL; di.dli_sname = NULL; }
         wsi_in_segv = 1;
         const char *b = strrchr(di.dli_fname, '/');
         WSI_TRACE("[CRASH] SIGSEGV addr=%p pc=%p %s+0x%lx %s lr=%p\n",
                   info ? info->si_addr : NULL, (void *)pc, b ? b + 1 : di.dli_fname,
                   (unsigned long)(pc - (uintptr_t)di.dli_fbase),
                   di.dli_sname ? di.dli_sname : "", (void *)lr);
         int n = 0;
         _Unwind_Backtrace(wsi_crash_cb, &n);
         wsi_in_segv = 0;
      }
   }
   /* encadenar al manejador anterior (box64/Wine) para no romper su emulación */
   if (wsi_old_segv.sa_flags & SA_SIGINFO) {
      wsi_old_segv.sa_sigaction(sig, info, uctx);
   } else if (wsi_old_segv.sa_handler == SIG_DFL || wsi_old_segv.sa_handler == SIG_IGN) {
      signal(sig, SIG_DFL);
      raise(sig);
   } else {
      wsi_old_segv.sa_handler(sig);
   }
}

__attribute__((constructor)) static void
wsi_crash_install(void)
{
   const char *d = getenv("WRAPPER_DIAG");
   if (!(d && atoi(d)))
      return;
   signal(SIGABRT, wsi_crash_handler);
   struct sigaction sa;
   memset(&sa, 0, sizeof(sa));
   sa.sa_sigaction = wsi_segv_handler;
   sa.sa_flags = SA_SIGINFO | SA_ONSTACK;
   sigemptyset(&sa.sa_mask);
   sigaction(SIGSEGV, &sa, &wsi_old_segv);   /* encadenado: se llama al anterior */
}
CEOF
echo "-> wsi_common.c: handler [CRASH] en SIGABRT añadido"
fi

# [ICD] qué funciones de superficie/swapchain/queue pide el cargador al wrapper (para ver si la presentación llega a pedirse).
python3 - <<'PY'
f = "src/vulkan/runtime/vk_instance.c"
try:
    t = open(f).read()
except FileNotFoundError:
    t = None
if t and "[ICD] proc_addr" not in t:
    o = t
    trace = ('   if (name && (strstr(name, "Surface") || strstr(name, "Swapchain") || '
             'strstr(name, "Present") || strstr(name, "QueueFamily")))\n'
             '      WSI_TRACE("[ICD] proc_addr(%s)\\n", name);\n')
    for fn in ("\nvk_instance_get_proc_addr(", "\nvk_instance_get_physical_device_proc_addr("):
        i = t.find(fn)
        j = t.find("\n{\n", i) if i >= 0 else -1
        if j >= 0:
            t = t[:j + 3] + trace + t[j + 3:]
    if t != o:
        t = "#include <stdio.h>\n#include <string.h>\n" + t
        open(f, "w").write(t)
        print("-> vk_instance.c: trazas [ICD] proc_addr")
PY

# vkGetPhysicalDeviceSurfaceSupportKHR revienta con 0xc0000005 dentro de la llamada Unix (Wine):
# migas por etapas en esa ruta + se evita leer a mano la estructura Display de Xlib.
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
        elif old not in t:
            print("-> AVISO: no se encontró el trozo en " + path + ": " + old.strip().splitlines()[0][:60])
    if t != o:
        open(path, "w").write(t)
        print("-> trazas de soporte de presentación añadidas en " + path)

patch("src/vulkan/wsi/wsi_common_x11.c", [
# 1) no tocar la estructura Display de Xlib (Wine siempre llama XInitThreads)
("      return x11_xlib_display_is_thread_safe(((VkIcdSurfaceXlib *)icd_surface)->dpy);",
 "      return true; /* parcheado: no se lee la estructura Display */"),
("   if (!x11_xlib_display_is_thread_safe(dpy))\n      return false;\n", "   /* parcheado: sin lectura de Display */\n"),
# 2) get_support por etapas
("   if (!x11_surface_is_thread_safe(icd_surface)) {\n      *pSupported = false;\n      return VK_SUCCESS;\n   }\n",
 "   WSI_TRACE(\"[WSI] get_support: entrada platform=%d qfi=%u\\n\", (int)icd_surface->platform, queueFamilyIndex);\n"
 "   if (!x11_surface_is_thread_safe(icd_surface)) {\n      *pSupported = false;\n      return VK_SUCCESS;\n   }\n"
 "   WSI_TRACE(\"[WSI] get_support: thread-safe ok\\n\");\n"),
("   struct wsi_x11_connection *wsi_conn =\n      wsi_x11_get_connection(wsi_device, conn);\n   if (!wsi_conn)\n      return VK_ERROR_OUT_OF_HOST_MEMORY;\n\n   if (!wsi_device->sw) {\n      if (!wsi_x11_check_for_dri3(wsi_conn)) {\n         *pSupported = false;",
 "   WSI_TRACE(\"[WSI] get_support: antes de get_connection conn=%p\\n\", (void *)conn);\n"
 "   struct wsi_x11_connection *wsi_conn =\n      wsi_x11_get_connection(wsi_device, conn);\n"
 "   WSI_TRACE(\"[WSI] get_support: wsi_conn=%p has_dri3=%d\\n\", (void *)wsi_conn, wsi_conn ? (int)wsi_conn->has_dri3 : -1);\n"
 "   if (!wsi_conn)\n      return VK_ERROR_OUT_OF_HOST_MEMORY;\n\n   if (!wsi_device->sw) {\n      if (!wsi_x11_check_for_dri3(wsi_conn)) {\n         *pSupported = false;"),
("   if (!visual_supported(get_visualtype_for_window(conn, window, NULL, NULL))) {\n      *pSupported = false;\n      return VK_SUCCESS;\n   }\n\n   *pSupported = true;",
 "   if (!visual_supported(get_visualtype_for_window(conn, window, NULL, NULL))) {\n      *pSupported = false;\n      return VK_SUCCESS;\n   }\n\n"
 "   WSI_TRACE(\"[WSI] get_support: visual ok -> soportado\\n\");\n   *pSupported = true;"),
# 3) creación de la conexión X por etapas
('   sync_cookie = xcb_query_extension(conn, 4, "SYNC");',
 '   WSI_TRACE("[WSI] conn_create: inicio conn=%p sw=%d\\n", (void *)conn, (int)wsi_dev->sw);\n   sync_cookie = xcb_query_extension(conn, 4, "SYNC");'),
("   if (!dri3_reply || !pres_reply || !xfixes_reply) {\n      free(dri3_reply);",
 "   WSI_TRACE(\"[WSI] conn_create: extensiones dri3=%p present=%p xfixes=%p randr=%p\\n\",\n"
 "             (void *)dri3_reply, (void *)pres_reply, (void *)xfixes_reply, (void *)randr_reply);\n"
 "   if (!dri3_reply || !pres_reply || !xfixes_reply) {\n      free(dri3_reply);"),
("   wsi_conn->has_xfixes = xfixes_reply->present != 0;",
 "   WSI_TRACE(\"[WSI] conn_create: dri3/present consultados\\n\");\n   wsi_conn->has_xfixes = xfixes_reply->present != 0;"),
("   wsi_conn->is_xwayland = wsi_x11_detect_xwayland(conn, randr_reply,\n                                                   xwl_reply);\n",
 "   WSI_TRACE(\"[WSI] conn_create: antes de detect_xwayland\\n\");\n"
 "   wsi_conn->is_xwayland = wsi_x11_detect_xwayland(conn, randr_reply,\n                                                   xwl_reply);\n"
 "   WSI_TRACE(\"[WSI] conn_create: xwayland=%d\\n\", (int)wsi_conn->is_xwayland);\n"),
("   list_inithead(&wsi_conn->screen_resources_list);\n\n   xcb_screen_iterator_t it",
 "   WSI_TRACE(\"[WSI] conn_create: antes de screen_resources\\n\");\n   list_inithead(&wsi_conn->screen_resources_list);\n\n   xcb_screen_iterator_t it"),
("   free(dri3_reply);\n   free(pres_reply);\n   free(randr_reply);\n   free(xwl_reply);",
 "   WSI_TRACE(\"[WSI] conn_create: screen_resources ok\\n\");\n   free(dri3_reply);\n   free(pres_reply);\n   free(randr_reply);\n   free(xwl_reply);"),
])

# entrada de wsi_GetPhysicalDeviceSurfaceSupportKHR
f = "src/vulkan/wsi/wsi_common.c"
try:
    t = open(f).read()
except FileNotFoundError:
    t = None
if t and "[WSI] -> SurfaceSupportKHR" not in t:
    i = t.find("wsi_GetPhysicalDeviceSurfaceSupportKHR(")
    j = t.find("\n{\n", i) if i >= 0 else -1
    if j >= 0:
        t = t[:j + 3] + '   WSI_TRACE("[WSI] -> SurfaceSupportKHR qfi=%u\\n", queueFamilyIndex);\n' + t[j + 3:]
        open(f, "w").write(t)
        print("-> wsi_common.c: traza de entrada de SurfaceSupportKHR")
PY

# create_swapchain devolvía -1000158000 (VK_ERROR_INVALID_DRM_FORMAT_MODIFIER_PLANE_LAYOUT_EXT):
# Mali no soporta modificadores DRM -> se desactivan en X11 (WRAPPER_X11_MODIFIERS=1 los reactiva)
# + migas por etapas en la creación de imágenes del swapchain.
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
        elif old not in t:
            print("-> AVISO: no se encontró el trozo en " + path + ": " + old.strip().splitlines()[0][:70])
    if t != o:
        open(path, "w").write(t)
        print("-> parche de modificadores/trazas de swapchain en " + path)

patch("src/vulkan/wsi/wsi_common_x11.c", [
("   return wsi_device->supports_modifiers && !wsi_device->x11.ignore_suboptimal;",
 "   { const char *_e = getenv(\"WRAPPER_X11_MODIFIERS\"); if (!(_e && atoi(_e))) return false; }\n"
 "   return wsi_device->supports_modifiers && !wsi_device->x11.ignore_suboptimal;"),
("      if (use_modifiers(wsi_device)) {\n         wsi_x11_get_dri3_modifiers(",
 "      WSI_TRACE(\"[WSI] swapchain: same_gpu=%d explicit_sync=%d use_modifiers=%d supports_modifiers=%d\\n\",\n"
 "                (int)drm_image_params.same_gpu, (int)drm_image_params.explicit_sync,\n"
 "                (int)use_modifiers(wsi_device), (int)wsi_device->supports_modifiers);\n"
 "      if (use_modifiers(wsi_device)) {\n         wsi_x11_get_dri3_modifiers("),
("   result = wsi_swapchain_init(wsi_device, &chain->base, device, pCreateInfo,\n                               image_params, pAllocator);\n",
 "   result = wsi_swapchain_init(wsi_device, &chain->base, device, pCreateInfo,\n                               image_params, pAllocator);\n"
 "   WSI_TRACE(\"[WSI] swapchain: wsi_swapchain_init -> %d\\n\", (int)result);\n"),
("      result = x11_image_init(device, chain, pCreateInfo, pAllocator,\n                              &chain->images[image]);\n      if (result != VK_SUCCESS)\n         goto fail_init_images;",
 "      result = x11_image_init(device, chain, pCreateInfo, pAllocator,\n                              &chain->images[image]);\n"
 "      WSI_TRACE(\"[WSI] swapchain: x11_image_init[%u] -> %d\\n\", image, (int)result);\n"
 "      if (result != VK_SUCCESS)\n         goto fail_init_images;"),
])

patch("src/vulkan/wsi/wsi_common.c", [
("   result = configure_image(chain, pCreateInfo, image_params,\n                            &chain->image_info);\n   if (result != VK_SUCCESS)\n      goto fail;\n",
 "   result = configure_image(chain, pCreateInfo, image_params,\n                            &chain->image_info);\n"
 "   WSI_TRACE(\"[WSI] swapchain_init: configure_image -> %d blit_type=%d image_type=%d\\n\", (int)result, (int)chain->blit.type, (int)image_params->image_type);\n"
 "   if (result != VK_SUCCESS)\n      goto fail;\n"),
("   result = wsi->CreateImage(chain->device, &info->create,\n                             &chain->alloc, &image->image);\n   if (result != VK_SUCCESS)\n      goto fail;\n",
 "   result = wsi->CreateImage(chain->device, &info->create,\n                             &chain->alloc, &image->image);\n"
 "   WSI_TRACE(\"[WSI] create_image: CreateImage -> %d tiling=%d fmt=%d usage=0x%x flags=0x%x\\n\", (int)result, (int)info->create.tiling, (int)info->create.format, (unsigned)info->create.usage, (unsigned)info->create.flags);\n"
 "   if (result != VK_SUCCESS)\n      goto fail;\n"),
("   result = info->create_mem(chain, info, image);\n   if (result != VK_SUCCESS)\n      goto fail;\n",
 "   result = info->create_mem(chain, info, image);\n"
 "   WSI_TRACE(\"[WSI] create_image: create_mem -> %d\\n\", (int)result);\n"
 "   if (result != VK_SUCCESS)\n      goto fail;\n"),
("   result = wsi->BindImageMemory(chain->device, image->image,\n                                 image->memory, 0);\n   if (result != VK_SUCCESS)\n      goto fail;\n",
 "   result = wsi->BindImageMemory(chain->device, image->image,\n                                 image->memory, 0);\n"
 "   WSI_TRACE(\"[WSI] create_image: BindImageMemory -> %d\\n\", (int)result);\n"
 "   if (result != VK_SUCCESS)\n      goto fail;\n"),
("      result = info->finish_create(chain, info, image);\n      if (result != VK_SUCCESS)\n         goto fail;\n",
 "      result = info->finish_create(chain, info, image);\n"
 "      WSI_TRACE(\"[WSI] create_image: finish_create -> %d\\n\", (int)result);\n"
 "      if (result != VK_SUCCESS)\n         goto fail;\n"),
])
PY

# Camino de blit del WSI (sw o buffer): crash tras BindImageMemory -> en wsi_finish_create_blit_context.
# - SetDebugUtilsObjectNameEXT crashea con los cmdbufs del wrapper -> desactivado (WRAPPER_WSI_LABELS=1 lo reactiva).
# - supports_scanout=false por defecto: sin imágenes OPTIMAL exportables por dma-buf (Mali las rechaza con -1000158000);
#   se usa el camino "prime": imagen normal + blit a buffer exportable. WRAPPER_WSI_SCANOUT=1 lo reactiva.
# - Lista los callbacks WSI que quedaron NULL y traza finish_create por etapas.
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
        elif old not in t:
            print("-> AVISO: no se encontró el trozo en " + path + ": " + old.strip().splitlines()[0][:70])
    if t != o:
        open(path, "w").write(t)
        print("-> parche de blit/callbacks en " + path)

NAMES = ("AllocateMemory AllocateCommandBuffers BindBufferMemory BindImageMemory BeginCommandBuffer "
         "CmdPipelineBarrier CmdCopyImage CmdCopyImageToBuffer CmdResetQueryPool ResetQueryPoolEXT "
         "CmdWriteTimestamp CreateBuffer CreateCommandPool CreateFence CreateImage CreateQueryPool "
         "CreateSemaphore DestroyBuffer DestroyCommandPool DestroyFence DestroyImage DestroyQueryPool "
         "DestroySemaphore EndCommandBuffer FreeMemory FreeCommandBuffers GetBufferMemoryRequirements "
         "GetFenceStatus GetImageMemoryRequirements GetImageSubresourceLayout GetMemoryFdKHR "
         "GetCalibratedTimestampsKHR GetQueryPoolResults GetSemaphoreFdKHR ResetFences QueueSubmit2 "
         "SetDebugUtilsObjectNameEXT WaitForFences MapMemory UnmapMemory").split()
chk = "".join("      CHK(%s);\n" % n for n in NAMES)

patch("src/vulkan/wsi/wsi_common.c", [
("   wsi->supports_scanout = true;\n",
 "   { const char *_sc = getenv(\"WRAPPER_WSI_SCANOUT\"); wsi->supports_scanout = (_sc && atoi(_sc)) ? true : false; }\n"),
("   wsi->SetDebugUtilsObjectNameEXT(device, &name_info);\n",
 "   /* el wrapper devuelve cmdbufs que no son vk_object_base: SetDebugUtilsObjectNameEXT crashea; solo etiquetas de depuración */\n   { const char *_lb = getenv(\"WRAPPER_WSI_LABELS\"); if (_lb && atoi(_lb) && wsi->SetDebugUtilsObjectNameEXT)\n        wsi->SetDebugUtilsObjectNameEXT(device, &name_info); }\n"),
("   return VK_SUCCESS;\nfail:\n   wsi_device_finish(wsi, alloc);",
 "   {\n#define CHK(f) if (!wsi->f) WSI_TRACE(\"[WSI] callback NULL: %s\\n\", #f)\n" + chk +
 "#undef CHK\n      WSI_TRACE(\"[WSI] wsi_device_init: supports_scanout=%d sw=%d\\n\", (int)wsi->supports_scanout, (int)wsi->sw);\n   }\n"
 "   return VK_SUCCESS;\nfail:\n   wsi_device_finish(wsi, alloc);"),
("      image->blit.cmd_buffers[i] = cmd_buffer;\n\n      wsi_label_cmd_buffer(wsi, chain->device, cmd_buffer, \"wsi blit\");\n",
 "      image->blit.cmd_buffers[i] = cmd_buffer;\n      WSI_TRACE(\"[WSI] blit: cmdbuf[%u] asignado\\n\", i);\n\n"
 "      wsi_label_cmd_buffer(wsi, chain->device, cmd_buffer, \"wsi blit\");\n      WSI_TRACE(\"[WSI] blit: label ok\\n\");\n"),
("      wsi->BeginCommandBuffer(cmd_buffer, &begin_info);\n",
 "      wsi->BeginCommandBuffer(cmd_buffer, &begin_info);\n      WSI_TRACE(\"[WSI] blit: BeginCommandBuffer ok (type=%d)\\n\", (int)chain->blit.type);\n"),
("      result = wsi->EndCommandBuffer(cmd_buffer);\n      if (result != VK_SUCCESS)\n         return result;\n",
 "      result = wsi->EndCommandBuffer(cmd_buffer);\n      WSI_TRACE(\"[WSI] blit: EndCommandBuffer -> %d\\n\", (int)result);\n      if (result != VK_SUCCESS)\n         return result;\n"),
])
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

# ---------- Dónde decide el wrapper la presentación (se imprime en el log del CI) ----------
echo "=== grep: SurfaceSupport / PRESENT_BIT / queue families en src/vulkan/wrapper ==="
grep -rn "SurfaceSupport\|VK_QUEUE_PRESENT_BIT\|GetPhysicalDeviceQueueFamilyProperties\|wsi_device_init" src/vulkan/wrapper 2>/dev/null | head -60 || true
echo "=== wrapper_physical_device.c alrededor de VK_QUEUE_PRESENT_BIT ==="
L=$(grep -n "VK_QUEUE_PRESENT_BIT" src/vulkan/wrapper/wrapper_physical_device.c 2>/dev/null | head -1 | cut -d: -f1)
[ -n "$L" ] && sed -n "$((L-25)),$((L+25))p" src/vulkan/wrapper/wrapper_physical_device.c || true
echo "=== fin grep ==="

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
