#ifndef ANDROID_CUTILS_LOG_H
#define ANDROID_CUTILS_LOG_H

#include <android/log.h>

#define ALOGE(...) __android_log_print(ANDROID_LOG_ERROR, "cutils_stub", __VA_ARGS__)
#define ALOGW(...) __android_log_print(ANDROID_LOG_WARN, "cutils_stub", __VA_ARGS__)
#define ALOGI(...) __android_log_print(ANDROID_LOG_INFO, "cutils_stub", __VA_ARGS__)
#define ALOGD(...) __android_log_print(ANDROID_LOG_DEBUG, "cutils_stub", __VA_ARGS__)

#endif
