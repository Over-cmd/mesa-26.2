#include <stdlib.h>
#include <dlfcn.h>
#include <string>
#include <iostream>
#include <unistd.h>
#include <mutex>

#include "graphicsenv_hook.hpp"
#include "wrapper_log.h"

#define LIBNAME "/system/lib64/libgraphicsenv.so"
#define WRAPPER_LAYERS_PATH "/data/data/com.winlator.cmod/files/imagefs/usr/lib:/data/data/com.termux/files/usr/lib"

static void *graphicsenv_handle = nullptr;
static std::string env_layers_path;
static std::once_flag init_flag;
static bool init_success = false;

static void *(*getInstance)() = nullptr;
static const std::string& (*getLayerPaths)(void *) = nullptr;
static void *(*getAppNamespace)(void *) = nullptr;

// Apuntadores adaptados para evitar fallos de ABI estrictos entre versiones de Android
static void (*setLayerPaths_ref)(void *, void *, const std::string&) = nullptr;
static void (*setLayerPaths_val)(void *, void *, std::string) = nullptr;

template <typename T>
void get_function_pointer(T& pointer, const std::string& name) {
   if (!pointer && graphicsenv_handle) {
      pointer = reinterpret_cast<T>(dlsym(graphicsenv_handle, name.c_str()));
   }
}

extern "C"
bool set_layer_paths() {
   std::call_once(init_flag, []() {
      if (!graphicsenv_handle) {
         graphicsenv_handle = dlopen(LIBNAME, RTLD_NOW | RTLD_GLOBAL);
         if (!graphicsenv_handle) {
            WRAPPER_LOG(error, "Failed to dlopen libgraphicsenv.so: %s", dlerror());
            return;
         }
      }

      // Resolver símbolos y nombres "mangled" para Android Bionic (libc++)
      get_function_pointer(getInstance, "_ZN7android11GraphicsEnv11getInstanceEv");
      get_function_pointer(getLayerPaths, "_ZN7android11GraphicsEnv13getLayerPathsEv");
      get_function_pointer(getAppNamespace, "_ZN7android11GraphicsEnv15getAppNamespaceEv");
      
      // Intentar múltiples variantes de símbolos para setLayerPaths según la versión de Android
      get_function_pointer(setLayerPaths_ref, "_ZN7android11GraphicsEnv13setLayerPathsEPNS_21NativeLoaderNamespaceERKNSt3__112basic_stringIcNS3_11char_traitsIcEENS3_9allocatorIcEEEE");
      if (!setLayerPaths_ref) {
         get_function_pointer(setLayerPaths_val, "_ZN7android11GraphicsEnv13setLayerPathsEPNS_21NativeLoaderNamespaceENSt3__112basic_stringIcNS3_11char_traitsIcEENS3_9allocatorIcEEEE");
      }

      if (!getInstance || !getAppNamespace || (!setLayerPaths_ref && !setLayerPaths_val)) {
         WRAPPER_LOG(error, "Failed to resolve mandatory function pointers from libgraphicsenv.so");
         return;    	
      }

      void *instance = getInstance();
      if (!instance) {
         WRAPPER_LOG(error, "Failed to obtain GraphicsEnv instance");
         return;
      }

      void *app_namespace = getAppNamespace(instance);
      if (!app_namespace) {
         WRAPPER_LOG(warn, "GraphicsEnv app namespace is null, continuing anyway...");
      }

      if (env_layers_path.empty()) {
         const char *env = getenv("WRAPPER_LAYER_PATH");
         if (env && env[0] != '\0') {
            env_layers_path = env;
         } else {
            env_layers_path = WRAPPER_LAYERS_PATH;
         }
      }

      WRAPPER_LOG(info, "Setting Vulkan layer paths to: %s", env_layers_path.c_str());

      // Ejecutar según la firma que haya resuelto el símbolo correctamente
      if (setLayerPaths_ref) {
         setLayerPaths_ref(instance, app_namespace, env_layers_path);
      } else if (setLayerPaths_val) {
         setLayerPaths_val(instance, app_namespace, env_layers_path);
      }

      // Verificación de rutas (rescatado de "nuestro" original)
      if (getLayerPaths) {
         auto path = getLayerPaths(instance);
         if (path.empty() || path.find(env_layers_path) == std::string::npos) {
            WRAPPER_LOG(warn, "Layer paths verification mismatch. Current: '%s', Expected container: '%s'", 
                        path.c_str(), env_layers_path.c_str());
         } else {
            WRAPPER_LOG(info, "Successfully applied and verified layer paths.");
         }
      } else {
         WRAPPER_LOG(info, "Successfully applied layer paths (verification skipped).");
      }

      init_success = true;
   });

   return init_success;
}
