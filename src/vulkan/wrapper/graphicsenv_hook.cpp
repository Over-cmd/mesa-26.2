#include <stdlib.h>
#include <stdio.h>
#include <dlfcn.h>
#include <string>
#include <iterator>
#include <fstream>
#include <unistd.h>
#include <mutex>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <sys/stat.h>
#include <dirent.h>
#include <sys/system_properties.h>

#include "graphicsenv_hook.hpp"
#include "wrapper_log.h"

#ifndef LIBNAME_64
#define LIBNAME_64 "/system/lib64/libgraphicsenv.so"
#endif
#ifndef LIBNAME_32
#define LIBNAME_32 "/system/lib/libgraphicsenv.so"
#endif

/* Verificación posterior con getLayerPaths(): lee un std::string devuelto por libc++ de la plataforma (std::__1)
   usando el de tu libc++ (std::__ndk1). Si el layout difiere puede dar basura o fallar. Ponga 0 para desactivarla. */
#ifndef GRAPHICSENV_HOOK_VERIFY
#define GRAPHICSENV_HOOK_VERIFY 1
#endif

static void *graphicsenv_handle = nullptr;
static std::mutex hook_mutex;
static bool init_attempted = false;
static bool init_success = false;
static std::string current_applied_paths = "";
static std::string last_custom_path = "";
static GraphicsEnvHookStatus current_status = GRAPHICSENV_STATUS_NOT_INITIALIZED;

// Definiciones de tipos para los punteros de función
static void *(*getInstance)() = nullptr;
static const std::string& (*getLayerPaths)(void *) = nullptr;
static void *(*getAppNamespace)(void *) = nullptr;

// Variantes de setLayerPaths (por referencia `const std::string&` y por valor `std::string`)
static void (*setLayerPaths_ref)(void *, void *, const std::string&) = nullptr;
static void (*setLayerPaths_val)(void *, void *, std::string) = nullptr;

template <typename T>
static inline void get_function_pointer(T& pointer, void* handle, const char* name) {
   if (!pointer && handle) {
      dlerror(); // Limpiar errores previos
      pointer = reinterpret_cast<T>(dlsym(handle, name));
      if (!pointer) {
         const char *err = dlerror();
         if (err) {
            WRAPPER_LOG(debug, "Symbol %s not found: %s", name, err);
         }
      }
   }
}

static int get_android_sdk_version() {
   char sdk_version_str[PROP_VALUE_MAX] = {0};
   if (__system_property_get("ro.build.version.sdk", sdk_version_str) > 0) {
      return std::atoi(sdk_version_str);
   }
   return 0;
}

static bool file_exists(const std::string& path) {
   struct stat buffer;
   return (stat(path.c_str(), &buffer) == 0 && S_ISREG(buffer.st_mode));
}

/* ¿La lista "a:b:c" ya contiene exactamente 'dir'? (comparación por elemento, no por subcadena) */
static bool path_list_contains(const std::string& list, const std::string& dir) {
   size_t start = 0;
   while (true) {
      size_t end = list.find(':', start);
      std::string tok = list.substr(start, end == std::string::npos ? std::string::npos : end - start);
      if (tok == dir) return true;
      if (end == std::string::npos) return false;
      start = end + 1;
   }
}

/* Añade a 'dest' los elementos de 'src' que no estén ya, sin duplicados ni elementos vacíos */
static void merge_path_list(std::string& dest, const std::string& src) {
   size_t start = 0;
   while (start <= src.size()) {
      size_t end = src.find(':', start);
      std::string tok = src.substr(start, end == std::string::npos ? std::string::npos : end - start);
      if (!tok.empty() && !path_list_contains(dest, tok)) {
         if (!dest.empty()) dest += ":";
         dest += tok;
      }
      if (end == std::string::npos) break;
      start = end + 1;
   }
}

/* Extrae "api_version": "X.Y.Z" de un manifiesto de capa */
static bool parse_manifest_api_version(const std::string& json, int& major, int& minor) {
   size_t key = json.find("\"api_version\"");
   if (key == std::string::npos) return false;
   size_t colon = json.find(':', key);
   if (colon == std::string::npos) return false;
   size_t q1 = json.find('"', colon);
   if (q1 == std::string::npos) return false;
   size_t q2 = json.find('"', q1 + 1);
   if (q2 == std::string::npos) return false;
   std::string v = json.substr(q1 + 1, q2 - q1 - 1);
   return sscanf(v.c_str(), "%d.%d", &major, &minor) == 2;
}

// Comprobación informativa para Mali G52 (Vulkan 1.0 / 1.1). Devuelve false si el manifiesto sugiere algo > 1.1;
// el resultado solo se usa para avisar en el log, nunca bloquea la capa.
static bool validate_layer_compatibility_for_mali_v11(const std::string& json_filepath) {
   std::ifstream file(json_filepath);
   if (!file.is_open()) return true; // Si no se puede abrir, no bloqueamos preventivamente

   std::string content((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
   bool ok = true;

   int maj = 0, min = 0;
   if (parse_manifest_api_version(content, maj, min) && (maj > 1 || (maj == 1 && min > 1))) {
      WRAPPER_LOG(warn, "    [Mali G52 / Vulkan 1.0-1.1] Layer manifest %s declares api_version %d.%d (> 1.1); may not work on a 1.1 driver.", json_filepath.c_str(), maj, min);
      ok = false;
   }
   // Extensiones de Vulkan 1.2+ que el Mali G52 no expone
   if (content.find("VK_KHR_buffer_device_address") != std::string::npos ||
       content.find("VK_EXT_descriptor_indexing") != std::string::npos ||
       content.find("VK_KHR_timeline_semaphore") != std::string::npos) {
      WRAPPER_LOG(warn, "    [Mali G52 / Vulkan 1.0-1.1 Warning] Layer manifest %s references Vulkan 1.2+ extensions (BufferDeviceAddress/DescriptorIndexing/TimelineSemaphore) that may require emulation or cause issues.", json_filepath.c_str());
      ok = false;
   }
   return ok;
}

static void parse_and_validate_layer_json(const std::string& json_filepath, const std::string& dir_path) {
   std::ifstream file(json_filepath);
   if (!file.is_open()) return;

   std::string line;
   std::string layer_name = "";
   std::string library_path = "";

   // Lector línea por línea (preservando tu lógica original)
   while (std::getline(file, line)) {
      if (layer_name.empty()) {
         size_t name_pos = line.find("\"name\"");
         if (name_pos != std::string::npos) {
            size_t f_q = line.find('"', name_pos + 6);
            size_t s_q = (f_q != std::string::npos) ? line.find('"', f_q + 1) : std::string::npos;
            if (f_q != std::string::npos && s_q != std::string::npos) {
               layer_name = line.substr(f_q + 1, s_q - f_q - 1);
            }
         }
      }
      if (library_path.empty()) {
         size_t lib_pos = line.find("\"library_path\"");
         if (lib_pos != std::string::npos) {
            size_t f_q = line.find('"', lib_pos + 14);
            size_t s_q = (f_q != std::string::npos) ? line.find('"', f_q + 1) : std::string::npos;
            if (f_q != std::string::npos && s_q != std::string::npos) {
               library_path = line.substr(f_q + 1, s_q - f_q - 1);
            }
         }
      }
   }

   // RED DE SEGURIDAD: Si el JSON estaba en una sola línea (minificado) y falló el bucle anterior, lo leemos completo
   if (layer_name.empty()) {
      file.clear();
      file.seekg(0, std::ios::beg);
      std::string content((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());

      auto extract_fallback = [](const std::string& json, const std::string& key) -> std::string {
         size_t key_pos = json.find("\"" + key + "\"");
         if (key_pos == std::string::npos) return "";
         size_t colon_pos = json.find(':', key_pos);
         if (colon_pos == std::string::npos) return "";
         size_t f_q = json.find('"', colon_pos);
         if (f_q == std::string::npos) return "";
         size_t s_q = json.find('"', f_q + 1);
         if (s_q == std::string::npos) return "";
         return json.substr(f_q + 1, s_q - f_q - 1);
      };

      layer_name = extract_fallback(content, "name");
      if (library_path.empty()) {
         library_path = extract_fallback(content, "library_path");
      }
   }

   if (!layer_name.empty()) {
      WRAPPER_LOG(info, " -> Detected Vulkan Layer Name: '%s' (Manifest: %s)", layer_name.c_str(), json_filepath.c_str());

      // Validar compatibilidad específica para Mali G52 (Vulkan 1.0 / 1.1): solo informativo
      (void)validate_layer_compatibility_for_mali_v11(json_filepath);

      if (!library_path.empty()) {
         std::string full_lib_path = (library_path[0] == '/') ? library_path : (dir_path + "/" + library_path);
         if (file_exists(full_lib_path)) {
            WRAPPER_LOG(info, "    Library binary verified successfully at: %s", full_lib_path.c_str());
         } else {
            WRAPPER_LOG(warn, "    WARNING: Library binary NOT found at expected path: %s", full_lib_path.c_str());
         }
      }
   }
}

static int inspect_vulkan_manifests_in_dir(const std::string& dir_path) {
   DIR *dir = opendir(dir_path.c_str());
   if (!dir) return 0;

   int count = 0;
   struct dirent *entry;
   while ((entry = readdir(dir)) != nullptr) {
      std::string filename(entry->d_name);
      if (filename.length() <= 5 || filename.substr(filename.length() - 5) != ".json") continue;

      bool is_file = (entry->d_type == DT_REG || entry->d_type == DT_LNK);
      if (entry->d_type == DT_UNKNOWN) {          // algunos sistemas de archivos no informan el tipo
         is_file = file_exists(dir_path + "/" + filename);
      }
      if (is_file) {
         count++;
         parse_and_validate_layer_json(dir_path + "/" + filename, dir_path);
      }
   }
   closedir(dir);
   return count;
}

static void append_valid_dir_and_subdirs(std::string& combined_path, const std::string& base_dir) {
   struct stat sb;
   const char* subdirs[] = { "", "/vulkan/implicit_layer.d", "/vulkan/explicit_layer.d" };

   for (const char* sub : subdirs) {
      std::string target = base_dir + sub;
      if (stat(target.c_str(), &sb) == 0 && S_ISDIR(sb.st_mode)) {
         if (!path_list_contains(combined_path, target)) {     // antes: find() de subcadena (descartaba rutas distintas)
            if (!combined_path.empty()) combined_path += ":";
            combined_path += target;
         }
      }
   }
}

static std::string build_auto_layers_path() {
   std::string combined_path = "";

   const char* candidate_paths[] = {
      "/data/data/com.bannerlator/files/imagefs/usr/lib",
      "/data/data/com.winlator.cmod/files/imagefs/usr/lib",
      "/data/data/com.winlator/files/imagefs/usr/lib",
      "/data/data/com.winlator.frost/files/imagefs/usr/lib",
      "/data/data/com.termux/files/usr/lib",
      "/usr/lib",
      "/usr/local/lib"
   };

   for (const char* path_str : candidate_paths) {
      append_valid_dir_and_subdirs(combined_path, path_str);
   }

   const char *home = getenv("HOME");
   if (home && home[0] != '\0') {
      append_valid_dir_and_subdirs(combined_path, std::string(home) + "/../files/imagefs/usr/lib");
   }

   return combined_path;
}

// Lógica interna reutilizable para inicialización y reseteo
static bool internal_apply_layer_paths(const char* explicit_custom_path) {
   try {
      int sdk_ver = get_android_sdk_version();
      WRAPPER_LOG(info, "Initializing GraphicsEnv hook (v%d.%d.%d) for Mali G52 / Vulkan 1.0-1.1. Android SDK: %d",
                  GRAPHICSENV_HOOK_VERSION_MAJOR, GRAPHICSENV_HOOK_VERSION_MINOR,
                  GRAPHICSENV_HOOK_VERSION_PATCH, sdk_ver);

      if (!graphicsenv_handle) {
         graphicsenv_handle = dlopen(LIBNAME_64, RTLD_NOW | RTLD_GLOBAL);
         if (!graphicsenv_handle) {
            WRAPPER_LOG(warn, "Failed to dlopen 64-bit %s: %s. Trying 32-bit fallback...", LIBNAME_64, dlerror());
            graphicsenv_handle = dlopen(LIBNAME_32, RTLD_NOW | RTLD_GLOBAL);
         }
         if (!graphicsenv_handle) {
            WRAPPER_LOG(error, "Failed to dlopen libgraphicsenv.so in both 64-bit and 32-bit paths: %s", dlerror());
            current_status = GRAPHICSENV_STATUS_DLOPEN_FAILED;
            return false;
         }
      }

      get_function_pointer(getInstance, graphicsenv_handle, "_ZN7android11GraphicsEnv11getInstanceEv");
      get_function_pointer(getLayerPaths, graphicsenv_handle, "_ZN7android11GraphicsEnv13getLayerPathsEv");
      get_function_pointer(getAppNamespace, graphicsenv_handle, "_ZN7android11GraphicsEnv15getAppNamespaceEv");
      get_function_pointer(setLayerPaths_ref, graphicsenv_handle, "_ZN7android11GraphicsEnv13setLayerPathsEPNS_21NativeLoaderNamespaceERKNSt3__112basic_stringIcNS3_11char_traitsIcEENS3_9allocatorIcEEEE");
      if (!setLayerPaths_ref) {
         get_function_pointer(setLayerPaths_val, graphicsenv_handle, "_ZN7android11GraphicsEnv13setLayerPathsEPNS_21NativeLoaderNamespaceENSt3__112basic_stringIcNS3_11char_traitsIcEENS3_9allocatorIcEEEE");
      }

      if (!getInstance || !getAppNamespace || (!setLayerPaths_ref && !setLayerPaths_val)) {
         WRAPPER_LOG(error, "Failed to resolve mandatory function pointers from libgraphicsenv.so");
         current_status = GRAPHICSENV_STATUS_SYMBOL_MISSING;
         return false;
      }

      void *instance = getInstance();
      if (!instance) {
         WRAPPER_LOG(error, "Failed to obtain GraphicsEnv instance (returned null)");
         current_status = GRAPHICSENV_STATUS_INSTANCE_NULL;
         return false;
      }

      void *app_namespace = getAppNamespace(instance);
      if (!app_namespace) {
         WRAPPER_LOG(warn, "GraphicsEnv app namespace is null, continuing anyway...");
      }

      std::string layers_path;
      std::string auto_path = build_auto_layers_path();

      if (explicit_custom_path && explicit_custom_path[0] != '\0') {
         merge_path_list(layers_path, explicit_custom_path);
         merge_path_list(layers_path, auto_path);
         WRAPPER_LOG(info, "Using explicit custom path merged with auto paths: %s", layers_path.c_str());
      } else {
         const char *env = getenv(ENV_WRAPPER_LAYER_PATH);
         const char *vulkan_env = getenv(ENV_VULKAN_LAYER_PATH);

         if (env && env[0] != '\0') {
            merge_path_list(layers_path, env);
            merge_path_list(layers_path, auto_path);
            WRAPPER_LOG(info, "Using %s merged with auto paths: %s", ENV_WRAPPER_LAYER_PATH, layers_path.c_str());
         } else if (vulkan_env && vulkan_env[0] != '\0') {
            merge_path_list(layers_path, vulkan_env);
            merge_path_list(layers_path, auto_path);
            WRAPPER_LOG(info, "Using %s merged with auto paths: %s", ENV_VULKAN_LAYER_PATH, layers_path.c_str());
         } else {
            layers_path = auto_path;
         }
      }

      if (layers_path.empty()) {
         WRAPPER_LOG(warn, "No valid Vulkan layer directories found on disk.");
         current_status = GRAPHICSENV_STATUS_NO_VALID_PATHS;
         return false;
      }

      // Inspeccionar directorios y manifiestos con validación de versión
      size_t start = 0, end = 0;
      while ((end = layers_path.find(':', start)) != std::string::npos) {
         std::string dir = layers_path.substr(start, end - start);
         if (!dir.empty()) {
            int json_count = inspect_vulkan_manifests_in_dir(dir);
            WRAPPER_LOG(info, "Inspected layer directory: %s (Found %d manifest(s))", dir.c_str(), json_count);
         }
         start = end + 1;
      }
      std::string last_dir = layers_path.substr(start);
      if (!last_dir.empty()) {
         int json_count = inspect_vulkan_manifests_in_dir(last_dir);
         WRAPPER_LOG(info, "Inspected layer directory: %s (Found %d manifest(s))", last_dir.c_str(), json_count);
      }

      setenv(ENV_VK_LAYER_PATH, layers_path.c_str(), 1);
      WRAPPER_LOG(info, "Injected %s: %s", ENV_VK_LAYER_PATH, layers_path.c_str());

      const char *enabled_layers = getenv(ENV_WRAPPER_ENABLED_LAYERS);
      if (enabled_layers && enabled_layers[0] != '\0') {
         setenv(ENV_VK_INSTANCE_LAYERS, enabled_layers, 1);
         WRAPPER_LOG(info, "Injected %s from %s: %s", ENV_VK_INSTANCE_LAYERS, ENV_WRAPPER_ENABLED_LAYERS, enabled_layers);
      }

      if (setLayerPaths_ref) {
         setLayerPaths_ref(instance, app_namespace, layers_path);
      } else if (setLayerPaths_val) {
         setLayerPaths_val(instance, app_namespace, layers_path);
      }

#if GRAPHICSENV_HOOK_VERIFY
      if (getLayerPaths) {
         // OJO: un try/catch no protege de un layout de std::string distinto (daría lectura inválida, no excepción)
         try {
            const std::string& path = getLayerPaths(instance);
            if (path.empty() || path.find(layers_path) == std::string::npos) {
               WRAPPER_LOG(warn, "Layer paths verification mismatch. Current: '%s', Expected: '%s'",
                           path.c_str(), layers_path.c_str());
            } else {
               WRAPPER_LOG(info, "Successfully applied and verified layer paths for Mali G52 target.");
            }
         } catch (...) {
            WRAPPER_LOG(warn, "Exception caught during getLayerPaths verification (ABI layout difference).");
         }
      }
#endif

      current_applied_paths = layers_path;
      current_status = GRAPHICSENV_STATUS_OK;
      return true;

   } catch (const std::exception &e) {
      WRAPPER_LOG(error, "Standard exception caught: %s", e.what());
      current_status = GRAPHICSENV_STATUS_EXCEPTION;
      return false;
   } catch (...) {
      WRAPPER_LOG(error, "Unknown exception caught during GraphicsEnv initialization.");
      current_status = GRAPHICSENV_STATUS_EXCEPTION;
      return false;
   }
}

extern "C" {

WRAPPER_API bool set_layer_paths(void) {
   std::lock_guard<std::mutex> lock(hook_mutex);
   if (!init_attempted) {
      init_attempted = true;
      init_success = internal_apply_layer_paths(nullptr);
   }
   return init_success;
}

WRAPPER_API bool set_layer_paths_custom(const char* custom_layers_path) {
   std::lock_guard<std::mutex> lock(hook_mutex);
   init_attempted = true;
   last_custom_path = (custom_layers_path && custom_layers_path[0] != '\0') ? custom_layers_path : "";
   init_success = internal_apply_layer_paths(last_custom_path.empty() ? nullptr : last_custom_path.c_str());
   return init_success;
}

WRAPPER_API bool is_layer_paths_initialized(void) {
   std::lock_guard<std::mutex> lock(hook_mutex);
   return init_success;
}

WRAPPER_API const char* get_applied_layer_paths(void) {
   /* Copia local al hilo: el original devolvía current_applied_paths.c_str(), que otro hilo podía invalidar
      con reset_and_rescan_layer_paths() mientras el llamador aún leía el puntero. */
   thread_local std::string snapshot;
   {
      std::lock_guard<std::mutex> lock(hook_mutex);
      if (!init_success) return nullptr;
      snapshot = current_applied_paths;
   }
   return snapshot.c_str();
}

WRAPPER_API size_t get_applied_layer_paths_copy(char* buffer, size_t capacity) {
   std::lock_guard<std::mutex> lock(hook_mutex);
   if (!init_success) { if (buffer && capacity) buffer[0] = '\0'; return 0; }
   const size_t len = current_applied_paths.size();
   if (buffer && capacity) {
      size_t n = (len < capacity - 1) ? len : capacity - 1;
      memcpy(buffer, current_applied_paths.data(), n);
      buffer[n] = '\0';
   }
   return len;
}

WRAPPER_API GraphicsEnvHookStatus get_graphicsenv_hook_status(void) {
   std::lock_guard<std::mutex> lock(hook_mutex);
   return current_status;
}

WRAPPER_API bool reset_and_rescan_layer_paths(void) {
   std::lock_guard<std::mutex> lock(hook_mutex);
   WRAPPER_LOG(info, "Resetting state and rescanning Vulkan layer paths...");
   init_attempted = true;
   current_applied_paths.clear();
   init_success = internal_apply_layer_paths(last_custom_path.empty() ? nullptr : last_custom_path.c_str());
   return init_success;
}

} // extern "C"
