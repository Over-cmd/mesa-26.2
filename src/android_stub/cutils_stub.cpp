#include <cstdlib>
#include <cstdint>
#include <cutils/native_handle.h>
#include <cutils/properties.h>
#include <cutils/trace.h>

extern "C" {

// --- Native Handle Stubs ---
native_handle_t* native_handle_create(int numFds, int numInts) {
    if (numFds < 0 || numInts < 0) return nullptr;
    size_t size = sizeof(native_handle_t) + sizeof(int) * (numFds + numInts);
    native_handle_t* h = static_cast<native_handle_t*>(malloc(size));
    if (h) {
        h->version = sizeof(native_handle_t);
        h->numFds = numFds;
        h->numInts = numInts;
    }
    return h;
}

int native_handle_delete(native_handle_t* h) {
    if (!h) return -1;
    free(h);
    return 0;
}

// --- Properties & Trace Stubs (Wrapper 25) ---
int
property_get(const char *key, char *value, const char *default_value)
{
   if (default_value && value) {
      // Copiar el valor por defecto de manera segura
      int i = 0;
      while (default_value[i] != '\0' && i < 919) { // PROPERTY_VALUE_MAX aprox
         value[i] = default_value[i];
         i++;
      }
      value[i] = '\0';
      return i;
   }
   return 0;
}

void
atrace_begin_body(const char * /*name*/)
{
}

void
atrace_end_body()
{
}

void
atrace_init()
{
}

uint64_t
atrace_get_enabled_tags()
{
   return 0; // ATRACE_TAG_NOT_READY equivalente
}

}
