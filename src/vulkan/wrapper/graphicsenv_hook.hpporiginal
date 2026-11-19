#ifndef GRAPHICSENV_HOOK_HPP
#define GRAPHICSENV_HOOK_HPP

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

/* 
 * Configura las rutas de las capas gráficas (Vulkan layers / GraphicsEnv).
 * Utiliza un atributo de visibilidad explícito para garantizar que se exporte 
 * correctamente en la biblioteca compartida (.so) cargada en Android.
 */
#if defined(__GNUC__) && __GNUC__ >= 4
  __attribute__((visibility("default")))
#endif
bool set_layer_paths();

#ifdef __cplusplus
}
#endif

#endif /* GRAPHICSENV_HOOK_HPP */
