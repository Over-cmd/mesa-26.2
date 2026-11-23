#ifndef __WRAPPER_UTIL_H
#define __WRAPPER_UTIL_H

#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#include <stdbool.h>

/* Macro segura y estándar (con do-while para evitar errores en if/else) */
#define CREATE_FOLDER(folder, mode) \
do { \
   struct stat sb; \
   if (stat(folder, &sb) != 0 || !S_ISDIR(sb.st_mode)) { \
      /* Sin umask(0): umask es del PROCESO entero y durante esa ventana cualquier otro hilo creaba ficheros con permisos \
         0666/0777. mkdir respeta la umask y el chmod posterior fija el modo pedido. */ \
      if (mkdir(folder, (mode)) == 0) \
         chmod(folder, (mode)); \
   } \
} while (0)

#ifdef __cplusplus
extern "C" {
#endif

char *
get_executable_name(void);

#ifdef __cplusplus
}
#endif

#endif /* __WRAPPER_UTIL_H */
