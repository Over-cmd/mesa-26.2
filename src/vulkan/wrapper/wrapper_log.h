#ifndef WRAPPER_LOG_H
#define WRAPPER_LOG_H

#include <unistd.h>
#include <libgen.h>
#include <string.h>
#include <stdlib.h>
#include <stdint.h>
#include <vulkan/vulkan.h>

#define WRAPPER_LOG_INFO       (1ull << 0)
#define WRAPPER_LOG_ERROR      (1ull << 1)
#define WRAPPER_LOG_SHADER     (1ull << 2)
#define WRAPPER_LOG_VALIDATION (1ull << 3)
#define WRAPPER_LOG_BCN        (1ull << 4)
#define WRAPPER_LOG_APIDUMP    (1ull << 5)
#define WRAPPER_LOG_WARN       (1ull << 6)   /* WRAPPER_LOG(warn, ...) se usaba pero este nivel no existía: nunca se registraba */
#define WRAPPER_LOG_DEBUG      (1ull << 7)   /* ídem para WRAPPER_LOG(debug, ...) */

struct wrapper_log {
   const char *name; /* Seguridad de tipos mejorada */
   unsigned long long value;
};

#ifdef __cplusplus
extern "C" {
#endif

void
init_wrapper_logging(void);

void                                                                         
dump_shader_code(const uint32_t *code, size_t size);

int 
get_wrapper_log_level(const char *option);

#if defined(__GNUC__) || defined(__clang__)
__attribute__((format(printf, 1, 3)))
#endif
void
write_to_logfile(const char *fmt, const char *level, ...);

/* Callback de Vulkan protegido correctamente dentro de extern "C" */
VKAPI_ATTR VkBool32 VKAPI_CALL 
wrapper_debug_utils_messenger(VkDebugUtilsMessageSeverityFlagBitsEXT messageSeverity,
                              VkDebugUtilsMessageTypeFlagsEXT messageTypes,
                              const VkDebugUtilsMessengerCallbackDataEXT *callbackData,
                              void *userData);

#ifdef __cplusplus
}
#endif

#define WRAPPER_LOG_LEVEL(s) (get_wrapper_log_level(#s))

/* 
 * MODO HÍBRIDO UNIFICADO:
 * Si defines WRAPPER_NO_LOGS al compilar, se anulan los logs para máximo rendimiento.
 * Si no, funcionan de manera condicional según la variable de entorno.
 */
#ifdef WRAPPER_NO_LOGS
#define WRAPPER_LOG(level, fmt, ...) do { } while (0)
#else
#define WRAPPER_LOG(level, fmt, ...) \
do {\
   if (WRAPPER_LOG_LEVEL(level)) {\
      write_to_logfile(fmt, #level, ##__VA_ARGS__); \
   }\
} while (0)
#endif

#endif /* WRAPPER_LOG_H */
