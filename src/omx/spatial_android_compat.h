// Minimal Android compatibility layer for cross-compilation
// Provides minimal Android framework API compatibility

#ifndef SPATIAL_ANDROID_COMPAT_H_
#define SPATIAL_ANDROID_COMPAT_H_

#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>

// Android log priorities
#define ANDROID_LOG_UNKNOWN     0
#define ANDROID_LOG_DEFAULT     1
#define ANDROID_LOG_VERBOSE     2
#define ANDROID_LOG_DEBUG       3
#define ANDROID_LOG_INFO        4
#define ANDROID_LOG_WARN        5
#define ANDROID_LOG_ERROR       6
#define ANDROID_LOG_FATAL       7
#define ANDROID_LOG_SILENT      8

// Minimal log functions - use standard C functions
#ifdef __ANDROID__
#include <android/log.h>
#else
// Stub implementations for cross-compilation
static inline void __android_log_print(int prio, const char* tag, const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    fprintf(stderr, "[%s] ", tag);
    vfprintf(stderr, fmt, args);
    fprintf(stderr, "\n");
    va_end(args);
}

static inline void __android_log_write(int prio, const char* tag, const char* text) {
    fprintf(stderr, "[%s] %s\n", tag, text);
}

static inline void __android_log_assert(const char* cond, const char* tag, const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    fprintf(stderr, "ASSERTION FAILED: %s [%s] ", cond, tag);
    vfprintf(stderr, fmt, args);
    fprintf(stderr, "\n");
    va_end(args);
    abort();
}

// Android log macros
#define ALOGV(...) __android_log_print(ANDROID_LOG_VERBOSE, __VA_ARGS__)
#define ALOGD(...) __android_log_print(ANDROID_LOG_DEBUG, __VA_ARGS__)
#define ALOGI(...) __android_log_print(ANDROID_LOG_INFO, __VA_ARGS__)
#define ALOGW(...) __android_log_print(ANDROID_LOG_WARN, __VA_ARGS__)
#define ALOGE(...) __android_log_print(ANDROID_LOG_ERROR, __VA_ARGS__)
#define ALOGF(...) __android_log_print(ANDROID_LOG_FATAL, __VA_ARGS__)
#define ALOGV_IF(cond, ...) do { if (cond) ALOGV(__VA_ARGS__); } while(0)
#define ALOGD_IF(cond, ...) do { if (cond) ALOGD(__VA_ARGS__); } while(0)
#define ALOGI_IF(cond, ...) do { if (cond) ALOGI(__VA_ARGS__); } while(0)
#define ALOGW_IF(cond, ...) do { if (cond) ALOGW(__VA_ARGS__); } while(0)
#define ALOGE_IF(cond, ...) do { if (cond) ALOGE(__VA_ARGS__); } while(0)
#define ALOGV(...) ALOGV_IF(1, __VA_ARGS__)
#define ALOGD(...) ALOGD_IF(1, __VA_ARGS__)
#define ALOGI(...) ALOGI_IF(1, __VA_ARGS__)
#define ALOGW(...) ALOGW_IF(1, __VA_ARGS__)
#define ALOGE(...) ALOGE_IF(1, __VA_ARGS__)

#define LOG_TAG "SoftAc3Omx"
#endif  // __ANDROID__

#endif  // SPATIAL_ANDROID_COMPAT_H_