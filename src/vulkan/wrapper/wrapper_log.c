#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <time.h>
#include <unistd.h>
#include <fcntl.h>
#include <limits.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <strings.h>
#include <pthread.h>

#include <vulkan/vulkan.h>

#include "wrapper_log.h"
#include "wrapper_util.h"

#ifndef PATH_MAX
#define PATH_MAX 1024
#endif

#define PATH_MAX_SIZE 1024
#ifndef WRAPPER_DIR
#define WRAPPER_DIR "/sdcard/wrapper"
#endif
#define WRAPPER_SHADER_LOG_PATH WRAPPER_DIR "/shaders"
#define WRAPPER_LOG_PATH WRAPPER_DIR "/logs"
#define WRAPPER_VALIDATION_LOG_PATH WRAPPER_DIR "/validation"

static struct wrapper_log wrapper_log_options[] = {
	{"info", WRAPPER_LOG_INFO},
	{"error", WRAPPER_LOG_ERROR},
	{"shader", WRAPPER_LOG_SHADER},
	{"validation", WRAPPER_LOG_VALIDATION},
	{"bcn", WRAPPER_LOG_BCN},
	{"apidump", WRAPPER_LOG_APIDUMP},
	{"warn", WRAPPER_LOG_WARN},
	{"debug", WRAPPER_LOG_DEBUG},
	{NULL, 0}
};

uint64_t wrapper_log_mask;

static void get_formatted_date_time(char *buf, size_t length) 
{  
   time_t rawtime = time(NULL);
   struct tm tmbuf;
   struct tm *ptm = localtime_r(&rawtime, &tmbuf);
   if (ptm) {
      strftime(buf, length, "%F_%H-%M-%S", ptm);
   } else {
      snprintf(buf, length, "unknown_date");
   }
}

static char exe_name[256] = "wrapper_app";
static pthread_once_t exe_name_once = PTHREAD_ONCE_INIT;

static void compute_executable_name(void) {
   int fd = open("/proc/self/cmdline", O_RDONLY);

   if (fd != -1) {
      char buf[PATH_MAX_SIZE];
      ssize_t len = read(fd, buf, sizeof(buf) - 1);
      close(fd);
      if (len > 0) {
         buf[len] = '\0';
         char *ptr = strrchr(buf, '/');
         if (ptr)
            ptr = ptr + 1;
         else
            ptr = buf;

         char *backslash = strrchr(ptr, '\\');
         if (backslash)
            ptr = backslash + 1;

         snprintf(exe_name, sizeof(exe_name), "%s", ptr);
      }
   }
}

/* Antes se releía /proc/self/cmdline y se reescribía un búfer estático en CADA llamada (sin sincronización). */
char *get_executable_name() {
   pthread_once(&exe_name_once, compute_executable_name);
   return exe_name;
}

static unsigned long long get_debug_flag(const char *option) {
   int index = 0;

   while (wrapper_log_options[index].name != NULL) {
      if (!strcasecmp(wrapper_log_options[index].name, option))
         return wrapper_log_options[index].value;

      index++;
   }

   return 0;
}

static void parse_wrapper_debug_str(char *wrapper_log_level_env) {
   if (!wrapper_log_level_env) {
      wrapper_log_mask = 0;
      return;
   }

   /* Usamos strdup para duplicar la cadena de entorno de forma segura antes de tokenizar */
   char *env_copy = strdup(wrapper_log_level_env);
   if (!env_copy) return;

   char *save = NULL;
   char *option = strtok_r(env_copy, ",", &save);
   while (option != NULL) {
      while (*option == ' ' || *option == '\t') option++;                       /* "info, error" */
      size_t n = strlen(option);
      while (n > 0 && (option[n - 1] == ' ' || option[n - 1] == '\t')) option[--n] = '\0';
      if (!strcasecmp(option, "all") || !strcmp(option, "*")) {
         for (int i = 0; wrapper_log_options[i].name != NULL; i++)
            wrapper_log_mask |= wrapper_log_options[i].value;
      } else {
         wrapper_log_mask |= get_debug_flag(option);
      }
      option = strtok_r(NULL, ",", &save);
   }
   free(env_copy);
}

int get_wrapper_log_level(const char *option) {
    uint64_t flag = get_debug_flag(option);

    if (wrapper_log_mask & flag)
       return 1;

    return 0;
}

static pthread_mutex_t wrapper_log_mutex = PTHREAD_MUTEX_INITIALIZER;

void write_to_logfile(const char *fmt, const char *level, ...)  {
   static FILE *wrapper_log_file = NULL;
   static int diag = -1;
   char stackbuf[1024];
   char *msg = stackbuf;
   va_list va_args, va_copy_args;

   /* Se formatea el mensaje ANTES de tomar el cierre y se escribe de una vez: antes eran tres fprintf separados
      y las líneas de hilos distintos se mezclaban; además la apertura perezosa del fichero no estaba protegida. */
   va_start(va_args, level);
   va_copy(va_copy_args, va_args);
   int needed = vsnprintf(stackbuf, sizeof(stackbuf), fmt, va_args);
   va_end(va_args);
   if (needed >= (int)sizeof(stackbuf)) {
      char *big = (char *)malloc((size_t)needed + 1);
      if (big) {
         vsnprintf(big, (size_t)needed + 1, fmt, va_copy_args);
         msg = big;
      }
   }
   va_end(va_copy_args);
   if (needed < 0) return;

   pthread_mutex_lock(&wrapper_log_mutex);

   if (!wrapper_log_file) {
      const char *env_filename = getenv("WRAPPER_LOG_FILE");
      char date[256];

      get_formatted_date_time(date, sizeof(date));

      if (env_filename) {
         if (!strcmp("stdout", env_filename)) {
            wrapper_log_file = stdout;
         } else {
            wrapper_log_file = fopen(env_filename, "w");
         }
      } else {
         char *wrapper_log_filename = NULL;
         if (asprintf(&wrapper_log_filename, "%s/%s_%s", WRAPPER_LOG_PATH, get_executable_name(), date) >= 0 && wrapper_log_filename) {
            wrapper_log_file = fopen(wrapper_log_filename, "w");
            free(wrapper_log_filename);
         }
      }

      if (!wrapper_log_file) {
         wrapper_log_file = stdout;
      }
   }

   fprintf(wrapper_log_file, "[%s]: %s\n", level, msg);
   fflush(wrapper_log_file);

   if (diag == -1)
      diag = getenv("WRAPPER_DIAG") ? atoi(getenv("WRAPPER_DIAG")) : 0;

   if (diag)
      fprintf(stderr, "[%s]: %s\n", level, msg);

   pthread_mutex_unlock(&wrapper_log_mutex);

   if (msg != stackbuf)
      free(msg);
}

void init_wrapper_logging()
{
   char *wrapper_log_level_env = getenv("WRAPPER_LOG_LEVEL");
   parse_wrapper_debug_str(wrapper_log_level_env);

   CREATE_FOLDER(WRAPPER_DIR, 0770);
   CREATE_FOLDER(WRAPPER_LOG_PATH, 0770);
   CREATE_FOLDER(WRAPPER_SHADER_LOG_PATH, 0770);
   CREATE_FOLDER(WRAPPER_VALIDATION_LOG_PATH, 0770);
}

void dump_shader_code(const uint32_t *code, size_t size) {
   char *file = NULL;
   static unsigned index = 0;   /* atómico: dos hilos creando shaders a la vez usaban el mismo índice y se pisaban el fichero */
   unsigned my_index = __atomic_fetch_add(&index, 1u, __ATOMIC_RELAXED);

   if (asprintf(&file, "%s/%s_shader_%u.spv", WRAPPER_SHADER_LOG_PATH, get_executable_name(), my_index) < 0)
      return;

   if (file) {
      FILE *fp = fopen(file, "wb");
      if (fp) {
         fwrite(code, 1, size, fp);
         fclose(fp);
      }
      free(file);
   }
}

VKAPI_ATTR VkBool32 VKAPI_CALL
wrapper_debug_utils_messenger(VkDebugUtilsMessageSeverityFlagBitsEXT messageSeverity,
                              VkDebugUtilsMessageTypeFlagsEXT messageTypes,
                              const VkDebugUtilsMessengerCallbackDataEXT *callbackData,
                              void *userData)
{
   static FILE *vvl_log_file = NULL;
   
   /* Protección estricta contra punteros nulos para prevenir cierres forzosos (crashes) */
   const char* messageIdName = (callbackData && callbackData->pMessageIdName) ? callbackData->pMessageIdName : "Unknown";
   int32_t messageIdNumber = callbackData ? callbackData->messageIdNumber : 0;
   const char* message = (callbackData && callbackData->pMessage) ? callbackData->pMessage : "";

   static pthread_mutex_t vvl_mutex = PTHREAD_MUTEX_INITIALIZER;
   pthread_mutex_lock(&vvl_mutex);

   if (!vvl_log_file) {
       char date[256];
       char *vvl_log_filename = NULL;

       get_formatted_date_time(date, sizeof(date));

       if (asprintf(&vvl_log_filename, "%s/%s_%s", WRAPPER_VALIDATION_LOG_PATH, get_executable_name(), date) >= 0 && vvl_log_filename) {
          vvl_log_file = fopen(vvl_log_filename, "w");
          free(vvl_log_filename);
       }
   }

   if (vvl_log_file) {
      fprintf(vvl_log_file, "[%s] Code %i : %s\n", messageIdName, messageIdNumber, message);
      fflush(vvl_log_file);
   }

   pthread_mutex_unlock(&vvl_mutex);

   return 0;
}
