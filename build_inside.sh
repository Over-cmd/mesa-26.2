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

# vkQueuePresentKHR -> SIGSEGV pc=0 con lr dentro del wrapper (offset 0x7184): llamada a puntero de función NULL.
# Causa más probable: wsi->QueueSubmit2 (synchronization2) -> la Mali (Vulkan 1.1) no lo tiene y el wrapper
# lo reenvía a una tabla con entrada NULL. Se sustituye por una versión que convierte VkSubmitInfo2 -> VkSubmitInfo
# y usa vkQueueSubmit (v1).
python3 - <<'PY'
path = "src/vulkan/wsi/wsi_common.c"
try:
    t = open(path).read()
except FileNotFoundError:
    t = None
if t is not None:
    compat = r'''
/* --- compat: QueueSubmit2 -> QueueSubmit (la Mali no soporta synchronization2) --- */
static PFN_vkQueueSubmit wsi_real_QueueSubmit;

static VkResult
wsi_compat_queue_submit2(VkQueue queue, uint32_t submitCount,
                         const VkSubmitInfo2 *pSubmits, VkFence fence)
{
   if (!wsi_real_QueueSubmit)
      return VK_ERROR_INITIALIZATION_FAILED;
   if (submitCount == 0 || !pSubmits)
      return wsi_real_QueueSubmit(queue, 0, NULL, fence);

   VkResult res = VK_SUCCESS;
   for (uint32_t i = 0; i < submitCount && res == VK_SUCCESS; i++) {
      const VkSubmitInfo2 *s = &pSubmits[i];
      VkSemaphore waits[16], sigs[16];
      VkPipelineStageFlags stages[16];
      uint64_t wv[16], sv[16];
      VkCommandBuffer cbs[16];
      uint32_t nw = s->waitSemaphoreInfoCount > 16 ? 16 : s->waitSemaphoreInfoCount;
      uint32_t ns = s->signalSemaphoreInfoCount > 16 ? 16 : s->signalSemaphoreInfoCount;
      uint32_t nc = s->commandBufferInfoCount > 16 ? 16 : s->commandBufferInfoCount;
      bool timeline = false;
      for (uint32_t j = 0; j < nw; j++) {
         waits[j] = s->pWaitSemaphoreInfos[j].semaphore;
         wv[j] = s->pWaitSemaphoreInfos[j].value;
         VkPipelineStageFlags m = (VkPipelineStageFlags)(s->pWaitSemaphoreInfos[j].stageMask & 0xffffffffu);
         stages[j] = m ? m : VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
         if (wv[j]) timeline = true;
      }
      for (uint32_t j = 0; j < ns; j++) {
         sigs[j] = s->pSignalSemaphoreInfos[j].semaphore;
         sv[j] = s->pSignalSemaphoreInfos[j].value;
         if (sv[j]) timeline = true;
      }
      for (uint32_t j = 0; j < nc; j++)
         cbs[j] = s->pCommandBufferInfos[j].commandBuffer;

      VkTimelineSemaphoreSubmitInfo tl = {
         .sType = VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO,
         .waitSemaphoreValueCount = nw, .pWaitSemaphoreValues = wv,
         .signalSemaphoreValueCount = ns, .pSignalSemaphoreValues = sv,
      };
      VkSubmitInfo si = {
         .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
         .pNext = timeline ? &tl : NULL,
         .waitSemaphoreCount = nw, .pWaitSemaphores = waits, .pWaitDstStageMask = stages,
         .commandBufferCount = nc, .pCommandBuffers = cbs,
         .signalSemaphoreCount = ns, .pSignalSemaphores = sigs,
      };
      res = wsi_real_QueueSubmit(queue, 1, &si, (i == submitCount - 1) ? fence : VK_NULL_HANDLE);
   }
   return res;
}
'''
    anchor = "VkResult\nwsi_device_init("
    hook_old = "   {\n#define CHK(f) if (!wsi->f) WSI_TRACE("
    hook_new = ("   wsi_real_QueueSubmit = (PFN_vkQueueSubmit)proc_addr(pdevice, \"vkQueueSubmit\");\n"
                "   wsi->QueueSubmit2 = wsi_compat_queue_submit2;\n"
                "   WSI_TRACE(\"[WSI] QueueSubmit2 -> compat (QueueSubmit v1) wsi-build=prof2 real=%p\\n\", (void *)wsi_real_QueueSubmit);\n")
    if "wsi_compat_queue_submit2" in t:
        print("-> compat QueueSubmit2 ya aplicado")
    elif anchor in t and hook_old in t:
        t = t.replace(anchor, compat + "\n" + anchor, 1)
        t = t.replace(hook_old, hook_new + hook_old, 1)
        open(path, "w").write(t)
        print("-> compat QueueSubmit2 aplicado en " + path)
    else:
        print("-> AVISO: no se encontró el ancla para el compat de QueueSubmit2 (anchor=%s hook=%s)" % (anchor in t, hook_old in t))
PY

# vkDestroyDevice: la Mali aborta (scudo/free) dentro de su vkDestroyDevice al cerrar el proceso.
# Se reescribe wrapper_DestroyDevice: DeviceWaitIdle, trazas [DESTROY], limpieza de objetos internos
# (null_*, bcn_*) y modos de diagnóstico con WRAPPER_DESTROY_MODE=0..3.
python3 - <<'PY'
import re
path = "src/vulkan/wrapper/wrapper_device.c"
try:
    t = open(path).read()
except FileNotFoundError:
    t = None
if t is not None:
    new_fn = open("/tmp/dd_new_fn.c").read() if False else r'''/* ---- vkDestroyDevice: orden de limpieza + trazas + modos de diagnóstico ----
 * WRAPPER_DESTROY_MODE (la Mali aborta al liberar la memoria ANTES que los buffers enlazados a ella):
 *   0  limpieza antigua (memoria, buffers, imágenes, fences): reproduce el abort
 *   1  igual que 0, pero sin llamar a vkFreeCommandBuffers del driver
 *   2  no destruye hijos; solo vkDestroyDevice, que los libera él (alternativa probada)
 *   3  no llama ni a vkDestroyDevice del driver (fuga a propósito; el proceso está terminando)
 *   4  (defecto, probado) limpieza ordenada: cmdbufs, fences, buffers, imágenes y la memoria al final */
static int
wrapper_destroy_mode(void)
{
   static int mode = -1;
   if (mode == -1)
      mode = getenv("WRAPPER_DESTROY_MODE") ? atoi(getenv("WRAPPER_DESTROY_MODE")) : 4;
   return mode;
}

#define DDT(...) do { if (wrapper_diag_on()) wrapper_diag_append("[DESTROY] " __VA_ARGS__); } while (0)

VKAPI_ATTR void VKAPI_CALL
wrapper_DestroyDevice(VkDevice _device, const VkAllocationCallbacks* pAllocator)
{
   VK_FROM_HANDLE(wrapper_device, device, _device);
   const struct vk_device_dispatch_table *dt = &device->dispatch_table;
   const bool have_driver = device->dispatch_handle != VK_NULL_HANDLE;
   const int mode = wrapper_destroy_mode();

   if (wrapper_diag_on()) {
      unsigned n_cb = 0, n_mem = 0, n_buf = 0, n_img = 0, n_fence = 0;
      list_for_each_entry(struct wrapper_command_buffer, c, &device->command_buffer_list, link) n_cb++;
      list_for_each_entry(struct wrapper_device_memory, m, &device->device_memory_list, link) n_mem++;
      list_for_each_entry(struct wrapper_buffer, b, &device->buffer_list, link) n_buf++;
      list_for_each_entry(struct wrapper_image, i, &device->image_list, link) n_img++;
      list_for_each_entry(struct wrapper_fence, f, &device->fence_list, link) n_fence++;
      DDT("inicio mode=%d cmdbufs=%u memory=%u buffers=%u images=%u fences=%u\n",
          mode, n_cb, n_mem, n_buf, n_img, n_fence);
   }

   /* La spec exige que el device esté ocioso al destruirlo; al cerrar el proceso
    * puede quedar trabajo del WSI (blit/present) en vuelo. */
   if (have_driver && mode != 3 && dt->DeviceWaitIdle) {
      VkResult idle = dt->DeviceWaitIdle(device->dispatch_handle);
      DDT("DeviceWaitIdle -> %d\n", idle);
   }

   wrapper_query_reset_finish(device);

   const bool ordered = (mode == 4);
   if (mode < 2 || ordered) {
      simple_mtx_lock(&device->resource_mutex);

      list_for_each_entry_safe(struct wrapper_command_buffer, wcb,
                               &device->command_buffer_list, link) {
         if (mode == 1) {
            /* El pool puede haber desaparecido ya: no se vuelve a liberar en el driver. */
            if (device->emulate_push_descriptor)
               wrapper_push_pool_destroy_all(wcb);
            wrapper_dynamic_render_objects_reset(wcb);
            list_del(&wcb->link);
            vk_object_free(&device->vk, &device->vk.alloc, wcb);
         } else {
            wrapper_command_buffer_destroy(device, wcb);
         }
      }
      DDT("command buffers liberados\n");
      if (!ordered) {
         list_for_each_entry_safe(struct wrapper_device_memory, mem,
                                  &device->device_memory_list, link) {
            wrapper_device_memory_destroy(mem);
         }
         DDT("memoria liberada\n");
      }

      simple_mtx_unlock(&device->resource_mutex);

      if (ordered) {
         list_for_each_entry_safe(struct wrapper_fence, wf,
                                  &device->fence_list, link) {
            wrapper_fence_destroy(device, wf, pAllocator);
         }
         DDT("fences destruidos\n");
      }
      list_for_each_entry_safe(struct wrapper_buffer, wb,
                               &device->buffer_list, link) {
         wrapper_buffer_destroy(device, wb, pAllocator);
      }
      DDT("buffers destruidos\n");
      list_for_each_entry_safe(struct wrapper_image, wi,
                               &device->image_list, link) {
         wrapper_image_destroy(device, wi, pAllocator);
      }
      DDT("imagenes destruidas\n");
      if (!ordered) {
         list_for_each_entry_safe(struct wrapper_fence, wf,
                                  &device->fence_list, link) {
            wrapper_fence_destroy(device, wf, pAllocator);
         }
         DDT("fences destruidos\n");
      } else {
         /* La memoria, la última: ya no queda ningún recurso enlazado a ella. */
         simple_mtx_lock(&device->resource_mutex);
         list_for_each_entry_safe(struct wrapper_device_memory, mem,
                                  &device->device_memory_list, link) {
            wrapper_device_memory_destroy(mem);
         }
         simple_mtx_unlock(&device->resource_mutex);
         DDT("memoria liberada\n");
      }

      /* Objetos propios del wrapper que antes se quedaban vivos en el driver. */
      if (have_driver) {
         if (device->null_sampler)
            dt->DestroySampler(device->dispatch_handle, device->null_sampler, NULL);
         if (device->null_image_view)
            dt->DestroyImageView(device->dispatch_handle, device->null_image_view, NULL);
         if (device->null_image)
            dt->DestroyImage(device->dispatch_handle, device->null_image, NULL);
         if (device->null_image_memory)
            dt->FreeMemory(device->dispatch_handle, device->null_image_memory, NULL);
         if (device->null_buffer)
            dt->DestroyBuffer(device->dispatch_handle, device->null_buffer, NULL);
         if (device->null_buffer_memory)
            dt->FreeMemory(device->dispatch_handle, device->null_buffer_memory, NULL);
         if (device->bcn_pipeline)
            dt->DestroyPipeline(device->dispatch_handle, device->bcn_pipeline, NULL);
         if (device->bcn_pipe_layout)
            dt->DestroyPipelineLayout(device->dispatch_handle, device->bcn_pipe_layout, NULL);
         if (device->bcn_set_layout)
            dt->DestroyDescriptorSetLayout(device->dispatch_handle, device->bcn_set_layout, NULL);
         if (device->bcn_shader)
            dt->DestroyShaderModule(device->dispatch_handle, device->bcn_shader, NULL);
         DDT("objetos internos destruidos\n");
      }
   }

   hash_table_u64_foreach(device->dynamic_pipeline_table, entry) {
      struct wrapper_dynamic_pipeline *pipeline = entry.data;
      if (pipeline->render_pass && (mode < 2 || ordered))
         device->dispatch_table.DestroyRenderPass(device->dispatch_handle,
                                                   pipeline->render_pass, NULL);
      free(pipeline);
   }
   hash_table_u64_foreach(device->image_view_table, entry)
      free(entry.data);
   hash_table_u64_foreach(device->imageless_fb_table, entry)
      free(entry.data);
   _mesa_hash_table_u64_destroy(device->dynamic_pipeline_table);
   _mesa_hash_table_u64_destroy(device->image_view_table);
   _mesa_hash_table_u64_destroy(device->imageless_fb_table);

   list_for_each_entry_safe(struct vk_queue, queue, &device->vk.queues, link) {
      vk_queue_finish(queue);
      vk_free2(&device->vk.alloc, pAllocator, queue);
   }
   DDT("colas liberadas\n");
   if (have_driver) {
      if (mode == 3) {
         DDT("modo 3: no se llama a vkDestroyDevice del driver\n");
      } else {
         DDT("llamando a vkDestroyDevice del driver\n");
         device->dispatch_table.DestroyDevice(device->dispatch_handle, pAllocator);
         DDT("vkDestroyDevice del driver devolvio\n");
      }
   }
   if (device->emulate_push_descriptor) {
      hash_table_u64_foreach(device->push_dsl_table, e)
         free(e.data);
      hash_table_u64_foreach(device->push_pl_table, e) {
         struct wrapper_push_pl *r = e.data;
         free(r->set_layouts);
         free(r);
      }
      hash_table_u64_foreach(device->push_template_table, e)
         free(e.data);
      _mesa_hash_table_u64_destroy(device->push_dsl_table);
      _mesa_hash_table_u64_destroy(device->push_pl_table);
      _mesa_hash_table_u64_destroy(device->push_template_table);
      simple_mtx_destroy(&device->push_mutex);
   }
   simple_mtx_destroy(&device->resource_mutex);
   simple_mtx_destroy(&device->query_reset_mutex);
   vk_device_finish(&device->vk);
   vk_free2(&device->vk.alloc, pAllocator, device);
}
'''
    if "wrapper_destroy_mode" in t:
        print("-> DestroyDevice ya parcheado")
    else:
        m = re.search(r"VKAPI_ATTR void VKAPI_CALL\s*\nwrapper_DestroyDevice\(", t)
        end_marker = "   vk_free2(&device->vk.alloc, pAllocator, device);\n}\n"
        e = t.find(end_marker, m.start()) if m else -1
        if m and e != -1:
            t = t[:m.start()] + new_fn + t[e + len(end_marker):]
            open(path, "w").write(t)
            print("-> wrapper_DestroyDevice reescrita en " + path)
        else:
            print("-> AVISO: no se encontró wrapper_DestroyDevice en " + path)
PY

# El WSI necesita el camino sw (buffer + xcb_put_image): sin él usa DRI3/DMA-BUF (inexistente en la Mali) y no presenta.
# Se fija por defecto al cargar la librería (antes de wsi_device_init); MIT-SHM queda activo (probado).
python3 - <<'PY'
path = "src/vulkan/wsi/wsi_common.c"
try:
    t = open(path).read()
except FileNotFoundError:
    t = None
if t is not None:
    snippet = r'''

/* --- camino de presentación por defecto del wrapper: sw (buffer + xcb_put_image) ---
 * WRAPPER_WSI_NATIVE=1 desactiva este ajuste; WRAPPER_WSI_NOSHM=1 añade "noshm" (sin MIT-SHM). */
#include <stdlib.h>
#include <string.h>
__attribute__((constructor(101)))
static void
wsi_default_env_init(void)
{
   const char *native = getenv("WRAPPER_WSI_NATIVE");
   if (native && atoi(native))
      return;
   const char *noshm_env = getenv("WRAPPER_WSI_NOSHM");
   const int want_noshm = noshm_env && atoi(noshm_env);
   const char *cur = getenv("MESA_VK_WSI_DEBUG");
   char buf[256];
   snprintf(buf, sizeof(buf), "%s", cur ? cur : "");
   if (!strstr(buf, "sw")) {
      size_t n = strlen(buf);
      snprintf(buf + n, sizeof(buf) - n, "%ssw", n ? "," : "");
   }
   if (want_noshm && !strstr(buf, "noshm")) {
      size_t n = strlen(buf);
      snprintf(buf + n, sizeof(buf) - n, "%snoshm", n ? "," : "");
   }
   setenv("MESA_VK_WSI_DEBUG", buf, 1);
}
'''
    if "wsi_default_env_init" in t:
        print("-> entorno WSI por defecto ya aplicado")
    else:
        open(path, "a").write(snippet)
        print("-> MESA_VK_WSI_DEBUG=sw por defecto (MIT-SHM activo) en " + path)
PY

# wrapper_device_memory.c: mem->alloc se guardaba como &device->vk.alloc cuando la app no daba allocator, y
# wrapper_device_memory_reset lo pasaba a FreeMemory del driver. La Mali reservó con pAllocator=NULL, así que
# liberaba con callbacks ajenos -> free() inválido (scudo) -> abort en vkDestroyDevice.
# vk_free2() ya hace fallback al allocator del device si alloc es NULL, así que basta guardar el original.
python3 - <<'PY'
path = "src/vulkan/wrapper/wrapper_device_memory.c"
try:
    t = open(path).read()
except FileNotFoundError:
    t = None
if t is not None:
    old = "(*out_mem)->alloc = alloc ? alloc : &device->vk.alloc;"
    new = "(*out_mem)->alloc = alloc;   /* NULL si la app no dio allocator: se reenvia igual al driver */"
    if new in t:
        print("-> mem->alloc ya parcheado")
    elif old in t:
        open(path, "w").write(t.replace(old, new, 1))
        print("-> mem->alloc: ya no se pasa el allocator de Mesa al FreeMemory del driver")
    else:
        print("-> AVISO: no se encontro mem->alloc en " + path)
PY

# Perfilado de la presentación (activo con WRAPPER_DIAG=1, o WRAPPER_PROF=1): tiempo medio por fase cada 60 presents.
python3 - <<'PY'
path = "src/vulkan/wsi/wsi_common.c"
try:
    t = open(path).read()
except FileNotFoundError:
    t = None
if t is not None:
    helper = r"""
/* --- perfilado de vkQueuePresentKHR (WRAPPER_PROF=1) --- */
static int
wsi_prof_enabled(void)
{
   static int on = -1;
   if (on < 0)
      on = getenv("WRAPPER_PROF") ? atoi(getenv("WRAPPER_PROF")) :
           (getenv("WRAPPER_DIAG") ? atoi(getenv("WRAPPER_DIAG")) : 0);
   return on;
}

static uint64_t
wsi_prof_now(void)
{
   struct timespec ts;
   if (!wsi_prof_enabled())
      return 0;
   clock_gettime(CLOCK_MONOTONIC, &ts);
   return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

static void
wsi_prof_frame(uint64_t t0, uint64_t t1, uint64_t t2, uint64_t tw, uint64_t tp)
{
   static uint64_t last_end, n, a_gap, a_thr, a_sub, a_wait, a_put, a_tot, win0;
   if (!wsi_prof_enabled())
      return;
   uint64_t t3 = wsi_prof_now();
   static int announced;
   if (!announced) {
      announced = 1;
      WSI_TRACE("[PROF] perfilado activo (informe cada 60 presents)\n");
   }
   if (!n)
      win0 = t0;
   if (last_end)
      a_gap += t0 - last_end;
   a_thr += t1 - t0;
   a_sub += t2 - t1;
   a_wait += tw;
   a_put += tp;
   a_tot += t3 - t0;
   last_end = t3;
   if (++n == 60) {
      double f = 1e-6 / (double)n;   /* ns -> ms por frame */
      WSI_TRACE(
         "[PROF] fps=%.1f | app/entre_presents=%.2fms | throttle=%.2fms submit=%.2fms"
         " espera_blit=%.2fms put_image=%.2fms | present_total=%.2fms\n",
         (double)n * 1e9 / (double)(t3 - win0), a_gap * f, a_thr * f, a_sub * f,
         a_wait * f, a_put * f, a_tot * f);
      n = a_gap = a_thr = a_sub = a_wait = a_put = a_tot = 0;
   }
}

"""
    edits = [
      ("VkResult\nwsi_common_queue_present(const struct wsi_device *wsi,",
       helper + "VkResult\nwsi_common_queue_present(const struct wsi_device *wsi,", False),
      ("   uint32_t current_frame = p_atomic_fetch_add(&dev->current_frame, 1);\n",
       "   uint32_t current_frame = p_atomic_fetch_add(&dev->current_frame, 1);\n"
       "   uint64_t _p_t0 = wsi_prof_now(), _p_t1 = 0, _p_t2 = 0, _p_tw = 0, _p_tp = 0;\n"
       "   { static unsigned _pc; if (_pc == 0 || ((_pc + 1) % 60) == 0) WSI_TRACE(\"[PROF] present #%u hook ok (WRAPPER_PROF=%s)\\n\", _pc + 1, getenv(\"WRAPPER_PROF\") ? getenv(\"WRAPPER_PROF\") : \"(unset)\"); _pc++; }\n", False),
      ("wsi-build=prof2 real=%p", "wsi-build=prof2+hook real=%p", False),
      ("   /* Gather up all the semaphores we need to wait on */\n",
       "   _p_t1 = wsi_prof_now();\n   /* Gather up all the semaphores we need to wait on */\n", False),
      ("   /* Now do blits on any blit queues */\n",
       "   _p_t2 = wsi_prof_now();\n   /* Now do blits on any blit queues */\n", False),
      ("      if (wsi->sw) {\n         wsi->WaitForFences(",
       "      uint64_t _pw0 = wsi_prof_now();\n      if (wsi->sw) {\n         wsi->WaitForFences(", False),
      ("      const VkPresentRegionKHR *region = NULL;\n",
       "      _p_tw += wsi_prof_now() - _pw0;\n      const VkPresentRegionKHR *region = NULL;\n", False),
      ("      results[i] = swapchain->queue_present(swapchain, image_index,",
       "      uint64_t _pp0 = wsi_prof_now();\n      results[i] = swapchain->queue_present(swapchain, image_index,", False),
      ("      if (results[i] != VK_SUCCESS && results[i] != VK_SUBOPTIMAL_KHR)\n",
       "      _p_tp += wsi_prof_now() - _pp0;\n      if (results[i] != VK_SUCCESS && results[i] != VK_SUBOPTIMAL_KHR)\n", False),
      ("   STACK_ARRAY_FINISH(results);\n",
       "   wsi_prof_frame(_p_t0, _p_t1, _p_t2, _p_tw, _p_tp);\n   STACK_ARRAY_FINISH(results);\n", False),
    ]
    if "wsi_prof_frame" in t:
        print("-> perfilado WSI ya aplicado")
    else:
        missing = [e[0].strip().splitlines()[0][:60] for e in edits if t.count(e[0]) != 1]
        if missing:
            print("-> AVISO: anclas del perfilado no encontradas (o repetidas): %s" % missing)
        else:
            for old, new, _ in edits:
                t = t.replace(old, new, 1)
            open(path, "w").write(t)
            print("-> perfilado WRAPPER_PROF aplicado en " + path)
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
