#ifndef ANDROID_CUTILS_PROPERTIES_H
#define ANDROID_CUTILS_PROPERTIES_H

#ifdef __cplusplus
extern "C" {
#endif

int property_get(const char *key, char *value, const char *default_value);
int property_set(const char *key, const char *value);

#ifdef __cplusplus
}
#endif

#endif
