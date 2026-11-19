#ifndef GRAPHICSENV_HOOK_HPP
#define GRAPHICSENV_HOOK_HPP

#pragma once

#ifndef __cplusplus
#include <stdbool.h>
#endif
#include <stddef.h>

/* ============================================================================
 * VERSIONADO Y METADATOS DEL M脫DULO
 * ============================================================================ */
#define GRAPHICSENV_HOOK_VERSION_MAJOR 1
#define GRAPHICSENV_HOOK_VERSION_MINOR 5
#define GRAPHICSENV_HOOK_VERSION_PATCH 0

#define GRAPHICSENV_HOOK_CHECK_VERSION(major, minor, patch) \
    ((GRAPHICSENV_HOOK_VERSION_MAJOR > (major)) || \
     (GRAPHICSENV_HOOK_VERSION_MAJOR == (major) && GRAPHICSENV_HOOK_VERSION_MINOR > (minor)) || \
     (GRAPHICSENV_HOOK_VERSION_MAJOR == (major) && GRAPHICSENV_HOOK_VERSION_MINOR == (minor) && \
      GRAPHICSENV_HOOK_VERSION_PATCH >= (patch)))

/* ============================================================================
 * CONSTANTES DE VARIABLES DE ENTORNO (WRAPPER Y EST脕NDAR VULKAN)
 * ============================================================================ */
#define ENV_WRAPPER_LAYER_PATH     "WRAPPER_LAYER_PATH"
#define ENV_WRAPPER_ENABLED_LAYERS "WRAPPER_ENABLED_LAYERS"
#define ENV_VULKAN_LAYER_PATH      "VULKAN_LAYER_PATH"
#define ENV_VK_LAYER_PATH          "VK_LAYER_PATH"
#define ENV_VK_INSTANCE_LAYERS     "VK_INSTANCE_LAYERS"

#ifdef __cplusplus
extern "C" {
#endif

/* ============================================================================
 * MACROS DE EXPORTACI脫N DE S脥MBOLOS
 * ============================================================================
 * Garantizan que los s铆mbolos sean visibles externamente al compilar la
 * biblioteca compartida (.so) en Android, incluso con -fvisibility=hidden.
 * (Con #ifndef para no chocar si otro header del wrapper ya define WRAPPER_API.)
 */
#ifndef WRAPPER_API
#  if defined(_MSC_VER)
#    define WRAPPER_API __declspec(dllexport)
#  elif defined(__GNUC__) && __GNUC__ >= 4
#    define WRAPPER_API __attribute__((visibility("default")))
#  else
#    define WRAPPER_API
#  endif
#endif

/* ============================================================================
 * C脫DIGOS DE DIAGN脫STICO DEL HOOK
 * ============================================================================ */
typedef enum {
    GRAPHICSENV_STATUS_OK = 0,
    GRAPHICSENV_STATUS_NOT_INITIALIZED,
    GRAPHICSENV_STATUS_DLOPEN_FAILED,
    GRAPHICSENV_STATUS_SYMBOL_MISSING,
    GRAPHICSENV_STATUS_INSTANCE_NULL,
    GRAPHICSENV_STATUS_NO_VALID_PATHS,
    GRAPHICSENV_STATUS_EXCEPTION
} GraphicsEnvHookStatus;

/* ============================================================================
 * INTERFAZ P脷BLICA DEL HOOK DE GRAPHICSENV
 * ============================================================================ */

/**
 * @brief Configura y aplica las rutas de las capas gr谩ficas (Vulkan layers / GraphicsEnv)
 *        utilizando el auto-descubrimiento de rutas, variables de entorno y validaci贸n de manifiestos.
 *        Solo se intenta una vez; para reintentar use reset_and_rescan_layer_paths().
 * @return true si la inicializaci贸n y aplicaci贸n de rutas fue exitosa, false en caso de error.
 */
WRAPPER_API bool set_layer_paths(void);

/**
 * @brief Igual que set_layer_paths() pero con una ruta expl铆cita
 *        (ej. "/data/data/com.bannerlator/.../usr/lib"). Se recuerda para reset_and_rescan_layer_paths().
 * @param custom_layers_path Rutas de las capas separadas por dos puntos (':').
 */
WRAPPER_API bool set_layer_paths_custom(const char* custom_layers_path);

/** @brief true si el hook ya se inicializ贸 correctamente. */
WRAPPER_API bool is_layer_paths_initialized(void);

/**
 * @brief Devuelve la cadena de rutas aplicada, o NULL si no se ha inicializado.
 *        El puntero es v谩lido hasta la siguiente llamada a esta funci贸n EN EL MISMO HILO
 *        (es una copia local al hilo, as铆 que otro hilo que llame a reset_* no la invalida).
 */
WRAPPER_API const char* get_applied_layer_paths(void);

/**
 * @brief Copia la cadena de rutas aplicada en 'buffer' (siempre terminada en NUL si capacity > 0).
 * @return longitud real de la cadena (sin NUL); si es >= capacity la copia qued贸 truncada; 0 si no hay rutas.
 */
WRAPPER_API size_t get_applied_layer_paths_copy(char* buffer, size_t capacity);

/** @brief C贸digo detallado del 煤ltimo estado de ejecuci贸n del hook. */
WRAPPER_API GraphicsEnvHookStatus get_graphicsenv_hook_status(void);

/**
 * @brief Resetea el estado y re-escanea las rutas y manifiestos de Vulkan, reutilizando la 煤ltima
 *        ruta expl铆cita pasada a set_layer_paths_custom() si la hubo.
 */
WRAPPER_API bool reset_and_rescan_layer_paths(void);

#ifdef __cplusplus
}
#endif

#endif /* GRAPHICSENV_HOOK_HPP */
